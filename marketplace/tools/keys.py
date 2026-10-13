"""Cérémonie des clés de signature — à exécuter HORS LIGNE (réseau coupé).

    python -m tools.keys root-init    --out ~/tl-root
    python -m tools.keys issue        --root ~/tl-root/root.key.enc \\
                                      --key-id market-2026a --usages plugin,mod,revocation \\
                                      --days 365 --out ~/tl-keys
    python -m tools.keys show-root    --root ~/tl-root/root.key.enc
    python -m tools.keys restore-root --out ~/tl-root     # depuis la sauvegarde papier

La clé RACINE n'existe sur disque que CHIFFRÉE par une phrase de passe
(Argon2id + XSalsa20-Poly1305) : le fichier root.key.enc peut être copié
n'importe où, il ne sert à rien sans la phrase. Une sauvegarde papier
(64 caractères + contrôle) permet de la reconstituer si tout est perdu.
Seule sa clé PUBLIQUE est compilée dans le launcher (kBuiltinRoots,
cpp/src/signing.cpp). Le serveur ne reçoit que la clé INTERMÉDIAIRE et son
certificat ; si elle fuit, on la révoque (keyIds) et on en émet une autre,
sans recompiler le launcher.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from argon2.low_level import Type, hash_secret_raw
from nacl import secret
from nacl.exceptions import CryptoError
from nacl.signing import SigningKey

from app.signing import KNOWN_USAGES, Signer, load_seed, make_cert, write_secret_file

SAFE_KEY_ID = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")


def cpp_array(pub: bytes) -> str:
    """Clé publique au format d'une entrée de kBuiltinRoots."""
    body = ", ".join(f"0x{b:02x}" for b in pub)
    return "PublicKey{" + body + "}"


# --- Racine chiffrée ------------------------------------------------------------

MIN_PASSPHRASE = 16
# Argon2id, 256 Mio et 4 passes : chaque essai d'un attaquant qui aurait le
# fichier coûte de l'ordre d'une seconde et 256 Mio de mémoire.
KDF = {"t": 4, "m": 256 * 1024, "p": 2}


def _kdf(passphrase: str, salt: bytes, t: int, m: int, p: int) -> bytes:
    return hash_secret_raw(passphrase.encode("utf-8"), salt, time_cost=t, memory_cost=m,
                           parallelism=p, hash_len=32, type=Type.ID)


def encrypt_seed(seed: bytes, passphrase: str, kdf: dict = KDF) -> str:
    salt = os.urandom(16)
    box = secret.SecretBox(_kdf(passphrase, salt, kdf["t"], kdf["m"], kdf["p"]))
    return json.dumps({"v": 1, "kdf": "argon2id", "salt": base64.b64encode(salt).decode(),
                       **kdf, "box": box.encrypt(seed).hex()})


def decrypt_seed(text: str, passphrase: str) -> bytes:
    d = json.loads(text)
    if d.get("v") != 1 or d.get("kdf") != "argon2id":
        raise ValueError("format de racine chiffrée inconnu")
    key = _kdf(passphrase, base64.b64decode(d["salt"]), int(d["t"]), int(d["m"]), int(d["p"]))
    try:
        seed = secret.SecretBox(key).decrypt(bytes.fromhex(d["box"]))
    except CryptoError:
        raise ValueError("phrase de passe incorrecte (ou fichier altéré)") from None
    if len(seed) != 32:
        raise ValueError("racine mal formée")
    return seed


def paper_code(seed: bytes) -> str:
    """Sauvegarde papier : 8 groupes de 8 caractères hex, puis 8 de contrôle
    (début du SHA-256) pour détecter une faute de recopie."""
    h = seed.hex()
    return " ".join(h[i:i + 8] for i in range(0, 64, 8)) + "  /  " + \
        hashlib.sha256(seed).hexdigest()[:8]


def parse_paper(text: str) -> bytes:
    body, _, check = text.partition("/")
    h = "".join(body.split()).lower()
    if len(h) != 64 or any(c not in "0123456789abcdef" for c in h):
        raise ValueError("64 caractères hexadécimaux attendus avant le « / »")
    seed = bytes.fromhex(h)
    if "".join(check.split()).lower() != hashlib.sha256(seed).hexdigest()[:8]:
        raise ValueError("contrôle faux : une faute de recopie quelque part")
    return seed


def ask_new_passphrase() -> str:
    print(f"Choisissez une phrase de passe d'au moins {MIN_PASSPHRASE} caractères "
          "(par exemple 5 ou 6 mots sans rapport entre eux). Rien ne s'affiche en tapant.")
    while True:
        p1 = getpass.getpass("Phrase de passe : ")
        if len(p1) < MIN_PASSPHRASE:
            print(f"Trop courte ({len(p1)} caractères).")
            continue
        if getpass.getpass("Encore une fois : ") != p1:
            print("Les deux saisies diffèrent.")
            continue
        return p1


def load_root(path: Path) -> SigningKey:
    if path.suffix == ".enc":
        pw = getpass.getpass("Phrase de passe de la racine : ")
        return SigningKey(decrypt_seed(path.read_text(), pw))
    return load_seed(path)  # ancienne racine en clair


def _write_root(out: Path, seed: bytes, passphrase: str) -> Path:
    key_path = out / "root.key.enc"
    write_secret_file(key_path, encrypt_seed(seed, passphrase) + "\n")
    (out / "root.pub").write_text(bytes(SigningKey(seed).verify_key).hex() + "\n")
    return key_path


def _print_public(seed: bytes) -> None:
    print("\nÀ coller dans kBuiltinRoots (cpp/src/signing.cpp) — c'est public :\n")
    print(f"    {cpp_array(bytes(SigningKey(seed).verify_key))},")


def cmd_root_init(a) -> int:
    out = Path(a.out).expanduser()
    if (out / "root.key.enc").exists() or (out / "root.key").exists():
        print(f"Une racine existe déjà dans {out} : refus de l'écraser.", file=sys.stderr)
        return 1
    passphrase = ask_new_passphrase()
    seed = os.urandom(32)
    key_path = _write_root(out, seed, passphrase)
    print(f"\nRacine créée, chiffrée : {key_path}")
    print("\n=== SAUVEGARDE PAPIER : à recopier à la main, puis fermer ce terminal ===")
    print(f"\n    {paper_code(seed)}\n")
    print("Ce code EST la clé racine, en clair : ne le photographiez pas, ne le tapez")
    print("nulle part. Rangez la feuille chez vous. Si la phrase de passe ET la feuille")
    print("sont perdues, la racine l'est aussi.")
    _print_public(seed)
    return 0


def cmd_restore_root(a) -> int:
    out = Path(a.out).expanduser()
    if (out / "root.key.enc").exists():
        print(f"{out / 'root.key.enc'} existe déjà : refus de l'écraser.", file=sys.stderr)
        return 1
    try:
        seed = parse_paper(getpass.getpass("Code papier (rien ne s'affiche) : "))
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
    key_path = _write_root(out, seed, ask_new_passphrase())
    print(f"Racine reconstituée : {key_path}")
    _print_public(seed)
    return 0


def cmd_show_root(a) -> int:
    try:
        pub = bytes(load_root(Path(a.root).expanduser()).verify_key)
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
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
    try:
        root = load_root(Path(a.root).expanduser())
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
    out = Path(a.out).expanduser()
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


def cmd_sign_update(a) -> int:
    """Signe un paquet de mise à jour du launcher (auto-update).

    Produit <zip>.sig à côté du zip, à publier comme asset de la release
    GitHub. Le launcher le télécharge à <asset>.sig et refuse la mise à
    jour si la signature manque ou est invalide. La clé doit porter l'usage
    « launcher » (émettez-la avec : issue --usages launcher)."""
    try:
        signer = Signer.load(Path(a.key).expanduser(), Path(a.cert).expanduser())
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
    zip_path = Path(a.zip).expanduser()
    if not zip_path.is_file():
        print(f"paquet introuvable : {zip_path}", file=sys.stderr)
        return 1
    h = hashlib.sha512()
    with open(zip_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    try:
        sig = signer.sign_update(h.hexdigest(), a.version, int(time.time()))
    except PermissionError as exc:
        print(exc, file=sys.stderr)
        return 1
    out = zip_path.with_name(zip_path.name + ".sig")
    out.write_text(sig)
    print(f"Signature écrite : {out}")
    print(f"Publiez {zip_path.name} ET {out.name} comme assets de la release.")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="tools.keys")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("root-init")
    r.add_argument("--out", required=True)
    s = sub.add_parser("show-root")
    s.add_argument("--root", required=True)
    rr = sub.add_parser("restore-root")
    rr.add_argument("--out", required=True)
    i = sub.add_parser("issue")
    i.add_argument("--root", required=True)
    i.add_argument("--key-id", required=True)
    i.add_argument("--usages", default="plugin,mod,revocation")
    i.add_argument("--days", type=int, default=365)
    i.add_argument("--out", required=True)
    u = sub.add_parser("sign-update")
    u.add_argument("--key", required=True)
    u.add_argument("--cert", required=True)
    u.add_argument("--zip", required=True)
    u.add_argument("--version", required=True)
    a = p.parse_args(argv)
    return {"root-init": cmd_root_init, "show-root": cmd_show_root, "issue": cmd_issue,
            "restore-root": cmd_restore_root, "sign-update": cmd_sign_update}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
