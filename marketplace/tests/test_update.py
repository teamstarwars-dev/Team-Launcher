"""Signature d'un paquet de mise à jour du launcher (H1)."""

from __future__ import annotations

import json

import pytest
from nacl.signing import SigningKey, VerifyKey

from app.signing import Signer, make_cert, open_payload, unb64, write_secret_file


def _signer(tmp, usages):
    root = SigningKey(b"\x01" * 32)
    inter = SigningKey(b"\x02" * 32)
    cert = make_cert(root, inter.verify_key, "up-1", usages, 1000, 4_000_000_000)
    write_secret_file(tmp / "k", bytes(inter).hex())
    (tmp / "c").write_text(json.dumps(cert))
    return Signer.load(tmp / "k", tmp / "c"), root


def test_sign_update_roundtrip(tmp_path):
    signer, root = _signer(tmp_path, ["launcher"])
    sha = "ab" * 64
    doc = json.loads(signer.sign_update(sha, "1.2.3", 2_000_000_000))
    assert doc["v"] == 1
    cert = open_payload(doc["cert"], root.verify_key)
    assert cert and "launcher" in cert["usages"]
    rel = open_payload(doc["release"], VerifyKey(unb64(cert["publicKey"])))
    assert rel and rel["type"] == "tl-launcher-release"
    assert rel["version"] == "1.2.3" and rel["sha512"] == sha and rel["keyId"] == "up-1"


def test_sign_update_requires_launcher_usage(tmp_path):
    signer, _ = _signer(tmp_path, ["plugin", "mod", "revocation"])
    with pytest.raises(PermissionError):
        signer.sign_update("00" * 64, "1.0", 0)
