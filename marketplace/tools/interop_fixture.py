"""Fichiers d'interopérabilité serveur -> launcher.

    python -m tools.interop_fixture ../cpp/tests/interop

Produit, par le MÊME chemin que le serveur (inspect_zip puis Signer), un
plugin signé, un mod signé et deux listes de révocation. cpp/tests/test_signing.cpp les
fait vérifier par le code du launcher ; tests/test_interop.py exige que
ce script les reproduise à l'octet près. Une dérive de format, d'un côté
comme de l'autre, casse donc un des deux tests.

Graines FIXES (0x01… racine, 0x02… intermédiaire), connues de tous : ce
sont des clés de test, identiques à make_key(1) et make_key(2) du test C++.
Ed25519 étant déterministe, la sortie l'est aussi.
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import zipfile
from pathlib import Path

from nacl.signing import SigningKey

from app.packages import inspect_zip
from app.signing import Signer, make_cert, write_secret_file

PLUGIN_ID = "interop"
VERSION = "1.0.0"
SIGNED_AT = 2_000_000_000

# Des accents dans le manifeste ET dans un nom de fichier : le launcher doit
# hacher et comparer les chemins en UTF-8, quelle que soit la page de code.
FILES: dict[str, bytes] = {
    "plugin.json": json.dumps({"name": "Interopérabilité", "version": VERSION, "api": 1,
                               "permissions": ["instances:read"],
                               "service": {"run": "app/service.exe"}},
                              ensure_ascii=False).encode(),
    "app/service.exe": b"binaire factice signe par le serveur Python\n",
    "données/lisez-moi.txt": "Fichier au nom accentué.\n".encode(),
}

MOD_ID, MOD_VERSION = "coolmod", "1.0"
MOD_FILES: dict[str, bytes] = {"coolmod-1.0.jar": b"PK faux jar signe par le serveur\n"}


def _signer(tmp: Path) -> Signer:
    root = SigningKey(b"\x01" * 32)
    inter = SigningKey(b"\x02" * 32)
    cert = make_cert(root, inter.verify_key, "py-int-1", ["plugin", "mod", "revocation"],
                     1000, 4_000_000_000)
    write_secret_file(tmp / "k", bytes(inter).hex())
    (tmp / "c").write_text(json.dumps(cert))
    return Signer.load(tmp / "k", tmp / "c")


def _files(content: dict[str, bytes], manifest: bool) -> dict[str, str]:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in content.items():
            z.writestr(name, data)
    return inspect_zip(buf.getvalue(), max_files=10, max_unpacked=1 << 20,
                       expect_manifest=manifest).files


def build() -> dict[str, bytes]:
    """Chemin relatif -> contenu, pour tout ce qui va dans le dossier."""
    files = _files(FILES, True)

    with tempfile.TemporaryDirectory() as t:
        signer = _signer(Path(t))
        sig = signer.sign_release(PLUGIN_ID, VERSION, files, SIGNED_AT)
        mod_sig = signer.sign_release(MOD_ID, MOD_VERSION, _files(MOD_FILES, False), SIGNED_AT,
                                      kind="mod")
        base = {"keyIds": [], "fileHashes": ["00" * 64], "killAll": False}
        rev7 = signer.sign_revocations({**base, "seq": 7, "issuedAt": SIGNED_AT,
                                        "releases": [{"id": "autre", "version": "2.0"}]})
        rev8 = signer.sign_revocations({**base, "seq": 8, "issuedAt": SIGNED_AT,
                                        "releases": [{"id": "autre", "version": "2.0"},
                                                     {"id": PLUGIN_ID, "version": VERSION}]})

    out = {f"plugin/{k}": v for k, v in FILES.items()}
    out["plugin/signature.json"] = sig.encode()
    out.update({f"mod/{k}": v for k, v in MOD_FILES.items()})
    out["mod/signature.json"] = mod_sig.encode()
    out["revocations-7.json"] = rev7.encode()
    out["revocations-8.json"] = rev8.encode()
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 1
    dest = Path(argv[0])
    for rel, data in build().items():
        p = dest / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    print(f"écrit dans {dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
