"""Démarrage sur un serveur Pterodactyl (œuf « Python » générique).

Pterodactyl ne donne ni shell ni systemd : un seul processus, un seul port
(SERVER_PORT), et une console qui écrit sur l'entrée standard. D'où :
  - la configuration est lue dans le fichier .env, à côté de ce script
    (droits gérés par le panneau) — les variables d'environnement déjà
    posées gardent la priorité ;
  - la console du panneau sert de console d'administration : les commandes
    tapées (grant, revoke, reset-2fa, list) sont lues sur stdin ;
  - HTTPS direct si TL_MARKET_TLS_CERT / TL_MARKET_TLS_KEY sont fournis
    (certificat d'origine Cloudflare, par exemple).
"""

from __future__ import annotations

import os
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_dotenv(path: Path) -> None:
    """Lecteur .env minimal : CLE=valeur, # commentaires, guillemets ôtés."""
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        os.environ.setdefault(key, value)


HELP = """Commandes de la console :
  list                         comptes et rôles
  grant <email> reviewer|admin donner un rôle
  revoke <email> reviewer|admin retirer un rôle
  reset-2fa <email>            effacer la 2FA (après vérification d'identité)
  help                         cette aide"""


def console_loop() -> None:
    from tools import admin

    for line in sys.stdin:
        argv = line.split()
        if not argv:
            continue
        if argv[0] in ("help", "?"):
            print(HELP, flush=True)
            continue
        try:
            admin.main(argv)
        except SystemExit:
            pass
        except Exception as exc:  # la console ne doit jamais tuer le serveur
            print(f"erreur : {exc}", flush=True)
        sys.stdout.flush()


def main() -> None:
    os.chdir(HERE)
    load_dotenv(HERE / ".env")
    import uvicorn

    try:
        # L'import est DANS le try : app/main.py construit l'application (et
        # se connecte à la base) dès qu'il est importé.
        from app.main import app  # refuse de démarrer si la configuration est incomplète
    except Exception as exc:
        if "Access denied" not in str(exc):
            raise
        # Diagnostic sans jamais afficher le mot de passe : longueur et début
        # de son SHA-256, à comparer avec le même calcul fait ailleurs.
        import hashlib

        pw = os.environ.get("TL_MARKET_DB_PASSWORD", "")
        print("\nLa base de données refuse le mot de passe.", flush=True)
        if pw:
            print(f"Mot de passe lu dans .env : {len(pw)} caractères, empreinte "
                  f"{hashlib.sha256(pw.encode()).hexdigest()[:12]}", flush=True)
        else:
            print("TL_MARKET_DB_PASSWORD est vide ou absent du .env.", flush=True)
        sys.exit(1)
    port = int(os.environ.get("SERVER_PORT") or os.environ.get("TL_MARKET_PORT") or "8300")
    cert = os.environ.get("TL_MARKET_TLS_CERT", "").strip()
    key = os.environ.get("TL_MARKET_TLS_KEY", "").strip()
    tls = {"ssl_certfile": cert, "ssl_keyfile": key} if cert and key else {}

    threading.Thread(target=console_loop, daemon=True).start()
    print(f"Marketplace : port {port}, {'HTTPS' if tls else 'HTTP (sans TLS)'}. "
          "Tapez « help » pour les commandes.", flush=True)
    # Derrière le proxy HTTPS de l'hébergeur, l'adresse vue est celle du
    # proxy : on lit celle qu'il transmet (X-Forwarded-For). Elle ne sert
    # qu'à limiter le débit de la télémétrie — l'authentification ne
    # dépend jamais de l'adresse, et l'adresse n'est jamais stockée.
    uvicorn.run(app, host="0.0.0.0", port=port, log_level="info",
                server_header=False, proxy_headers=True, forwarded_allow_ips="*", **tls)


if __name__ == "__main__":
    main()
