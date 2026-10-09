# Team Launcher API — guide VPS

API comptes / amis / messages / signaling vocal / presence.
Node.js 20+ · Postgres 14+ · ecoute HTTP locale (TLS via reverse proxy).

## 1. Base de donnees

```bash
sudo -u postgres psql -c "CREATE USER teamlauncher WITH PASSWORD 'MOT-DE-PASSE-SOLIDE';"
sudo -u postgres psql -c "CREATE DATABASE teamlauncher OWNER teamlauncher;"
```

## 2. Code + dependances

```bash
cd /opt
# copier le dossier server/ ici (scp, git clone...)
cd /opt/teamlauncher-api
npm install --omit=dev --no-audit --no-fund
cp .env.example .env
nano .env   # DATABASE_URL + ADMIN_TOKEN (+ PORT si besoin)
psql "$DATABASE_URL" -f db/schema.sql
```

Contenu minimal du `.env` :

```
PORT=3000
DATABASE_URL=postgres://teamlauncher:MOT-DE-PASSE-SOLIDE@localhost:5432/teamlauncher
ADMIN_TOKEN=<64 hexas : node -e "console.log(require('crypto').randomBytes(32).toString('hex'))">
```

## 3. Service systemd

`/etc/systemd/system/teamlauncher-api.service` :

```
[Unit]
Description=Team Launcher API
After=network.target postgresql.service

[Service]
Type=simple
User=teamlauncher
WorkingDirectory=/opt/teamlauncher-api
EnvironmentFile=/opt/teamlauncher-api/.env
ExecStart=/usr/bin/node src/server.js
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo useradd -r -s /usr/sbin/nologin teamlauncher 2>/dev/null
sudo chown -R teamlauncher:teamlauncher /opt/teamlauncher-api
sudo systemctl daemon-reload
sudo systemctl enable --now teamlauncher-api
curl http://127.0.0.1:3000/api/health   # -> {"ok":true}
```

## 4. Reseau

- N'ouvrir que le necessaire : l'API derriere pare-feu, acces launcher uniquement.
- Domaine + TLS plus tard : reverse proxy (nginx/caddy) en frontal,
  l'API reste en HTTP local, le launcher pointera sur `https://api....`.
- Logs : `journalctl -u teamlauncher-api -f`.

## 5. Contrat (resume)

- Auth : `Authorization: Bearer <64 hexas>`.
- `POST /api/accounts/register|login {username, password}` (pseudo 3-24 : lettres, chiffres, `_` et `.` ; mdp 8+).
- `GET /api/me` · `POST /api/accounts/logout` · `POST /api/presence {status}`.
- Amis : `GET /api/friends` · `POST /api/friends/request|accept|decline|block` · `DELETE /api/friends/:username`.
- Messages : `GET /api/messages/:username?before=&limit=` · `POST /api/messages {to, text}` (2000 max).
- Appels : `GET /api/calls/active` · `POST /api/calls/invite {to, lobbyId}` · `POST /api/calls/:id/accept|decline|hangup`.
- Temps reel : `GET /api/events?since=<id>` (long-polling 25 s, evenements supprimes a la livraison).
- Sante : `GET /api/health` · stats : `GET /api/admin/stats` (header `X-Admin-Token`).
