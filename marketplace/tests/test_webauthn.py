"""Windows Hello / clés FIDO2 (WebAuthn) comme second facteur."""

from __future__ import annotations

import dataclasses

import pytest
from conftest import Account
from fastapi.testclient import TestClient
from softauthn import SoftAuthenticator

from app.api import create_app
from app.signing import Signer

ORIGIN, RP = "https://panel.example.org", "panel.example.org"


@pytest.fixture()
def c(env):
    settings = dataclasses.replace(env["settings"], public_url=ORIGIN)
    return TestClient(create_app(settings, env["db"],
                                 Signer.load(env["key_path"], env["cert_path"])))


def _add_key(acc, key, name="PC Windows Hello"):
    opts = acc.post("/v1/auth/webauthn/register/options").json()
    assert opts["rp"]["id"] == RP
    return acc.post("/v1/auth/webauthn/register/verify",
                    json={"name": name, "credential": key.register(opts)})


def test_add_key_then_login_without_code(c):
    acc = Account(c, "dev@ex.org")
    key = SoftAuthenticator(ORIGIN, RP)
    assert _add_key(acc, key).status_code == 201
    assert acc.get("/v1/me").json()["webauthn"] == 1
    assert [k["name"] for k in acc.get("/v1/auth/webauthn/credentials").json()] == ["PC Windows Hello"]

    acc.login()  # nouvelle session : mot de passe seul
    assert acc.get("/v1/me").json()["mfaOk"] is False
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    assert acc.post("/v1/auth/webauthn/login/verify",
                    json={"credential": key.assert_(opts)}).status_code == 200
    assert acc.get("/v1/me").json()["mfaOk"] is True
    assert acc.post("/v1/projects", json={"kind": "plugin", "slug": "x", "name": "Xy"}).status_code == 201


def test_adding_a_key_requires_2fa_session(c):
    acc = Account(c, "dev@ex.org")
    acc.login()  # mot de passe seul : on ne pose pas de clé avec ça
    assert acc.post("/v1/auth/webauthn/register/options").status_code == 403


def test_refusals(c):
    acc = Account(c, "dev@ex.org")
    key = SoftAuthenticator(ORIGIN, RP)
    _add_key(acc, key)
    acc.login()

    # Mauvaise origine (site d'hameçonnage) : refusé.
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    r = acc.post("/v1/auth/webauthn/login/verify",
                 json={"credential": key.assert_(opts, origin="https://panel.example.org.evil.test")})
    assert r.status_code == 401

    # Défi à usage unique : rejouer une réponse valide échoue.
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    good = key.assert_(opts)
    assert acc.post("/v1/auth/webauthn/login/verify", json={"credential": good}).status_code == 200
    acc.login()
    assert acc.post("/v1/auth/webauthn/login/verify", json={"credential": good}).status_code == 400

    # Clé clonée : compteur rejoué.
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    assert acc.post("/v1/auth/webauthn/login/verify",
                    json={"credential": key.assert_(opts, replay_count=1)}).status_code == 401

    # Clé d'un autre compte : inconnue ici.
    other = Account(c, "other@ex.org")
    other.login()
    other_key = SoftAuthenticator(ORIGIN, RP)
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    assert acc.post("/v1/auth/webauthn/login/verify",
                    json={"credential": other_key.assert_(opts)}).status_code == 401
    # Et sans clé enregistrée, pas de défi.
    assert other.post("/v1/auth/webauthn/login/options").status_code == 404


def test_registration_refusals_and_removal(c):
    acc = Account(c, "dev@ex.org")
    key = SoftAuthenticator(ORIGIN, RP)
    opts = acc.post("/v1/auth/webauthn/register/options").json()
    bad = key.register(opts, origin="https://evil.test")
    assert acc.post("/v1/auth/webauthn/register/verify",
                    json={"name": "x", "credential": bad}).status_code == 400
    assert _add_key(acc, key).status_code == 201
    # Même clé deux fois : refus (exclue des options, et vérifiée au serveur).
    assert _add_key(acc, key).status_code == 409
    kid = acc.get("/v1/auth/webauthn/credentials").json()[0]["id"]
    other = Account(c, "other@ex.org")
    assert other.post(f"/v1/auth/webauthn/credentials/{kid}/delete").status_code == 404
    assert acc.post(f"/v1/auth/webauthn/credentials/{kid}/delete").status_code == 200
    assert acc.get("/v1/me").json()["webauthn"] == 0


def test_disabled_without_public_url(env):
    acc = Account(env["client"], "dev@ex.org")
    assert acc.post("/v1/auth/webauthn/register/options").status_code == 400
