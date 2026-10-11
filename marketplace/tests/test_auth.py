from __future__ import annotations

from conftest import PASSWORD, Account, plugin_zip, submit

from app import security


def test_password_too_short(env):
    r = env["client"].post("/v1/auth/register",
                           json={"email": "a@ex.org", "password": "court", "display_name": "aa"})
    assert r.status_code == 400


def test_duplicate_email(env):
    Account(env["client"], "a@ex.org", totp=False)
    r = env["client"].post("/v1/auth/register",
                           json={"email": "A@EX.org", "password": PASSWORD, "display_name": "bb"})
    assert r.status_code == 409


def test_wrong_password_and_unknown_account_same_answer(env):
    Account(env["client"], "a@ex.org", totp=False)
    c = env["client"]
    r1 = c.post("/v1/auth/login", json={"email": "a@ex.org", "password": "mauvais mot de passe"})
    r2 = c.post("/v1/auth/login", json={"email": "z@ex.org", "password": "mauvais mot de passe"})
    assert r1.status_code == r2.status_code == 401
    assert r1.json() == r2.json()


def test_lockout(env):
    Account(env["client"], "a@ex.org", totp=False)
    c = env["client"]
    for _ in range(security.LOCK_AFTER):
        c.post("/v1/auth/login", json={"email": "a@ex.org", "password": "mauvais mot de passe"})
    # Même le BON mot de passe est refusé pendant le verrouillage.
    r = c.post("/v1/auth/login", json={"email": "a@ex.org", "password": PASSWORD})
    assert r.status_code == 429


def test_no_submission_without_2fa(env):
    a = Account(env["client"], "a@ex.org", totp=False)
    r = a.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    assert r.status_code == 403
    assert "obligatoire" in r.json()["detail"]


def test_password_alone_is_half_a_session(env):
    a = Account(env["client"], "a@ex.org")
    a.login()  # nouvelle session : mot de passe seul
    assert a.get("/v1/me").json()["mfaOk"] is False
    r = a.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    assert r.status_code == 403
    assert a.post("/v1/auth/totp/verify", json={"code": a.code(1)}).status_code == 200
    r = a.post("/v1/projects", json={"kind": "plugin", "slug": "hello", "name": "Hello"})
    assert r.status_code == 201


def test_totp_code_is_single_use(env):
    a = Account(env["client"], "a@ex.org")
    code = a.code(1)
    a.login()
    assert a.post("/v1/auth/totp/verify", json={"code": code}).status_code == 200
    a.login()
    # Même code, nouvelle session : un code intercepté ne se rejoue pas.
    assert a.post("/v1/auth/totp/verify", json={"code": code}).status_code == 401


def test_totp_cannot_be_replaced_by_session_holder(env):
    a = Account(env["client"], "a@ex.org")
    assert a.post("/v1/auth/totp/setup").status_code == 409


def test_totp_secret_encrypted_at_rest(env):
    from app import models as m
    a = Account(env["client"], "a@ex.org")
    with env["db"]() as s:
        enc = s.get(m.Developer, a.id).totp_secret_enc
    assert a.secret not in enc and len(enc) > 40


def test_session_token_hashed_and_logout(env):
    from app import models as m
    a = Account(env["client"], "a@ex.org")
    with env["db"]() as s:
        assert s.get(m.Session, a.token) is None
        assert s.get(m.Session, security.token_hash(a.token)) is not None
    assert a.post("/v1/auth/logout").status_code == 200
    assert a.get("/v1/me").status_code == 401


def test_bad_bearer(env):
    r = env["client"].get("/v1/me", headers={"Authorization": "Bearer nimportequoi"})
    assert r.status_code == 401
    assert env["client"].get("/v1/me").status_code == 401
