# Team Launcher v6.0.0

**Réécriture complète en C++. Plus de runtime .NET à installer, et le
launcher passe de 46 Mo à 6,4 Mo.**

C'est la première version du portage : tout a été réécrit, du lancement
du jeu à l'interface, et Linux est désormais une plateforme de plein
droit et non un projet annexe.

---

## En chiffres

| | v5 (C#) | v6 (C++) |
|---|---|---|
| Installeur | 46 Mo | **6,4 Mo** |
| Installé | — | **17 Mo** |
| Runtime .NET | requis | **aucun** |
| Linux | non | **oui** (.deb et script) |

Mémoire au repos : environ 95 Mo, mesurés fenêtre ouverte.

---

## Ce que ce launcher fait et que les autres ne font pas

**Il vous dit quel mod a planté le jeu.** Collez-lui un rapport de
crash : il croise la liste des mods qu'il contient avec les paquets Java
cités dans la trace, et nomme le coupable — avec son fichier `.jar`.
Quand la cause est ailleurs (mémoire, pilote, session expirée), il le dit
et n'accuse personne à tort.

**Il détecte les conflits de mods avant que vous lanciez.** Mod prévu
pour un autre chargeur, version de Minecraft incompatible, dépendance
manquante, même mod présent en double : tout est signalé avant le
démarrage, pas après un plantage. Les cinq formats de manifeste sont lus
(Fabric, Quilt, Forge, NeoForge, et l'ancien `mcmod.info`).

**Il modifie vos mondes.** Éditeur de carte avec lecture et écriture
directes des régions, remplissage et remplacement de zones, copier-coller
de structures, annulation. Les palettes de blocs des trois époques sont
gérées (1.18+, 1.13–1.17, et ≤ 1.12).

**Il génère une ville depuis OpenStreetMap.** Vous donnez des
coordonnées, il bâtit le quartier dans votre monde.

**Il intègre vos amis.** Liste d'amis, messages privés et appels vocaux
1:1, à travers Discord — donc sans serveur à héberger ni compte de plus.

**Il compte votre temps de jeu même fermé.** Vous pouvez lui dire de se
fermer quand la partie démarre, pour qu'il ne consomme rien pendant que
vous jouez : au démarrage suivant, il retrouve la session et crédite le
temps joué, estimé d'après les fichiers que le jeu a écrits. Le même
rattrapage s'applique si le launcher a planté ou si le PC s'est éteint
en cours de partie.

**Il vient avec de quoi développer des mods.** Squelette de projet
Fabric, Forge ou NeoForge, et le launcher télécharge lui-même Gradle et
le bon JDK : rien à installer à la main.

---

## Tout le reste

**Instances** — création, import et export en un clic, duplication,
étiquettes, favoris, profils de lancement (mémoire, arguments JVM, jeu de
mods actifs), partage par code court.

**Mods et contenu** — recherche Modrinth et CurseForge, installation en
un clic, mises à jour avec le changelog de l'auteur affiché **avant** le
bouton, comparateur entre deux instances, suggestions tirées des
dépendances facultatives déclarées.

**Modpacks** — import CurseForge et Modrinth (`.zip`, `.mrpack`), par
fichier, par URL ou par glisser-déposer.

**Mondes** — explorateur de fichiers, sauvegardes automatiques avec
rotation et quota, restauration en un clic avec la liste des mondes
contenus, synchronisation depuis une installation CurseForge existante.

**Comptes** — Microsoft avec renouvellement silencieux, plusieurs comptes
et bascule rapide, mode hors-ligne.

**Serveurs** — liste, favoris, ping, hébergement Pterodactyl.

**Skins** — bibliothèque, aperçu, application.

**Bedrock** — détection, lancement et réglages de l'édition Bedrock
installée (Windows).

**Modèles 3D** — visionneuse `.bbmodel`, `.json` et `.geo.json`.

**Téléchargements** — file persistante qui reprend après un
redémarrage, annulation, reprise, cache disque des métadonnées, mode
hors-ligne.

**Confort** — thèmes clair et sombre, barre de titre accordée aux
couleurs du système, mode daltonisme, échelle de l'interface, recherche
globale (Ctrl+K), notifications, journal.

**Vitesse de lancement** — la détection de Java coûtait près d'une
seconde, et elle était refaite à chaque partie. Elle est désormais mise
en cache : 911 ms au premier lancement, 2 µs ensuite, mesurés. Pendant
que vous choisissez votre instance, le launcher prépare en silence les
fichiers que le jeu va lire.

**Démarrage et fermeture** — lancement automatique avec le système,
ouverture directe sur une instance choisie, et trois comportements au
démarrage de la partie : ne rien faire, réduire le launcher, ou le
fermer.

**Mises à jour** — vérification automatique du canal stable ou
préliminaire, à la fréquence de votre choix, avec les notes de version
affichées avant l'installation. Le flux de mise à jour est fonctionnel
dès cette version — il ne l'était pas en v5.

**Aide** — centre d'aide intégré : ouverture d'un ticket, envoi d'une
suggestion, accès au salon Discord, diagnostic système, export des
journaux pour un signalement, et un « Quoi de neuf » affiché après
chaque mise à jour.

**Pour les outils tiers** — API HTTP locale protégée par clés à portées
(`diag`, `read`, `control`), événements sortants qui poussent un crash
**déjà analysé** vers votre bot, et plugins déclaratifs exécutés comme
des processus séparés.

**Service de diagnostic hébergé** — `tl_diagd`, un service autonome de
500 Ko qui expose la même analyse de mods et de crash que le launcher,
pour les serveurs et les communautés qui veulent l'offrir à leurs
joueurs. Les clés d'accès sont délivrées sur demande, par formulaire.
Les rapports qu'on lui envoie ne sont jamais écrits sur disque.

---

## Installation

**Windows** — `TeamLauncher-6.0.0-Setup.exe`.
**Linux** — `teamlauncher_6.0.0_amd64.deb` (Debian, Ubuntu et dérivées),
ou le script `install.sh` pour une installation dans votre dossier
personnel.

Les deux installeurs embarquent tout le nécessaire, SDK Discord compris :
aucun composant à télécharger séparément, et aucun runtime à installer.

Les binaires **ne sont pas signés** : Windows SmartScreen affichera un
avertissement au premier lancement. Vérifiez donc ce que vous avez
téléchargé — les empreintes SHA-256 sont ci-dessous et dans
`SHA256SUMS.txt`, à côté des fichiers.

| Fichier | Taille | SHA-256 |
|---|---|---|
| `TeamLauncher-6.0.0-Setup.exe` | 6,4 Mo | `4017be143982c3c3fadd20345eb96d95ec84e6738b3be5a2460b7b909eb85063` |
| `teamlauncher_6.0.0_amd64.deb` | 5,8 Mo | `46ab302f68754cbf0a10f6af57fcdb6b5cdea4d9390f39f13a61a73717476a88` |

---

## Limites connues, dites franchement

- **Pas de signature de code.** SmartScreen avertira. C'est un choix
  assumé : un certificat coûte cher pour un projet gratuit.
- **Bedrock : lancement et réglages seulement.** Le launcher ne gère ni
  l'installation ni les mods Bedrock.
- **Le vocal et les amis passent par Discord**, qui doit être installé et
  connecté. Les messages privés sont par ailleurs plafonnés par Discord à
  100 envois par tranche de deux heures, pour l'ensemble de
  l'application.
- **CurseForge peut être indisponible** si la clé API embarquée est
  révoquée. Modrinth reste alors pleinement accessible, et vous pouvez
  coller votre propre clé dans les paramètres.
- **macOS n'est pas pris en charge.**

---

## Pour qui vient de la v5

Vos instances, vos comptes et vos réglages sont conservés : le fichier de
configuration est partagé avec la v5. Rien à réimporter.

Deux changements de comportement à connaître :

- **Le bouton de fermeture quitte maintenant le launcher** par défaut,
  au lieu de le réduire. Le comportement précédent reste disponible dans
  *Paramètres → Général → Démarrage et fermeture*.
- **Le numéro de version a disparu** de la barre de titre et de la barre
  latérale. Il reste visible dans la page Aide.
