# Marketplace Team Launcher — backend (V8)

FastAPI + MariaDB. Reçoit les plugins et mods, les fait relire par des
humains, signe ce qui est approuvé, publie la liste de révocation que lit
le launcher.

## Principes

- **Racine hors ligne.** La clé racine ne touche jamais ce serveur. Seule
  sa clé publique est compilée dans le launcher (`kBuiltinRoots`,
  `cpp/src/signing.cpp`). Le serveur signe avec une **intermédiaire** à
  durée limitée, révocable sans recompiler le launcher.
- **2FA obligatoire** pour toute action sensible (soumettre, relire,
  administrer). Le mot de passe seul ouvre une session « à moitié ». Un code
  TOTP ne sert qu'une fois. Les secrets TOTP sont chiffrés en base.
- **Séparation des rôles.** Un relecteur ne statue jamais sur sa propre
  soumission. Les rôles se donnent sur le serveur (`tools.admin`), pas par
  l'API.
- **Archives hostiles.** Rien n'est décompressé sur disque ; chemins,
  liens, doublons de casse et bombes ZIP sont refusés à la réception —
  sources comprises.
- **Aucun secret par défaut.** Sans configuration complète, le serveur
  refuse de démarrer.

## Installation (VPS)

```bash
sudo apt install mariadb-server python3-venv python3-pip
python3 -m venv ~/tl-market-venv
~/tl-market-venv/bin/pip install -r requirements.txt
```

Base :

```sql
CREATE DATABASE tl_market CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER 'tl_market'@'localhost' IDENTIFIED BY '…';
GRANT ALL ON tl_market.* TO 'tl_market'@'localhost';
```

Variables d'environnement (fichier `.env` hors dépôt, droits 0600) :

| Variable | Contenu |
|---|---|
| `TL_MARKET_DB_URL` | `mysql+pymysql://tl_market:…@localhost/tl_market?charset=utf8mb4` |
| `TL_MARKET_STORAGE` | dossier des archives, hors racine web |
| `TL_MARKET_SIGNING_KEY` | clé intermédiaire (fichier 0600) |
| `TL_MARKET_SIGNING_CERT` | son certificat signé par la racine |
| `TL_MARKET_TOTP_KEY` | 64 caractères hex : `python -c "import secrets;print(secrets.token_hex(32))"` |

Lancement, derrière un reverse proxy HTTPS :

```bash
uvicorn app.main:app --host 127.0.0.1 --port 8300
```

## Hébergement Pterodactyl (Pixel Horizon)

Œuf « Python » générique. Pas de shell ni de systemd : `run.py` lit le
`.env` posé à côté de lui, écoute sur `SERVER_PORT`, et la **console du
panneau** sert de console d'administration (`help`, `list`,
`grant <email> reviewer|admin`, `revoke …`, `reset-2fa <email>`).

1. Image Docker : Python 3.12 ou plus récent. Variables de démarrage :
   fichier `run.py`, requirements `requirements.txt`, pas de dépôt git.
2. Onglet *Databases* : créer la base, reporter hôte, nom, utilisateur et
   mot de passe dans `TL_MARKET_DB_URL`.
3. Déposer (SFTP ou gestionnaire de fichiers) le contenu de `marketplace/`
   **sans** `tests/`, puis `keys/` (intermédiaire, certificat d'origine) et
   `.env` (modèle : `.env.example`).
4. HTTPS : le launcher l'exige. Domaine sur Cloudflare, enregistrement A
   proxifié vers l'IP du nœud, *Origin Rule* qui redirige vers le port
   attribué, certificat d'origine Cloudflare dans `keys/origin.*`, mode SSL
   *Full (strict)*.

## Cérémonie des clés (hors ligne)

Réseau coupé pendant la cérémonie. La racine n'est écrite que **chiffrée**
(phrase de passe, Argon2id) : `root.key.enc` peut être copié n'importe où,
il ne sert à rien sans la phrase. Une sauvegarde **papier** (64 caractères +
contrôle) permet de la reconstituer (`restore-root`).

```bash
python -m tools.keys root-init --out ~/tl-root
python -m tools.keys issue --root ~/tl-root/root.key.enc     --key-id market-2026a --usages plugin,mod,revocation --days 365 --out ~/tl-keys
```

`root-init` affiche la ligne à coller dans `kBuiltinRoots`. Copier
ensuite `market-2026a.key` et `market-2026a.cert.json` sur le serveur.
Rotation : émettre `market-2026b`, basculer le serveur, révoquer
`market-2026a` (`POST /v1/admin/revoke {"keyIds": ["market-2026a"]}`)
depuis la nouvelle clé.

## Rôles

```bash
python -m tools.admin grant dev@exemple.org reviewer
python -m tools.admin grant moi@exemple.org admin
python -m tools.admin reset-2fa dev@exemple.org   # après vérification d'identité hors bande
```

## Tests

```bash
python -m pytest tests                      # SQLite en mémoire
TL_MARKET_TEST_DB_URL=mysql+pymysql://… python -m pytest tests   # MariaDB (base de test VIDÉE)
```

`tests/test_interop.py` vérifie que le serveur reproduit à l'octet près
`cpp/tests/interop`, que le launcher vérifie de son côté
(`TLTestSigning`). Après un changement de format volontaire :
`python -m tools.interop_fixture ../cpp/tests/interop`.

## API

| Méthode | Chemin | Accès |
|---|---|---|
| POST | `/v1/auth/register`, `/login`, `/logout` | public / session |
| POST | `/v1/auth/totp/setup`, `/confirm`, `/verify` | session |
| GET | `/v1/me` | session |
| POST | `/v1/projects` | 2FA |
| POST | `/v1/projects/{kind}/{slug}/submissions` (binary, source, version) | 2FA, propriétaire |
| GET | `/v1/submissions/{id}` | 2FA |
| GET | `/v1/review/queue`, `/submissions/{id}`, `/submissions/{id}/source-diff` | relecteur |
| POST | `/v1/review/submissions/{id}/approve`, `/reject` | relecteur, pas le sien |
| POST | `/v1/admin/revoke`, `/v1/admin/killswitch` | admin |
| GET | `/v1/public/catalog`, `/revocations`, `/download/{kind}/{slug}/{version}` | public |
