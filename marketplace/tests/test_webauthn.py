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


def _add_key(acc, key, name="PC Windows Hello", kind="platform"):
    opts = acc.post("/v1/auth/webauthn/register/options", json={"kind": kind}).json()
    assert opts["rp"]["id"] == RP
    return acc.post("/v1/auth/webauthn/register/verify",
                    json={"kind": kind, "name": name, "credential": key.register(opts)})


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
    assert acc.post("/v1/auth/webauthn/register/options", json={"kind": "platform"}).status_code == 403


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
    opts = acc.post("/v1/auth/webauthn/register/options", json={"kind": "platform"}).json()
    bad = key.register(opts, origin="https://evil.test")
    assert acc.post("/v1/auth/webauthn/register/verify",
                    json={"kind": "platform", "name": "x", "credential": bad}).status_code == 400
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
    assert acc.post("/v1/auth/webauthn/register/options", json={"kind": "platform"}).status_code == 400


def test_kinds_select_authenticator_and_require_pin(c):
    acc = Account(c, "dev@ex.org")
    hello = acc.post("/v1/auth/webauthn/register/options", json={"kind": "platform"}).json()
    sel = hello["authenticatorSelection"]
    assert sel["authenticatorAttachment"] == "platform" and sel["userVerification"] == "required"
    assert hello["hints"] == ["client-device"]
    fido = acc.post("/v1/auth/webauthn/register/options", json={"kind": "security-key"}).json()
    assert fido["authenticatorSelection"]["authenticatorAttachment"] == "cross-platform"
    assert fido["hints"] == ["security-key"]
    assert acc.post("/v1/auth/webauthn/register/options", json={"kind": "autre"}).status_code == 422

    # Clé touchée SANS son code PIN : refusée à l'ajout…
    key = SoftAuthenticator(ORIGIN, RP)
    r = acc.post("/v1/auth/webauthn/register/verify",
                 json={"kind": "security-key", "credential": key.register(fido, uv=False)})
    assert r.status_code == 400
    # … et acceptée avec le code ; nom par défaut selon le type.
    fido = acc.post("/v1/auth/webauthn/register/options", json={"kind": "security-key"}).json()
    assert acc.post("/v1/auth/webauthn/register/verify",
                    json={"kind": "security-key", "credential": key.register(fido)}).status_code == 201
    assert acc.get("/v1/auth/webauthn/credentials").json()[0]["name"] == "Clé FIDO2"
    # Défi d'un type présenté pour l'autre : refusé.
    opts = acc.post("/v1/auth/webauthn/register/options", json={"kind": "platform"}).json()
    other = SoftAuthenticator(ORIGIN, RP)
    assert acc.post("/v1/auth/webauthn/register/verify",
                    json={"kind": "security-key", "credential": other.register(opts)}).status_code == 400

    # Connexion sans code PIN : refusée.
    acc.login()
    opts = acc.post("/v1/auth/webauthn/login/options").json()
    assert opts["userVerification"] == "required"
    assert acc.post("/v1/auth/webauthn/login/verify",
                    json={"credential": key.assert_(opts, uv=False)}).status_code == 401
