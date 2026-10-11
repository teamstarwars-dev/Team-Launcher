"""Banc de test : une racine et une intermédiaire jetables, une base en
mémoire (SQLite par défaut, MariaDB si TL_MARKET_TEST_DB_URL est posé),
et des aides pour créer des comptes avec 2FA."""

from __future__ import annotations

import io
import json
import os
import time
import zipfile
from pathlib import Path

import pyotp
import pytest
from fastapi.testclient import TestClient
from nacl.signing import SigningKey
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from app import models as m
from app.api import create_app
from app.config import Settings
from app.db import Base, make_engine, make_session_factory
from app.signing import Signer, make_cert, write_secret_file

PASSWORD = "un mot de passe assez long"


@pytest.fixture()
def keys(tmp_path):
    root = SigningKey.generate()
    inter = SigningKey.generate()
    now = int(time.time())
    cert = make_cert(root, inter.verify_key, "test-inter", ["plugin", "mod", "revocation"],
                     now - 60, now + 86400)
    kp, cp = tmp_path / "inter.key", tmp_path / "inter.cert.json"
    write_secret_file(kp, bytes(inter).hex())
    cp.write_text(json.dumps(cert))
    return {"root": root, "inter": inter, "key_path": kp, "cert_path": cp}


@pytest.fixture()
def env(tmp_path, keys):
    url = os.environ.get("TL_MARKET_TEST_DB_URL")
    if url:
        engine = make_engine(url)
        Base.metadata.drop_all(engine)
    else:
        engine = create_engine("sqlite://", connect_args={"check_same_thread": False},
                               poolclass=StaticPool)
    Base.metadata.create_all(engine)
    settings = Settings(database_url=url or "sqlite://", storage_dir=tmp_path / "storage",
                        signing_key_path=keys["key_path"], signing_cert_path=keys["cert_path"],
                        totp_enc_key=os.urandom(32), max_upload_bytes=4 * 1024 * 1024,
                        max_unpacked_bytes=16 * 1024 * 1024, max_files=200)
    factory = make_session_factory(engine)
    app = create_app(settings, factory, Signer.load(keys["key_path"], keys["cert_path"]))
    yield {"client": TestClient(app), "db": factory, "settings": settings, **keys}
    if url:
        Base.metadata.drop_all(engine)


class Account:
    """Un compte connecté, 2FA posée et présentée."""

    def __init__(self, client: TestClient, email: str, totp: bool = True):
        self.c, self.email = client, email
        r = client.post("/v1/auth/register",
                        json={"email": email, "password": PASSWORD, "display_name": email[:10]})
        assert r.status_code == 201, r.text
        self.id = r.json()["id"]
        self.login()
        self.secret = None
        if totp:
            r = self.post("/v1/auth/totp/setup")
            assert r.status_code == 200, r.text
            self.secret = r.json()["secret"]
            r = self.post("/v1/auth/totp/confirm", json={"code": self.code()})
            assert r.status_code == 200, r.text

    def login(self):
        r = self.c.post("/v1/auth/login", json={"email": self.email, "password": PASSWORD})
        assert r.status_code == 200, r.text
        self.token = r.json()["token"]

    def code(self, offset_steps: int = 0) -> str:
        return pyotp.TOTP(self.secret).at(time.time() + 30 * offset_steps)

    @property
    def h(self):
        return {"Authorization": f"Bearer {self.token}"}

    def get(self, url, **kw):
        return self.c.get(url, headers=self.h, **kw)

    def post(self, url, **kw):
        return self.c.post(url, headers=self.h, **kw)


def set_role(env, account: Account, reviewer=False, admin=False):
    with env["db"]() as s:
        d = s.get(m.Developer, account.id)
        d.is_reviewer, d.is_admin = reviewer, admin
        s.commit()


def make_zip(files: dict[str, bytes | str]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


def plugin_zip(slug="hello", version="1.0.0", extra: dict | None = None) -> bytes:
    manifest = {"name": "Hello", "version": version, "description": "Plugin de test",
                "api": 1, "permissions": ["instances:read"],
                "service": {"run": "hello.exe"}}
    files = {"plugin.json": json.dumps(manifest), "hello.exe": b"MZ fake binary " + slug.encode()}
    files.update(extra or {})
    return make_zip(files)


def submit(acc: Account, slug: str, version: str, binary: bytes, source: bytes | None = None,
           kind: str = "plugin"):
    source = source if source is not None else make_zip({"src/main.cpp": "int main(){}\n"})
    return acc.post(f"/v1/projects/{kind}/{slug}/submissions", data={"version": version},
                    files={"binary": ("b.zip", binary), "source": ("s.zip", source)})
