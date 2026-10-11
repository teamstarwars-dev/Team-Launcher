"""Authentificateur WebAuthn LOGICIEL, pour les tests.

Fabrique de vraies réponses WebAuthn (attestation « none », signatures
ECDSA P-256) comme le ferait Windows Hello ou une clé FIDO2 : le serveur
les vérifie avec la même bibliothèque qu'en production.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import struct

import cbor2
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec


def b64u(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def unb64u(s: str) -> bytes:
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


class SoftAuthenticator:
    def __init__(self, origin: str, rp_id: str):
        self.origin, self.rp_id = origin, rp_id
        self.key = ec.generate_private_key(ec.SECP256R1())
        self.cred_id = os.urandom(32)
        self.count = 0

    def _cose(self) -> bytes:
        n = self.key.public_key().public_numbers()
        return cbor2.dumps({1: 2, 3: -7, -1: 1, -2: n.x.to_bytes(32, "big"),
                            -3: n.y.to_bytes(32, "big")})

    def _client_data(self, typ: str, challenge_b64u: str, origin: str | None = None) -> bytes:
        return json.dumps({"type": typ, "challenge": challenge_b64u,
                           "origin": origin or self.origin, "crossOrigin": False}).encode()

    def register(self, options: dict, origin: str | None = None, uv: bool = True) -> dict:
        cd = self._client_data("webauthn.create", options["challenge"], origin)
        auth_data = (hashlib.sha256(self.rp_id.encode()).digest()
                     + bytes([0x45 if uv else 0x41])  # UP|AT (+UV : code PIN saisi)
                     + struct.pack(">I", self.count) + b"\0" * 16
                     + struct.pack(">H", len(self.cred_id)) + self.cred_id + self._cose())
        att = cbor2.dumps({"fmt": "none", "attStmt": {}, "authData": auth_data})
        return {"id": b64u(self.cred_id), "rawId": b64u(self.cred_id), "type": "public-key",
                "response": {"clientDataJSON": b64u(cd), "attestationObject": b64u(att)},
                "clientExtensionResults": {}}

    def assert_(self, options: dict, origin: str | None = None, replay_count: int | None = None,
                uv: bool = True) -> dict:
        self.count = self.count + 1 if replay_count is None else replay_count
        cd = self._client_data("webauthn.get", options["challenge"], origin)
        auth_data = (hashlib.sha256(self.rp_id.encode()).digest()
                     + bytes([0x05 if uv else 0x01])  # UP (+UV : code PIN saisi)
                     + struct.pack(">I", self.count))
        sig = self.key.sign(auth_data + hashlib.sha256(cd).digest(), ec.ECDSA(hashes.SHA256()))
        return {"id": b64u(self.cred_id), "rawId": b64u(self.cred_id), "type": "public-key",
                "response": {"clientDataJSON": b64u(cd), "authenticatorData": b64u(auth_data),
                             "signature": b64u(sig), "userHandle": None},
                "clientExtensionResults": {}}
