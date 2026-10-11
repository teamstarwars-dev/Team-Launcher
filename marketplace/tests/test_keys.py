"""Cérémonie des clés : racine chiffrée, sauvegarde papier, émission."""

from __future__ import annotations

import json
import re

import pytest
from nacl.signing import VerifyKey

from app.signing import open_payload, unb64
from tools import keys

PASS = "cheval agrafe batterie correcte nuage"
FAST = {"t": 1, "m": 8 * 1024, "p": 1}


def test_encrypt_roundtrip_and_wrong_passphrase():
    seed = bytes(range(32))
    doc = keys.encrypt_seed(seed, PASS, FAST)
    assert seed.hex() not in doc
    assert keys.decrypt_seed(doc, PASS) == seed
    with pytest.raises(ValueError, match="incorrecte"):
        keys.decrypt_seed(doc, PASS + "x")
    d = json.loads(doc)
    d["box"] = d["box"][:-2] + ("00" if d["box"][-2:] != "00" else "11")
    with pytest.raises(ValueError):
        keys.decrypt_seed(json.dumps(d), PASS)


def test_paper_code_roundtrip_and_typo():
    seed = bytes(range(32))
    code = keys.paper_code(seed)
    assert keys.parse_paper(code) == seed
    assert keys.parse_paper(code.upper().replace(" ", "")) == seed  # casse et espaces libres
    typo = ("1" if code[0] != "1" else "2") + code[1:]
    with pytest.raises(ValueError, match="contrôle"):
        keys.parse_paper(typo)
    with pytest.raises(ValueError):
        keys.parse_paper("trop court / 00000000")


def test_full_ceremony(tmp_path, monkeypatch, capsys):
    answers = []
    monkeypatch.setattr(keys.getpass, "getpass", lambda prompt="": answers.pop(0))

    answers[:] = ["court", PASS, PASS]  # une phrase trop courte est refusée
    assert keys.main(["root-init", "--out", str(tmp_path / "root")]) == 0
    out = capsys.readouterr().out
    assert "Trop courte" in out
    code = re.search(r"((?:[0-9a-f]{8} ){7}[0-9a-f]{8}  /  [0-9a-f]{8})", out).group(1)
    pub_line = re.search(r"PublicKey\{[^}]+\}", out).group(0)
    enc = tmp_path / "root" / "root.key.enc"
    assert enc.exists() and not (tmp_path / "root" / "root.key").exists()
    assert code.split("  /  ")[0].replace(" ", "") not in enc.read_text()

    # Jamais d'écrasement d'une racine existante.
    assert keys.main(["root-init", "--out", str(tmp_path / "root")]) == 1

    answers[:] = ["mauvaise phrase de passe !"]
    assert keys.main(["issue", "--root", str(enc), "--key-id", "market-2026a",
                      "--out", str(tmp_path / "keys")]) == 1
    answers[:] = [PASS]
    assert keys.main(["issue", "--root", str(enc), "--key-id", "market-2026a",
                      "--out", str(tmp_path / "keys")]) == 0
    root_pub = VerifyKey(bytes.fromhex((tmp_path / "root" / "root.pub").read_text().strip()))
    cert = json.loads((tmp_path / "keys" / "market-2026a.cert.json").read_text())
    content = open_payload(cert, root_pub)
    assert content and content["keyId"] == "market-2026a"
    assert set(content["usages"]) == {"plugin", "mod", "revocation"}

    # Tout perdu sauf la feuille : on reconstitue la MÊME racine.
    answers[:] = [code, "une autre phrase de passe longue", "une autre phrase de passe longue"]
    assert keys.main(["restore-root", "--out", str(tmp_path / "restored")]) == 0
    assert re.search(r"PublicKey\{[^}]+\}", capsys.readouterr().out).group(0) == pub_line
    assert (tmp_path / "restored" / "root.pub").read_text() == \
        (tmp_path / "root" / "root.pub").read_text()
    # Code mal recopié : refus.
    answers[:] = ["0" * 64 + " / 00000000"]
    assert keys.main(["restore-root", "--out", str(tmp_path / "r2")]) == 1
