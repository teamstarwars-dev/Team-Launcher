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

### Phase 7 — Démarrage, aide, mises à jour
Fermer vs réduire, lancement au démarrage du système, jeu affiché au lancement, menu d'aide (centre d'aide, Discord, ticket, export zip des journaux, suggestion, « Quoi de neuf »), canal stable/bêta, notes de version après mise à jour, mode diagnostic complet.

### Phase 8 — Architecture et performance
Système de plugins internes, API HTTP locale, préchargement de la JVM pendant la sélection d'instance.

### Chantier séparé — Social (phase 1 texte)
Amis par pseudo, messages privés texte + images, petits groupes, bloquer/signaler, bouton « appel vocal » désactivé avec mention « à venir ». **Nécessite un serveur backend** (comptes, amis, messages) : hébergement, modération, conservation des données et coûts sont à décider avant la première ligne de code. À planifier à part, comme tu l'as indiqué.

---

## Points à trancher

1. **Stockage du jeton sous Linux** : DPAPI n'a pas d'équivalent. Recommandation : libsecret quand il est disponible, repli sur un fichier en 0600 avec avertissement visible.
2. **Social** : le backend engage de la modération et de la conservation de données personnelles (messages privés, images). À cadrer avant de commencer.
3. **Notifications natives** : sous Windows elles passent par une identité d'application enregistrée (AppUserModelID), ce que l'installeur peut poser ; sous Linux par D-Bus.
