"""Mots de passe, 2FA et sessions."""

from __future__ import annotations

import hashlib
import hmac
import secrets
import time

import pyotp
from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError, VerifyMismatchError
from nacl import secret, utils
from nacl.exceptions import CryptoError

# Argon2id, paramètres par défaut de argon2-cffi (RFC 9106, profil bas en
# mémoire) : coûteux pour un attaquant qui aurait volé la base, supportable
# pour un serveur qui reçoit quelques connexions par minute.
_hasher = PasswordHasher()

MIN_PASSWORD = 12
MAX_PASSWORD = 256  # au-delà, rien ne justifie de faire hacher le serveur
LOCK_AFTER = 8      # échecs consécutifs avant verrouillage
LOCK_SECONDS = 15 * 60
TOTP_STEP = 30


def now() -> int:
    return int(time.time())


def password_ok(pw: str) -> str | None:
    """None si le mot de passe est acceptable, sinon la raison."""
    if len(pw) < MIN_PASSWORD:
        return f"mot de passe trop court ({MIN_PASSWORD} caractères au moins)"
    if len(pw) > MAX_PASSWORD:
        return "mot de passe trop long"
    return None


def hash_password(pw: str) -> str:
    return _hasher.hash(pw)


def verify_password(stored: str, pw: str) -> bool:
    try:
        return _hasher.verify(stored, pw)
    except (VerifyMismatchError, VerificationError, InvalidHashError):
        return False


# Empreinte factice : vérifier un compte INEXISTANT prend le même temps
# qu'un compte réel, sans quoi la durée de réponse révélerait quels
# courriels sont inscrits.
_DUMMY_HASH = _hasher.hash(secrets.token_urlsafe(16))


def burn_time() -> None:
    verify_password(_DUMMY_HASH, "x" * MIN_PASSWORD)


# --- 2FA ----------------------------------------------------------------------


def new_totp_secret() -> str:
    return pyotp.random_base32()


def encrypt_secret(key: bytes, plain: str) -> str:
    box = secret.SecretBox(key)
    return box.encrypt(plain.encode(), utils.random(secret.SecretBox.NONCE_SIZE)).hex()


def decrypt_secret(key: bytes, enc: str) -> str | None:
    try:
        return secret.SecretBox(key).decrypt(bytes.fromhex(enc)).decode()
    except (CryptoError, ValueError):
        return None


def totp_uri(secret_b32: str, email: str) -> str:
    return pyotp.TOTP(secret_b32).provisioning_uri(name=email, issuer_name="Team Launcher")


def check_totp(secret_b32: str, code: str, last_step: int, at: int | None = None) -> int | None:
    """Rend le pas accepté, ou None.

    Un pas d'écart toléré de chaque côté (horloge du téléphone). Et surtout :
    un code ne sert QU'UNE FOIS. Le pas accepté doit être strictement
    postérieur au dernier accepté — sans cela, un code intercepté resterait
    réutilisable pendant toute sa fenêtre de validité.
    """
    code = code.strip()
    if len(code) != 6 or not code.isdigit():
        return None
    t = now() if at is None else at
    totp = pyotp.TOTP(secret_b32)
    for offset in (-1, 0, 1):
        moment = t + offset * TOTP_STEP
        if hmac.compare_digest(totp.at(moment), code):
            step = moment // TOTP_STEP
            return step if step > last_step else None
    return None


# --- Sessions -----------------------------------------------------------------


def new_session_token() -> tuple[str, str]:
    """(jeton remis au client, empreinte stockée en base)."""
    token = secrets.token_urlsafe(32)
    return token, token_hash(token)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()
