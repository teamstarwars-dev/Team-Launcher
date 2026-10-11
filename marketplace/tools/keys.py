"""Cérémonie des clés de signature — à exécuter HORS LIGNE.

    python -m tools.keys root-init  --out /media/cle-usb/tl-root
    python -m tools.keys issue      --root /media/cle-usb/tl-root/root.key \\
                                    --key-id market-2026a --usages plugin,mod,revocation \\
                                    --days 365 --out ./keys
    python -m tools.keys show-root  --root /media/cle-usb/tl-root/root.key

La clé RACINE ne quitte jamais le support hors ligne : seule sa clé
PUBLIQUE est compilée dans le launcher (kBuiltinRoots, cpp/src/signing.cpp).
Le serveur ne reçoit que la clé INTERMÉDIAIRE et son certificat ; si elle
fuit, on la révoque (keyIds) et on en émet une autre, sans recompiler le
launcher.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from nacl.signing import SigningKey

from app.signing import KNOWN_USAGES, load_seed, make_cert, write_secret_file

SAFE_KEY_ID = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")


def cpp_array(pub: bytes) -> str:
    """Clé publique au format d'une entrée de kBuiltinRoots."""
    body = ", ".join(f"0x{b:02x}" for b in pub)
    return "PublicKey{" + body + "}"


def cmd_root_init(a) -> int:
    out = Path(a.out)
    key_path = out / "root.key"
    if key_path.exists():
        print(f"{key_path} existe déjà : refus d'écraser une racine.", file=sys.stderr)
        return 1
    root = SigningKey.generate()
    write_secret_file(key_path, bytes(root).hex() + "\n")
    pub = bytes(root.verify_key)
    (out / "root.pub").write_text(pub.hex() + "\n")
    print(f"Racine créée : {key_path} (0600). Gardez-la hors ligne, en deux copies.")
    print("\nÀ coller dans kBuiltinRoots (cpp/src/signing.cpp) :\n")
    print(f"    {cpp_array(pub)},")
    return 0


def cmd_show_root(a) -> int:
    pub = bytes(load_seed(Path(a.root)).verify_key)
    print(pub.hex())
    print(f"    {cpp_array(pub)},")
    return 0


def cmd_issue(a) -> int:
    if not a.key_id or len(a.key_id) > 64 or not set(a.key_id) <= SAFE_KEY_ID:
        print("key-id : 1 à 64 caractères parmi [A-Za-z0-9-_.]", file=sys.stderr)
        return 1
    usages = [u for u in a.usages.split(",") if u]
    if not usages or any(u not in KNOWN_USAGES for u in usages):
        print(f"usages : parmi {', '.join(KNOWN_USAGES)}", file=sys.stderr)
        return 1
    if not 1 <= a.days <= 3 * 365:
        # Une intermédiaire de longue vie, c'est une racine qui ne dit pas
        # son nom : la rotation n'a de sens que si les clés expirent.
        print("days : entre 1 et 1095", file=sys.stderr)
        return 1
    root = load_seed(Path(a.root))
    out = Path(a.out)
    key_path, cert_path = out / f"{a.key_id}.key", out / f"{a.key_id}.cert.json"
    if key_path.exists() or cert_path.exists():
        print(f"{a.key_id} existe déjà dans {out}", file=sys.stderr)
        return 1
    inter = SigningKey.generate()
    now = int(time.time())
    cert = make_cert(root, inter.verify_key, a.key_id, usages, now, now + a.days * 86400)
    write_secret_file(key_path, bytes(inter).hex() + "\n")
    cert_path.write_text(json.dumps(cert) + "\n")
    print(f"Intermédiaire {a.key_id} : {key_path} (0600) et {cert_path}")
    print(f"Valide jusqu'au {time.strftime('%Y-%m-%d', time.gmtime(now + a.days * 86400))} (UTC).")
    print("Copiez ces deux fichiers sur le serveur (TL_MARKET_SIGNING_KEY / _CERT).")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="tools.keys")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("root-init")
    r.add_argument("--out", required=True)
    s = sub.add_parser("show-root")
    s.add_argument("--root", required=True)
    i = sub.add_parser("issue")
    i.add_argument("--root", required=True)
    i.add_argument("--key-id", required=True)
    i.add_argument("--usages", default="plugin,mod,revocation")
    i.add_argument("--days", type=int, default=365)
    i.add_argument("--out", required=True)
    a = p.parse_args(argv)
    return {"root-init": cmd_root_init, "show-root": cmd_show_root, "issue": cmd_issue}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
