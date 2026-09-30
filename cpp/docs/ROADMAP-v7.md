# Team Launcher v7 — feuille de route

Liste fournie par l'utilisateur le 29/09/2026, à livrer **sur Windows et Linux**.

## Ordre de travail : Linux d'abord

Le portage Linux est en cours et inachevé. Écrire les ~60 fonctionnalités maintenant, puis les porter, reviendrait à faire deux fois le travail : chaque page d'interface, chaque appel système et chaque test seraient à reprendre. **On termine donc la cible Linux avant d'ouvrir la phase 1**, ensuite chaque fonctionnalité est écrite une fois pour les deux plateformes.

---

## Déjà présent — à NE PAS redévelopper

Vérifié dans le code le 29/09/2026 :

| Demandé | État réel |
|---|---|
| Export / import d'instance en un clic | **Fait.** `export_zip` / `import_zip`, dans le menu contextuel d'instance et le menu « Importer (.zip) ». |
| Sauvegardes avec rotation | **Fait pour la rotation** (10 plus récentes) et la restauration, dans `backup.hpp`. Manquent : la **programmation** toutes les X heures, le **N configurable**, et l'aperçu (date, taille, capture). |
| Mode portable | **Fait dans le moteur** (`DataStore::isPortable`, données dans `<exe>/data`), activé par un drapeau au lancement. Manque : l'exposer dans les paramètres. |
| Rapport de crash | `crash_analyzer` existe. Manquent : le rapport **simplifié** et l'envoi/export en un clic. |
| Notifications | `notify_toast` existe, mais ce sont des bulles **internes** à la fenêtre. Manquent : les notifications **système natives**, visibles launcher minimisé. |
| Vérification d'intégrité | Les téléchargements sont déjà vérifiés en SHA-1 à l'installation. Manquent : la vérification **à la demande** d'une instance existante et la **réparation**. |

### Détection automatique de la RAM idéale

Tu demandais de vérifier avant de redévelopper. Résultat : `game_launcher.hpp` **déclare** `ideal_ram_gb(long long totalMb)` et `ideal_ram_gb()`, avec la règle déjà écrite — *moitié de la RAM totale, bornée à [2, 8] Go, 4 Go si indéterminée*. Mais **aucune de ces deux fonctions n'est définie, et aucune n'est appelée** : c'est une déclaration morte.

Donc : à développer, mais en appliquant la règle déjà spécifiée plutôt qu'en en inventant une autre. `total_ram_mb()` et `available_ram_mb()`, eux, existent bel et bien.

---

## Phases

Découpage par dépendance et par coût. Chaque phase se termine par un build vert sur les deux plateformes et ses tests.

### Phase 0 — Terminer Linux
Reprise du chantier en cours : `http` (libcurl), stockage du jeton (pas d'équivalent DPAPI), lancement de processus, raccourcis `.desktop`, IPC Discord par socket Unix, chemins XDG. Puis l'installeur Linux (`.deb` + AppImage), dont le squelette existe déjà dans `installer/linux/`.

### Phase 1 — Réglages et socle
- `ideal_ram_gb` : implémenter et brancher dans l'interface
- Niveau de journal configurable (Traçage → Éteint) + rotation et compression gzip des anciens logs
- Curseurs : tâches d'analyse de mods (1–12), téléchargements simultanés (1–20), espace de sauvegarde alloué
- Fréquence de vérification des mises à jour
- Chemin du contenu configurable (`instancesDir` existe, à exposer proprement)
- Format de date
- Mode portable exposé dans les paramètres

### Phase 2 — Thèmes et accessibilité
Variantes classique / clair, mode daltonisme à contraste adapté, échelle de l'interface, police plus lisible (quelques options à comparer), raccourcis clavier personnalisables.

### Phase 3 — Instances et navigation
Favoris épinglés en tête, tags et catégories, menu contextuel enrichi (lancer en vanilla, ouvrir le dossier mods, réparer, paramètres du jeu), glisser-déposer de `.zip`/`.mrpack` sur la fenêtre, recherche globale (Ctrl+K), refonte de la barre latérale en icônes avec infobulles.

### Phase 4 — Téléchargements et fiabilité
Page dédiée à deux onglets (En cours / Terminés) avec compteurs, tout sélectionner, recherche et tri, état vide soigné. File d'attente **persistante** reprenant après un crash. Cache disque des métadonnées CurseForge/Modrinth. Vérification d'intégrité avec réparation. Mode hors-ligne renforcé. Chargement paresseux des vignettes.

### Phase 5 — Mods et découverte
Détection de conflits avant lancement (versions incompatibles, dépendances manquantes), suggestions de mods complémentaires, changelog rapide sur mise à jour d'un mod, flux de recommandations, comparateur de modpacks.

### Phase 6 — Sauvegardes, comptes, profils
Sauvegarde programmée toutes les X heures avec rotation sur N, restauration en un clic avec aperçu, presets de lancement, gestion de plusieurs comptes Microsoft avec bascule rapide.

### Phase 7 — Démarrage, aide, mises à jour — **FAITE le 30/09/2026**
Fermer vs réduire, lancement au démarrage du système, jeu affiché au lancement, menu d'aide (centre d'aide, Discord, ticket, export zip des journaux, suggestion, « Quoi de neuf »), canal stable/bêta, notes de version après mise à jour, mode diagnostic complet.

Livré : `startup.cpp` (entrée de session, `--autostart`), `support.cpp` (rapport machine + export zip expurgé), `ui_help.cpp` (page 17), canal bêta dans `maintenance.cpp`, vérification périodique, format de date, et `syscolor.cpp` — barre de titre accordée au système, demandée le même jour. Détail et pièges dans `ETAPE1-estimations.md`.

Non livré, et pourquoi : l'icône de **zone de notification**. SDL2 n'en propose pas (SDL3 oui) ; le mode « réduire » laisse donc la fenêtre dans la barre des tâches, et la sortie explicite vit dans la page Aide.

### Phase 8 — Architecture et performance
Système de plugins internes, API HTTP locale, préchargement de la JVM pendant la sélection d'instance.

### Chantier séparé — Social

Périmètre : amis par pseudo, messages privés texte + images, petits groupes de discussion, bloquer/signaler, et **appel vocal**.

**Décision du 29/09/2026 : le vocal s'appuiera sur la technologie de Discord**, pas sur une pile maison.

C'est le bon choix, et de loin. Un vocal correct demande bien plus que « transporter du son » : codec Opus, annulation d'écho, suppression de bruit, contrôle de gain, gigue et perte de paquets, traversée de NAT avec serveurs TURN de repli, et un relais média à héberger. C'est un métier à soi seul, et un poste de coût permanent. Discord a déjà tout cela, et les joueurs y sont.

#### TRANCHÉ le 30/09/2026 : tout chez Discord

Amis, messages **et** vocal passent par Discord. Aucun serveur à héberger, aucune donnée personnelle à conserver, aucune modération à assurer nous-mêmes — et une seule identité, donc aucune des incohérences décrites plus bas.

**État au 30/09/2026.** La couche `social.hpp` et la page 16 sont écrites, complètes et **indépendantes du fournisseur** : liste d'amis, conversation, ajout par pseudo, blocage, signalement, et bouton d'appel vocal désactivé avec la mention « à venir ».

**Le SDK est intégré** (version 1.10.19337, 1er septembre 2026), lié sur les deux plateformes, et le client rapporte son véritable état de connexion. Reste à faire : la liaison de compte (flux par code d'appareil, `Client::GetTokenFromDevice`), puis la liste d'amis réelle, puis la messagerie.

L'intégration est **optionnelle à la compilation** : sans le dossier `third_party/discord_social_sdk`, tout compile et la page explique ce qui manque. Indispensable, puisque l'archive ne peut pas être récupérée automatiquement.

### Coût en taille — plafond relevé à 20 Mo

Décision du 30/09/2026. La bibliothèque du SDK pèse **9,4 Mo sous Windows** et **13,7 Mo sous Linux**, ce qui porte la livraison de 6 à environ **15 Mo côté Windows et 17 Mo côté Linux**. Le plafond initial de 5 Mo est donc dépassé d'un facteur trois.

L'alternative examinée — charger la bibliothèque dynamiquement et ne la télécharger qu'au premier usage du social — était techniquement possible : le SDK expose bien une API C sous son en-tête C++. Mais Discord ne fournit aucun mécanisme pour cela, il aurait fallu écrire à la main la résolution des fonctions utilisées. Écartée au profit de la simplicité.

Repère à garder en tête : la version C# pesait **46 Mo**. Même avec le SDK, on reste trois fois plus léger.

### Plafond de débit des communications

Contrainte découverte le 30/09/2026 dans la documentation, et qui conditionne toute mise en service : les messages directs sont limités à **100 envois par tranche de 2 heures, par application et non par utilisateur**. Idem pour les opérations de salon.

Lever ce plafond demande de **candidater** (bouton *Comms Access*), avec des prérequis : avoir déjà intégré la liaison de compte, la Rich Presence, les invitations de jeu **et** la liste d'amis unifiée ; une intégration complète de bout en bout, cas d'erreur compris ; des justificatifs dont une capture vidéo ; et des mesures de protection des mineurs. Discord peut refuser.

Conséquence sur l'ordre des travaux : la **liste d'amis et les invitations ne sont pas plafonnées**. On livre donc d'abord un social utile sans dépendre de l'approbation, et la messagerie vient ensuite.

Il manque le **SDK social de Discord** : une bibliothèque native à récupérer sur le portail développeur après acceptation de leurs conditions, puis à lier au launcher. À savoir — les amis et les messages privés ne passent **pas** par le canal IPC local déjà utilisé pour la Rich Presence, qui ne sait que déclarer une activité.

Tant que le SDK n'est pas là, la page affiche ce qui manque et **aucune donnée fictive** : une fausse liste d'amis ou un faux fil de discussion laisseraient croire que les messages partent. Brancher le SDK revient à implémenter les fonctions de `social.cpp` et à définir `TL_HAS_DISCORD_SOCIAL` ; l'interface, le modèle et les règles ne sont pas à refaire.

#### Pourquoi les autres options ont été écartées

Si le vocal vient de Discord mais que les amis et les messages viennent d'un backend maison, **il y a deux systèmes d'identité en parallèle**, et ils ne se recouvrent pas :

- Un ami ajouté par pseudo dans le launcher n'est pas forcément joignable sur Discord.
- Le bouton « appeler » serait donc actif ou grisé selon un lien Discord que l'utilisateur n'a pas fait, sans qu'il comprenne pourquoi.
- Bloquer quelqu'un côté launcher ne le bloque pas côté vocal.

Trois sorties possibles, à choisir explicitement :

1. **Tout chez Discord.** Le SDK social de Discord couvre aussi les amis, les messages privés et les salons — donc potentiellement l'essentiel du périmètre, sans backend du tout. Une seule identité, aucune donnée personnelle à héberger, aucune modération à assurer nous-mêmes. En contrepartie : dépendance totale à Discord, obligation que l'utilisateur ait un compte, et soumission à leurs conditions et à leurs évolutions d'API. **C'est l'option que je recommande de regarder en premier**, parce qu'elle supprime le chantier backend entier — le plus coûteux et le plus risqué de la liste.
2. **Texte maison, vocal Discord.** Il faut alors assumer le lien de comptes : chaque utilisateur associe son compte Discord, et le bouton d'appel n'apparaît qu'entre deux comptes liés. Le message doit être explicite, pas un bouton grisé sans explication.
3. **Lien simple, sans SDK.** Le launcher se contente d'ouvrir un salon vocal Discord existant (`discord://` ou lien d'invitation). Presque rien à développer, mais ce n'est pas un « appel » entre deux personnes : c'est rejoindre un salon commun.

#### Conséquences à vérifier avant de s'engager

- **Conditions d'utilisation de Discord** : ce que leur SDK autorise dans une application tierce, et ce qu'il impose en affichage et en attribution.
- **Utilisateur sans Discord** : que voit-il ? Le vocal doit se dégrader proprement, pas planter ni afficher un bouton mort.
- **Linux** : le vocal doit fonctionner sur les deux plateformes, ou l'absence doit être annoncée clairement.

#### Ce qui reste à notre charge dans tous les cas

Si l'option 2 ou 3 est retenue, le backend reste nécessaire pour les comptes, les amis et les messages — avec ce que cela implique : hébergement, **modération**, conservation de données personnelles (messages privés et images), et coûts récurrents. À cadrer avant de commencer.

---

## Points à trancher

1. **Stockage du jeton sous Linux** : DPAPI n'a pas d'équivalent. Recommandation : libsecret quand il est disponible, repli sur un fichier en 0600 avec avertissement visible.
2. **Social** : le vocal passera par Discord (décidé le 29/09/2026). Reste à trancher **jusqu'où** : si on prend aussi les amis et les messages chez Discord, le backend maison disparaît entièrement — donc plus de modération ni de données personnelles à héberger. Sinon il faut assumer deux systèmes d'identité et le lien de comptes. Voir la section Social.
3. **Notifications natives** : sous Windows elles passent par une identité d'application enregistrée (AppUserModelID), ce que l'installeur peut poser ; sous Linux par D-Bus.
