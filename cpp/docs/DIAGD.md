# `tl_diagd` — service de diagnostic, exploitation

Service autonome servant **uniquement** les deux analyses de
[l'API](API.md) : `POST /v1/diag/crash` et `POST /v1/diag/mods`.

Il ne lit aucune instance et ne lance aucune partie : ces capacités-là
demandent un launcher sur la machine, et un serveur n'en a pas.

**506 Ko sous Windows, 563 Ko sous Linux.** Il ne dépend que de `std`, de
nlohmann et de miniz — ni SDL, ni SDK Discord, ni libcurl, ni
configuration de launcher, ni base de données. C'est délibéré : lier le
service à `tl_core` aurait traîné tout cela pour rien, avec une surface
d'attaque sans rapport avec ce qu'il fait.

---

## 1. Lancer

```bash
tl_diagd --keys /etc/teamlauncher/keys.json --host 127.0.0.1 --port 8080
```

| Option | Défaut | |
|---|---|---|
| `--keys <fichier>` | *(requis)* | Sans clés, tout serait refusé. |
| `--host <adresse>` | `127.0.0.1` | Laissez le bouclage : le TLS vient du proxy. |
| `--port <n>` | `8080` | |
| `--max-body <octets>` | `4194304` (4 Mo) | Un rapport de crash dépasse rarement 100 Ko. |

Routes : `POST /v1/diag/crash`, `POST /v1/diag/mods`, `GET /v1`, et
`GET /healthz` — **sans clé**, pour que la supervision n'en demande pas
une ; elle ne révèle que le numéro de version.

### TLS

Volontairement absent. Mettez un proxy devant (Caddy, nginx) :

```
api.exemple.fr {
    reverse_proxy 127.0.0.1:8080
    request_body { max_size 4MB }
}
```

Embarquer OpenSSL dans le service aurait ajouté une grosse dépendance
pour refaire moins bien ce qu'un proxy fait déjà, et pour devoir suivre
ses mises à jour de sécurité nous-mêmes.

---

## 2. Délivrer une clé

```bash
tl_diagd --new-key "Bot support Portaldev" --rate 30
```

```
Secret à remettre au demandeur (affiché une seule fois) :
  06995fc0…589d

Ligne à ajouter au tableau "keys" du fichier :
  {"id":"ak_06995fc00006","hash":"1d06269e…","appName":"Bot support Portaldev",
   "ratePerMin":30,"revoked":false}
```

Collez la ligne dans le fichier, remettez le secret au demandeur. **Le
service ne conserve que l'empreinte** : si le secret est perdu, il faut
en générer un autre — personne ne peut le relire.

### Le fichier de clés

```json
{ "keys": [
  {"id":"ak_…","hash":"<sha1 du secret>","appName":"…",
   "ratePerMin":60,"revoked":false}
] }
```

Il est **rechargé quand sa date de modification change** : ajouter ou
révoquer une clé ne demande pas de redémarrage. Un fichier devenu
illisible ne révoque personne — les clés en mémoire sont conservées et
l'erreur part au journal. Couper l'accès à tout le monde parce qu'une
virgule manque serait le pire comportement possible.

C'est volontairement rudimentaire. Vous aurez cinq clés, pas cinq mille ;
un système de comptes coûterait plus qu'il ne rapporterait, et la
délivrance à la main vous laisse le droit de dire non sans avoir à coder
une politique.

---

## 3. La règle qui ne se rattrape pas

**Le corps des requêtes n'est jamais écrit sur disque.**

Un rapport de crash contient le nom de compte Windows dans les chemins de
fichiers, parfois des noms de serveurs, parfois des dossiers personnels.
L'analyse se fait en mémoire ; le journal ne reçoit que la route, le code,
l'identifiant de clé, la **taille** et le verdict :

```
2026-10-01T15:49:03Z /v1/diag/crash 200 key=ak_06995fc00006 bytes=14203 cause=mod_error
2026-10-01T15:49:16Z /v1/diag/mods  200 key=ak_06995fc00006 bytes=29    errors=0
```

Même un plantage interne ne renvoie que `{"error":"erreur interne"}` : le
détail d'une exception pourrait porter un fragment du corps.

Ce n'est pas qu'une précaution juridique. « On n'enregistre rien de ce que
vous envoyez » est une phrase que vos concurrents ne peuvent pas écrire,
et elle n'est vraie que tant que personne n'ajoute un journal de débogage
« juste le temps de comprendre un bug ». Si vous devez le faire un jour,
faites-le derrière un drapeau explicite, et dites-le sur le formulaire.

---

## 4. Plafond de débit

`ratePerMin` par clé (60 par défaut), compté par minute civile. Au-delà :
`429` avec un en-tête `Retry-After`. C'est un garde-fou contre la boucle
accidentelle, pas une politique commerciale.

Vérifié : avec `ratePerMin: 30`, 35 appels dans la même minute donnent
29 réponses `200` et 6 `429`.

---

## 5. Service systemd

```ini
[Unit]
Description=Team Launcher — service de diagnostic
After=network.target

[Service]
ExecStart=/opt/teamlauncher/tl_diagd --keys /etc/teamlauncher/keys.json --port 8080
User=tldiag
Restart=on-failure

# Il ne lit qu'un fichier de clés et n'écrit rien : autant le lui imposer.
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
ReadOnlyPaths=/etc/teamlauncher

[Install]
WantedBy=multi-user.target
```

Le journal part sur la sortie standard, donc dans `journalctl -u
tl-diagd`. Pensez à une rotation : une ligne par requête, c'est peu, mais
ça finit par s'accumuler.

---

## 6. Vérifier une installation

```bash
curl -s http://127.0.0.1:8080/healthz
# {"ok":true,"version":"6.0.0"}

curl -s -X POST http://127.0.0.1:8080/v1/diag/crash \
     -H "Authorization: Bearer $SECRET" \
     --data-binary @un-rapport.txt | jq '.cause, .suspects[0].modId'
```

Si `/healthz` répond mais que les analyses rendent `401`, le fichier de
clés n'a pas été chargé : regardez la ligne `clés rechargées : N` au
démarrage du journal.
