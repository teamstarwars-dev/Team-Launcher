"""API sociale (V8, section 6) : comptes, amis, messages, présence, appels.

Reprise de l'ancienne API Node (server/), AVEC LE MÊME CONTRAT : mêmes
routes, mêmes corps JSON, erreurs sous la forme {"error": "…"}, DELETE
accepté via X-HTTP-Method-Override, événements par long-polling. Le
launcher (cpp/src/account.cpp) n'a donc qu'à changer d'adresse.

Ce qui change par rapport à Node, et seulement en mieux : Argon2id au lieu
de bcrypt, verrouillage après trop d'échecs, durée constante pour un
compte inexistant, et un seul compte pour le social et la marketplace.
"""

from __future__ import annotations

import asyncio
import json
import os
import re
import secrets
import time
from typing import Any, Callable

from fastapi import APIRouter, Body, FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from sqlalchemy import delete, func, or_, select, update
from sqlalchemy.orm import sessionmaker

from . import models as m
from . import security

USERNAME_RE = re.compile(r"^[a-zA-Z0-9_.]{3,24}$")
SESSION_TTL = 90 * 86400  # session d'un joueur : on ne le déconnecte pas chaque jour
ONLINE_WINDOW = 120       # en ligne = activité vue il y a moins de 2 minutes
MAX_EVENTS = 500          # par compte, anti-fuite (comme Node)
MAX_BODY = 64 * 1024


class ApiError(Exception):
    def __init__(self, code: int, msg: str):
        self.code, self.msg = code, msg


class Limiter:
    """Fenêtre glissante par adresse, en mémoire (l'adresse n'est jamais
    stockée en base)."""

    def __init__(self, max_hits: int, window: float):
        self.max, self.window, self.hits = max_hits, window, {}

    def check(self, request: Request) -> None:
        key = request.client.host if request.client else "?"
        now = time.monotonic()
        recent = [t for t in self.hits.get(key, []) if now - t < self.window]
        if len(recent) >= self.max:
            raise ApiError(429, "trop de requetes, reessaie plus tard")
        recent.append(now)
        self.hits[key] = recent
        if len(self.hits) > 50_000:
            self.hits.clear()


def iso(ts: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(ts))


def _s(body: dict, key: str) -> str:
    v = body.get(key)
    return "" if v is None else str(v)


def install(app: FastAPI, session_factory: sessionmaker) -> None:
    """Branche l'API sociale sur l'application (préfixe /api)."""

    auth_limiter = Limiter(30, 15 * 60)
    msg_limiter = Limiter(60, 60)
    call_limiter = Limiter(20, 60)

    @app.exception_handler(ApiError)
    async def api_error(request: Request, exc: ApiError):
        return JSONResponse({"error": exc.msg}, status_code=exc.code)

    previous_validation = app.exception_handlers.get(RequestValidationError)

    @app.exception_handler(RequestValidationError)
    async def validation(request: Request, exc: RequestValidationError):
        if request.url.path.startswith("/api/"):
            return JSONResponse({"error": "json invalide"}, status_code=400)
        if previous_validation:
            return await previous_validation(request, exc)
        from fastapi.exception_handlers import request_validation_exception_handler
        return await request_validation_exception_handler(request, exc)

    @app.middleware("http")
    async def method_override(request: Request, call_next):
        # Le client C++ ne fait que GET/POST : DELETE via cet en-tête.
        if request.url.path.startswith("/api/"):
            ov = request.headers.get("x-http-method-override", "")
            if request.method == "POST" and re.fullmatch(r"[A-Z]+", ov):
                request.scope["method"] = ov
            cl = request.headers.get("content-length")
            if cl and cl.isdigit() and int(cl) > MAX_BODY:
                return JSONResponse({"error": "requete trop volumineuse"}, status_code=413)
        return await call_next(request)

    r = APIRouter(prefix="/api")

    # --- utilitaires --------------------------------------------------------

    def db(fn: Callable) -> Any:
        with session_factory() as s:
            return fn(s)

    def find(s, username: str):
        if not USERNAME_RE.match(username or ""):
            return None
        return s.scalar(select(m.Developer).where(m.Developer.username_key == username.lower()))

    def pair(a: int, b: int) -> tuple[int, int]:
        return (a, b) if a < b else (b, a)

    def public(u: m.Developer) -> dict:
        return {"id": u.id, "username": u.username}

    def are_friends(s, a: int, b: int) -> bool:
        x, y = pair(a, b)
        f = s.get(m.Friendship, (x, y))
        return f is not None and f.status == "accepted"

    def push_event(s, user_id: int, type_: str, payload: dict) -> None:
        s.add(m.UserEvent(user_id=user_id, type=type_, payload=json.dumps(payload),
                          created_at=security.now()))
        s.flush()
        top = s.scalar(select(func.max(m.UserEvent.id)).where(m.UserEvent.user_id == user_id))
        if top and top > MAX_EVENTS:
            s.execute(delete(m.UserEvent).where(m.UserEvent.user_id == user_id,
                                                m.UserEvent.id <= top - MAX_EVENTS))

    def new_session(s, user: m.Developer) -> str:
        token = secrets.token_hex(32)  # 64 hexas, comme l'ancienne API
        now = security.now()
        s.add(m.Session(token_hash=security.token_hash(token), developer_id=user.id,
                        created_at=now, expires_at=now + SESSION_TTL, mfa_ok=False,
                        last_seen=now))
        return token

    def current(s, request: Request) -> tuple[m.Developer, str]:
        header = request.headers.get("authorization", "").strip()
        if not header.lower().startswith("bearer ") or len(header) < 20:
            raise ApiError(401, "authentification requise")
        h = security.token_hash(header[7:].strip())
        sess = s.get(m.Session, h)
        if sess is None or sess.expires_at <= security.now():
            raise ApiError(401, "session invalide")
        user = s.get(m.Developer, sess.developer_id)
        if user is None or not user.username:
            raise ApiError(401, "session invalide")
        sess.last_seen = security.now()  # battement de présence
        return user, h

    def online(s, user: m.Developer) -> bool:
        if user.presence == "offline":
            return False
        seen = s.scalar(select(func.max(m.Session.last_seen))
                        .where(m.Session.developer_id == user.id)) or 0
        return security.now() - seen < ONLINE_WINDOW

    # --- santé et comptes ---------------------------------------------------

    @r.get("/health")
    def health():
        db(lambda s: s.execute(select(1)))
        return {"ok": True}

    @r.post("/accounts/register", status_code=201)
    def register(request: Request, body: dict = Body(default_factory=dict)):
        auth_limiter.check(request)
        username, password = _s(body, "username").strip(), _s(body, "password")
        if not USERNAME_RE.match(username):
            raise ApiError(400, "pseudo invalide (3-24 : lettres, chiffres, _ et .)")
        if len(password) < 8 or len(password) > 128:
            raise ApiError(400, "mot de passe : 8 caracteres minimum")

        def go(s):
            if find(s, username):
                raise ApiError(409, "pseudo deja pris")
            u = m.Developer(username=username, username_key=username.lower(),
                            display_name=username, password_hash=security.hash_password(password),
                            created_at=security.now())
            s.add(u)
            s.flush()
            token = new_session(s, u)
            s.commit()
            return {"token": token, "user": public(u)}
        return db(go)

    @r.post("/accounts/login")
    def login(request: Request, body: dict = Body(default_factory=dict)):
        auth_limiter.check(request)
        username, password = _s(body, "username").strip(), _s(body, "password")

        def go(s):
            u = find(s, username)
            if u is None:
                security.burn_time()
                raise ApiError(401, "identifiants invalides")
            if u.locked_until > security.now():
                raise ApiError(429, "compte verrouille temporairement, reessaie plus tard")
            if not security.verify_password(u.password_hash, password):
                u.failed_logins += 1
                if u.failed_logins >= security.LOCK_AFTER:
                    u.locked_until = security.now() + security.LOCK_SECONDS
                    u.failed_logins = 0
                s.commit()
                raise ApiError(401, "identifiants invalides")
            u.failed_logins = 0
            token = new_session(s, u)
            s.commit()
            return {"token": token, "user": public(u)}
        return db(go)

    @r.get("/me")
    def me(request: Request):
        def go(s):
            u, _ = current(s, request)
            s.commit()
            return {"user": public(u)}
        return db(go)

    @r.post("/accounts/logout")
    def logout(request: Request):
        def go(s):
            _, h = current(s, request)
            s.execute(delete(m.Session).where(m.Session.token_hash == h))
            s.commit()
            return {"ok": True}
        return db(go)

    @r.post("/presence")
    def presence(request: Request, body: dict = Body(default_factory=dict)):
        status = _s(body, "status") or "online"
        if status not in ("online", "busy", "offline"):
            raise ApiError(400, "statut inconnu")

        def go(s):
            u, _ = current(s, request)
            u.presence = status
            s.commit()
            return {"ok": True}
        return db(go)

    # --- amis ---------------------------------------------------------------

    @r.get("/friends")
    def friends(request: Request):
        def go(s):
            u, _ = current(s, request)
            rows = s.scalars(select(m.Friendship).where(
                or_(m.Friendship.user_a == u.id, m.Friendship.user_b == u.id))).all()
            out = {"friends": [], "pendingIn": [], "pendingOut": [], "blocked": []}
            for f in rows:
                other = s.get(m.Developer, f.user_b if f.user_a == u.id else f.user_a)
                if other is None:
                    continue
                if f.status == "accepted":
                    out["friends"].append({"username": other.username,
                                           "online": online(s, other)})
                elif f.status == "blocked":
                    if f.requested_by == u.id:
                        out["blocked"].append(other.username)
                elif f.requested_by == u.id:
                    out["pendingOut"].append(other.username)
                else:
                    out["pendingIn"].append(other.username)
            s.commit()
            return out
        return db(go)

    @r.post("/friends/request", status_code=201)
    def friend_request(request: Request, body: dict = Body(default_factory=dict)):
        def go(s):
            u, _ = current(s, request)
            other = find(s, _s(body, "username").strip())
            if other is None:
                raise ApiError(404, "utilisateur introuvable")
            if other.id == u.id:
                raise ApiError(400, "impossible avec soi-meme")
            a, b = pair(u.id, other.id)
            if s.get(m.Friendship, (a, b)) is not None:
                raise ApiError(409, "demande deja en cours ou deja amis")
            s.add(m.Friendship(user_a=a, user_b=b, status="pending", requested_by=u.id,
                               created_at=security.now()))
            push_event(s, other.id, "friend.request", {"from": public(u)})
            s.commit()
            return {"ok": True}
        return db(go)

    def _friend_change(request: Request, body: dict, fn: Callable):
        def go(s):
            u, _ = current(s, request)
            other = find(s, _s(body, "username").strip())
            if other is None:
                raise ApiError(404, "utilisateur introuvable")
            out = fn(s, u, other, *pair(u.id, other.id))
            s.commit()
            return out
        return db(go)

    @r.post("/friends/accept")
    def friend_accept(request: Request, body: dict = Body(default_factory=dict)):
        def fn(s, u, other, a, b):
            f = s.get(m.Friendship, (a, b))
            if f is None or f.status != "pending" or f.requested_by != other.id:
                raise ApiError(404, "aucune demande a accepter")
            f.status = "accepted"
            push_event(s, other.id, "friend.accepted", {"by": public(u)})
            return {"ok": True}
        return _friend_change(request, body, fn)

    @r.post("/friends/decline")
    def friend_decline(request: Request, body: dict = Body(default_factory=dict)):
        def fn(s, u, other, a, b):
            s.execute(delete(m.Friendship).where(m.Friendship.user_a == a,
                                                 m.Friendship.user_b == b,
                                                 m.Friendship.status == "pending"))
            return {"ok": True}
        return _friend_change(request, body, fn)

    @r.delete("/friends/{username}")
    def friend_remove(username: str, request: Request):
        def fn(s, u, other, a, b):
            s.execute(delete(m.Friendship).where(m.Friendship.user_a == a,
                                                 m.Friendship.user_b == b))
            return {"ok": True}
        return _friend_change(request, {"username": username}, fn)

    @r.post("/friends/block")
    def friend_block(request: Request, body: dict = Body(default_factory=dict)):
        def fn(s, u, other, a, b):
            if other.id == u.id:
                raise ApiError(400, "impossible avec soi-meme")
            f = s.get(m.Friendship, (a, b))
            if f is None:
                s.add(m.Friendship(user_a=a, user_b=b, status="blocked", requested_by=u.id,
                                   created_at=security.now()))
            else:
                f.status, f.requested_by = "blocked", u.id
            return {"ok": True}
        return _friend_change(request, body, fn)

    @r.post("/friends/unblock")
    def friend_unblock(request: Request, body: dict = Body(default_factory=dict)):
        def fn(s, u, other, a, b):
            f = s.get(m.Friendship, (a, b))
            if f is None or f.status != "blocked":
                raise ApiError(404, "personne bloquee avec ce pseudo")
            f.status = "accepted"
            push_event(s, other.id, "friend.accepted", {"by": public(u)})
            return {"ok": True}
        return _friend_change(request, body, fn)

    # --- messages -----------------------------------------------------------

    @r.get("/messages/{username}")
    def messages(username: str, request: Request, limit: str = "50", before: str = "0"):
        def go(s):
            u, _ = current(s, request)
            other = find(s, username)
            if other is None:
                raise ApiError(404, "utilisateur introuvable")
            if not are_friends(s, u.id, other.id):
                raise ApiError(403, "conversation reservee aux amis")
            n = min(max(int(limit) if limit.isdigit() else 50, 1), 100)
            q = select(m.DirectMessage).where(or_(
                (m.DirectMessage.sender == u.id) & (m.DirectMessage.recipient == other.id),
                (m.DirectMessage.sender == other.id) & (m.DirectMessage.recipient == u.id)))
            if before.isdigit() and int(before) > 0:
                q = q.where(m.DirectMessage.id < int(before))
            rows = s.scalars(q.order_by(m.DirectMessage.id.desc()).limit(n)).all()
            s.commit()
            return {"messages": [{"id": x.id, "mine": x.sender == u.id,
                                  "sender": u.username if x.sender == u.id else other.username,
                                  "text": x.body, "at": iso(x.created_at)}
                                 for x in reversed(rows)]}
        return db(go)

    @r.post("/messages", status_code=201)
    def send_message(request: Request, body: dict = Body(default_factory=dict)):
        def go(s):
            u, _ = current(s, request)
            msg_limiter.check(request)
            text = _s(body, "text")
            if not 1 <= len(text) <= 2000:
                raise ApiError(400, "message : 1 a 2000 caracteres")
            other = find(s, _s(body, "to").strip())
            if other is None:
                raise ApiError(404, "utilisateur introuvable")
            if not are_friends(s, u.id, other.id):
                raise ApiError(403, "envoi reserve aux amis")
            dm = m.DirectMessage(sender=u.id, recipient=other.id, body=text,
                                 created_at=security.now())
            s.add(dm)
            s.flush()
            push_event(s, other.id, "message.new", {"message": {
                "id": dm.id, "sender": u.username, "text": text, "at": iso(dm.created_at)}})
            s.commit()
            return {"id": dm.id, "at": iso(dm.created_at)}
        return db(go)

    # --- appels (signalisation) -----------------------------------------------

    @r.get("/calls/active")
    def call_active(request: Request):
        def go(s):
            u, _ = current(s, request)
            c = s.scalar(select(m.Call).where(
                or_(m.Call.caller == u.id, m.Call.callee == u.id),
                m.Call.status.in_(("ringing", "active"))).order_by(m.Call.created_at.desc())
                .limit(1))
            s.commit()
            if c is None:
                return {"call": None}
            peer = s.get(m.Developer, c.callee if c.caller == u.id else c.caller)
            return {"call": {"id": c.id, "status": c.status, "lobbyId": c.lobby_id,
                             "peer": peer.username if peer else "", "peerId": peer.id if peer else 0,
                             "outgoing": c.caller == u.id}}
        return db(go)

    @r.post("/calls/invite", status_code=201)
    def call_invite(request: Request, body: dict = Body(default_factory=dict)):
        def go(s):
            u, _ = current(s, request)
            call_limiter.check(request)
            lobby = _s(body, "lobbyId")
            if not 4 <= len(lobby) <= 128:
                raise ApiError(400, "lobby invalide")
            other = find(s, _s(body, "to").strip())
            if other is None:
                raise ApiError(404, "utilisateur introuvable")
            if not are_friends(s, u.id, other.id):
                raise ApiError(403, "appel reserve aux amis")
            now = security.now()
            # Un seul appel vivant à la fois par utilisateur.
            s.execute(update(m.Call).where(
                or_(m.Call.caller == u.id, m.Call.callee == u.id),
                m.Call.status.in_(("ringing", "active"))).values(status="ended", updated_at=now))
            cid = secrets.token_hex(8)
            s.add(m.Call(id=cid, caller=u.id, callee=other.id, lobby_id=lobby, status="ringing",
                         created_at=now, updated_at=now))
            push_event(s, other.id, "call.invite", {"callId": cid, "from": public(u),
                                                     "lobbyId": lobby})
            s.commit()
            return {"callId": cid}
        return db(go)

    def _call_action(cid: str, request: Request, allowed: tuple, only_callee: bool,
                     nxt: str, event: str):
        def go(s):
            u, _ = current(s, request)
            c = s.get(m.Call, cid)
            if c is None:
                raise ApiError(404, "appel introuvable")
            if u.id not in (c.caller, c.callee):
                raise ApiError(403, "pas concerne")
            if c.status not in allowed or (only_callee and u.id != c.callee):
                raise ApiError(409, "transition impossible")
            c.status, c.updated_at = nxt, security.now()
            push_event(s, c.callee if u.id == c.caller else c.caller, event,
                       {"callId": c.id, "by": public(u), "lobbyId": c.lobby_id})
            s.commit()
            return {"ok": True}
        return db(go)

    @r.post("/calls/{cid}/accept")
    def call_accept(cid: str, request: Request):
        return _call_action(cid, request, ("ringing",), True, "active", "call.accept")

    @r.post("/calls/{cid}/decline")
    def call_decline(cid: str, request: Request):
        return _call_action(cid, request, ("ringing",), True, "ended", "call.decline")

    @r.post("/calls/{cid}/hangup")
    def call_hangup(cid: str, request: Request):
        return _call_action(cid, request, ("ringing", "active"), False, "ended", "call.hangup")

    # --- événements (long-polling) ----------------------------------------------

    @r.get("/events")
    async def events(request: Request, since: str = "0", wait: str = "25"):
        since_id = int(since) if since.isdigit() else 0
        # `wait` ne sert qu'aux tests ; le launcher garde les 25 s de Node.
        deadline = time.monotonic() + min(25.0, float(wait) if wait.isdigit() else 25.0)

        def poll(s):
            u, _ = current(s, request)
            rows = s.scalars(select(m.UserEvent).where(m.UserEvent.user_id == u.id,
                                                       m.UserEvent.id > since_id)
                             .order_by(m.UserEvent.id).limit(100)).all()
            out = [{"id": e.id, "type": e.type, "data": json.loads(e.payload),
                    "at": iso(e.created_at)} for e in rows]
            if out:  # distribution unique, comme Node
                s.execute(delete(m.UserEvent).where(m.UserEvent.user_id == u.id,
                                                    m.UserEvent.id <= out[-1]["id"]))
            s.commit()
            return out

        while True:
            out = await run_in_threadpool(db, poll)
            if out or time.monotonic() >= deadline or await request.is_disconnected():
                return {"events": out}
            await asyncio.sleep(1)

    # --- administration -----------------------------------------------------------

    @r.get("/admin/stats")
    def admin_stats(request: Request):
        expected = os.environ.get("TL_MARKET_SOCIAL_ADMIN_TOKEN", "")
        given = request.headers.get("x-admin-token", "")
        if not expected or not secrets.compare_digest(given, expected):
            raise ApiError(403, "interdit")

        def go(s):
            return {"users": s.scalar(select(func.count()).select_from(m.Developer)
                                      .where(m.Developer.username.is_not(None))),
                    "sessions": s.scalar(select(func.count()).select_from(m.Session)),
                    "messages": s.scalar(select(func.count()).select_from(m.DirectMessage)),
                    "liveCalls": s.scalar(select(func.count()).select_from(m.Call)
                                          .where(m.Call.status.in_(("ringing", "active"))))}
        return db(go)

    # Toute autre route sous /api : 404 au format de l'API.
    @r.api_route("/{rest:path}", methods=["GET", "POST", "PUT", "PATCH", "DELETE"])
    def not_found(rest: str):
        raise ApiError(404, "introuvable")

    app.include_router(r)
