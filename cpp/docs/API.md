# API Team Launcher — v1

> **Envoie-moi un log de crash, je te dis quel mod.
> Envoie-moi une liste de mods, je te dis ce qui va casser.**

Cette API donne accès à deux analyses que Team Launcher est seul à savoir
faire, et à la lecture de l'état du launcher. Elle n'est **pas** un outil
de pilotage à distance : son intérêt est le verdict qu'elle rend.

---

## 1. En deux minutes

```bash
# L'utilisateur active l'API et vous remet une clé (voir §2).
BASE=http://127.0.0.1:27850
KEY=ak_votre_cle

# « Quel mod a planté ? » — le log brut suffit, tel quel.
curl -s -X POST "$BASE/v1/diag/crash" \
     -H "Authorization: Bearer $KEY" \
     --data-binary @crash-2026-09-01_17.44.59-client.txt
```

```json
{
  "found": true,
  "cause": "mod_error",
  "title": "Le crash vient du mod « webscreen ».",
  "action": "→ Désactive ce mod pour confirmer, puis cherche une version compatible.",
  "loader": "Forge",
  "mcVersion": "1.12.2",
  "exception": "java.lang.NullPointerException: Can't use a null-name for the registry, object null.",
  "suspects": [
    {
      "modId": "webscreen",
      "version": "2.0.0",
      "source": "WebDisplay2-2.0.0.jar",
      "evidence": "at fr.webscreen.registry.ModItems.registerItems(ModItems.java:35)",
      "score": 60
    }
  ],
  "summary": "Le crash vient du mod « webscreen ».\n→ Désactive ce mod…"
}
```

Remarquez `source` : le fichier s'appelle **WebDisplay2**, pas
« webscreen ». Chercher le coupable par nom de fichier aurait échoué.

---

## 2. Obtenir un accès

L'API vit **sur la machine de l'utilisateur**, sur `127.0.0.1`. Elle est
désactivée par défaut. Pour que votre application y accède, l'utilisateur
doit, dans **Paramètres → Avancé** :

1. cocher **Activer l'API locale** (port par défaut : 27850) ;
2. dans **Clés d'application**, saisir le nom de votre application,
   cocher les portées nécessaires, et cliquer **Créer une clé**.

La clé n'est affichée **qu'une seule fois**. Team Launcher n'en conserve
que l'empreinte : personne, pas même l'utilisateur, ne peut la relire
ensuite. Si elle est perdue, il faut en créer une autre.

L'utilisateur peut **révoquer** votre clé à tout moment, sans gêner les
autres applications, et voit combien d'appels vous avez faits et quand.

> Demandez **le minimum de portées**. Une application qui réclame
> `control` pour afficher un état sera refusée par les utilisateurs
> attentifs, et ils auront raison.

---

## 3. Authentification

Toutes les requêtes portent la clé :

```
Authorization: Bearer ak_votre_cle
```

### Portées

| Portée | Donne accès à | Ce que ça expose |
|---|---|---|
| `diag` | `/v1/diag/*` | **Rien de la machine.** L'analyse porte uniquement sur ce que vous envoyez. |
| `read` | `/v1/status`, `/v1/instances` | Le pseudo du joueur, la liste de ses instances et leur temps de jeu. |
| `control` | `/v1/launch` | Le pouvoir de démarrer une partie. |

### Codes d'erreur

| Code | Signification | Que faire |
|---|---|---|
| `401` | Clé absente, inconnue ou révoquée | Ne pas réessayer en boucle. Demander une nouvelle clé. |
| `403` | Clé valable, **portée manquante** | Demander la portée à l'utilisateur. Réessayer ne servira jamais. |
| `400` | Corps mal formé ou champ requis absent | Le message dit lequel. |
| `404` | Chemin inconnu | — |

La distinction 401/403 est volontaire : dans un cas vous n'êtes pas
authentifié, dans l'autre vous l'êtes mais on ne vous a pas donné ce
droit.

---

## 4. Analyse de crash

```
POST /v1/diag/crash        portée : diag
```

Le corps est **le log brut**, envoyé tel quel (`text/plain`), ou bien
`{"log": "..."}` en JSON. Les deux sont acceptés : imposer un échappement
JSON sur 400 Ko de texte serait une corvée inutile.

Le meilleur résultat s'obtient avec un **rapport de crash**
(`crash-reports/*.txt`) plutôt qu'avec `latest.log` : lui seul contient la
liste des mods chargés, qui permet de nommer le coupable.

### Réponse

| Champ | Type | Description |
|---|---|---|
| `found` | bool | Faux = rien de reconnu. Les autres champs sont alors vides. |
| `cause` | string | Clé stable, voir le tableau ci-dessous. |
| `title` | string | Phrase lisible, en français. |
| `action` | string | Ce que l'utilisateur doit faire. |
| `loader` | string | `Forge`, `NeoForge`, `Fabric`, `Quilt`, ou vide. |
| `mcVersion` | string | Déduite du rapport, ou vide. |
| `exception` | string | Première ligne d'exception, telle quelle. |
| `suspects` | array | Mods mis en cause, **le plus probable en premier**. |
| `summary` | string | Tout ce qui précède, mis en forme, prêt à afficher. |

Chaque suspect : `modId`, `version`, `source` (le .jar), `evidence` (la
ligne de pile qui l'accuse) et `score` (0–100).

### Valeurs de `cause`

| Valeur | Sens |
|---|---|
| `mod_error` | Un mod identifié a levé l'exception. |
| `mod_conflict` | Doublon ou dépendance manquante. |
| `out_of_memory` | Mémoire insuffisante. |
| `heap_too_large` | Plus de RAM demandée que disponible. |
| `java_version` | Mauvaise version de Java. |
| `missing_class` | Classe absente (mod incomplet ou corrompu). |
| `session_expired` | Session Microsoft expirée. |
| `graphics_driver` | Échec graphique avéré. |
| `file_locked` | Fichier bloqué (antivirus). |
| `network` | Coupure réseau pendant le chargement. |
| `unknown` | Rien de reconnu. |

### Ce que l'analyse ne fait pas, et pourquoi

**Elle n'accuse pas un mod quand la cause est l'environnement.** Si le
rapport contient `Caused by: OutOfMemoryError`, la réponse est
`out_of_memory` et `suspects` est **vide** — même si la trace de pile cite
un mod. Cas réel : un `NoClassDefFoundError` sur une classe d'OptiFine
dont la vraie cause, trente lignes plus bas, était un manque de mémoire.
Accuser OptiFine aurait envoyé l'utilisateur désinstaller un mod innocent.

Si votre interface affiche « mod en cause », ne l'affichez donc que
lorsque `suspects` n'est pas vide. `found` seul ne suffit pas.

---

## 5. Analyse de mods

```
POST /v1/diag/mods         portée : diag
Content-Type: application/json
```

```json
{
  "loader": "Fabric",
  "mcVersion": "1.20.1",
  "mods": [
    { "file": "sodium.jar", "manifest": "{\"id\":\"sodium\", …}" },
    { "id": "jei", "version": "15.2.0", "loader": "forge",
      "mcRange": "[1.20.1,1.21)",
      "deps": [{ "id": "cloth-config", "mandatory": true, "range": ">=11" }] }
  ]
}
```

`loader` est **requis** (`Vanilla`, `Fabric`, `Forge`, `NeoForge`,
`Quilt`). `mcVersion` est facultatif : sans lui, la compatibilité de
version n'est pas vérifiée, le reste si.

Chaque entrée de `mods` prend **l'une ou l'autre** forme :

- **`manifest`** : le contenu brut de `fabric.mod.json`, `quilt.mod.json`,
  `META-INF/mods.toml`, `META-INF/neoforge.mods.toml` ou `mcmod.info`. Le
  format est deviné ; `"format"` permet de le forcer (`fabric`, `quilt`,
  `forge`, `neoforge`, `mcmod.info`). **C'est la forme à préférer** : lire
  ces cinq formats est précisément ce que vous venez chercher.
- **champs déjà analysés** : `id`, `version`, `loader`, `mcRange`, `deps`.

`disabled: true` exclut l'entrée de l'analyse (elle est comptée dans
`skipped`).

### Réponse

```json
{
  "errors": 2, "warnings": 1, "infos": 0,
  "analysed": 42, "skipped": 3,
  "suggestions": ["iris"],
  "issues": [
    { "severity": "error", "severityLabel": "Bloquant",
      "file": "jei-forge.jar",
      "title": "Prévu pour Forge",
      "detail": "L'instance utilise Fabric. Ce mod ne se chargera pas…" }
  ]
}
```

`severity` vaut `error`, `warning` ou `info` — **ce sont ces clés qu'il
faut comparer**, pas `severityLabel`, qui est du texte destiné à être lu
et peut changer.

`suggestions` liste les dépendances **facultatives** absentes. Ce ne sont
pas des problèmes : ce sont des mods que les mods installés savent
exploiter. Elles sortent de ce que les mods déclarent, pas d'un catalogue.

### Ce que l'analyse sait, et qu'on ne devine pas

- **Quilt charge les mods Fabric** : ce n'est pas signalé comme un conflit.
- **Forge et NeoForge** sont proches mais ont divergé : avertissement, pas
  blocage.
- `minecraft`, `java`, `fabricloader`, `forge`… sont fournis par la
  plateforme et ne comptent jamais comme dépendances manquantes.
- Les intervalles Maven (`[1.20,1.21)`) et les prédicats semver
  (`>=1.20 <1.21`, `~1.20.1`, `||`) sont évalués correctement, y compris
  le fait que `1.0` est postérieur à `1.0-beta1`.
- **Deux jars déclarant le même `modId`** sont signalés : c'est le conflit
  le plus fréquent, et le plus invisible.

---

## 6. État et contrôle

```
GET  /v1/status       portée : read
GET  /v1/instances    portée : read
POST /v1/launch       portée : control
GET  /v1              toute clé valable — auto-description
```

`GET /v1/status` :

```json
{ "version": "6.0.0", "state": "idle",
  "selectedInstance": "3e66…", "playerName": "Theo" }
```

`state` vaut `idle`, `preparing`, `running` ou `error`. L'instantané est
rafraîchi **une fois par seconde** : interroger plus vite ne donnera rien
de plus.

`GET /v1/instances` rend un tableau de `{id, name, loader, mcVersion,
launches, playSeconds, lastPlayed}`.

`POST /v1/launch` avec `{"id": "…"}` rend `{"queued": true}` — la demande
est **mise en file**, pas exécutée dans la requête. Elle est ignorée si
une partie tourne déjà ou si l'identifiant est inconnu ; dans les deux cas
le launcher l'écrit dans `launcher.log`. Suivez `/v1/status` pour savoir
ce qu'il est advenu.

---

## 7. Événements sortants

**C'est le mode à préférer pour un bot de support.** Au lieu
d'interroger, vous êtes appelé.

L'utilisateur enregistre votre adresse dans **Paramètres → Avancé →
Événements sortants**, avec un secret de votre choix et les types qui vous
intéressent. Aucune clé d'API n'est nécessaire : c'est lui qui vous
désigne.

Votre serveur reçoit un `POST application/json`, avec votre secret dans
l'en-tête **`X-TeamLauncher-Secret`** (jamais dans le corps). Répondez
`2xx` ; tout le reste est compté comme un échec, et **dix échecs
consécutifs désactivent l'abonnement**.

> **HTTPS obligatoire**, sauf vers `127.0.0.1` / `localhost` pour vos
> essais. Le corps contient le nom de l'instance et le secret voyage en
> en-tête : en clair sur le réseau, les deux seraient lisibles.

### `crash`

Le plus utile : le crash arrive **déjà analysé**. Vous n'avez rien à
savoir du format des rapports de crash.

```json
{
  "event": "crash",
  "time": 1790000000,
  "exitCode": 1,
  "instance": { "id": "3e66…", "name": "Survie 1.12",
                "loader": "Forge", "mcVersion": "1.12.2" },
  "diagnosis": {
    "found": true,
    "cause": "mod_error",
    "title": "Le crash vient du mod « webscreen ».",
    "action": "→ Désactive ce mod pour confirmer…",
    "summary": "…",
    "suspects": [ { "modId": "webscreen", "version": "2.0.0",
                    "source": "WebDisplay2-2.0.0.jar", "score": 60 } ]
  }
}
```

L'événement part **même quand `found` est faux** : un crash que nous
n'avons pas su reconnaître est justement celui qu'un humain doit voir.

### `game_start` et `game_stop`

```json
{ "event": "game_start", "time": 1790000000,
  "instance": { "id": "…", "name": "…", "loader": "…", "mcVersion": "…" } }

{ "event": "game_stop", "time": 1790000900, "exitCode": 0,
  "instance": { … }, "playSeconds": 900 }
```

`game_start` n'est émis que lorsque le processus du jeu tourne réellement
— pas au clic sur Jouer, qui peut encore échouer à l'installation.
`game_stop` ne concerne que les fins **normales** : un arrêt anormal part
en `crash`, avec son diagnostic. Jamais les deux pour un même plantage.

### `test`

Le bouton **Tester** envoie `{"event":"test","time":…,"message":"Essai
depuis Team Launcher."}`. Acceptez-le comme les autres.

---

## 8. Limites et garanties

**Ce sur quoi vous pouvez compter :**

- Les clés de `cause` et les valeurs de `severity` sont **stables**. Les
  textes français (`title`, `action`, `detail`, `severityLabel`) ne le
  sont pas : ne les comparez jamais.
- Un champ pourra être **ajouté** dans une réponse ; aucun ne disparaîtra
  en v1. Ignorez ce que vous ne connaissez pas.
- `/v1/diag/*` ne lit aucun fichier de l'utilisateur et n'émet aucune
  requête réseau. Tout vient de votre corps de requête.

**Ce dont il faut tenir compte :**

- L'API est **locale**. Elle n'existe que si Team Launcher tourne, et elle
  n'est joignable que depuis la même machine.
- La file de `/v1/launch` est plafonnée à 8 demandes, celle des événements
  à 64.
- `/v1/status` ne change pas plus d'une fois par seconde.
- Il n'y a **pas de pagination** : la liste des instances est rendue
  entière.

---

## 9. Un bot de support en vingt lignes

```js
// Reçoit les crashs de Team Launcher et les poste dans un salon.
import express from "express";
const app = express();
app.use(express.json({ limit: "2mb" }));

app.post("/hook", (req, res) => {
  if (req.get("X-TeamLauncher-Secret") !== process.env.SECRET)
    return res.sendStatus(401);
  const { event, instance, diagnosis } = req.body;
  res.sendStatus(200);                       // accuser réception d'abord
  if (event !== "crash") return;

  const mod = diagnosis?.suspects?.[0];
  postToDiscord(
    mod
      ? `**${instance.name}** a planté : le mod \`${mod.modId}\` ` +
        `(${mod.source}) est en cause.\n${diagnosis.action}`
      : `**${instance.name}** a planté : ${diagnosis.title || "cause inconnue"}`
  );
});

app.listen(8080);
```

Notez l'ordre : on répond **avant** de traiter. Un destinataire lent est
compté comme en échec, et dix échecs d'affilée coupent l'abonnement.

---

*Team Launcher v6 — API v1. Les remarques et demandes de clé passent par
le formulaire.*
