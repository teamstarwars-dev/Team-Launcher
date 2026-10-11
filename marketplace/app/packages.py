"""Inspection d'une archive soumise.

Une archive reçue est une donnée HOSTILE tant qu'elle n'a pas été relue.
Ce module ne l'extrait jamais sur le disque : il la parcourt en mémoire,
calcule l'empreinte de chaque fichier, et refuse tout ce qui pourrait
nuire au serveur ou, plus tard, au poste qui l'installera.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import stat
import zipfile
from dataclasses import dataclass

# Même règle que le plugin host du launcher pour l'identifiant d'un plugin.
SLUG_RE = re.compile(r"^[a-z0-9_-][a-z0-9._-]{0,63}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z.+_-]{0,31}$")


class PackageError(ValueError):
    pass


@dataclass
class Inspected:
    files: dict[str, str]  # chemin -> SHA-512 hex
    manifest: dict
    total_bytes: int


def _safe_name(name: str) -> str:
    """Valide un nom d'entrée et le rend tel quel, ou lève PackageError."""
    if not name or len(name) > 512:
        raise PackageError("nom d'entrée vide ou trop long")
    if "\\" in name:
        raise PackageError(f"séparateur « \\ » dans « {name} » : utilisez « / »")
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        raise PackageError(f"chemin absolu refusé : {name}")
    parts = name.rstrip("/").split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise PackageError(f"chemin refusé : {name}")
    if any(ord(c) < 0x20 for c in name):
        raise PackageError("caractère de contrôle dans un nom d'entrée")
    return name


def inspect_zip(data: bytes, *, max_files: int, max_unpacked: int,
                expect_manifest: bool = True) -> Inspected:
    try:
        zf = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as exc:
        raise PackageError("archive ZIP illisible") from exc

    infos = zf.infolist()
    if len(infos) > max_files:
        raise PackageError(f"trop de fichiers ({len(infos)} > {max_files})")

    files: dict[str, str] = {}
    seen_folded: set[str] = set()
    total = 0
    manifest_raw: bytes | None = None

    for info in infos:
        name = _safe_name(info.filename)
        if info.is_dir():
            continue
        # Lien symbolique stocké dans les attributs Unix de l'entrée.
        if stat.S_ISLNK(info.external_attr >> 16):
            raise PackageError(f"lien symbolique refusé : {name}")
        if info.flag_bits & 0x1:
            raise PackageError(f"entrée chiffrée refusée : {name}")
        # Deux noms qui ne diffèrent que par la casse entreraient en
        # collision sous Windows : l'un écraserait l'autre à l'installation.
        folded = name.casefold()
        if folded in seen_folded:
            raise PackageError(f"doublon (à la casse près) : {name}")
        seen_folded.add(folded)
        if name == "signature.json":
            raise PackageError("signature.json est produit par le serveur, pas par le développeur")

        # Bombe ZIP : la taille DÉCLARÉE ne prouve rien. On compte les
        # octets réellement décompressés et on s'arrête au plafond.
        h = hashlib.sha512()
        size = 0
        with zf.open(info) as f:
            while chunk := f.read(64 * 1024):
                size += len(chunk)
                total += len(chunk)
                if total > max_unpacked:
                    raise PackageError("archive trop volumineuse une fois décompressée")
                h.update(chunk)
                if name == "plugin.json" and size <= 256 * 1024:
                    manifest_raw = (manifest_raw or b"") + chunk
        if name == "plugin.json" and size > 256 * 1024:
            raise PackageError("plugin.json démesuré")
        files[name] = h.hexdigest()

    if not files:
        raise PackageError("archive vide")

    manifest: dict = {}
    if expect_manifest:
        if manifest_raw is None:
            raise PackageError("plugin.json absent de la racine de l'archive")
        try:
            manifest = json.loads(manifest_raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PackageError("plugin.json illisible") from exc
        if not isinstance(manifest, dict):
            raise PackageError("plugin.json : objet attendu")
    return Inspected(files=files, manifest=manifest, total_bytes=total)


def check_plugin_manifest(manifest: dict, slug: str, version: str) -> None:
    """Cohérence entre ce que dit le manifeste et ce qui est soumis."""
    if not SLUG_RE.match(slug):
        raise PackageError("identifiant de plugin invalide")
    if not VERSION_RE.match(version):
        raise PackageError("version invalide")
    if str(manifest.get("version", "")) != version:
        raise PackageError(
            f"la version de plugin.json (« {manifest.get('version', '')} ») ne correspond "
            f"pas à la version soumise (« {version} »)")
    if not isinstance(manifest.get("name", ""), str) or not manifest.get("name"):
        raise PackageError("plugin.json : « name » manquant")
