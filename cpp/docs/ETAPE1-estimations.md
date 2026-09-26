# Team Launcher C++ — estimations Étape 1

Réécriture C#/Avalonia (~46 Mo AOT) → C++ (cible 1,5–3 Mo, plafond 5 Mo).  
Version produit : **6.0** (suite de la v5.0 Avalonia).

## Taille binaire — par composant (Release, -Os, gc-sections, LTO)

| Composant | Mode lien | Taille estimée | Notes |
|---|---|---|---|
| Exécutable (cible + ImGui + app) | — | **0,8 – 1,5 Mo** | -Os, strip, gc-sections, LTO |
| Dear ImGui | static (dans exe) | +0,3 – 0,5 Mo | ~150–250 Ko utiles après gc |
| SDL2 | **dynamic** (SDL2.dll) | **~0,4 – 0,8 Mo** (dll) | jamais statique |
| OpenGL loader (backend ImGui) | static minimal | +0,05 – 0,1 Mo | gl3w/glew minimal ou custom |
| TLS/SSL (schannel Windows / OpenSSL Linux) | dynamic syst. / openssl | ~0 (schannel) / +0,3 Mo lib | Windows : schannel = 0 disque add. |
| cpp-httplib | header-only | +0,05 – 0,1 Mo code | sans libcurl |
| nlohmann/json | header-only | +0,1 – 0,2 Mo code | si trop lourd → simdjson plus tard |
| mimalloc / jemalloc | static optionnel | +0,1 – 0,2 Mo | évaluer gain vs taille |
| **Total disque install** | | **≈ 1,5 – 3,0 Mo** | hors UPX |
| Avec UPX (si AV OK) | | ≈ 0,8 – 1,5 Mo | tester antivirus avant livraison |

**Plafond 5 Mo** : marge ~2 Mo pour features + durcissement (strip, chiffrement chaînes).

### Hors cible (référence C# actuelle)
| | C# AOT | C++ cible |
|---|---|---|
| Exe | ~46 Mo (AOT) / ~150 Mo self-cont. | **1,5–3 Mo** |
| Runtime | .NET / CLR | aucun |

## RAM launcher — par niveau

### Niveau de base (dès le squelette)
| Technique | RAM attendue | Risque |
|---|---|---|
| Fenêtre seule + ImGui (démo) | **~15–30 Mo** | — |
| Lazy load pages (onglets) | −5 à −15 Mo vs tout charger | faible |
| Arena / pool pour sessions UI | −2 à −5 Mo (moins de fragmentation) | faible |
| mmap jars/assets au lieu de ReadAllBytes | −50 à −200 Mo pendant install | faible (fichiers stables) |
| Pas de CLR / GC .NET | −40 à −80 Mo vs v5 | — |

**Cible idle : ~20–40 Mo** (vs ~100–200+ Mo Avalonia observé souvent).

### Niveau avancé (au cas par cas, mesurer avant/après)
| Technique | Gain potentiel | Risque / complexité |
|---|---|---|
| `madvise(MADV_DONTNEED)` gros assets | −10 à −50 Mo | faible (Linux) |
| `SetProcessWorkingSetSize` en arrière-plan | −20 à −80 Mo | risque thrashing si trop agressif |
| SoA listes mods/serveurs | −5 à −15 Mo (cache) | refactor médium |
| String interning (noms mods/versions) | −2 à −10 Mo | **use-after-free** si lifetime mal gérée → validation stricte |
| Bitpacking métadonnées | −1 à −3 Mo | complexité, peu justifié au début |
| Unload écran quitté | −10 à −30 Mo | simple si ownership claire |
| zstd en mémoire données froides | variable | complexité + CPU |

**Ne pas appliquer aveuglément** : string interning + bitpacking = bugs mémoire possibles → mesurer d'abord.

## RAM / perfs du jeu lancé (Java)

| Mesure | Effet | Portée launcher |
|---|---|---|
| Flags type Aikar's (G1GC) | −latence GC, +stabilité | cmdline `-XX:...` |
| Détection RAM dispo → `-Xmx` | évite OOM / swap | `GlobalMemoryStatus` / `/proc/meminfo` |
| JRE embarqué/recommandé (Temurin/Zulu) | pas de « broken Java » système | download JRE dédié |
| Bedrock | lancement URI propre uniquement | pas de tuning mémoire |

UI doit afficher : **la RAM du jeu dépend du moteur Minecraft (Mojang), pas du launcher.**

## Métriques obligatoires à chaque dépendance
- Taille binaire avant/après (exé + dll)
- RAM idle et en usage (ex. `taskmgr` / `massif`) avant/après

## État squelette
- [x] Version figée : **6.0.0**
- [x] Toolchain : clang 20.1.8 + CMake 4.4.3 + Ninja (LLVM + MSVC headers/WinSDK)
- [x] third_party : imgui 1.91.8, SDL 2.30.9 (shared), cpp-httplib 0.18.3, nlohmann/json 3.11.3
- [x] Premier build Release + mesure taille/RAM — **fait le 24/09/2026**
- [x] LTO réel activé : clang-cl ignore `/GL`+`/LTCG` → `/clang:-flto=thin` (bitcode, lld-link auto)
- [x] Fix build-release.bat : ninja renvoie `-1` → `if errorlevel 1` faux sur négatif → test `if not "%NINJA_RC%"=="0"`
- [x] **Portage DataStore** (module 1/4) — 24/09/2026 :
  - `src/datastore.hpp/.cpp` : AppSettings complet (modèles non portés = JSON brut conservé), `default.env` en fichier externe (`assets/default.env`), merge PascalCase compat System.Text.Json, écriture atomique tmp+`MoveFileExW` (6×150 ms + fallback), debounce 500 ms (thread), `--portable`, `TL_DATA_DIR` (test)
  - Tests : `tests/test_datastore.cpp` → `TLTestDataStore.exe` — **ALL TESTS PASSED** (1er load, save/reload, debounce, config corrompue)
  - Bugs trouvés/corrigés : flux tmp encore ouvert pendant `MoveFileExW` (err 32) → fermer avant move ; comportement fidèle C# sur config corrompue (ApplyDefaults sauté)
  - Taille : `tl_datastore.lib` 1 403 Ko (bitcode LTO), test exe **239 Ko** ; **TeamLauncher.exe inchangé 442,5 Ko** (datastore non encore linké au squelette — branchement dès l'UI Settings, pour ne pas réécrire le config.json utilisateur prématurément)
- [x] **Portage métier module 2/4** (GameInstaller/GameLauncher C#) — 24/09/2026 :
  - `src/game_launcher.hpp/.cpp` : `offline_session` (MD5 OfflinePlayer + endianness Guid .NET, réf. UUID test = `98dd2756bee621bcf8a8e92344183641`), `find_java`/`detect_java_major` (cache), `download_java` (Adoptium), RAM (`GlobalMemoryStatusEx`), `build_jvm_args`/`build_game_args` (moderne + legacy + joinServer), `start_game`/`wait_game`/log writer (pipes), `latest_release`
  - `src/game_installer.hpp/.cpp` : `install()` complet (json version, client jar, libs x8 + natives, assets x16 + 2e passe, marker `.tl-verified`), branches NeoForge/Forge/Fabric/Vanilla, `ensure_*_installed` (installeurs officiels via `run_jar_installer` + `forge-install.log`), `maven_name_to_path`/`rules_allow`/`extract_jvm_args`
  - `src/http_win.*` : WinHTTP (HTTPS + redirects schannel natifs, remplace cpp-httplib/OpenSSL), retries, fichier partiel supprimé, debug `TL_HTTP_DEBUG=1`
  - `src/util_hash.*` : SHA1/MD5 BCrypt ; `src/util_zip.*` : miniz (cloné `third_party/miniz` + `miniz_export.h` synthétique) avec garde-fou zip-slip ; `src/util_parallel.hpp` : pool x8/x16 avec ré-throw 1re exception
  - CMake : lib statique **`tl_core`** (6 src métier + 4 miniz), liens `winhttp`+`bcrypt`, remplace `tl_datastore` ; tests cibles `TLTestDataStore` + `TLTestGamePort`
  - Tests : **ALL TESTS PASSED** (offline : maven/rules/jvm+game args/UUID/SHA1/parallel/zip-slip ; réseau `TL_TEST_NET=1` : manifeste Mojang, redirect Adoptium, `latest_release`, échec propre `install`)
  - Bugs trouvés/corrigés : `BCryptOpenAlgorithmProvider` 5 args (max 4) ; `miniz_export.h` absent (header généré CMake) ; WinHTTP `MultiByteToWideChar` off-by-one (buffer `n-1` → conversion en échec silencieux = URL vide) ; handle `WinHttpConnect` détruit avant usage de la requête ; `WinHttpAddRequestHeaders("*/*")` = ERROR_INVALID_PARAMETER 87 (attend `"Name: value"`) → tableau `lppszAcceptTypes` ; null-deref `*cancel` dans `download_file` sans cancel ; `gzip` non requis
  - Fidélité C# vérifiée : `jar` = **toujours le jar vanilla** (même Forge) ; clé `isForge` **présente** (false) pour NeoForge/Fabric, **absente** pour vanilla — GameLauncher C# lit la présence (`TryGetProperty(..., out _)`) → args Forge appliqués NeoForge/Fabric aussi
  - Taille : `tl_core.lib` **3 112 Ko** (bitcode LTO, non livré), `TLTestGamePort.exe` **509,5 Ko**, `TLTestDataStore.exe` 239 Ko ; **TeamLauncher.exe toujours 442,5 Ko** (aucun symbole tl_core référencé par le squelette UI → objets drop au link ; la croissance arrive au branchement UI)
- [ ] Reste module 2 : brancher install/launch à l'UI — **FAIT (module 3)**
- [x] **Module 3 : UI ImGui + launch_flow** — 24/09/2026 :
  - `src/launch_flow.*` : portage de `GameLauncher.PlayCore` (version latest si vide, session offline, `install()`, find/download Java, marge RAM C#, args ordre JVM→mainClass→jeu, `start_game`, en-tête + pump `game-log.txt`) ; reste : auth MS, backup, telemétrie, crash-analyzer
  - `src/ui.*` : palette v5 (#0e0e13/#15151b/#22222a/#f2f2f5/#8a8a99/#3b82f6), sidebar (Jouer/Paramètres), page Jouer (version/loader/RAM, bouton accent états Jouer→Lancement...→En jeu..., progression+Annuler, journal auto-scroll), page Paramètres (nom joueur, Java+parcourir via GetOpenFileNameW, RAM, FPS, minimiser, ouvrir dossiers via ShellExecuteW) ; machine d'états Idle/Preparing/GameRunning/Error ; `DataStore::load/save/shutdown` branchés ; console masquée (`TL_CONSOLE=1` pour la garder)
  - Hooks de test : `TL_AUTO_PLAY=1`, `TL_AUTO_VERSION=<id>`, `TL_HTTP_DEBUG=1`
  - **Validation e2e réelle** : lancement auto 1.12.2 → install vérifiée en 1 027 ms (cache C#), jeu démarré (PID Java, textures/sound/narrator loggés dans `game-log.txt`), fermeture détectée → « Jeu fermé (code -1) » (code **signé** comme C# `ExitCode`) ; `minimizeOnLaunch` respecté ; joueur réel (`TeamUN6713`) lu du config partagé ; accents FR OK dans ImGui (police par défaut couvre Latin-1)
  - Bugs trouvés/corrigés : `ImGuiWindowFlags_NoScrollOnMouseWheel` inexistant → `NoScrollWithMouse` ; `AddText` veut `ImU32` ; deadlock `push_log` sous mutex ; code de sortie non signé (4294967295 au lieu de -1)
- [x] Mesures après branchement UI — 24/09/2026 :
  | Composant | Avant UI | Après UI | Écart |
  |---|---|---|---|
  | TeamLauncher.exe | 442,5 Ko | **1 031 Ko** | +588 Ko (tl_core complet + launch_flow + ui + nlohmann sortis du .lib) |
  | SDL2.dll | 1 637 Ko | 1 637 Ko | — |
  | **Total disque** | 2,03 Mo | **2,61 Mo** | ✅ cible 1,5–3 Mo, plafond 5 Mo |
  | RAM idle | 65 Mo | **66 Mo** | ~stable |
- [x] **Décision 24/09/2026 : portage COMPLET** de l'app C# (68 fichiers, 723 Ko, 15 pages + ~40 services). Périmètre = **toutes les pages et fonctionnalités**.
- [x] **3bis — Cœur launcher** — **fait le 24/09/2026** :
  - Page Instances : toolbar (`＋ Créer` modal, `Importer` popup 3 entrées : `.zip` / dossier / presse-papiers désactivé), filtre élargi Name/Loader/McVersion, tri nom (`_stricmp`) + temps de jeu, cartes fidèles C# (petit 200×220 banner 90, grand 280×300 banner 120, gap 14), compteurs `fTiny` 10 px, bouton Jouer pleine largeur, description 2 lignes (carte grand), états vides fidèles C#, menu contextuel carte (AttachContextMenu : Détails/Mods/Monds/Journaux/Screenshots/Partager désactivés, Modifier/Ouvrir/Exporter .zip/Dupliquer/Supprimer actifs)
  - Page Accueil : greet, stats (instances/temps/lancements), hero dernière instance, récents, « Tout gérer > » zone cliquable, état vide
  - Stats `Launches`/`PlaySeconds`/`LastPlayed` mises à jour au lancement (instance test exclue) ; page Jouer branchée sur l'instance sélectionnée (gameDir=id, RAM/JvmArgs par instance)
  - Helpers UI : import `.zip` (GetOpenFileNameW + `zip_extract_all`), import dossier (SHBrowseForFolderW + copie récursive), export `.zip` (`zip_create_from_dir`, writer miniz), dupliquer, `make_instance`
  - Hooks : `TL_AUTO_PAGE=<0..3>`, `TL_AUTO_MODAL=<1|2|3>`, `TL_AUTO_CTX=1`
  - **Bug critique corrigé** : gel à la fermeture en pleine partie — `close_game()` fermait `outRead` pendant que le pump (thread détaché) attendait `ReadFile` → `CloseHandle` bloqué tant que le jeu vit → fix : pipes laissés ouverts (libérés à la fin du process, EOF naturel à la sortie du jeu) ; diagnostic via `TL_DEBUG_SHUTDOWN=1` (traces stderr de la chaîne de shutdown)
  - Nettoyage `tl_auto` au démarrage (dossier jetable d'un test survivant à une fermeture dure — stop brutal, `ui::shutdown()` jamais exécuté)
  - Tests : `TLTestDataStore` + `TLTestGamePort` ALL PASSED (zip roundtrip, section 14) ; e2e complet : lancement 1.12.2 → fermeture pendant la partie → shutdown complet (`SH: complete`), `config.json` SHA256 inchangé, `tl_auto` supprimé, fermeture propre sans jeu en cours
  - Captures : `build/final-instances.png`, `final-home.png`, `final-ctx.png`
  - **Écarts connus** : glyphes `• ▽ ✎ ▶` hors de `GetGlyphRangesDefault()` (Latin-1) → `·`, « Editer », tri sans préfixe ; wrap des compteurs sur 2 lignes (ProggyClean large) ; toolbar coupée en fenêtre étroite (éléments de fin hors écran) ; import presse-papiers, Partager (module 4) et fenêtre Détails reportés
  - Mesures : TeamLauncher.exe **1 207 Ko** (+176 Ko), total disque **2,78 Mo** ✅, RAM idle 77 Mo
- [x] **3ter — Pages réseau** — **fait le 25/09/2026** :
  - Navigation 9 entrées (Accueil, Instances, Jouer, Serveurs, Skins, Actualités, Bedrock, Compte, Paramètres — `g.page` 0–8) + toasts `notify_toast` (Notifier 320×84, bordure accent)
  - **Actualités** (`ui_news.cpp`) : « Voir le site web » (ShellExecuteW), flux `NewsUrl` + cache offline `news-cache.json`, historique `changelog.json` seedé (C# `Changelog.DefaultEntries`), cartes fidèles (badge tag accent, wrap 700)
  - **Compte** (`ui_account.cpp`) : profil, choix du pseudo, mode offline, bloc Microsoft désactivé (module 4)
  - **Bedrock** (`ui_bedrock.cpp`) : détection installé + lancement URI
  - **Serveurs** (`ui_servers.cpp`) : ping SLP winsock (état status 1.7, timeout 3,5 s, statuts « En ligne / Hors ligne / Impossible de joindre / Ping en cours... »), 3 onglets (Mes serveurs, Favoris, Villes de la team), copie code court, modales CRUD, quirks C# conservés
  - **Skins** (`ui_skins.cpp` + `skin_service.*` + `util_image.*` + `third_party/stb`) : aperçu gauche 360 px (rotation glisser 0,6°/px, zoom molette 0,4–3×, quad GL `AddImageQuad`), bibliothèque droite (grille cartes 100×128, clic = sélection, double-clic = appliquer, cœur dessiné en primitives), imports **fichier multi-sélection / pseudo / adresse** (modales Win32), « Récupérer le mien » (mc-heads, force refresh), favoris `skins-favorites.json` (format C#), 4 onglets (Tous/Officiels/Favoris/En ligne) + recherche locale/Enter, catalogue en ligne (Wayback NameMC → cool-skins → 34 pseudos populaires, **quirk `AddRange` C# reproduit** : seuls les skins à `SkinId` passent), section COMPTE (3 modes) + « Appliquer sur mon compte », application = CustomSkinLoader via Modrinth (Forge, `latest_release()` piston-meta) + copie `config/CustomSkinLoader/LocalSkin/<pseudo>.png`
  - Worker unique (file tâches > file vignettes), textures GL créées/libérées uniquement sur le main thread ; décodage PNG/JPEG via stb_image (`STBI_ONLY_PNG/JPEG`, `STBI_NO_STDIO`)
  - Hooks : `TL_AUTO_PAGE=<0..8>`, `TL_AUTO_TAB` (Serveurs 0–2, Skins 0–3, lue une fois), `TL_AUTO_MODAL`, `TL_AUTO_CTX`
  - Glisser-droper global `SDL_DROPFILE` → `tl::ui::on_drop_file` → `skins_import_path` : `.png/.jpg/.jpeg` copiés + toast « Skin importé » ; `.mrpack/.zip/.jar` → toast « Import non porté » ; autre → « Fichier non supporté / Type {ext} non reconnu. »
  - Captures : `build/3ter-home.png`, `3ter-news.png`, `3ter-account.png`, `3ter-bedrock.png`, `3ter-servers*.png` (3), `3ter-skins.png`, `3ter-skins-online.png`
  - Tests : `TLTestDataStore` + `TLTestGamePort` **ALL TESTS PASSED** ; fermeture propre (`CloseMainWindow`) sans kill, `skins_stop()` joint le worker et libère les textures avant la destruction du contexte GL
  - **Écarts connus** : glyphes `✓ ♥ ↓ ✕ ⟳ 📁 👤 🔗 📥 🌐 🔍 ⬆ … — →` hors `GetGlyphRangesDefault()` (Latin-1) → mots français + cœur/coeurs dessinés ; vignettes sans résolution UUID Mojang (avatar mc-heads par pseudo, même rendu visuel) ; live NameMC omis (Cloudflare 403 constaté) ; favoris stockés dans le dossier données (C# : dossier de l'exe) ; zoom de l'aperçu rendu utilisable (quasi inerte en C#) ; colonne gauche sans scroll (hauteur de boîte adaptative `clamp(avail-178, 260, 420)`) ; upload du skin sur le compte Microsoft reporté au module 4 ; import de modpacks par glisser-droper reporté aux modules 4–5 ; texte d'indice « Glissez pour tourner » collé au bouton « Recadrer » (fenêtre à 360 px)
- [x] **4a — Auth Microsoft (MsAuth)** — **fait le 25/09/2026** :
  - `src/ms_auth.hpp/.cpp` (dans `tl_core`) : portage complet de `MsAuth.cs` — device code OAuth (`login.live.com/oauth20_connect.srf` + `oauth20_token.srf`, scope `service::user.auth.xboxlive.com::MBI_SSL`, ID client officiel `00000000402b5328`), renouvellement silencieux par `refresh_token`, chaîne Xbox Live → XSTS → `login_with_xbox` → profil, skin officiel décodé des `properties` du profil vers `skins/<pseudo>.png`
  - Caches **interopérables avec la v5 C#** (mêmes chemins, mêmes clés, même chiffrement) : `msauth.json` (refresh token DPAPI+base64, écriture atomique tmp+rename) et `session-cache.json` (`name`/`uuid`/`token` DPAPI/`ts`, TTL 604 800 s) ; DPAPI via `CryptProtectData`/`CryptUnprotectData` (`CRYPTPROTECT_UI_FORBIDDEN` = `DataProtectionScope.CurrentUser`), base64 via `CryptBinaryToStringA` (`NOCRLF`) ; fallback « ancien format en clair » conservé
  - Machine d'états `Idle/RequestingCode/WaitingCode/Chaining/Done/Error` : `login_start()` (thread de fond, page Compte) et `login_blocking()` (en ligne, `launch_flow`) partagent le même flux — un seul à la fois, annulation propagée
  - `src/ui_auth.cpp` : modale ImGui (portage `ShowCodeDialog` 520×360 : code en 40 px accent dans un encart, « Rouvrir la page » / « Copier le code » / « Annuler », URL de repli) + `auth_sync()` (pseudo + `accountMode` + toast au succès) ; `tl_core` reste sans ImGui
  - `launch_flow.cpp` : session Microsoft réelle si `accountMode == "microsoft"`, `PlayerName` mis à jour comme le C#, sinon session hors ligne
  - CMake : `ms_auth.cpp` dans `tl_core`, liens `crypt32` + `shell32`, cible de test `TLTestMsAuth`
  - **Bug latent corrigé au passage** : `http_win.cpp` ne compilait plus depuis l'ajout de `extraHeaders` (appel `make_request(conn, c)` à 2 arguments) — ninja n'avait jamais recompilé le fichier ; valeurs par défaut rétablies. `post_string()` accepte désormais un `cancel`
  - Tests `tests/test_ms_auth.cpp` (**ALL TESTS PASSED**) : vecteurs base64 identiques à `Convert.ToBase64String`, binaire 0–255, aller-retour DPAPI + rejet d'un non-blob, `url_encode`/`form_body` (scope et `grant_type` percent-encodés), `FormatUuid`, cache de session (aller-retour, jeton jamais en clair sur disque, `"0"`/vide/expiré rejetés, ancien format en clair, JSON corrompu), jeton de rafraîchissement (aller-retour, réécriture, jeton vide ignoré, pas de `.tmp` résiduel), `get_session`/`has_session`/`logout` ; **réseau `TL_TEST_NET=1`** : vrai device code obtenu chez Microsoft (`https://www.microsoft.com/link`), annulation → `Idle` sans jeton écrit
  - **Validation e2e réelle** : la session DPAPI écrite par la **v5 C#** le 23/09 est relue et déchiffrée par le port (`Cache disque : OK (nom=TeamUN6713)`), lancement 1.12.2 avec le vrai compte → `Setting user: TeamUN6713` côté jeu, 1 235 ms au total ; modale validée avec un code réel, fermeture propre (`EXIT=0`)
  - Hooks : `TL_AUTO_LOGIN=1` (ouvre la modale au démarrage), `TL_NO_BROWSER=1` (n'ouvre pas le navigateur)
  - **Divergences assumées vs C#** : (1) `logout()` supprime aussi `session-cache.json` — le C# ne supprimait que `msauth.json`, donc la session revenait toute seule pendant 7 jours ; (2) si la connexion Microsoft échoue, le lancement s'arrête avec un message — le C# passait une session nulle à `BuildGameArgs` (`NullReferenceException`) ; (3) textes de la modale coupés en deux lignes (police ImGui bien plus large que Segoe UI)
  - ~~Bug C# reproduit tel quel~~ → **corrigé le 26/09/2026, cf. « 4a bis » ci-dessous**
  - **Reste du module 4** : Lang fr/en, Settings complet, Onboarding, Backup, CrashAnalyzer, Health, Telemetry, Discord, UpdateService, Admin, packs Mr/Cf/Share, serveurs hébergés, InstanceDetail, AutoShortcut, AppTasks ; upload du skin sur le compte Microsoft (page Skins) toujours à porter
  - Mesures : `TeamLauncher.exe` **1 575,5 Ko** (+99,5 Ko), total disque **3,14 Mo** ✅ (plafond 5 Mo), RAM idle **70,1 Mo** working set / 92,2 Mo privé
  - Captures : `build/4-auth-modal.png`, `build/4-auth-account.png`
- [x] **4b — Lang fr/en + thème configurable + Paramètres complet** — **fait le 25/09/2026** :
  - `src/lang.hpp/.cpp` (dans `tl_core`) : portage de `Lang.cs`, les **deux** mécanismes C# — `t("fr", "en")` (= `Lang.T`) et `t("fr")` via dictionnaire (= `Lang.Apply`/`FrToEn`, porté puis complété avec les libellés propres au C++). ~210 entrées, `unordered_map<string_view, const char*>` construite une fois ; en français `t()` renvoie l'argument sans lookup (coût nul)
  - **Bascule à chaud** : ImGui redessine chaque frame, donc changer la langue s'applique immédiatement — le C# devait relancer l'exe (`Lang.RestartApp`) ou reconstruire les pages
  - Retrofit complet des 11 fichiers `ui_*.cpp` (~5 000 lignes) : seuls `"TEAM"`/`"LAUNCHER"` (marque) et le `"X"` de fermeture restent littéraux. Alias `tr()` et non `t()` : plusieurs pages ont des locales nommées `t` qui auraient masqué la fonction silencieusement (le local `tr` de `ui_skins.cpp` a été renommé `taskRes`)
  - **Identifiants de modales stabilisés** (`###instmodal`, `###instdelete`, `###msauth`, `###newhosted`, `###delsrv`, `###skinimport`) : sans cela changer de langue fermait la modale ouverte, l'ID ImGui étant dérivé du titre
  - Statuts de la page Skins traduits **à l'affichage** et non à l'écriture : ils sont produits par le worker, qui ne doit pas lire `settings.language` (course de données)
  - **Thème configurable** (`theme_reload()` / `theme_apply_style()` dans `ui.cpp`) : `kBg`/`kCard`/`kAccent` passent de constantes d'en-tête à variables, alimentées par `settings.bgColor/cardColor/accentColor` (format `#rrggbb`, filtre `LegacyColors` de `Theme.cs`). Sans personnalisation la palette v5 est **rendue à l'identique** ; avec, les couleurs dérivées (bordure, survol, boutons, accent clair/foncé) suivent
  - `src/ui_settings.cpp` réécrit (80 → ~370 lignes) : 4 onglets fidèles à `SettingsPage` — **Général** (pseudo, Java + Parcourir, RAM, dossier des instances, URL actualités, langue), **Apparence** (3 champs couleur + pastille d'aperçu, « Couleurs par défaut », image de fond), **Intégrations** (Discord, URL de mise à jour + version, clé CurseForge, télémétrie + webhook), **Avancé** (compteur FPS, minimiser au lancement, maintenance). Les réglages non encore branchés à un service sont **conservés dans le config** et signalés dans l'UI
  - Textes périmés corrigés : « Compte Microsoft : non disponible en v6.0 » et « Langue : la traduction arrive avec le portage » (page Paramètres), en-tête de la page Jouer qui affichait toujours « Compte hors ligne »
  - Tests `tests/test_lang.cpp` (**ALL TESTS PASSED**) : langue par défaut et valeur inconnue → `fr`, paire explicite, dictionnaire fr/en, chaîne multi-lignes, clé absente rendue telle quelle, `nullptr`, surcharge `std::string`, `set_language` (écrit les réglages, pas de redémarrage), taille et cohérence de la table (aucune traduction identique au français par oubli, hors sigles voulus). Les 4 suites passent
  - **Validation visuelle** : `build/4b-settings-fr.png`, `4b-settings-en.png`, `4b-home-en.png`, `4b-instances-en.png`, `4b-account-en.png`, `4b-appearance.png` (thème vert/violet personnalisé appliqué, couleurs dérivées comprises)
  - **Écarts connus** : les statuts construits par concaténation (« Skin de X importé. », « Import de Y... ») restent en français en mode `en` — il faudrait des clés paramétrées ; l'**affichage** de l'image de fond derrière l'interface n'est pas porté (seul le chemin est conservé) ; pas de sélecteur de couleur natif (saisie hexa + aperçu) ; toolbar Instances toujours coupée en fenêtre étroite
  - Mesures : `TeamLauncher.exe` **1 610 Ko** (+34,5 Ko), total disque **3,17 Mo** ✅, RAM idle 77,7 Mo
- [x] **4c — Services de maintenance & fiabilité** — **fait le 25/09/2026** :
  - `src/backup.hpp/.cpp` : portage de `BackupService.cs` — zip de `<instance>/saves` vers `backups/mondes-AAAA-MM-JJ_HH-mm.zip`, rotation sur les **10** plus récentes, `list()` triée du plus récent au plus ancien, `restore()`, `remove()`
  - `src/crash_analyzer.hpp/.cpp` : portage de `CrashAnalyzer.cs` — les **9 règles** (OOM, version de Java, classe manquante, session expirée, pilote graphique, heap, antivirus, réseau, conflit de mods). **Sans `<regex>`** : les « regex » C# ne contiennent que des alternatives littérales (aucun métacaractère hors `|`), donc une recherche de sous-chaîne insensible à la casse est strictement équivalente — et n'embarque pas `<regex>` dans le binaire. `analyze_instance()` privilégie un `crash-reports/*.txt` de moins de 5 min, sinon les 300 dernières lignes de `game-log.txt`
  - `src/maintenance.hpp/.cpp` : `HealthService` (5 vérifications : Java, espace disque via `GetDiskFreeSpaceExW`, serveurs Mojang, écriture réelle dans le dossier des instances, auth Microsoft), `CleanupService` (installeurs Forge/NeoForge + archives JRE du runtime, `.zip` de plus de 30 jours du dossier de données), et la partie **portable** d'`UpdateService` : vérification de la dernière release via l'API GitHub + `compare_versions()`
  - `src/telemetry.hpp/.cpp` : portage de `TelemetryService.cs` (embeds Discord : crash, lancement, suppression d'instance, démarrage). **Worker unique** avec file d'attente, joint par `stop()` au shutdown — pas de threads détachés
  - Branchements : sauvegarde **pré-session** dans `launch_flow` (nouveau champ `LaunchRequest::instanceId`), puis à la fermeture du jeu — sauvegarde **post-session**, **analyse de crash** (toast + journal) et **rapport de télémétrie** ; les 3 boutons de Paramètres > Avancé (Diagnostic, Libérer de l'espace, Vérifier les mises à jour) ne sont plus des placeholders et tournent sur un worker joint au shutdown (`settings_stop()`)
  - Hooks de test : `TL_AUTO_TAB` étendu à la page Paramètres (0–3), `TL_AUTO_DIAG=1` (lance le diagnostic sans clic)
  - Tests `tests/test_services.cpp` (**ALL TESTS PASSED**) : backup (dossier absent / vide / archive créée, restauration fidèle, fichier hors archive supprimé, aucun dossier `restore-*` résiduel, rotation à 10, suppression), crash (les 9 règles, insensibilité à la casse, égalité des alternatives d'une même règle, priorité des règles, `tail_lines`, priorité crash-report sur game-log, message générique, dossier absent), cleanup (3 fichiers ciblés supprimés, 2 fichiers proches conservés, second passage à zéro, `.zip` récent conservé), `compare_versions` (longueurs inégales, préfixe `v`, suffixe `-beta`), télémétrie (rien sans webhook). Les **5 suites** passent
  - **Validation e2e réelle** : lancement 1.12.2, crash-report OOM planté dans l'instance, processus Java tué → `Analyse de crash : Minecraft a manqué de mémoire. → Augmente la RAM allouée` dans `launcher.log`, fermeture propre (`EXIT=0`). Diagnostic réseau réel : les 5 vérifications passent
  - **Divergence assumée vs C#** : `backup::restore()` extrait **puis** remplace ; le C# supprimait `saves` avant d'extraire, donc une archive corrompue faisait perdre les mondes (cas couvert par un test)
  - **Non porté** : Velopack (téléchargement/installation de la mise à jour) — la vérification indique la version disponible et ouvre la page de la release ; `PresenceService` (Discord) ; le compteur FPS dans les instances (dépend de l'API Modrinth)
  - Correctif d'affichage : les tirets cadratins `—` des chaînes visibles remplacés par `-`/`·` (hors `GetGlyphRangesDefault()`, ils s'affichaient en `?`)
  - Captures : `build/4c-advanced.png`, `build/4c-diagnostic.png`
  - Mesures : `TeamLauncher.exe` **1 675 Ko** (+65 Ko), total disque **3,23 Mo** ✅, RAM idle 79,9 Mo
- [x] **4d — CurseForge & import de modpacks** — **fait le 26/09/2026** :
  - `util_zip` : deux primitives ajoutées — `zip_read_entry()` (entrée lue en mémoire, pour les manifestes) et `zip_extract_prefix()` (extrait `overrides/` en retirant le préfixe, garde-fou zip-slip)
  - `src/curseforge.hpp/.cpp` : portage de `CurseForgeApi.cs` — `search()` (gameId 432, tri popularité, 25 résultats), `get_files()`, `get_files_by_ids()` et `get_project_classes()` (POST par lots de 64), `download_file()` (URL directe ou CDN de secours, `x-api-key` exigé depuis juillet 2026), `compute_fingerprint()` / `murmur2()` (variante CurseForge : `\t \n \r` retirés avant hachage), `sanitize()`, `loader_name()`
  - **Clé API** : lue via `DataStore::settings.curseForgeApiKey` avec repli sur `CURSEFORGE_API_KEY` (fidèle au C#). Elle vit dans le `config.json` de l'utilisateur et **n'est jamais écrite dans le dépôt**. HTTP 401/403 → message explicite renvoyant vers Paramètres > Intégrations
  - `src/pack_import.hpp/.cpp` : portage de `CfPackImporter.cs` et `MrPackImporter.cs`. `detect()` reconnaît le format **d'après le contenu de l'archive** (manifest.json / modrinth.index.json) et non l'extension ; `import_any()` aiguille. Les overrides sont extraits, puis les fichiers listés téléchargés (classe du projet → `mods` / `resourcepacks` / `shaderpacks`)
  - `src/ui_packs.cpp` : sélecteur de fichier, worker de fond, bandeau de progression avec Annuler en haut de la page Instances, ajout de l'instance au config **sur le main thread** à la fin, toast de bilan (téléchargés / échecs). Branché sur le menu « Importer » et sur le **glisser-déposer** (`.zip` / `.mrpack` — remplace l'ancien « Import non porté »)
  - `new_guid()` et `make_instance()` remontés de `ui.cpp` vers `tl_core` (`datastore.cpp`) : les importeurs en ont besoin sans dépendre de l'UI
  - **Divergences assumées vs C#** : (1) téléchargement **séquentiel** au lieu de 4 en parallèle — l'API CurseForge limite le débit et un import raté coûte du quota ; (2) l'import **n'écrit pas** dans le config, il rend l'instance à l'appelant (le C# faisait `Instances.Add` + `Save` depuis le service, ce qui ajoutait l'instance même en cas d'échec partiel) ; (3) garde-fou sur les chemins `files[].path` du .mrpack (le C# écrivait où le manifeste demandait — évasion possible hors du dossier de l'instance) ; (4) la version Minecraft d'un .mrpack est aussi lue depuis `dependencies.minecraft`, que le C# ignorait
  - Tests `tests/test_packs.cpp` (**ALL TESTS PASSED**) : primitives zip (lecture d'entrée, extraction préfixée, zip-slip bloqué, archive illisible), MurmurHash2 (déterminisme, sensibilité, les 4 longueurs résiduelles, positivité, normalisation `\t\n\r`), `sanitize`, `loader_name`, détection des 3 formats, import CurseForge (métadonnées, overrides, Id de 32 hexa, config non modifié), priorité des loaders (NeoForge/Forge coupent, Fabric non), manifeste cassé, import Modrinth (version via `dependencies`, overrides), chemin malveillant compté en échec, annulation, et la clé API (message d'aide, aucun appel sans clé, priorité réglages > environnement)
  - **Validation réseau réelle** : **une seule** requête à l'API (`TL_TEST_NET=1` + `TL_CF_KEY`) → 25 résultats, premier = Just Enough Items (JEI) #238222, 627 666 962 téléchargements, loaders `neoforge fabric`. Quota volontairement ménagé
  - **Non porté** : `PackService` et `PackShareService` (partage de pack par code court), recherche CurseForge dans l'UI (le client est prêt, la page Exploration reste à faire en 3quater)
  - Mesures : `TeamLauncher.exe` **1 738 Ko** (+63 Ko), total disque **3,30 Mo** ✅, RAM idle 80,4 Mo
- [x] **4a bis — Durée de vie de la session Microsoft** — **fait le 26/09/2026** (spécifié par l'utilisateur) :
  - Deux durées désormais distinctes, là où le C# n'en avait qu'une :
    - `kSessionTtlSec` = **7 776 000 s (90 jours)** : durée de conservation du *fichier* `session-cache.json` (pseudo, uuid, dernier jeton connu). C'était 7 jours.
    - `kMcTokenFreshSec` = **82 800 s (23 h)** : au-delà, le jeton Minecraft est considéré périmé (~24 h côté Mojang, 1 h de marge). `AuthSession::expiresAt` porte maintenant **cette** échéance, pas celle du fichier.
  - `session_usable()` (identité connue, pour l'affichage) est séparé de `session_fresh()` (jeton encore accepté, requis pour lancer le jeu). `login_blocking()` exige la fraîcheur : plus de démarrage avec un jeton mort — le symptôme était un jeu « non authentifié » qui ne se voyait qu'en rejoignant un serveur online.
  - `silent_renew()` : refresh token → Xbox Live → XSTS → `login_with_xbox` → profil, **sans aucune interaction**. Appelé automatiquement quand le jeton a plus de 23 h.
  - `startup_refresh()` : appelé depuis `ui::init()` sur un thread de fond, à **chaque lancement du launcher**. Microsoft fait tourner le refresh token à chaque usage, donc cela fait glisser la fenêtre de validité tant que l'utilisateur est actif — et régénère le jeton Minecraft d'avance pour que « Jouer » parte sans attente. N'ouvre **jamais** la modale : sans jeton conservé ou en mode hors ligne, l'appel ne fait rien.
  - **Classification des échecs** (`enum RenewFail`) : `RefreshRefused` (Microsoft a rejeté le jeton conservé → reconnexion par code d'appareil) est distingué de `Transient` (Xbox/XSTS/Mojang indisponible, réseau coupé → session **conservée**, nouvelle tentative plus tard). Seul le premier cas redemande une connexion.
  - **Commenté explicitement dans le code** : l'expiration finale ne dépend pas du launcher. Microsoft peut invalider le refresh token à tout moment (inactivité prolongée, changement ou réinitialisation du mot de passe, révocation depuis account.microsoft.com, exigence de ré-authentification forte). Ce n'est pas une anomalie : le jeton mort est supprimé et on retombe proprement sur l'écran de connexion — jamais de plantage ni d'échec muet.
  - Tests mis à jour + ajoutés (`TLTestMsAuth`, **ALL TESTS PASSED**) : bornes du TTL à 90 j, non-rejet à 8 j (l'ancien plafond), `expiresAt = ts + 23 h`, session de 30 h lisible mais non fraîche (identité conservée), `startup_refresh()` sans jeton conservé (aucune modale, état laissé à `Idle`, rien écrit) et en mode hors ligne (aucun thread lancé)
  - **Validation réelle** : session du 23/09 (jeton de **75,5 h**) → au démarrage, refresh token pivoté sur disque, chaîne Xbox → XSTS → `login_with_xbox` → profil HTTP 200, `Jeton Minecraft régénéré en silence (aucune reconnexion)`, cache redescendu à 0,5 min d'âge. **Aucune reconnexion demandée.**
  - **Incident rencontré et traité** : au premier essai, Mojang a renvoyé `INTERNAL_SERVER_ERROR` sur `login_with_xbox` alors que le refresh venait de réussir. La première version du code annonçait « session expirée » — diagnostic faux qui aurait poussé l'utilisateur à se reconnecter sans raison. D'où la classification `Transient`. L'essai suivant a réussi sans modification d'état : l'erreur était bien passagère.
  - Mesure : `TeamLauncher.exe` **1 743 Ko** (+5 Ko)
- [ ] **3quater — Outils** : Explorer (NBT/chunks/mondes, WorldSync, import), Modes (ExplorePage), Cartes (MapEditor + CityGenerator), Dev (ModDev), 3D (ModelViewer)
- [ ] **4 — Services & infra** (auth Microsoft = 4a, Lang + Settings = 4b ci-dessus) : Onboarding, BackupService, CrashAnalyzer, Notifier, Health, Telemetry, Discord présence, UpdateService, AdminService, packs (Mr/Cf/Share/CurseForgeApi), serveurs hébergés (ServerHost/ServerPanel/PterodactylApi), InstanceDetail/Import dialogs, AutoShortcut, AppTasks, DragDrop
- [ ] **5 — cible Linux** : après validation de 3 + 3bis + 3ter + 3quater + 4

Notes techniques portage :
- `ui.cpp` sera découpé en `ui_home.cpp`, `ui_instances.cpp`, … (un fichier par page) au-delà de ~2 000 lignes
- Aucune nouvelle dépendance attendue (WinHTTP + ImGui + nlohmann + miniz couvrent CurseForge, auth, news, skins)
- Navigation fidèle C# : Accueil, Instances, Serveurs, Skins, Explorer, Modes, Admin, Actus, Bedrock, Cartes, Dev, 3D, Compte, Paramètres (+Changelog petit)

## Mesures réelles — squelette (Étape 1, 24/09/2026)

Build : `cpp\build-release.bat` (Release, -Os, gc-sections, LTO, clang-cl 20.1.8).

| Composant | Taille mesurée | Estimation initiale | Écart |
|---|---|---|---|
| TeamLauncher.exe | **443 Ko** | 0,8 – 1,5 Mo | mieux que prévu |
| SDL2.dll | **1 637 Ko** | 0,4 – 0,8 Mo | **pire** : SDL2 complet (audio/joystick/haptic/sensor) |
| **Total disque** | **2,03 Mo** | 1,5 – 3,0 Mo | ✅ dans la cible, plafond 5 Mo respecté |

| RAM | Mesurée | Cible |
|---|---|---|
| Idle (fenêtre + ImGui, 6 s après lancement) | **65 Mo** working set / 87 Mo privé | ~20–40 Mo |

Rendu vérifié : fenêtre « Team Launcher v6.0.0 », ImGui, ~180 FPS (capture : `build/etape1-screenshot.png`).

### Écarts à traiter
1. **SDL2.dll 1,6 Mo** — réduire via `SDL_AUDIO=OFF`/`SDL_SENSOR=OFF`... (options CMake SDL) ou accepter (hors binaire, cible disque OK).
2. **RAM 65 Mo** — probablement driver GPU (OpenGL context) + heap ; à découper au portage : mesurer sans ImGui puis avec. Objectif revu : < 50 Mo idle.

## Mesures réelles — module 3bis (24/09/2026)

| Composant | Après 3bis | Après 3ter | 4a (auth MS) | 4b (Lang + Paramètres) | 4c (maintenance) |
|---|---|---|---|---|---|
| TeamLauncher.exe | 1 207 Ko | 1 476 Ko | 1 575,5 Ko | 1 610 Ko | **1 675 Ko** (+65 : backup, crash, health, telemetry) |
| SDL2.dll | 1 637 Ko | 1 637 Ko | 1 637 Ko | 1 637 Ko | 1 637 Ko |
| **Total disque** | 2,78 Mo | 3,04 Mo | 3,14 Mo | 3,17 Mo | 3,23 Mo |
| RAM idle | 77 Mo | 76,5 Mo | 70,1 Mo | 77,7 Mo | 79,9 Mo |

Après **4d (CurseForge + modpacks)** : `TeamLauncher.exe` **1 738 Ko**, total disque
**3,30 Mo** ✅ (plafond 5 Mo ; cible haute de 3 Mo dépassée de 0,30 Mo), RAM idle
**80,4 Mo**.

(mesures ponctuelles, ±10 Mo sur la RAM)

Captures de validation 3ter : `build/3ter-home.png`, `3ter-news.png`, `3ter-account.png`, `3ter-bedrock.png`, `3ter-servers{,-favoris,-villes}.png`, `3ter-skins.png`, `3ter-skins-online.png` (40 skins NameMC via archive Wayback).
