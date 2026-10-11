"""Signature Ed25519 — miroir exact du vérificateur du launcher.

Les documents produits ici sont vérifiés par cpp/src/signing.cpp. Le format
doit donc rester IDENTIQUE des deux côtés ; tests/test_signing_interop
fait vérifier par le launcher lui-même ce que ce module signe.

Format d'un document signé : {"payload": base64, "sig": base64}. La
signature porte sur les octets du payload ; rien n'est re-sérialisé.
"""

from __future__ import annotations

import base64
import json
import os
from dataclasses import dataclass
from pathlib import Path

from nacl.signing import SigningKey, VerifyKey
from nacl.exceptions import BadSignatureError

KNOWN_USAGES = ("plugin", "mod", "revocation")


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def unb64(s: str) -> bytes:
    return base64.b64decode(s, validate=True)


def sign_payload(payload: dict, key: SigningKey) -> dict:
    # separators compacts et ensure_ascii=False : peu importe la forme
    # exacte, puisque c'est CET octet-là qui est signé et transmis tel quel.
    raw = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
    return {"payload": b64(raw), "sig": b64(key.sign(raw).signature)}


def open_payload(doc: dict, pub: VerifyKey) -> dict | None:
    try:
        raw = unb64(doc["payload"])
        pub.verify(raw, unb64(doc["sig"]))
        return json.loads(raw)
    except (KeyError, ValueError, TypeError, BadSignatureError):
        return None


# --- Clés ---------------------------------------------------------------------


def load_seed(path: Path) -> SigningKey:
    seed = bytes.fromhex(path.read_text().strip())
    if len(seed) != 32:
        raise ValueError(f"{path} : graine Ed25519 de 32 octets attendue")
    return SigningKey(seed)


def write_secret_file(path: Path, text: str) -> None:
    """Écrit un fichier lisible par son seul propriétaire (0600), sans
    fenêtre pendant laquelle il serait lisible par d'autres."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)


def make_cert(root: SigningKey, inter_pub: VerifyKey, key_id: str, usages: list[str],
              not_before: int, not_after: int) -> dict:
    if not_after <= not_before:
        raise ValueError("période de validité incohérente")
    bad = [u for u in usages if u not in KNOWN_USAGES]
    if bad:
        raise ValueError(f"usages inconnus : {bad}")
    return sign_payload(
        {
            "type": "tl-key-cert",
            "v": 1,
            "keyId": key_id,
            "publicKey": b64(bytes(inter_pub)),
            "usages": usages,
            "notBefore": not_before,
            "notAfter": not_after,
        },
        root,
    )


@dataclass
class Signer:
    """La clé intermédiaire en service sur ce serveur, et son certificat."""

    key: SigningKey
    cert: dict
    key_id: str
    usages: set[str]

    @classmethod
    def load(cls, key_path: Path, cert_path: Path) -> "Signer":
        key = load_seed(key_path)
        cert = json.loads(cert_path.read_text())
        # On relit le contenu du certificat (sans vérifier la racine, qui
        # n'est pas ici) pour s'assurer qu'il correspond bien à la clé.
        content = json.loads(unb64(cert["payload"]))
        if unb64(content["publicKey"]) != bytes(key.verify_key):
            raise ValueError("le certificat ne correspond pas à la clé de signature")
        return cls(key, cert, content["keyId"], set(content.get("usages", [])))

    def sign_release(self, plugin_id: str, version: str, files: dict[str, str],
                     signed_at: int, kind: str = "plugin") -> str:
        """signature.json pour une publication approuvée.

        kind « plugin » : vérifié par le launcher (type tl-plugin-release).
        kind « mod » : même structure, type tl-mod-release et usage « mod »,
        pour que la signature d'un mod ne puisse JAMAIS passer pour celle
        d'un plugin — un mod ne tourne pas sous les mêmes garde-fous.
        """
        if kind not in ("plugin", "mod"):
            raise ValueError("type de publication inconnu")
        if kind not in self.usages:
            raise PermissionError(f"cette clé n'est pas habilitée à signer : {kind}")
        release = {
            "type": "tl-plugin-release" if kind == "plugin" else "tl-mod-release",
            "v": 1,
            "pluginId": plugin_id,
            "version": version,
            "keyId": self.key_id,
            "signedAt": signed_at,
            "files": files,
        }
        return json.dumps({"v": 1, "cert": self.cert,
                           "release": sign_payload(release, self.key)})

    def sign_revocations(self, content: dict) -> str:
        if "revocation" not in self.usages:
            raise PermissionError("cette clé n'est pas habilitée à signer la révocation")
        if self.key_id in content.get("keyIds", []):
            # Le launcher refuserait cette liste : on ne la publie pas.
            raise ValueError("une liste ne peut pas révoquer la clé qui la signe")
        payload = {"type": "tl-revocations", "v": 1, **content}
        return json.dumps({"v": 1, "cert": self.cert, "list": sign_payload(payload, self.key)})
