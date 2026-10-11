"""Configuration du backend, lue dans l'environnement.

Aucun secret n'a de valeur par défaut : un serveur mal configuré doit
REFUSER de démarrer, pas tourner avec une clé de démonstration que tout le
monde connaît (le dépôt est public).
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


class ConfigError(RuntimeError):
    pass


def _required(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise ConfigError(f"variable d'environnement manquante : {name}")
    return value


@dataclass(frozen=True)
class Settings:
    # mysql+pymysql://utilisateur:motdepasse@hote/base?charset=utf8mb4
    database_url: str
    # Dossier des archives soumises (binaires et sources). Hors racine web.
    storage_dir: Path
    # Clé intermédiaire de signature : clé PRIVÉE (fichier, droits 0600)
    # et son certificat signé par la racine. La racine, elle, n'est JAMAIS
    # sur ce serveur.
    signing_key_path: Path
    signing_cert_path: Path
    # Clé symétrique (32 octets, hex) qui chiffre les secrets 2FA au repos :
    # une copie de la base ne suffit pas à générer les codes des comptes.
    totp_enc_key: bytes
    # Durée de vie d'une session, en secondes.
    session_ttl: int = 12 * 3600
    # Plafonds d'une soumission. Une archive est une donnée hostile tant
    # qu'elle n'a pas été relue.
    max_upload_bytes: int = 64 * 1024 * 1024
    max_unpacked_bytes: int = 256 * 1024 * 1024
    max_files: int = 4096


def load() -> Settings:
    key_hex = _required("TL_MARKET_TOTP_KEY")
    try:
        key = bytes.fromhex(key_hex)
    except ValueError as exc:
        raise ConfigError("TL_MARKET_TOTP_KEY doit être de l'hexadécimal") from exc
    if len(key) != 32:
        raise ConfigError("TL_MARKET_TOTP_KEY doit faire 32 octets (64 caractères hex)")
    return Settings(
        database_url=_required("TL_MARKET_DB_URL"),
        storage_dir=Path(_required("TL_MARKET_STORAGE")),
        signing_key_path=Path(_required("TL_MARKET_SIGNING_KEY")),
        signing_cert_path=Path(_required("TL_MARKET_SIGNING_CERT")),
        totp_enc_key=key,
    )
