"""Le parcours complet : soumission, review, signature, catalogue,
téléchargement, révocation, kill-switch."""

from __future__ import annotations

import hashlib
import io
import json
import zipfile

from conftest import Account, make_zip, plugin_zip, set_role, submit
from nacl.signing import VerifyKey

from app.signing import open_payload, unb64


def _setup(env):
    c = env["client"]
    dev = Account(c, "dev@ex.org")
    rv = Account(c, "rv@ex.org")
    set_role(env, rv, reviewer=True)
    assert dev.post("/v1/projects",
                    json={"kind": "plugin", "slug": "hello", "name": "Hello"}).status_code == 201
    return dev, rv


def _verify_release(env, zip_bytes: bytes, slug: str, version: str):
    """Ce que fera le launcher, en Python : cert par la racine, publication
    par l'intermédiaire, liste EXACTE des fichiers et leurs empreintes."""
    z = zipfile.ZipFile(io.BytesIO(zip_bytes))
    sig = json.loads(z.read("signature.json"))
    cert = open_payload(sig["cert"], env["root"].verify_key)
    assert cert and cert["type"] == "tl-key-cert"
    rel = open_payload(sig["release"], VerifyKey(unb64(cert["publicKey"])))
    assert rel and rel["type"] == "tl-plugin-release"
    assert rel["pluginId"] == slug and rel["version"] == version
    actual = {n: hashlib.sha512(z.read(n)).hexdigest()
              for n in z.namelist() if n != "signature.json" and not n.endswith("/")}
    assert rel["files"] == actual


def test_full_review_flow(env):
    dev, rv = _setup(env)
    r = submit(dev, "hello", "1.0.0", plugin_zip())
    assert r.status_code == 201, r.text
    sid = r.json()["id"]

    # Rien de public tant que ce n'est pas relu.
    assert env["client"].get("/v1/public/catalog").json() == []
    assert env["client"].get("/v1/public/download/plugin/hello/1.0.0").status_code == 404

    q = rv.get("/v1/review/queue").json()
    assert [x["id"] for x in q] == [sid]
    d = rv.get(f"/v1/review/submissions/{sid}").json()
    assert set(d["files"]) == {"plugin.json", "hello.exe"}
    assert d["changesSincePrevious"] is None

    assert rv.post(f"/v1/review/submissions/{sid}/approve", json={}).status_code == 200
    assert dev.get(f"/v1/submissions/{sid}").json()["status"] == "approved"
    # Une décision est définitive.
    assert rv.post(f"/v1/review/submissions/{sid}/reject",
                   json={"note": "trop tard"}).status_code == 409

    cat = env["client"].get("/v1/public/catalog").json()
    assert [(x["slug"], x["version"]) for x in cat] == [("hello", "1.0.0")]
    assert cat[0]["permissions"] == ["instances:read"]

    dl = env["client"].get("/v1/public/download/plugin/hello/1.0.0")
    assert dl.status_code == 200
    _verify_release(env, dl.content, "hello", "1.0.0")


def test_reviewer_cannot_approve_own_submission(env):
    c = env["client"]
    rv = Account(c, "rv@ex.org")
    set_role(env, rv, reviewer=True)
    rv.post("/v1/projects", json={"kind": "plugin", "slug": "mine", "name": "Mine"})
    sid = submit(rv, "mine", "1.0.0", plugin_zip("mine")).json()["id"]
    assert rv.get("/v1/review/queue").json()[0]["own"] is True
    r = rv.post(f"/v1/review/submissions/{sid}/approve", json={})
    assert r.status_code == 403
    assert "propre" in r.json()["detail"]


def test_non_reviewer_has_no_access(env):
    dev, _ = _setup(env)
    sid = submit(dev, "hello", "1.0.0", plugin_zip()).json()["id"]
    assert dev.get("/v1/review/queue").status_code == 403
    assert dev.post(f"/v1/review/submissions/{sid}/approve", json={}).status_code == 403


def test_reviewer_needs_2fa_in_session(env):
    dev, rv = _setup(env)
    submit(dev, "hello", "1.0.0", plugin_zip())
    rv.login()
    assert rv.get("/v1/review/queue").status_code == 403


def test_reject_requires_a_reason(env):
    dev, rv = _setup(env)
    sid = submit(dev, "hello", "1.0.0", plugin_zip()).json()["id"]
    assert rv.post(f"/v1/review/submissions/{sid}/reject", json={"note": " "}).status_code == 400
    assert rv.post(f"/v1/review/submissions/{sid}/reject",
                   json={"note": "accès réseau non déclaré"}).status_code == 200
    st = dev.get(f"/v1/submissions/{sid}").json()
    assert st["status"] == "rejected" and "réseau" in st["note"]


def test_only_owner_submits(env):
    dev, _ = _setup(env)
    other = Account(env["client"], "other@ex.org")
    assert submit(other, "hello", "1.0.0", plugin_zip()).status_code == 404


def test_manifest_version_must_match(env):
    dev, _ = _setup(env)
    r = submit(dev, "hello", "2.0.0", plugin_zip(version="1.0.0"))
    assert r.status_code == 400


def test_duplicate_version_refused(env):
    dev, _ = _setup(env)
    assert submit(dev, "hello", "1.0.0", plugin_zip()).status_code == 201
    assert submit(dev, "hello", "1.0.0", plugin_zip()).status_code == 409


def test_update_shows_changes_and_source_diff(env):
    dev, rv = _setup(env)
    s1 = submit(dev, "hello", "1.0.0", plugin_zip(),
                make_zip({"src/main.cpp": "int main(){}\n"})).json()["id"]
    rv.post(f"/v1/review/submissions/{s1}/approve", json={})
    s2 = submit(dev, "hello", "1.1.0",
                plugin_zip(version="1.1.0", extra={"hello.exe": b"MZ v2", "data.txt": "x"}),
                make_zip({"src/main.cpp": "int main(){ return 1; }\n"})).json()["id"]
    d = rv.get(f"/v1/review/submissions/{s2}").json()["changesSincePrevious"]
    assert d["previousVersion"] == "1.0.0"
    assert d["added"] == ["data.txt"]
    assert set(d["changed"]) == {"hello.exe", "plugin.json"}
    diff = rv.get(f"/v1/review/submissions/{s2}/source-diff").text
    assert "-int main(){}" in diff and "+int main(){ return 1; }" in diff


def test_catalog_shows_latest_only(env):
    dev, rv = _setup(env)
    for v in ("1.0.0", "1.1.0"):
        sid = submit(dev, "hello", v, plugin_zip(version=v)).json()["id"]
        rv.post(f"/v1/review/submissions/{sid}/approve", json={})
    cat = env["client"].get("/v1/public/catalog").json()
    assert [(x["slug"], x["version"]) for x in cat] == [("hello", "1.1.0")]


def test_mod_release_is_not_a_plugin_release(env):
    c = env["client"]
    dev, rv = Account(c, "dev@ex.org"), Account(c, "rv@ex.org")
    set_role(env, rv, reviewer=True)
    dev.post("/v1/projects", json={"kind": "mod", "slug": "coolmod", "name": "Cool"})
    jar = make_zip({"coolmod-1.0.jar": b"PK fake jar"})
    sid = submit(dev, "coolmod", "1.0", jar, kind="mod").json()["id"]
    rv.post(f"/v1/review/submissions/{sid}/approve", json={})
    z = zipfile.ZipFile(io.BytesIO(c.get("/v1/public/download/mod/coolmod/1.0").content))
    sig = json.loads(z.read("signature.json"))
    cert = open_payload(sig["cert"], env["root"].verify_key)
    rel = open_payload(sig["release"], VerifyKey(unb64(cert["publicKey"])))
    assert rel["type"] == "tl-mod-release"


# --- Révocation et kill-switch ---------------------------------------------------


def _admin(env):
    a = Account(env["client"], "admin@ex.org")
    set_role(env, a, admin=True)
    return a


def _current_list(env):
    doc = json.loads(env["client"].get("/v1/public/revocations").text)
    cert = open_payload(doc["cert"], env["root"].verify_key)
    assert "revocation" in cert["usages"]
    lst = open_payload(doc["list"], VerifyKey(unb64(cert["publicKey"])))
    assert lst["type"] == "tl-revocations"
    return lst


def test_revocation_sequence(env):
    dev, rv = _setup(env)
    sid = submit(dev, "hello", "1.0.0", plugin_zip()).json()["id"]
    rv.post(f"/v1/review/submissions/{sid}/approve", json={})
    ad = _admin(env)
    assert env["client"].get("/v1/public/revocations").status_code == 404

    r = ad.post("/v1/admin/revoke", json={"releases": [{"id": "hello", "version": "1.0.0"}],
                                          "note": "porte dérobée"})
    assert r.status_code == 200 and r.json()["seq"] == 1
    lst = _current_list(env)
    assert lst["seq"] == 1 and lst["releases"] == [{"id": "hello", "version": "1.0.0"}]
    # Une version révoquée disparaît du catalogue et n'est plus servie.
    assert env["client"].get("/v1/public/catalog").json() == []
    assert env["client"].get("/v1/public/download/plugin/hello/1.0.0").status_code == 404

    # Les listes s'accumulent : la n+1 contient tout ce que contenait la n.
    ad.post("/v1/admin/revoke", json={"fileHashes": ["ab" * 64]})
    lst = _current_list(env)
    assert lst["seq"] == 2 and lst["releases"] and lst["fileHashes"] == ["ab" * 64]


def test_killswitch(env):
    ad = _admin(env)
    assert ad.post("/v1/admin/killswitch", json={"on": True}).json()["seq"] == 1
    assert _current_list(env)["killAll"] is True
    assert ad.post("/v1/admin/killswitch", json={"on": False}).json()["seq"] == 2
    assert _current_list(env)["killAll"] is False


def test_cannot_revoke_own_signing_key(env):
    ad = _admin(env)
    r = ad.post("/v1/admin/revoke", json={"keyIds": ["test-inter"]})
    assert r.status_code == 400


def test_reviewer_is_not_admin(env):
    _, rv = _setup(env)
    assert rv.post("/v1/admin/killswitch", json={"on": True}).status_code == 403


def test_review_page_and_security_headers(env):
    c = env["client"]
    r = c.get("/review")
    assert r.status_code == 200 and "Review des plugins" in r.text
    csp = r.headers["content-security-policy"]
    assert "script-src 'self'" in csp and "frame-ancestors 'none'" in csp
    assert "unsafe-inline" not in csp
    assert r.headers["x-frame-options"] == "DENY"
    assert c.get("/static/review.js").status_code == 200
    # Les en-têtes s'appliquent aussi à l'API.
    assert c.get("/v1/public/catalog").headers["x-content-type-options"] == "nosniff"
    # Pas de documentation publique de l'API.
    assert c.get("/docs").status_code == 404
