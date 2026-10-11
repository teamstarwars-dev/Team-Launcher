"""API REST de la marketplace (V8).

Trois publics, trois niveaux d'exigence :
  - développeurs : compte + 2FA OBLIGATOIRE avant toute soumission ;
  - relecteurs   : 2FA, et jamais d'approbation de leur propre soumission ;
  - launcher     : lecture publique du catalogue et de la liste de
                   révocation, rien d'autre.
"""


# PAS de « from __future__ import annotations » ici : FastAPI doit pouvoir
# résoudre les alias de dépendances (Db, Auth, Mfa…), locaux à create_app.

import difflib
import io
import json
import re
import time
import zipfile
from pathlib import Path
from typing import Annotated, Iterator

from fastapi import APIRouter, Depends, FastAPI, File, Form, Header, HTTPException, UploadFile
from fastapi import Request
from fastapi.responses import FileResponse, PlainTextResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from sqlalchemy import func, select
from sqlalchemy.orm import Session as DbSession, sessionmaker

from . import models as m
from . import notify, packages, security
from .config import Settings
from .signing import Signer

STATIC_DIR = Path(__file__).parent / "static"

# L'interface de review affiche du contenu écrit par les développeurs
# relus : aucun script en ligne, aucune ressource tierce, pas d'affichage
# dans un cadre (clickjacking sur « Approuver »).
SECURITY_HEADERS = {
    "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; "
                               "connect-src 'self'; img-src 'self'; form-action 'self'; "
                               "base-uri 'none'; frame-ancestors 'none'",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Cache-Control": "no-store",
}

EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]{1,189}\.[^@\s]{2,}$")


# --- Schémas -----------------------------------------------------------------


class RegisterIn(BaseModel):
    email: str = Field(max_length=254)
    password: str = Field(max_length=security.MAX_PASSWORD)
    display_name: str = Field(min_length=2, max_length=64)


class LoginIn(BaseModel):
    email: str = Field(max_length=254)
    password: str = Field(max_length=security.MAX_PASSWORD)


class CodeIn(BaseModel):
    code: str = Field(min_length=6, max_length=6)


class ProjectIn(BaseModel):
    kind: str = Field(pattern="^(plugin|mod)$")
    slug: str = Field(max_length=64)
    name: str = Field(min_length=2, max_length=128)


class NoteIn(BaseModel):
    note: str = Field(default="", max_length=4000)


class RevokeIn(BaseModel):
    keyIds: list[str] = Field(default_factory=list, max_length=100)
    releases: list[dict] = Field(default_factory=list, max_length=500)
    fileHashes: list[str] = Field(default_factory=list, max_length=500)
    note: str = Field(default="", max_length=4000)


# Raisons comptées : celles que le launcher sait nommer (voir
# cpp/src/plugin_telemetry.cpp). Rien de libre : aucun texte ne passe.
TELEMETRY_KINDS = ("start_failed", "exited", "denied", "rate_abuse", "permission_probe",
                   "invalid_requests", "bad_frame", "killed_other")


class TelemetryEvent(BaseModel):
    plugin: str = Field(max_length=64)
    version: str = Field(max_length=32)
    kind: str = Field(max_length=24)
    count: int = Field(ge=1, le=10_000)


class TelemetryIn(BaseModel):
    v: int = Field(ge=1, le=1)
    launcher: str = Field(default="", max_length=32)
    os: str = Field(default="", max_length=16)
    events: list[TelemetryEvent] = Field(max_length=200)


class KillIn(BaseModel):
    on: bool
    note: str = Field(default="", max_length=4000)


# --- Application ---------------------------------------------------------------


def create_app(settings: Settings, session_factory: sessionmaker, signer: Signer) -> FastAPI:
    app = FastAPI(title="Team Launcher — marketplace", version="8.0.0",
                  docs_url=None, redoc_url=None)  # pas de documentation publique
    settings.storage_dir.mkdir(parents=True, exist_ok=True)

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        for k, v in SECURITY_HEADERS.items():
            response.headers.setdefault(k, v)
        return response

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    @app.get("/review", include_in_schema=False)
    def review_page():
        return FileResponse(STATIC_DIR / "review.html")

    def db() -> Iterator[DbSession]:
        s = session_factory()
        try:
            yield s
        finally:
            s.close()

    Db = Annotated[DbSession, Depends(db)]

    def audit(s: DbSession, actor: int | None, action: str, target: str = "", note: str = ""):
        s.add(m.AuditEvent(at=security.now(), actor_id=actor, action=action, target=target,
                           note=note))

    # --- Authentification -----------------------------------------------------

    def session_of(s: Db, authorization: Annotated[str | None, Header()] = None
                   ) -> tuple[m.Session, m.Developer]:
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(401, "authentification requise")
        row = s.get(m.Session, security.token_hash(authorization[7:].strip()))
        if row is None or row.expires_at <= security.now():
            raise HTTPException(401, "session absente ou expirée")
        dev = s.get(m.Developer, row.developer_id)
        if dev is None:
            raise HTTPException(401, "compte introuvable")
        return row, dev

    Auth = Annotated[tuple[m.Session, m.Developer], Depends(session_of)]

    def mfa(auth: Auth) -> m.Developer:
        row, dev = auth
        # Le mot de passe seul n'ouvre rien de sensible : 2FA posée ET
        # présentée dans cette session.
        if not dev.totp_enabled:
            raise HTTPException(403, "double authentification obligatoire : configurez-la d'abord")
        if not row.mfa_ok:
            raise HTTPException(403, "code de double authentification requis")
        return dev

    Mfa = Annotated[m.Developer, Depends(mfa)]

    def reviewer(dev: Mfa) -> m.Developer:
        if not dev.is_reviewer:
            raise HTTPException(403, "réservé aux relecteurs")
        return dev

    def admin(dev: Mfa) -> m.Developer:
        if not dev.is_admin:
            raise HTTPException(403, "réservé aux administrateurs")
        return dev

    Reviewer = Annotated[m.Developer, Depends(reviewer)]
    Admin = Annotated[m.Developer, Depends(admin)]

    auth_r = APIRouter(prefix="/v1/auth")

    @auth_r.post("/register", status_code=201)
    def register(body: RegisterIn, s: Db):
        email = body.email.strip().lower()
        if not EMAIL_RE.match(email):
            raise HTTPException(400, "adresse électronique invalide")
        if (why := security.password_ok(body.password)) is not None:
            raise HTTPException(400, why)
        if s.scalar(select(m.Developer).where(m.Developer.email == email)) is not None:
            # Même réponse qu'un succès serait plus discret, mais l'inscription
            # demande de toute façon une confirmation 2FA immédiate : un
            # message clair vaut mieux qu'un compte fantôme.
            raise HTTPException(409, "adresse déjà inscrite")
        dev = m.Developer(email=email, display_name=body.display_name.strip(),
                          password_hash=security.hash_password(body.password),
                          created_at=security.now())
        s.add(dev)
        s.flush()
        audit(s, dev.id, "register")
        s.commit()
        return {"id": dev.id}

    @auth_r.post("/login")
    def login(body: LoginIn, s: Db):
        email = body.email.strip().lower()
        dev = s.scalar(select(m.Developer).where(m.Developer.email == email))
        if dev is None:
            security.burn_time()  # même durée qu'un compte réel
            raise HTTPException(401, "identifiants invalides")
        if dev.locked_until > security.now():
            raise HTTPException(429, "compte verrouillé temporairement après trop d'échecs")
        if not security.verify_password(dev.password_hash, body.password):
            dev.failed_logins += 1
            if dev.failed_logins >= security.LOCK_AFTER:
                dev.locked_until = security.now() + security.LOCK_SECONDS
                dev.failed_logins = 0
                audit(s, dev.id, "lockout")
            s.commit()
            raise HTTPException(401, "identifiants invalides")
        dev.failed_logins = 0
        token, h = security.new_session_token()
        s.add(m.Session(token_hash=h, developer_id=dev.id, created_at=security.now(),
                        expires_at=security.now() + settings.session_ttl, mfa_ok=False))
        audit(s, dev.id, "login")
        s.commit()
        return {"token": token, "totpEnabled": dev.totp_enabled}

    @auth_r.post("/totp/setup")
    def totp_setup(auth: Auth, s: Db):
        _, dev = auth
        if dev.totp_enabled:
            # Changer de second facteur se fait par un administrateur : sinon
            # quiconque vole un mot de passe pourrait poser SA 2FA.
            raise HTTPException(409, "la double authentification est déjà configurée")
        sec = security.new_totp_secret()
        dev.totp_secret_enc = security.encrypt_secret(settings.totp_enc_key, sec)
        s.commit()
        return {"secret": sec, "uri": security.totp_uri(sec, dev.email)}

    def _check_code(dev: m.Developer, code: str, s: DbSession) -> bool:
        sec = security.decrypt_secret(settings.totp_enc_key, dev.totp_secret_enc)
        if not sec:
            return False
        step = security.check_totp(sec, code, dev.totp_last_step)
        if step is None:
            return False
        dev.totp_last_step = step
        return True

    @auth_r.post("/totp/confirm")
    def totp_confirm(body: CodeIn, auth: Auth, s: Db):
        row, dev = auth
        if dev.totp_enabled:
            raise HTTPException(409, "déjà configurée")
        if not _check_code(dev, body.code, s):
            s.commit()
            raise HTTPException(401, "code invalide")
        dev.totp_enabled = True
        row.mfa_ok = True
        audit(s, dev.id, "totp_enabled")
        s.commit()
        return {"ok": True}

    @auth_r.post("/totp/verify")
    def totp_verify(body: CodeIn, auth: Auth, s: Db):
        row, dev = auth
        if not dev.totp_enabled:
            raise HTTPException(409, "double authentification non configurée")
        if not _check_code(dev, body.code, s):
            s.commit()
            raise HTTPException(401, "code invalide ou déjà utilisé")
        row.mfa_ok = True
        s.commit()
        return {"ok": True}

    @auth_r.post("/logout")
    def logout(auth: Auth, s: Db):
        row, _ = auth
        s.delete(row)
        s.commit()
        return {"ok": True}

    app.include_router(auth_r)

    # --- Développeurs ----------------------------------------------------------

    dev_r = APIRouter(prefix="/v1")

    @dev_r.get("/me")
    def me(auth: Auth):
        row, dev = auth
        return {"id": dev.id, "email": dev.email, "displayName": dev.display_name,
                "totpEnabled": dev.totp_enabled, "mfaOk": row.mfa_ok,
                "reviewer": dev.is_reviewer, "admin": dev.is_admin}

    @dev_r.post("/projects", status_code=201)
    def create_project(body: ProjectIn, dev: Mfa, s: Db):
        if not packages.SLUG_RE.match(body.slug):
            raise HTTPException(400, "identifiant invalide (minuscules, chiffres, - _ .)")
        if s.scalar(select(m.Project).where(m.Project.kind == body.kind,
                                            m.Project.slug == body.slug)) is not None:
            raise HTTPException(409, "identifiant déjà pris")
        p = m.Project(kind=body.kind, slug=body.slug, name=body.name.strip(), owner_id=dev.id,
                      created_at=security.now())
        s.add(p)
        s.flush()
        audit(s, dev.id, "project_create", f"{body.kind}:{body.slug}")
        s.commit()
        return {"id": p.id}

    @dev_r.get("/projects/mine")
    def my_projects(dev: Mfa, s: Db):
        rows = s.scalars(select(m.Project).where(m.Project.owner_id == dev.id)).all()
        return [{"kind": p.kind, "slug": p.slug, "name": p.name, "banned": p.banned} for p in rows]

    async def _read_capped(f: UploadFile, cap: int) -> bytes:
        data = await f.read(cap + 1)
        if len(data) > cap:
            raise HTTPException(413, "fichier trop volumineux")
        return data

    @dev_r.post("/projects/{kind}/{slug}/submissions", status_code=201)
    async def submit(kind: str, slug: str, dev: Mfa, s: Db,
                     version: Annotated[str, Form(max_length=32)],
                     binary: Annotated[UploadFile, File()],
                     source: Annotated[UploadFile, File()]):
        p = s.scalar(select(m.Project).where(m.Project.kind == kind, m.Project.slug == slug))
        if p is None or p.owner_id != dev.id:
            raise HTTPException(404, "projet introuvable")
        if p.banned:
            raise HTTPException(403, "projet suspendu")
        if s.scalar(select(m.Submission).where(m.Submission.project_id == p.id,
                                               m.Submission.version == version)):
            raise HTTPException(409, "cette version a déjà été soumise")
        bin_data = await _read_capped(binary, settings.max_upload_bytes)
        src_data = await _read_capped(source, settings.max_upload_bytes)
        try:
            ins = packages.inspect_zip(bin_data, max_files=settings.max_files,
                                       max_unpacked=settings.max_unpacked_bytes,
                                       expect_manifest=(kind == "plugin"))
            if kind == "plugin":
                packages.check_plugin_manifest(ins.manifest, slug, version)
            else:
                if not packages.VERSION_RE.match(version):
                    raise packages.PackageError("version invalide")
                packages.check_mod_files(ins.files)
            # Les sources sont relues par un humain : on les inspecte aussi,
            # pour qu'un relecteur n'ouvre jamais une bombe ZIP.
            packages.inspect_zip(src_data, max_files=settings.max_files * 4,
                                 max_unpacked=settings.max_unpacked_bytes,
                                 expect_manifest=False)
        except packages.PackageError as exc:
            raise HTTPException(400, str(exc)) from exc

        sub = m.Submission(project_id=p.id, version=version, uploaded_by=dev.id,
                           created_at=security.now(), status="pending",
                           binary_sha512=_sha512(bin_data), source_sha512=_sha512(src_data),
                           files_json=json.dumps(ins.files, sort_keys=True),
                           manifest_json=json.dumps(ins.manifest))
        s.add(sub)
        s.flush()
        d = settings.storage_dir / str(sub.id)
        d.mkdir(parents=True, exist_ok=False)
        (d / "binary.zip").write_bytes(bin_data)
        (d / "source.zip").write_bytes(src_data)
        audit(s, dev.id, "submit", f"{kind}:{slug}@{version}")
        s.commit()
        notify.new_submission(settings.discord_webhook, settings.public_url, kind, slug, p.name,
                              version, dev.display_name)
        return {"id": sub.id, "status": sub.status}

    @dev_r.get("/submissions/{sid}")
    def submission_status(sid: int, dev: Mfa, s: Db):
        sub = s.get(m.Submission, sid)
        if sub is None:
            raise HTTPException(404, "soumission introuvable")
        p = s.get(m.Project, sub.project_id)
        if p.owner_id != dev.id and not dev.is_reviewer:
            raise HTTPException(404, "soumission introuvable")
        return {"id": sub.id, "project": p.slug, "version": sub.version, "status": sub.status,
                "note": sub.review_note}

    app.include_router(dev_r)

    # --- Review -------------------------------------------------------------------

    rev_r = APIRouter(prefix="/v1/review")

    @rev_r.get("/queue")
    def queue(rv: Reviewer, s: Db):
        rows = s.execute(select(m.Submission, m.Project)
                         .join(m.Project, m.Project.id == m.Submission.project_id)
                         .where(m.Submission.status == "pending")
                         .order_by(m.Submission.created_at)).all()
        return [{"id": sub.id, "kind": p.kind, "slug": p.slug, "name": p.name,
                 "version": sub.version, "submittedAt": sub.created_at,
                 "own": sub.uploaded_by == rv.id} for sub, p in rows]

    def _previous_approved(s: DbSession, sub: m.Submission) -> m.Submission | None:
        return s.scalar(select(m.Submission)
                        .where(m.Submission.project_id == sub.project_id,
                               m.Submission.status == "approved",
                               m.Submission.id != sub.id)
                        .order_by(m.Submission.reviewed_at.desc(), m.Submission.id.desc()).limit(1))

    @rev_r.get("/submissions/{sid}")
    def review_detail(sid: int, rv: Reviewer, s: Db):
        sub = s.get(m.Submission, sid)
        if sub is None:
            raise HTTPException(404, "soumission introuvable")
        p = s.get(m.Project, sub.project_id)
        files = json.loads(sub.files_json)
        prev = _previous_approved(s, sub)
        changes = None
        if prev is not None:
            old = json.loads(prev.files_json)
            changes = {
                "previousVersion": prev.version,
                "added": sorted(set(files) - set(old)),
                "removed": sorted(set(old) - set(files)),
                "changed": sorted(k for k in set(files) & set(old) if files[k] != old[k]),
            }
        return {"id": sub.id, "kind": p.kind, "slug": p.slug, "version": sub.version,
                "status": sub.status, "manifest": json.loads(sub.manifest_json),
                "files": files, "changesSincePrevious": changes,
                "own": sub.uploaded_by == rv.id}

    @rev_r.get("/submissions/{sid}/source-diff", response_class=PlainTextResponse)
    def source_diff(sid: int, rv: Reviewer, s: Db):
        """Différence des SOURCES avec la dernière version approuvée — la
        re-review à chaque mise à jour porte sur ce qui a changé."""
        sub = s.get(m.Submission, sid)
        if sub is None:
            raise HTTPException(404, "soumission introuvable")
        prev = _previous_approved(s, sub)
        new = _text_files(settings.storage_dir / str(sub.id) / "source.zip")
        old = _text_files(settings.storage_dir / str(prev.id) / "source.zip") if prev else {}
        out: list[str] = []
        budget = 2 * 1024 * 1024
        for name in sorted(set(new) | set(old)):
            a, b = old.get(name, ""), new.get(name, "")
            if a == b:
                continue
            for line in difflib.unified_diff(a.splitlines(), b.splitlines(),
                                             f"a/{name}", f"b/{name}", lineterm=""):
                out.append(line)
                budget -= len(line) + 1
                if budget <= 0:
                    out.append("… (différence tronquée à 2 Mio)")
                    return "\n".join(out)
        return "\n".join(out) if out else "(aucune différence dans les fichiers texte)"

    def _decide(sid: int, rv: m.Developer, s: DbSession, approve: bool, note: str):
        sub = s.get(m.Submission, sid)
        if sub is None:
            raise HTTPException(404, "soumission introuvable")
        if sub.status != "pending":
            raise HTTPException(409, f"soumission déjà traitée ({sub.status})")
        if sub.uploaded_by == rv.id:
            # Séparation des rôles : un relecteur ne valide jamais son propre
            # travail. Un compte relecteur compromis ne peut donc pas, seul,
            # faire signer n'importe quoi.
            raise HTTPException(403, "un relecteur ne peut pas statuer sur sa propre soumission")
        p = s.get(m.Project, sub.project_id)
        sub.reviewer_id = rv.id
        sub.review_note = note
        sub.reviewed_at = security.now()
        if approve:
            sub.signature_json = signer.sign_release(p.slug, sub.version,
                                                     json.loads(sub.files_json),
                                                     sub.reviewed_at, kind=p.kind)
            sub.status = "approved"
        else:
            sub.status = "rejected"
        audit(s, rv.id, "approve" if approve else "reject",
              f"{p.kind}:{p.slug}@{sub.version}", note)
        s.commit()
        return {"id": sub.id, "status": sub.status}

    @rev_r.post("/submissions/{sid}/approve")
    def approve(sid: int, body: NoteIn, rv: Reviewer, s: Db):
        return _decide(sid, rv, s, True, body.note)

    @rev_r.post("/submissions/{sid}/reject")
    def reject(sid: int, body: NoteIn, rv: Reviewer, s: Db):
        if not body.note.strip():
            raise HTTPException(400, "un refus doit être motivé")
        return _decide(sid, rv, s, False, body.note)

    app.include_router(rev_r)

    # --- Administration : révocation et kill-switch ---------------------------------

    adm_r = APIRouter(prefix="/v1/admin")

    def _publish(s: DbSession, actor: int, mutate, action: str, note: str):
        last = s.scalar(select(m.Revocation).order_by(m.Revocation.seq.desc()).limit(1))
        content = json.loads(last.content_json) if last else {
            "keyIds": [], "releases": [], "fileHashes": [], "killAll": False}
        mutate(content)
        seq = (last.seq + 1) if last else 1
        content["seq"] = seq
        content["issuedAt"] = security.now()
        doc = signer.sign_revocations(content)
        s.add(m.Revocation(seq=seq, issued_at=content["issuedAt"],
                           kill_all=bool(content["killAll"]),
                           content_json=json.dumps(content), document=doc))
        audit(s, actor, action, f"seq={seq}", note)
        s.commit()
        return {"seq": seq}

    @adm_r.post("/revoke")
    def revoke(body: RevokeIn, ad: Admin, s: Db):
        def add(c):
            c["keyIds"] = sorted(set(c["keyIds"]) | set(body.keyIds))
            rel = {(r["id"], r["version"]) for r in c["releases"]}
            for r in body.releases:
                if isinstance(r.get("id"), str) and isinstance(r.get("version"), str):
                    rel.add((r["id"], r["version"]))
            c["releases"] = [{"id": i, "version": v} for i, v in sorted(rel)]
            c["fileHashes"] = sorted(set(c["fileHashes"]) | set(body.fileHashes))
        try:
            out = _publish(s, ad.id, add, "revoke", body.note)
        except ValueError as exc:
            raise HTTPException(400, str(exc)) from exc
        # Les versions révoquées disparaissent aussi du catalogue.
        for r in body.releases:
            subs = s.scalars(select(m.Submission)
                             .join(m.Project, m.Project.id == m.Submission.project_id)
                             .where(m.Project.slug == r.get("id"),
                                    m.Submission.version == r.get("version"))).all()
            for sub in subs:
                sub.status = "revoked"
        s.commit()
        return out

    @adm_r.post("/killswitch")
    def killswitch(body: KillIn, ad: Admin, s: Db):
        def set_kill(c):
            c["killAll"] = body.on
        return _publish(s, ad.id, set_kill, "killswitch_on" if body.on else "killswitch_off",
                        body.note)

    @adm_r.get("/telemetry")
    def telemetry_summary(ad: Admin, s: Db, days: int = 7):
        days = max(1, min(days, 90))
        since = int(time.strftime("%Y%m%d", time.gmtime(security.now() - days * 86400)))
        rows = s.execute(select(m.TelemetryDaily.plugin, m.TelemetryDaily.version,
                                m.TelemetryDaily.kind, func.sum(m.TelemetryDaily.count))
                         .where(m.TelemetryDaily.day >= since)
                         .group_by(m.TelemetryDaily.plugin, m.TelemetryDaily.version,
                                   m.TelemetryDaily.kind)).all()
        out = [{"plugin": p, "version": v, "kind": k, "count": int(c)} for p, v, k, c in rows]
        out.sort(key=lambda r: -r["count"])
        return {"days": days, "rows": out}

    app.include_router(adm_r)

    # --- Public : ce que lit le launcher ------------------------------------------

    pub_r = APIRouter(prefix="/v1/public")

    # Garde-fou par adresse, en mémoire seulement : l'adresse sert à limiter
    # le débit pendant une heure et n'est JAMAIS écrite en base.
    tele_seen: dict[str, list[float]] = {}

    @pub_r.post("/telemetry", status_code=204)
    def telemetry(body: TelemetryIn, request: Request, s: Db):
        ip = request.client.host if request.client else "?"
        now_f = time.monotonic()
        recent = [t for t in tele_seen.get(ip, []) if now_f - t < 3600]
        if len(recent) >= 12:
            raise HTTPException(429, "trop d'envois")
        recent.append(now_f)
        tele_seen[ip] = recent
        if len(tele_seen) > 50_000:  # borne la mémoire
            tele_seen.clear()

        # Ne compte que des versions PUBLIÉES : le point d'entrée est public,
        # il ne doit pas servir à remplir la base de noms inventés.
        published = {(p.slug, sub.version) for sub, p in s.execute(
            select(m.Submission, m.Project)
            .join(m.Project, m.Project.id == m.Submission.project_id)
            .where(m.Project.kind == "plugin",
                   m.Submission.status.in_(("approved", "revoked")))).all()}
        day = int(time.strftime("%Y%m%d", time.gmtime(security.now())))
        for e in body.events:
            if e.kind not in TELEMETRY_KINDS or (e.plugin, e.version) not in published:
                continue
            row = s.get(m.TelemetryDaily, (day, e.plugin, e.version, e.kind))
            if row is None:
                s.add(m.TelemetryDaily(day=day, plugin=e.plugin, version=e.version,
                                       kind=e.kind, count=e.count))
            else:
                row.count += e.count
            s.flush()
        s.commit()
        return Response(status_code=204)

    @pub_r.get("/revocations", response_class=PlainTextResponse)
    def revocations(s: Db):
        last = s.scalar(select(m.Revocation).order_by(m.Revocation.seq.desc()).limit(1))
        if last is None:
            raise HTTPException(404, "aucune liste publiée")
        return last.document

    @pub_r.get("/catalog")
    def catalog(s: Db):
        rows = s.execute(select(m.Submission, m.Project)
                         .join(m.Project, m.Project.id == m.Submission.project_id)
                         .where(m.Submission.status == "approved", m.Project.banned.is_(False))
                         .order_by(m.Project.slug, m.Submission.reviewed_at.desc(), m.Submission.id.desc())).all()
        seen: set[tuple[str, str]] = set()
        out = []
        for sub, p in rows:
            if (p.kind, p.slug) in seen:
                continue  # seulement la dernière version approuvée
            seen.add((p.kind, p.slug))
            manifest = json.loads(sub.manifest_json)
            out.append({"kind": p.kind, "slug": p.slug, "name": p.name, "version": sub.version,
                        "description": str(manifest.get("description", ""))[:500],
                        "permissions": manifest.get("permissions", []),
                        "sha512": sub.binary_sha512})
        return out

    @pub_r.get("/download/{kind}/{slug}/{version}")
    def download(kind: str, slug: str, version: str, s: Db):
        row = s.execute(select(m.Submission, m.Project)
                        .join(m.Project, m.Project.id == m.Submission.project_id)
                        .where(m.Project.kind == kind, m.Project.slug == slug,
                               m.Submission.version == version,
                               m.Submission.status == "approved")).first()
        if row is None:
            raise HTTPException(404, "version introuvable ou non approuvée")
        sub, _ = row
        # Archive approuvée + signature.json produit à l'approbation.
        src = (settings.storage_dir / str(sub.id) / "binary.zip").read_bytes()
        buf = io.BytesIO()
        with zipfile.ZipFile(io.BytesIO(src)) as zin, \
                zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                zout.writestr(info, zin.read(info))
            zout.writestr("signature.json", sub.signature_json)
        return Response(buf.getvalue(), media_type="application/zip",
                        headers={"Content-Disposition": f'attachment; filename="{slug}-{version}.zip"'})

    app.include_router(pub_r)
    return app


def _sha512(data: bytes) -> str:
    import hashlib
    return hashlib.sha512(data).hexdigest()


def _text_files(path) -> dict[str, str]:
    """Fichiers texte d'une archive (déjà inspectée à la réception)."""
    out: dict[str, str] = {}
    try:
        with zipfile.ZipFile(path) as zf:
            for info in zf.infolist():
                if info.is_dir() or info.file_size > 512 * 1024:
                    continue
                try:
                    out[info.filename] = zf.read(info).decode("utf-8")
                except UnicodeDecodeError:
                    continue  # binaire : comparé par empreinte dans review_detail
    except (FileNotFoundError, zipfile.BadZipFile):
        pass
    return out
