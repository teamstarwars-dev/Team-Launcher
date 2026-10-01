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
  - **Non porté ici** : Velopack (téléchargement/installation de la mise à jour) — la vérification indique la version disponible et ouvre la page de la release ; le téléchargement + l'installation sont complétés au module **4j** ci-dessous ; `PresenceService` (Discord) — porté au module **4g** ; le compteur FPS dans les instances (dépend de l'API Modrinth)
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
- [x] **4a ter — Durcissement sécurité du bloc auth** — **fait le 26/09/2026** (demandé par l'utilisateur avant de poursuivre l'implémentation) :
  - Audit du bloc (`ms_auth.cpp` 987 l., `ui_auth.cpp`, `http_win.cpp`, `test_ms_auth.cpp`) : TLS sain — WinHTTP/schannel, `WINHTTP_FLAG_SECURE` systématique en https, **aucun** `SECURITY_FLAG_IGNORE_*` ni `SetOption` de contournement dans tout le fichier.
  - **Plus aucun secret en clair sur disque** : `save_session_cache()` écrivait le bearer token Minecraft en clair quand DPAPI échouait (`cipher.value_or(s.token)`, repli hérité du C#) — désormais rien n'est persisté dans ce cas (session en mémoire seule, comme `save_refresh_token()` le faisait déjà). Seule la *lecture* du format clair est conservée (compat fichiers v5).
  - `write_atomic()` factorisé (tmp+rename, ex-`File.Replace` du C#) : `msauth.json` **et** `session-cache.json`, plus de fichier tronqué visible par une autre instance ou l'antivirus.
  - `secure_wipe()` (`SecureZeroMemory`, opaque pour l'optimiseur) + garde RAII sur tous les chemins de sortie : refresh token (passé par valeur + copie appelante écrasée), access token Microsoft après la chaîne XBL/XSTS/MC, device code, jetons XBL/XSTS, tampons disque relus, jeton de session au `logout()`.
  - `browser_url_allowed()` : `https` + hôte Microsoft/Xbox exigés (exact `login.live.com`, `login.microsoftonline.com`, `microsoft.com`, `account.microsoft.com`, ou suffixe `.live.com`/`.microsoft.com`/`.microsoftonline.com`/`.xbox.com` avec frontière de point — `fakemicrosoft.com` et `microsoft.com.evil.io` refusés), appliqué à `open_browser()` (flux) comme à `open_url()` (bouton « Rouvrir la page »). L'URL vient du JSON serveur : refus journalisé sinon.
  - `sanitize_file_stem()` : pseudo serveur → `[A-Za-z0-9_-]`, 64 car. max, `skin` sinon — le chemin `skins/<pseudo>.png` ne dépend plus de la confiance serveur.
  - Journaux : dump `login_with_xbox` tronqué à 300 car. comme le message affiché, valeur du code appareil retirée du log (déjà à l'écran et au presse-papiers).
  - Tests `TLTestMsAuth` §8bis (**ALL TESTS PASSED**, 6 suites vertes au total) : wipe effectif, atomicité (contenu + aucun `.tmp` résiduel), sanitize (`../../secret`→`secret`, vide→`skin`, plafond 64), allowlist (5 acceptées / 6 refusées).
  - **Résidu assumé** : les copies éphémères dans les corps JSON temporaires (`dump()` des requêtes) sont détruites en fin d'appel sans écrasement — même processus, durée de vie très courte.
  - Mesure : `TeamLauncher.exe` **1 945 Ko** (+5 Ko)
- [x] **4e — Page Exploration (Modrinth + CurseForge)** — **fait le 26/09/2026** :
  - Motivation : le client CurseForge du 4d n'était appelé par aucune page — `search()` était du code mort. Cette page le rend utilisable.
  - `src/modrinth.hpp/.cpp` : portage de `ModrinthApi` (Apis.cs) — `search()` (facets `project_type`, 25 résultats) et `download_project_file()` (filtre loader + version, fichier `primary` sinon le premier). Aucune clé d'API requise.
  - `src/content_install.hpp/.cpp` : portage de `ExplorePage.InstallHitAsync` et de ses aides, **séparé de l'UI pour rester testable** — `resolve_loader()` (quilt → fabric, Vanilla → refus explicite), `resolve_mc_version()` (`latest`/`?` → dernière release Mojang), `pick_compatible()` (version du jeu + loader, un fichier sans loader déclaré est accepté), et `install()` qui oriente mod/shader (vers l'instance) ou modpack (crée une instance via `pack_import`)
  - `src/ui_explore.cpp` : barre de recherche, sélecteurs source (Modrinth/CurseForge) et type (Modpacks/Mods/Shaders), cartes de résultat (titre, téléchargements formatés k/M, description, loaders), bouton Installer, modale « dans quelle instance ? » (portage d'`InstancePickDialog`), worker de fond annulable. Avertissement en clair si la clé CurseForge manque.
  - Navigation : nouvelle entrée **Exploration** placée après Instances, mais portant l'index **9** — les index 0–8 sont déjà documentés (`TL_AUTO_PAGE`) et restent stables ; l'ordre visuel est indépendant de l'index.
  - Hooks : `TL_AUTO_SEARCH=<terme>`, `TL_AUTO_SOURCE=0|1`, `TL_AUTO_TYPE=0|1|2`
  - Tests (dans `TLTestPacks`, **ALL TESTS PASSED**) : `category_key`/`curseforge_class`/`dest_subdir`, `resolve_loader` (les 4 loaders + quilt→fabric + Vanilla refusé + objet vide), `resolve_mc_version` sans réseau, `pick_compatible` (mauvaise version, mauvais loader, bon, sans loader déclaré, sans contrainte, insensibilité à la casse, aucun compatible, liste vide), et les garde-fous d'`install()` (sans instance cible, instance Vanilla, annulation)
  - **Validation réseau** : recherche Modrinth réelle → 25 résultats, premier = Sodium (`sodium`, `fabric neoforge quilt`). Modrinth n'a pas de quota, contrairement à CurseForge dont la clé reste ménagée.
  - **Validation visuelle** : `build/4e-explore.png` (état vide) et `build/4e-explore-modrinth.png` (recherche « sodium » réelle : Sodium 231,0 M téléchargements `fabric neoforge quilt`, Sodium Extra, Reese's Sodium Options… boutons Installer alignés, descriptions repliées)
  - **Capture d'écran : nouvelle approche.** Les scripts PowerShell qui photographient l'écran ont deux défauts — ils capturent ce qui est au premier plan (une fenêtre tierce si le launcher n'a pas le focus) et l'antivirus bloque les versions durcies (motif « P/Invoke user32 + capture d'écran »). Le launcher se capture donc lui-même : `TL_SCREENSHOT=<chemin.png>` lit son propre framebuffer avec `glReadPixels` après `TL_SCREENSHOT_DELAY` secondes (défaut 6), écrit le PNG via l'encodeur intégré de miniz (`tdefl_write_image_to_png_file_in_memory_ex`, aucune dépendance de plus) puis sort normalement — le chemin de shutdown reste donc exercé. Aucune autre fenêtre ne peut apparaître dans l'image et rien ne dépend du focus. `image::write_png()` ajouté dans `util_image`.
  - **Non porté** : icônes des résultats (`iconUrl` est récupéré mais pas affiché — il faudrait le pipeline de vignettes asynchrone de la page Skins), tri et filtres avancés, pagination au-delà de 25 résultats
  - Mesure : `TeamLauncher.exe` **1 818 Ko** (+75 Ko)
- [x] **4f — Assistant de premier lancement** — **fait le 26/09/2026** :
  - `src/ui_onboarding.cpp` : portage d'`OnboardingDialog.cs`. Déclenchement fidèle au C# (`Program.cs`) : `!onboardingDone` **ou** `accountMode` vide.
  - **Périmètre volontairement réduit à ce qui existe vraiment** : le C# instanciait le dialogue avec `SkipImport = true`, donc l'étape « import des instances existantes » n'était jamais atteinte en usage réel. Deux étapes portées — Bienvenue et Compte (Microsoft, ou pseudo hors-ligne avec validation du champ vide). La détection de `.minecraft` reste à faire, côté page Instances.
  - **Enchaînement avec la modale d'auth** : deux modales ImGui empilées se gênent. Quand l'utilisateur choisit Microsoft, l'assistant pose `accountMode` (comme le C#), lance `auth::login_start()` puis **s'efface** le temps de la connexion ; il conclut selon le résultat (succès → `onboardingDone`, échec/annulation → retour à l'étape Compte).
  - Pas de croix de fermeture : l'assistant doit aboutir, comme le C# tant qu'aucun mode de compte n'était choisi.
  - Correctif visuel : le champ pseudo était invisible (`FrameBg` et fond de modale tous deux à `kCard`) → fond forcé à `kBg` dans la modale.
  - Hook : `TL_AUTO_ONBOARD_STEP=0|1`
  - **Validation** : dossier de données vierge → l'assistant s'ouvre, `config.json` créé avec `onboardingDone=false` et `accountMode` vide. Captures `build/4f-onboarding-1.png` (Bienvenue) et `build/4f-onboarding-2.png` (Compte). Config utilisateur réel non touché (vérifié après coup).
  - Mesure : `TeamLauncher.exe` **1 824,5 Ko** (+6,5 Ko)
- [x] **4g — Raccourci bureau + Rich Presence Discord** — **fait le 26/09/2026** :
  - `src/shortcut.hpp/.cpp` : portage de `MainForm.EnsureDesktopShortcut`. Le C# passait par l'objet COM `WScript.Shell` ; ici **`IShellLinkW` + `IPersistFile`** directement — même résultat sans dépendre du Windows Script Host, désactivé par politique de sécurité sur certains postes. Création à la première ouverture (`settings.autoShortcut`), plus un bouton « Raccourci sur le bureau » dans Paramètres > Avancé pour le recréer (le C# n'offrait aucun moyen de le refaire).
    - **Un raccourci existant n'est jamais écrasé** : le drapeau est simplement posé. Vérifié sur le vrai Bureau — le `.lnk` laissé par la v5 est ressorti avec le même hash et la même date.
  - `src/presence.hpp/.cpp` : portage de `PresenceService.cs`. Le C# utilisait la bibliothèque `DiscordRPC` .NET ; ici le **protocole IPC de Discord est parlé directement** — tube nommé `\\.\pipe\discord-ipc-N` (0 à 9), trames `[opcode int32 LE][longueur int32 LE][JSON]`, HANDSHAKE puis `SET_ACTIVITY`. Aucune dépendance ajoutée.
    - Thread dédié : l'UI ne fait que déposer l'état souhaité. Chien de garde à 20 s comme le C# (Discord coupe le lien au redémarrage, à la mise à jour, à la fermeture). `CLOSE` reçu ou écriture en échec → reconnexion.
    - Deux présences : « Dans le launcher » (temps de jeu total) et « Joue à &lt;instance&gt; » (chrono de session, loader + version, temps total sur l'instance). **Quirk C# conservé** : si le serveur rejoint correspond à une ville de la team, l'état devient « &lt;ville&gt; — ville de &lt;propriétaire&gt; », la comparaison se faisant sur l'hôte seul (port ignoré).
    - Présence effacée proprement au shutdown (`SET_ACTIVITY` avec `activity: null`), thread joint.
    - Paramètres > Intégrations : le bouton « Appliquer » (`presence::reload()`) et un indicateur Connecté/Non connecté remplacent la mention « non porté ».
  - Tests (`TLTestServices`, **ALL TESTS PASSED**) : écriture d'un `.lnk` réel dans le dossier de test avec vérification de l'en-tête ShellLink (`4C 00 00 00`), refus sur cible vide, `desktop_dir()` existant, `ensure_desktop_shortcut(force=false)` sans effet quand le drapeau est posé ; garde-fous de la présence (désactivée, activée sans identifiant, `reload()` à l'arrêt) — **aucune IPC déclenchée par les tests**.
  - **Validation e2e réelle** : Discord ouvert, tube `discord-ipc-0` présent, App ID de l'utilisateur configuré → poignée de main réussie et trame READY exploitée : `[Presence] connecté (user=teamstarwars., id=…)`, format identique au C#.
  - **Non porté ici** : `AppTasks` (registre de tâches de fond avec panneau et annulation par tâche) — porté au module **4l** ci-dessous, avec rebranchage partiel des workers (Exploration, packs, mises à jour, diagnostic) ; skins, serveurs, télémétrie et lancement restent à rebrancher, ce n'est pas un petit lot
  - Mesure : `TeamLauncher.exe` **1 871,5 Ko** (+47 Ko)
- [x] **4h — Partage de packs entre membres de la team** — **fait le 26/09/2026** :
  - `src/pack_share.hpp/.cpp` : portage de `PackShareService.cs`. Export = empreintes SHA1 des mods (`.jar`) et shaders (`.zip`), résolution sur Modrinth, plus l'inventaire des configs / resource packs / mondes (le C# les liste sans les transporter). Import = recréation d'une instance et retéléchargement depuis les URL du descriptif. Mêmes noms de champs que le C# : les descriptifs de la v5 restent lisibles.
  - **Le « code court » (ABCD-EFGH) n'est volontairement pas porté.** `InstancesPage.GenerateShareCode` le dérive des SHA1, mais **rien ne sait le résoudre** : il n'existe ni serveur ni registre, et `ImportAsync` n'accepte que le JSON complet. Le bouton du C# ne pouvait donc pas fonctionner. On expose le seul chemin réel : copier/coller le descriptif.
  - UI : les deux entrées grisées de la page Instances sont actives — « Partager (copier le pack) » (menu contextuel) copie le descriptif dans le presse-papiers avec un bilan « N/M fichiers reconnus sur Modrinth », et « Importer partagé (presse-papiers) » lit le presse-papiers, vérifie le format avant de lancer quoi que ce soit, puis reconstruit l'instance.
  - **Problème d'API rencontré et contourné** : `POST /v2/version_files` (résolution par lot) est **bloqué par Cloudflare** — 403 avec page de blocage, quel que soit le User-Agent, vérifié hors du launcher avec trois UA différents dont celui recommandé par Modrinth. Les GET passent. Le code tente le lot puis **retombe sur `GET /v2/version_file/<sha1>`**, une requête par empreinte (404 = fichier non publié sur Modrinth). Le repli est journalisé.
  - Au passage : `mr::version_files()` journalise désormais les refus de l'API. Sans cela un blocage passait pour « aucun mod reconnu » et envoyait chercher le problème au mauvais endroit.
  - Tests (`TLTestPacks`, **ALL TESTS PASSED**) : export d'une instance factice (2 mods, 1 shader, config, resource pack, monde imbriqué) — comptages, SHA1 de 40 caractères, tailles, chemin relatif du monde ; `looks_like_pack` (format présent, texte libre, vide, objet sans Mods, tolérance sans Format) ; import (descriptif vide, texte invalide, aller-retour complet, clés insensibles à la casse, valeurs par défaut, config non modifié)
  - **Validation réseau en boucle fermée** : téléchargement d'un fichier Modrinth → SHA1 local → résolution par empreinte → projet `AANobbMI`, `sodium-fabric-0.5.13+mc1.20.1.jar`. Le SHA1 calculé est identique à celui annoncé par l'API. Une empreinte inventée ne résout rien.
  - Mesure : `TeamLauncher.exe` **1 940 Ko** (+68,5 Ko)
- [x] **4i — Serveurs Pterodactyl (panel distant)** — **fait le 26/09/2026** :
  - `src/server_host.hpp/.cpp` (127 + 416 lignes, 107 + 368 non vides) : portage de `PterodactylApi.cs` (Client API), sans ImGui. Modèle `Host` quadruplet (nom d'affichage, URL du panel, clé Client API, identifiant du serveur) à la place du couple global `VpsUrl`/`VpsApiKey` du C# : la même app conserve autant d'hôtes que voulu. `valid_host()` (scheme http(s), clé non vide, identifiant court `[A-Za-z0-9_-]` ≤ 64 caractères — pas de segment injectable dans l'URL), `api_url()` (échappement `%XX` des identifiants, suppression d'un éventuel `/` final), `auth_headers()` (clé jamais journalisée ni affichée ; champ `Password` côté modale).
  - Parsing séparé des appels réseau (`parse_server_list`, `parse_server_state`, `parse_websocket_token`, `parse_file_list`, `parse_allocations`) et persistance (`load_hosts` / `save_hosts` / `add_host` / `remove_host`) : entrée invalide ignorée à la lecture, ré-ajout d'un hôte à (URL, identifiant) identiques = remplacement au lieu d'un doublon, écriture via `DataStore::save()`. Le config reste compatible : `VpsUrl`/`VpsApiKey` sont toujours lus/écrits mais **plus lus par personne**.
  - UI `ui_servers.cpp` (1 133 lignes, 1 048 non vides) : l'onglet « Mes serveurs » de la page Serveurs affiche des cartes d'hôte 340×208 (pastille d'état, URL panel, identifiant serveur, puis état + CPU / RAM / joueurs quand la carte est à jour), avec Démarrer / Arrêter / Redémarrer / Console / Supprimer ; boutons « Nouvel hôte » (modale 4 champs, validation affichée en ligne) et « Actualiser ». Console = modale d'envoi d'une commande (`POST /command`), exactement le `SendCommandAsync` du C#.
  - **Rien ne bloque le thread UI** : file d'attente + condition variable (`ptero_worker` / `ptero_enqueue`), états par hôte sous mutex, toasts posés par le worker puis consommés sur le thread UI ; `servers_ptero_stop()` joint le worker au shutdown.
  - **Écarts connus** : (1) **l'Avalonia v5 n'avait aucune UI Pterodactyl** — `PterodactylApi.cs` n'est appelé nulle part dans `TeamLauncher.Avalonia` (seules les clés `VpsUrl`/`VpsApiKey` existent dans `Models.cs`) : l'écran ci-dessus reprend celui du WinForms v4 (`src/TeamLauncher/Pages/ServersPage.cs`) sous forme d'hôtes multiples ; (2) en C# l'onglet « Mes serveurs » listait les **serveurs hébergés localement** (`ServerHost.cs` : téléchargement du serveur Mojang/Forge/Fabric, `eula.txt`, `server.properties`, console locale, whitelist, sauvegarde du monde, tunnel playit.gg, relance auto) — ce module **n'est pas porté**, son champ `HostedServers` est écrit puis jamais lu ; (3) non portés de `PterodactylApi.cs` : `UploadFileAsync` (upload multipart), `CreateServerAsync` (Application API admin) ; (4) portés mais inexploités par l'UI : `list_servers`, `list_files`, `delete_files`, `allocations`, `websocket_token` — prêts pour un futur explorateur de fichiers ; (5) le C# rendait l'état par défaut sur **toute** erreur (`GetServerStateAsync`) — conservé (`noexcept`, `catch (...)`) ; (6) pas de polling de monitoring CPU/RAM à 5 s comme la `System.Windows.Timer` du C# : l'état d'une carte n'est refait qu'à l'ouverture de l'onglet ou au clic sur « Actualiser ».
  - Tests (`tests/test_server_host.cpp`, 355 lignes, 327 non vides — cible `TLTestServerHost`) : URL et en-têtes d'autorisation, `valid_host` (10 cas), parsing liste / état / token / fichiers / allocations avec champs manquants et corps vides, refus avant tout réseau (signal de démarrage inconnu, hôte invalide, réponse non 200/201/204), `server_state` et `websocket_token` jamais levants, aller-retour JSON des hôtes, persistance (`PteroHosts` présent dans le config, remplacement sans doublon, entrée corrompue ignorée), appels réels sous `TL_TEST_NET=1` + `TL_PTERO_URL`/`TL_PTERO_KEY`/`TL_PTERO_SID`. **Non exécuté dans le cadre de cette tâche (exécutions interdites) → non vérifié.**
  - Hook : `TL_AUTO_TAB=0..2` (onglets de la page Serveurs).
  - Capture : `build/4i-ptero.png` (32 114 o, 31,4 Ko).
  - Mesure : agrégée avec 4j/4k/4l (un seul build) — voir « Mesures réelles — modules 4i → 4l ».
- [x] **4j — Mise à jour auto (UpdateService complet)** — **fait le 26/09/2026** :
  - 4c n'avait porté que la **vérification** (`updates::check`, `compare_versions`) : `src/maintenance.hpp/.cpp` (118 + 529 lignes, 92 + 473 non vides) complète par `current_version()`, `download_update()` (progression + annulation), `updates_dir()`, `staged()` / `has_staged()` / `clear_staged()` et `install_staged_and_restart()`.
  - **Divergence majeure assumée** : le C# déléguait tout à Velopack (`UpdateManager`, `DownloadUpdatesAsync`, `ApplyUpdatesAndRestart`, commentaire en tête de `maintenance.hpp`) — ni portable, ni applicable en l'état. Le flux tient en 4 temps : `check()` lit `releases/latest` ; `download_update()` place le zip Windows x64 dans `<dossier de données>/updates/` (écriture `.part` puis renommage) ; l'intégrité se limite à HTTPS + **taille annoncée** (`assetSize`) — le C# s'appuyait sur la signature Velopack ; `install_staged_and_restart()` écrit `updates/pending.json` (écriture atomique) puis lance un script `apply-update.bat` détaché qui attend la mort du PID, déploie via `Expand-Archive`, supprime zip + marqueur et relance l'exécutable. L'application est **différée** parce que l'exécutable en cours est verrouillé sous Windows.
  - Choix de l'asset : `.zip` avec bonus `win`/`windows` et `x64`, malus `linux`/`macos`/`arm64` ; release sans paquet Windows → « Cette release ne contient pas de paquet Windows (.zip) » + lien vers la page de la version.
  - UI (`ui_settings.cpp`) : onglet **Intégrations**, section « MISES À JOUR AUTOMATIQUES » — portage du bloc `SettingsPage` — : champ « URL du flux de mises à jour », « Version installée : » + `TL_VERSION_STRING`, bouton « Vérifier les mises à jour », états (« Vérification… », « Nouvelle version disponible : vX (tu es en vY) », à jour), changelog affiché en `%.800s`, « Télécharger la mise à jour » avec barre de progression (`Ko / Ko`) et « Annuler », puis « Installer vX et redémarrer » (pousse `SDL_QUIT`) et « Supprimer le paquet ». Tout tourne sur un worker joint au shutdown (`upd_start`), jamais sur le thread UI. Le 3e bouton de la section Avancé (« Vérifier les mises à jour ») reste celui de 4c : vérification + ouverture de la page de la release.
  - `main.cpp` signale un marqueur présent **avant** l'interface : `update pending: vX` sur stderr.
  - **Écarts connus** : (1) **pas de vérification automatique au démarrage** — `App.axaml.cs:56` appelait `UpdateChecker.CheckOnStartupAsync()` (dialogue Oui/Non, téléchargement, relance immédiate) ; en C++ tout est manuel depuis les Paramètres ; (2) **pas de signature Velopack** : HTTPS + taille annoncée seulement (signalé dans le commentaire du code) ; (3) `UpdateUrl` était transmis à Velopack qui en dérivait le manifeste, ici l'URL sert **telle quelle** de point d'API JSON — un ancien URL Velopack ne fonctionnera pas ; (4) version 3 champs (`6.0.0`, projet CMake) contre 4 champs d'assembly en C# — `compare_versions` tolère les longueurs inégales ; (5) `download_update` et `install_staged_and_restart` ne sont couverts par aucun test (réseau + OS).
  - Tests : `tests/test_services.cpp` §§4 / 4bis / 4ter / 4quater (cible `TLTestServices`, **ALL TESTS PASSED** au moment du portage de 4c) : `compare_versions` (préfixe `v`, suffixe `-beta`, zéros initiaux, longueurs inégales, comparaison numérique), `select_asset_url` (bonus / malus, aucun zip), `parse_release_json` (déjà à jour, tag absent, JSON cassé), marqueur staged (création, lecture, suppression, zip vide ignoré). **Non exécuté ici → non vérifié.**
  - Hooks : `TL_AUTO_TAB=0..3` (onglet de Paramètres), `TL_AUTO_DIAG=1` (Avancé).
  - Capture : `build/4j-update.png` (57 069 o, 55,7 Ko).
  - Mesure : agrégée avec 4i/4k/4l — voir « Mesures réelles — modules 4i → 4l ».
- [x] **4k — Modale détail d'instance (InstanceDetail)** — **fait le 26/09/2026, affiné le 27/09/2026** :
  - `src/ui_instancedetail.cpp` (462 lignes, 437 non vides) : **pas de `.hpp`** — `open_instance_detail(id, tab)` et `instance_detail_modal()` sont déclarés dans `ui_internal.hpp`. Modale ImGui **760×560**, non redimensionnable, contre une fenêtre `InstanceDetailWindow` **960×680** redimensionnable. Onglets rendus en boutons plutôt qu'en onglets ImGui.
  - Bandeau : nom en grand, loader + version Minecraft + nombre de lancements + temps de jeu (`format_playtime`, aligné sur le `FormatTime` C# « 1h05 » / « 12min ») et notes ; pied de modale : Jouer (désactivée pendant la préparation ou le jeu), Modifier (édition différée d'une frame pour ne pas empiler deux modales), Dossier, Fermer.
  - Onglets : **Infos** (description, compteur, loader, version, RAM max, temps de jeu, message « Shaders, resource packs, configs et screenshots : non portés »), **Mods** (`*.jar` + `*.jar.disabled`, le même périmètre que le `*.jar*` du C#, nom de base, taille, bascule Activer/Désactiver par renommage, suppression, « Ouvrir le dossier mods »), **Mondes** (`saves/` hors préfixe `_backup_`, taille récursive, « Ouvrir » par monde, « Ouvrir le dossier saves »), **Journaux** (`game-log.txt` puis `logs/latest.log`, cache des 500 dernières lignes, recherche insensible à la casse appliquée à l'affichage, couleur ERROR/WARN).
  - Helpers purs testables sans ImGui (namespace `tl::ui::detail`, entre le `#else` du mode compilation et le `#endif` `TL_DETAIL_LOGIC_ONLY`) : `mod_is_disabled`, `mod_base_name`, `format_size_fr`, `sort_mod_paths`, `resolve_log_file` — seuls ces 5 sont atteignables par le test ; `list_mod_files`, `dir_size_bytes` et `reload_log_cache` vivent dans l'espace `tl::ui` (anonyme) sous le même `#ifndef`, avec la modale.
  - Ouverture : menu contextuel d'une carte d'instance (Détails / Mods / Mondes / Journaux — Screenshots ouvre le dossier en repli, comme `OpenFolder`), hook `TL_AUTO_DETAIL=<0..3>`, appel `instance_detail_modal()` dans `ui::frame`.
  - **Écarts connus** : (1) **4 onglets sur 8** — Description, Shaders, Resource Packs, Configs, Screenshots remplacés par un message et le bouton de repli « Dossier » ; (2) **pas de bannière image** (`ImagePath` est toujours `""` au C++ et jamais chargé) ; (3) pas d'affichage de la dernière session (« Vu le jj/mm/aa ») bien que `LastPlayed` soit écrit à la fin d'une partie ; (4) Description : compteurs **mods + mondes seulement** (le C# ajoute shaders et resource packs dans « Contenu de l'instance ») et contenu de « Configuration » en lignes séparées plutôt qu'en bloc structuré ; (5) **ajouts par rapport au C#** : boutons Activer/Désactiver et Supprimer dans Mods — en C# un clic sur une ligne **ouvrait le fichier** (`OpenPath`), il n'y avait ni bascule ni suppression — et bouton « Ouvrir » par monde ; (6) **repli CurseForge absent** : la liste des mondes C# scannait aussi `curseforge/minecraft/Instances/<nom>/saves` ; (7) non portés, signalés « non porté » dans l'UI plutôt qu'inventés : « + Ajouter un mod » (sélecteur de fichier), « Installer WorldEdit » (téléchargement `mediafilez.forgecdn.net`), import CurseForge + lecture `level.dat` (WorldTools) ; (8) Journaux : la recherche filtre les 500 lignes du cache (le C# relisait tout le fichier et affichait le nombre de résultats), pas de bascule de défilement automatique ni de mention « 500 dernières lignes sur N », pas de coloration DEBUG/INFO ; (9) Mondes : nom du dossier affiché tel quel alors que le C# lisait `level.dat` pour le nom du monde, et pas de date de dernière session, de statut « régions à nettoyer » ni d'origine « CurseForge + Launcher » / « Launcher seul » (ces trois données viennent de `WorldTools`, non porté).
  - Tests (`tests/test_instance_detail.cpp`, 109 lignes, 97 non vides — cible `TLTestInstanceDetail`, inclusion directe du `.cpp` avec `TL_DETAIL_LOGIC_ONLY`) : `mod_is_disabled`, `mod_base_name`, `format_size_fr`, `sort_mod_paths`, `resolve_log_file`. **Non exécuté ici → non vérifié.**
  - Captures : `build/4k-detail.png` (40 866 o), `build/4k-check.png` et `build/4k-detail3.png` (40 858 o chacune).
  - Mesure : agrégée avec 4i/4j/4l — voir « Mesures réelles — modules 4i → 4l ».
- [x] **4l — AppTasks (tâches de fond)** — **fait le 26/09/2026** :
  - `src/apptasks.hpp/.cpp` (85 + 243 lignes, 69 + 218 non vides) : portage de `AppTasks.cs` (Id / Title / Status + événement `Changed`), enrichi des états `Running` / `Done` / `Failed` / `Cancelled`, de la progression `0..1` (`-1` = indéterminée, barre animée côté UI), de l'horodatage `startedUnix`, du drapeau d'annulation copié au snapshot et du compteur `version()` (équivalent polling de l'événement C#). API : `create` / `update` / `finish` / `fail` / `cancel` / `cancelled` / `snapshot` / `count` / `count_running` / `clear_finished` / `reset_for_tests` et `run()` (le `Run` du C# : thread `std::async`, `set_status`, `on_error`, exception de cancellation distinguée de l'échec).
  - **Divergence assumée** : le C# retirait l'entrée du panneau en fin de tâche (`finally { Items.Remove }`) ; ici l'entrée passe en état terminal et reste affichée jusqu'au bouton « Effacer les terminées » — sinon un import rapide passerait inaperçu.
  - UI `src/ui_apptasks.cpp` (213 lignes, 189 non vides) : panneau dessiné une fois par frame par `apptasks_frame()` dans `ui::frame`, qui **sort immédiatement quand `count() == 0`** (invisible dès qu'aucune tâche ne tourne). Titre « Taches de fond (n) » avec `n` = tâches **en cours** (sans `n` quand aucune n'est en cours) ; par ligne : titre traduit, libellé d'état (En cours / Terminée / Échouée / Annulée), statut détaillé, barre de progression réelle (pourcentage) ou barre animée si la progression est indéterminée, et bouton « Annuler » **uniquement pendant l'exécution** (les états terminaux affichent une barre pleine ou vide, sans bouton) ; enfin « Effacer les terminees », grisé quand rien n'est terminé. Une branche « Aucune tache en cours… » existe mais reste morte tant que `count() == 0` coupe l'affichage. `apptasks_frame()` fait aussi le relais des annulations bouton → registre à chaque frame.
  - **Branchements** (relevé le 27/09/2026 à 11:12, relais panneau → worker via `apptasks_begin` / `apptasks_end` déclarés dans `ui_internal.hpp`) : `ui_explore.cpp` (installations depuis Exploration, 2 jobs), `ui_packs.cpp` (import de pack, partage, import de modpack, 3 jobs) et `ui_settings.cpp` (vérification / téléchargement de mise à jour via l'overload `upd_start(titre, statut, job)`, plus le diagnostic de maintenance via `maint_start`, lui aussi doublé worker + tâche). Le lien tâche → `std::atomic<bool>` locale de la page est posé à la création et **recopié chaque frame** par `relay_cancels()` : le bouton « Annuler » du panneau n'écrit que dans le registre, la page sonde déjà son propre atomic ; `apptasks_end` propage une annulation restée dans le registre avant de détacher le lien, et une tâche déjà annulée ne bascule plus en Terminée.
  - **Reste à rebrancher** : skins, serveurs (pings Pterodactyl / console), télémétrie et flux de lancement — ces workers gardent leur propre barre / annulation locale, donc leurs opérations n'apparaissent **pas** dans le panneau. Le rebranchage complet annoncé au 4g est donc **partiel** : registre, panneau et relais livrés, quatre familles de workers dessus, les autres non.
  - Aucun hook `TL_AUTO_*` ni capture dédiés : le panneau n'est visible que pendant une opération réelle (aucune capture 4l dans `cpp/build`).
  - Tests (`tests/test_apptasks.cpp`, 257 lignes, 242 non vides — cible `TLTestApptasks`) : cycle de vie complet, clamp de progression, échec, annulation, isolation et tri des snapshots, `clear_finished`, `run()` (succès, échec, `on_error`), concurrence (8 threads × 50 créations puis terminaisons croisées). **Non exécuté ici → non vérifié.**
  - Mesure : agrégée avec 4i/4j/4k — voir « Mesures réelles — modules 4i → 4l ».
- [x] **S — Durcissement sécurité** — **27/09/2026** :
  - **S2 — allowlist d'hôtes** (`http_win.cpp`) : toute requête sortante dont l'hôte n'est pas autorisé est refusée. Contrôle appliqué aux **deux** points d'entrée (`do_request` et `get_to_file`) — aucun chemin de contournement.
    - Règles de correspondance : une entrée « IP » (chiffres et points) exige l'**égalité stricte**, sinon `183.51.255.207.183` passerait par suffixe ; un domaine correspond à lui-même ou à un sous-domaine avec **frontière `.`** (`evilexample.com` ne correspond donc pas à `example.com`).
    - Normalisation avant comparaison : minuscules, espaces de bord, point final de FQDN, crochets IPv6 (`[::1]`). `host_from_url()` retire `userinfo@` et `:port` sans parseur externe.
    - Liste de base : services Microsoft/Xbox/Minecraft, Mojang, Modrinth, CurseForge + `forgecdn.net`, Forge/NeoForge/Fabric, Adoptium, GitHub, Discord, mc-heads/NameMC/archive.org, IP d'admin, boucle locale. Vérifiée par recoupement avec **toutes** les URL littérales de `cpp/src`.
    - **`gitlab.com` ajouté le 27/09** : Modrinth autorise `cdn.modrinth.com`, `github.com`, `raw.githubusercontent.com` **et** `gitlab.com` dans les URL de téléchargement d'un `.mrpack`. Sans cette entrée, importer un modpack hébergeant un fichier sur GitLab échouait **en silence** (compté en échec).
    - Hôtes dynamiques de la configuration utilisateur (`adminServerUrl`, `updateUrl`, `newsUrl`, `discordTelemetryWebhook`, `vpsUrl`, `PanelUrl` des hôtes Pterodactyl) enregistrés une seule fois au premier contrôle (`std::call_once`) — `datastore.cpp` n'inclut pas `http_win.hpp`, pas de dépendance circulaire.
  - **S2 — rédaction des journaux** : `redact_url()` masque `userinfo@`, le segment jeton d'un webhook Discord (`/webhooks/<id>/***`) et les valeurs des clés de query sensibles (`key`, `token`, `access_token`, `refresh_token`, `api_key`, `code`, `secret`, `sig`, `signature`, `password`, `auth`, `bearer`, `x-api-key`). Utilisée par toutes les traces `TL_HTTP_DEBUG` et par le message de refus.
  - **S3 — obfuscation des chaînes** (`obf.hpp`, header-only) : `TL_OBF("…")` encode le littéral **à la compilation** (`consteval`, clé dérivée de `__LINE__`/`__COUNTER__`) ; seul le tableau XORé atteint `.rdata`, le déchiffrement se fait dans un tampon local avec lectures `volatile` pour empêcher LTO de re-matérialiser le clair. Le macro n'accepte qu'un littéral (déduction `char[N]`).
    - **Ce n'est pas du chiffrement** et le header le dit : c'est une barrière contre la lecture passive (`strings` sur l'exécutable). Les vrais secrets restent côté DPAPI.
    - Employé pour : ID client Microsoft, endpoints `login.live.com`, IP du serveur d'admin, webhook de télémétrie par défaut.
  - **Secrets de configuration** (`secrets.hpp/.cpp`) : cinq champs de `config.json` chiffrés DPAPI (CurrentUser, sans entropie, UI interdite) avec le marqueur `enc:v1:` — `azureClientId`, `curseForgeApiKey`, `vpsApiKey`, `discordTelemetryWebhook`, `adminServerUrl`, plus la clé API de chaque hôte Pterodactyl.
    - **Migration v5 → v6 sans perte** : une valeur lue sans marqueur est acceptée telle quelle (fichiers de l'app Avalonia), signalée par `plainSecrets`, et le fichier est rechiffré au prochain enregistrement. Si DPAPI échoue, `encrypt_value()` rend le clair plutôt que d'écraser la donnée ; un blob illisible est conservé brut.
    - `secure_wipe()` (`SecureZeroMemory`) pour les secrets en fin de vie — `std::string` ne s'efface pas à la destruction.
    - `ms_auth` ne duplique pas ce code : `ms_auth.hpp` réexporte `tl::secrets::*` par `using`.
  - Tests `tests/test_security.cpp` (**ALL TESTS PASSED**) : ~40 hôtes réels autorisés, refus des hôtes inconnus, non-contournement par suffixe (IP et domaine), casse et point final, IPv6, `allow_host()` dynamique, et les six familles de rédaction. Aucune requête n'est émise.
  - **Validation e2e** : sur une **copie** du `config.json` réel, la clé CurseForge passe de `$2a$10…` à `enc:v1:AQAAA…` sans laisser de trace en clair, puis l'app la déchiffre et la recherche CurseForge aboutit (JEI, 628,1 M téléchargements) — chaîne complète chiffré-sur-disque → déchiffré-en-mémoire → client API → allowlist. Téléchargements réels inchangés (manifeste Mojang, redirection Adoptium).
  - **Défauts corrigés le 27/09** (l'arbre ne compilait pas) :
    - `tr(fr, en)` rend un `const char*` : trois concaténations avec un littéral étaient invalides (`ui_packs.cpp` ×2, `ui_settings.cpp`) → `std::string(tr(...)) + …`
    - `catalog_fetch` (`ui_skins.cpp`) : la déclaration avait reçu un paramètre d'annulation absent de la définition et du site d'appel. Intention terminée — le jeton de la tâche prime désormais sur celui de la page, annuler une recherche n'arrête plus les autres travaux du worker.
    - `test_security.cpp` attendait que `sig=zz` **ne soit pas** masqué alors que `sig` est dans la liste sensible. Une signature d'URL est un secret : c'est l'attente du test qui était fausse, l'implémentation est conservée.
  - Mesure : `TeamLauncher.exe` **2 107,5 Ko**
- [x] **4i — Télémétrie d'administration (AdminService)** — **fait le 27/09/2026**, clôt le module 4 :
  - `src/admin.hpp/.cpp` : battement toutes les 5 min, événements (`launcher_start`), rapports d'erreur. Mêmes chemins (`api/telemetry/{heartbeat,event,error}`) et mêmes noms de champs que le C# — le serveur d'admin n'a pas à changer.
  - **Désactivée par défaut** (`adminTelemetryEnabled = false`) : rien ne part tant que l'utilisateur ne l'active pas, comme le C#.
  - **Divergence assumée vs C#, et c'est un correctif** : le C# lançait **trois processus `wmic`** à chaque battement (RAM, CPU, GPU). `wmic` est déprécié et absent de Windows 11 24H2 et suivants — il renvoyait donc des champs **vides** tout en payant trois créations de processus. Ce port lit les mêmes informations par API native : `GlobalMemoryStatusEx`, registre (`ProcessorNameString`, `CurrentVersion`), `EnumDisplayDevices` pour le GPU principal. Aucun sous-processus, et les champs sont réellement remplis.
  - Second correctif : `ProductName` reste bloqué sur « Windows 10 » sous Windows 11, et `Environment.OSVersion` du C# renvoyait « 10.0.26200 » — la donnée ne permettait pas de distinguer les deux systèmes. Le build fait foi (≥ 22000). Vérifié : `Windows 11 Home 25H2 (build 26200)`.
  - `installation_id()` génère l'identifiant anonyme à la demande puis le conserve (le C# le posait par défaut dans le modèle ; ici le champ était vide).
  - Arrêt immédiat à la fermeture : le thread attend sur une `condition_variable`, pas un `sleep` de 5 min.
  - Tests (`TLTestServices`, **ALL TESTS PASSED**) : rien n'est émis quand c'est désactivé ou sans URL, identifiant généré une seule fois puis stable (32 hexa), informations machine non vides (CPU et GPU compris — ce que le C# ne garantissait plus), et présence des neuf clés du battement. L'URL de test pointe vers `127.0.0.1:9`, donc même un envoi accidentel n'atteindrait aucun serveur.
  - Mesure : `TeamLauncher.exe` **2 125,5 Ko** (+18 Ko)
- [x] **3quater-a — Lecteur NBT** — **fait le 27/09/2026** (socle de l'Explorateur et de l'éditeur de cartes) :
  - `src/nbt.hpp/.cpp` : les 12 types de tags, compounds et listes imbriqués, lecture depuis du NBT brut, **gzip** (`level.dat`) ou **zlib** (chunks `.mca`), avec détection automatique de la compression d'après les octets de tête. Décompression par miniz, aucune dépendance ajoutée.
  - **CORRECTIF vs C# — le lecteur `NbtReader.cs` ne pouvait pas fonctionner.** Le NBT est **gros-boutiste** (format Java) alors que `BinaryReader` de .NET lit en **petit-boutiste**, sans aucune compensation dans le code. Conséquence : une longueur de nom `00 04` y valait **1024** au lieu de 4, et l'analyse partait hors des rails dès le premier tag nommé. `ChunkReader.cs` faisait pourtant correctement le gros-boutiste à la main pour l'en-tête de région, juste avant d'appeler `NbtReader`. Le port lit le format correctement ; un test dédié verrouille précisément ce piège.
  - **Robustesse** : ces fichiers viennent de mondes téléchargés ou partagés. Toutes les lectures sont bornées, la profondeur d'imbrication est plafonnée (`kMaxDepth = 64`), et les tailles de tableaux sont vérifiées **contre les octets réellement disponibles avant toute allocation** — une longueur de 2 milliards annoncée sur un fichier de 20 octets est refusée sans allouer. Un fichier corrompu ou hostile donne `nullopt`, jamais un débordement.
  - Tests `tests/test_nbt.cpp` (**ALL TESTS PASSED**) : les 12 types dans un même compound (signe du Byte, ordre des octets sur Short/Int/Long, bits exacts de Float/Double), liste de compounds, compound imbriqué, liste vide déclarée `TAG_End`, accesseurs avec clé absente ou mauvais type ; les trois compressions ; et les entrées invalides — racine non-compound, type inconnu, longueur négative, longueur énorme, troncature en pleine valeur, imbrication au-delà de la limite, flux zlib/gzip illisibles.
  - **Validation sur fichier réel** : le test cherche un `level.dat` dans les instances de la machine et le lit — `LevelName="New World"`, `DataVersion=1343` (Minecraft 1.12.2). Le test est sauté proprement si aucun monde n'est présent.
  - Taille inchangée (2 125,5 Ko) : `nbt.cpp` n'est pas encore référencé par l'UI, le linker l'écarte.
  - Reste du 3quater : lecture des régions `.mca` (chunks, palette 1.18+ et format 1.12), page Explorateur, WorldSync, éditeur de cartes, CityGenerator, ModDev, ModelViewer.
- [x] **3quater-b — Lecture des régions `.mca`** — **fait le 27/09/2026** :
  - `src/region.hpp/.cpp` : en-tête 8 Ko (1024 offsets + 1024 horodatages), secteurs de 4 Ko, nommage `r.<x>.<z>.mca` avec coordonnées négatives, listing des chunks présents, lecture du NBT d'un chunk (par coordonnées locales ou monde), noms de blocs, carte de hauteurs, blocs distincts, et effacement de chunks (base du « supprimer la sélection » de l'éditeur de cartes).
  - **Quatre correctifs vs `ChunkReader.cs`** :
    1. **`Y` de section est un TAG_Byte signé** (−4 à 19 en 1.18+). Le C# le lisait en `byte` non signé : une section à −1 devenait 255, soit une altitude de 4080. Un test vérifie qu'une section à −1 produit bien des blocs de −16 à −1, et rien à 4080.
    2. **Le format 1.13–1.17 (`Palette` + `BlockStates`) n'était pas géré du tout.** Le C# ne connaissait que 1.18+ (`block_states`) et ≤ 1.12 (`Blocks`). De plus, **avant la 1.16 les entrées chevauchent les `long`** alors qu'à partir de la 1.16 chaque `long` est rembourré : les deux dépaquetages sont implémentés, le choix se faisant sur `DataVersion` (bascule à 2529).
    3. Le C# supposait **toujours zlib** ; l'octet de compression peut aussi valoir 1 (gzip) ou 3 (non compressé), les deux étant légaux. Les trois sont pris en charge.
    4. `GetHeightmap` du C# reconstruisait **tous** les blocs du chunk (jusqu'à ~98 000 chaînes) puis sondait 384 hauteurs par colonne. Ici les sections sont décodées une fois, parcourues de haut en bas, et une colonne remplie n'est plus retouchée.
  - Les mondes ≤ 1.12 n'ont pas de palette : les blocs sont des identifiants numériques (avec le demi-octet `Add` pour les IDs > 255, que le C# ignorait). Ils sont rendus `minecraft:id_<n>` — la table numérique → nom de la 1.12 (≈ 250 entrées) n'est pas embarquée, la coloration de carte se fera par identifiant.
  - Tests `tests/test_region.cpp` (**ALL TESTS PASSED**) sur des régions fabriquées à la volée : nommage (négatifs, extensions et formes invalides), en-tête et horodatage, lecture d'un chunk présent / absent / hors bornes, blocs et carte de hauteurs, section à Y négatif, chunk non compressé, effacement (idempotent, hors bornes ignorées), et fichiers invalides — trop courts, offset pointant au-delà du fichier.
  - **Validation sur monde réel** : 2 régions détectées dans une instance de la machine, 26 chunks dans `r.-1.0.mca`, chunk (−1, 3) lu avec 11 blocs distincts et 256/256 colonnes renseignées.
  - Taille inchangée (2 125,5 Ko) : `region.cpp` n'est pas encore référencé par l'UI.
  - Reste du 3quater : page Explorateur, WorldSync, éditeur de cartes, CityGenerator, ModDev, ModelViewer.
- [x] **3quater-c — Mondes et page Explorateur** — **fait le 27/09/2026** :
  - `src/world.hpp/.cpp` : lecture de `level.dat` (nom, dernière partie, `DataVersion`, mode de jeu, extrême, graine), listing des mondes d'une instance trié du plus récemment joué, repères de version, taille sur disque, comptage et suppression des régions vides.
  - **CORRECTIF vs `WorldTools.cs`** : `ReadLevelDat` cherchait la suite d'octets « LevelName » **n'importe où** dans le NBT décompressé puis lisait une longueur juste après — un contournement du lecteur NBT cassé. La première occurrence pouvait tomber au milieu d'une autre donnée, et rien ne garantissait qu'on lisait le bon tag. Le fichier est désormais réellement analysé.
  - La graine est lue dans `RandomSeed` **et** dans `WorldGenSettings/seed` (elle a migré en 1.16), ce que le C# ne faisait pas.
  - `delete_empty_regions()` collecte puis supprime : modifier un dossier pendant qu'on l'itère est un comportement indéfini, ce que faisait le C#.
  - `src/ui_explorer.cpp` : page **Explorateur** (index 10), deux onglets — **Mondes** (carte par monde : mode de jeu, version, taille, nombre de régions, dernière partie, boutons « Ouvrir le dossier » et « Fichiers ») et **Fichiers** (navigation dans l'arborescence de l'instance, retour, dossier parent, ouverture dans Windows). Sélecteur d'instance commun aux deux onglets, avec une vue agrégée « Toutes les instances ».
  - La navigation ne remonte jamais au-dessus de la racine des instances. La barre d'adresse est en lecture seule : le C# la laissait éditable sans rien valider.
  - Défaut corrigé pendant l'écriture : poser `ImGuiTabItemFlags_SetSelected` à chaque frame rendait le second onglet inaccessible — la bascule est maintenant ponctuelle (`pendingTab`).
  - Tests `tests/test_world.cpp` (**ALL TESTS PASSED**) : `level.dat` fabriqué puis relu (tous les champs), échecs propres (absent, non-gzip, NBT valide sans compound `Data`), régions vides comptées et supprimées puis idempotence, tri du listing et dossiers sans `level.dat` ignorés, repères de version (exact, approché, inconnu, très ancien).
  - **Validation sur données réelles** : la page affiche les mondes de la machine — *Greenfield V0.5.1* (Survie, 1.12.2, 743,7 Mo, **139 régions**), *New World* et *Nouveau monde* (Créatif, 1.12.2), triés par dernière partie. Capture `build/3q-explorateur.png`. La pile NBT → région → monde est donc validée de bout en bout sur des fichiers Mojang authentiques.
  - Mesure : `TeamLauncher.exe` **2 179 Ko** (+53,5 Ko — `nbt`, `region` et `world` sont désormais référencés par l'UI et entrent dans le binaire).
  - Reste du 3quater : WorldSync, éditeur de cartes, CityGenerator, ModDev, ModelViewer.
- [x] **3quater-d — Éditeur de cartes** — **fait le 27/09/2026** :
  - `src/ui_mapeditor.cpp` (index 11) : portage d'`EditorCanvas.cs` / `MapEditorPage.cs` — grille des chunks d'un monde, sélection au glisser (clic droit pour désélectionner), molette pour zoomer, bouton du milieu pour déplacer, « Ajuster la vue », suppression de la sélection avec confirmation.
  - **Correctif de performance dans `region::list_chunks`** : il lisait le fichier **entier**. Sur Greenfield (139 régions, 743 Mo) l'éditeur aurait été inutilisable, alors que seul l'en-tête de 8 Ko porte la table des offsets. Lecture réduite à 8 Ko + `file_size` pour valider les offsets. Chargement des 139 régions désormais quasi instantané.
  - Rendu adaptatif : sous 2 px par chunk, dessiner 1024 rectangles par région n'apporte rien et coûte cher — la région est alors remplie en dégradé selon sa densité. Culling au viewport dans les deux modes.
  - **Donnée réelle surprenante rencontrée** : Greenfield contient des régions à `rx=19531, rz=19531`, soit ~625 000 chunks du cluster principal (≈ 10 millions de blocs). L'étendue fait donc **19 542 × 19 541 régions**. Deux défauts en ont découlé et ont été corrigés : (1) un plancher de zoom à 0,05 px/chunk envoyait tout hors écran — l'ajustement descend maintenant à 0,0005 ; (2) à cette échelle un rectangle de région fait 0,45 px et ne dessine **rien** — une taille minimale de 2 px garantit qu'une région isolée reste repérable. L'étendue est affichée dans les statistiques pour que l'anomalie soit visible.
  - Suppression regroupée par région : chaque `.mca` n'est ouvert qu'une fois, via `region::clear_chunks`. Confirmation explicite, avec avertissement d'irréversibilité et invitation à sauvegarder.
  - Hooks : `TL_AUTO_MAP=1` (premier monde trouvé) ou `TL_AUTO_MAP=<extrait du nom>`.
  - **Validation sur données réelles** : *Nouveau monde* (750 chunks, 4 régions — la jonction des quatre régions est visible au centre) et *Greenfield v0.5.1* (**75 726 chunks, 139 régions**, chargement en moins de 10 s app comprise). Captures `build/3q-mapeditor.png` et `build/3q-mapeditor-gros.png`.
  - **Non porté** : les opérations WorldEdit sur les blocs (`pos1`/`pos2`, `//set`, `//replace`, `//copy`, `//paste`, `//undo`). Elles supposent l'**écriture** des chunks (`ChunkWriter.cs`), pas seulement la lecture — c'est un chantier distinct, et le rendu/la sélection de chunks est la partie réellement utilisée.
  - Mesure : `TeamLauncher.exe` **2 208,5 Ko** (+29,5 Ko)
- [x] **3quater-e — Synchronisation des mondes (WorldSync)** — **fait le 27/09/2026** :
  - `src/worldsync.hpp/.cpp` : portage de `WorldSyncService.cs` — détection des instances CurseForge (`<profil>/curseforge/minecraft/Instances`), listing des mondes aux **deux** emplacements (`<instance>/minecraft/saves` côté CurseForge, `<instance>/saves` côté launcher), rapprochement par nom d'instance (insensible à la casse), et import avec archivage préalable.
  - Troisième onglet **Synchronisation** dans la page Explorateur — pas de nouvelle entrée de navigation : il s'agit de mondes, la page existe déjà.
  - **Divergence assumée — sécurité de la donnée** : le C# archivait le monde existant dans un `catch {}` puis le supprimait **quoi qu'il arrive**. Si l'archivage échouait (disque plein, fichier verrouillé), le monde était détruit sans filet. Ici un échec d'archivage **annule l'import** : rien n'est supprimé.
  - **Correctif** : le C# datait un monde par `LastWriteTime` du **dossier**, que Windows ne met pas à jour quand un fichier imbriqué change — un monde joué apparaissait donc comme non modifié. On prend la date la plus récente de toute l'arborescence, ce qui est le sens réel de « modifié en jeu ».
  - Tests `tests/test_worldsync.cpp` (**ALL TESTS PASSED**) : les deux emplacements, dossier sans `level.dat` ignoré, import complet avec arborescence imbriquée, second import déclenchant l'archivage et remplaçant bien le contenu, échecs (source absente, instance de destination vide), et détection CurseForge non bloquante.
  - **Validation sur données réelles** : **4 instances CurseForge** détectées sur la machine, 1 rapprochement établi avec les instances du launcher.
  - Mesure : `TeamLauncher.exe` **2 239,5 Ko** (+31 Ko)
- [x] **3quater-f — Écriture NBT et écriture de chunks** — **fait le 27/09/2026** :
  - Prérequis découvert en attaquant `CityGenerator.cs` : ce n'est pas de l'orchestration d'Arnis (le commentaire d'en-tête dit d'ailleurs « Remplace Arnis »), mais un générateur complet OpenStreetMap → Minecraft qui **écrit** les chunks dans les `.mca`. Son sérialiseur (`SerializeCompound`, `WriteTag`) utilise un `BinaryWriter` **petit-boutiste** : comme le lecteur, il produisait des chunks que Minecraft ne peut pas relire.
  - `nbt::write / write_gzip / write_zlib / write_file_gzip` + fabriques de tags (`make_int`, `make_string`, `make_compound`, `make_list`…) : sérialisation **gros-boutiste**, liste vide écrite avec `TAG_End` comme type d'élément (ce que fait Mojang), longueur de chaîne tronquée à 65 535 plutôt qu'écrite fausse, profondeur plafonnée symétriquement à la lecture.
  - `region::write_chunk_local / write_chunk` : compression zlib (schéma 2), **allocation de secteurs** — recherche d'une plage libre en tenant compte des autres chunks, extension du fichier sinon —, mise à jour de l'offset, du nombre de secteurs et de l'horodatage. Le champ « nombre de secteurs » tenant sur un octet, un chunk de plus de 255 secteurs est refusé proprement.
  - **Écriture atomique** : fichier temporaire puis remplacement. Une coupure en cours d'écriture ne laisse jamais une région à moitié écrite — le C# écrasait le fichier en place, ce qui pouvait corrompre un monde entier.
  - Tests (**ALL TESTS PASSED**) : aller-retour NBT sur les 12 types (valeurs négatives, accents, tableaux, compound imbriqué, liste de compounds, liste vide) plus vérification directe des premiers octets (`0A 00 06 R…`), aller-retour gzip et zlib ; côté région — création d'une région inexistante, relecture, horodatage posé, absence de fichier temporaire résiduel, remplacement par un chunk **beaucoup plus gros** forçant une réallocation, écriture d'un voisin sans abîmer le premier, coordonnées hors bornes refusées, écriture par coordonnées monde avec création de la région.
  - Débloque aussi les opérations WorldEdit de l'éditeur de cartes, restées en suspens en 3quater-d.
  - Mesure : `TeamLauncher.exe` **2 240 Ko**
  - **À savoir pour la suite de CityGenerator** : l'hôte `overpass-api.de` n'est pas dans l'allowlist S2 — il faudra l'y ajouter, sinon la récupération OSM sera refusée.
- [x] **3quater-g — Générateur de ville : OSM, projection, rastérisation** — **fait le 27/09/2026** :
  - `src/citygen.hpp/.cpp` : portage de la partie données de `CityGenerator.cs` — analyse d'une emprise, requête Overpass, classement des entités (bâtiments, voies, eau, parcs, rails), projection en coordonnées Minecraft, et rastérisation en blocs.
  - **Malgré le libellé « Arnis » de l'UI du C#, aucun outil externe n'est piloté** : le service interroge lui-même l'API Overpass. Le commentaire d'en-tête du C# le dit d'ailleurs (« Remplace Arnis »).
  - `overpass-api.de` **ajouté à l'allowlist S2** — sans quoi la requête aurait été refusée. Test correspondant ajouté.
  - **CORRECTIF vs C#** : le classement testait la seule **présence** du tag `natural`, donc une forêt (`natural=wood`), une falaise ou une plage devenaient de l'eau. On exige `natural=water`. Un test verrouille le cas.
  - Garde-fous absents du C# : hauteur de bâtiment plafonnée (un `building:levels` aberrant ne crée pas une tour de 10 000 blocs), emprise de polygone abandonnée au-delà de 4 millions de cases, segment de ligne aberrant ignoré, et plafond global sur le nombre de blocs produits. Sans eux, une zone mal saisie partait en allocation de plusieurs gigaoctets.
  - Erreurs Overpass distinguées : 429 et 504 (service saturé) donnent un message qui invite à réessayer ou à réduire l'emprise, plutôt qu'un code HTTP brut.
  - Tests `tests/test_citygen.cpp` (**ALL TESTS PASSED**), **entièrement hors ligne** — les réponses Overpass sont fabriquées dans le test : emprise (espaces, champs manquants, non numériques, ordre inversé, hors bornes), classement des six types, hauteur déduite des niveaux, largeur selon la catégorie de voie, `natural=wood` correctement écarté, projection (signe des axes, ordre de grandeur en mètres, resserrement de la longitude par le cosinus de la latitude), entrées dégradées, géométrie (point dans polygone, remplissage par couches, polygone dégénéré, emprise aberrante, ligne épaisse), et plafond de rastérisation.
  - **Reste pour finir le générateur** : l'écriture des blocs dans le monde. Le socle existe (`nbt::write`, `region::write_chunk`), il manque la ré-encodage palette + `block_states` d'une section existante.
  - Mesure : `TeamLauncher.exe` **2 240 Ko** (inchangé : `citygen` n'est pas encore référencé par l'UI)
- [x] **3quater-h — Modification de blocs et écriture de la ville dans un monde** — **fait le 27/09/2026** :
  - `region::set_blocks(chunk, edits)` : dernière pièce manquante du socle. Elle décode la section concernée, reconstruit la **palette** et le tableau d'indices, puis ré-encode. Les trois formats de la lecture sont couverts : 1.18+ (`block_states`), 1.13–1.17 (`Palette` + `BlockStates`, disposition **tassée** avant la 1.16 et **rembourrée** à partir de 1.16, pivot sur `DataVersion` 2529), et ≤ 1.12 (`Blocks` + `Data` en identifiants numériques, via une table interne `region::legacy_id_for` limitée aux blocs du générateur — un nom absent est compté `unsupported` plutôt que posé au hasard).
  - Une section réduite à une seule entrée de palette voit son tableau `data` **omis**, comme le fait Minecraft ; une section absente n'est **pas créée** (poser un bloc dans le vide supposerait fabriquer terrain, biomes et lumière) et est comptée `skipped`.
  - `citygen::paste_into_world(worldDir, blocks, originX, originZ, …)` : regroupe les blocs par chunk **sans les dupliquer** (tri d'un tableau d'index, pas une `map` de vecteurs — sur 4 millions de blocs la différence est de plusieurs centaines de Mo), lit chaque chunk une fois, applique tous ses changements en un appel, puis réécrit. Annulable et rapporté dans le panneau des tâches.
  - **Un chunk jamais généré est ignoré**, pas fabriqué : le compte est remonté à l'UI avec le conseil d'aller explorer la zone en jeu. L'écriture reste atomique **par fichier de région**, pas sur l'ensemble — d'où l'invitation à sauvegarder le monde, affichée sur la page.
  - **DIVERGENCE assumée vs C#** : `CityGenerator.SerializeCompound` écrivait le NBT en **petit-boutiste** (`BinaryWriter` de .NET). Aucune ville produite par le C# n'aurait jamais pu être relue par Minecraft. Tout le chemin est ici interne et validé par un aller-retour disque.
  - Nouvelle page **`src/ui_citygen.cpp` (page 12, « Ville OSM » dans la barre)** : saisie de l'emprise avec taille estimée en km, altitude de base, décalage X/Z, choix instance + monde, bouton **Aperçu** (rastérisation seule) et **Générer dans le monde**. Crochet de test `TL_AUTO_CITY=<emprise>`.
  - Tests : `tests/test_region.cpp` — palette agrandie par un nom neuf, nom déjà présent, bloc hors chunk et section absente comptés en `skipped`, table 1.12 (dont la forme `minecraft:id_<n>`), chunk sans sections, et **relecture après écriture disque**. `tests/test_citygen.cpp` — monde absent refusé proprement, liste vide, collage sur un chunk réel avec coordonnées **négatives** (le modulo doit rester dans 0–15), chunk manquant non créé, annulation. **15 suites : ALL TESTS PASSED.**
  - Validation réseau réelle : emprise `2.2930,48.8560,2.2960,48.8590` (Paris 7e) → **165 entités, 527 241 blocs**, emprise 1 043 × 906 blocs. L'étendue dépasse la boîte parce qu'Overpass renvoie les chemins **entiers** qui la traversent, nœuds extérieurs compris.
  - **Correctif d'outillage** : `/EHsc` est désormais posé explicitement sur la cible `TeamLauncher` dans `CMakeLists.txt`. Il ne venait jusque-là que du `CMAKE_CXX_FLAGS` par défaut, donc un cache CMake régénéré de travers désactivait silencieusement les exceptions et cassait une dizaine de fichiers UI.
  - Trois avertissements de compilation traités au passage : troncature réelle de `snprintf` dans `backup.cpp` (buffer de 24 pour 25 octets — le suffixe aléatoire perdait un caractère), comparaison morte sur `kMaxString` dans `nbt.cpp` (une longueur sur 2 octets ne peut pas dépasser 1 Mo), variable inutilisée dans `ui_explore.cpp`, et fonction morte `to_double` dans `citygen.cpp`. **Build sans aucun avertissement sur `src/`.**
  - Mesure : `TeamLauncher.exe` **2 367 Ko** (2 423 808 o), soit **+127 Ko** depuis 3quater-g. Avec `SDL2.dll` (1 637 Ko) : **3,91 Mo** — plafond de 5 Mo respecté.
  - **Reste du 3quater** : opérations WorldEdit de l'éditeur de cartes (désormais débloquées), ModDev, ModelViewer.
- [x] **3quater-i — Opérations WorldEdit** — **fait le 27/09/2026** :
  - Nouveau module `src/worldedit.hpp/.cpp` : `bounds_of`, `set_region` (//set), `replace_region` (//replace, joker `*`), `copy_region` (//copy), `paste` (//paste), et une classe `History` (//undo / //redo). L'éditeur de cartes n'en est que la façade.
  - **L'implémentation C# (`EditorCanvas.cs`) était inutilisable en pratique — cinq défauts corrigés** :
    1. `SetBlock` **relisait et réécrivait le chunk entier pour chaque bloc**. Un cuboïde de 16×16×16 déclenchait 4 096 lectures et 4 096 réécritures complètes du fichier de région. Ici les blocs sont regroupés par chunk : une lecture, une écriture.
    2. `SaveUndo` copiait **tous** les fichiers de région du monde en mémoire, jusqu'à 20 instantanés. Sur un monde de 700 Mo : 14 Go de RAM. Ici seuls les **chunks touchés** sont retenus, en NBT compressé, avec un budget explicite (20 étapes / 128 Mo, les plus anciennes abandonnées d'abord).
    3. `GetBlock` matérialisait le dictionnaire de tous les blocs du chunk (~98 000 entrées) pour lire une position. Nouvelle classe `region::ChunkView` : les sections sont décodées **une fois par chunk**, puis interrogées en O(1). `block_at` reste pour les lectures ponctuelles.
    4. `PasteAsync` calculait le décalage depuis le **premier bloc du presse-papier** au lieu du coin minimal de la sélection. Comme l'air n'est pas copié, un coin minimal vide décalait tout le collage. Le presse-papier est désormais stocké en coordonnées **relatives au coin minimal**.
    5. Aucune borne de volume : une sélection de 10 000 blocs de côté lançait 10¹² itérations. Plafond à **8 000 000** de blocs, refus explicite au-delà.
  - **Piège trouvé pendant le portage** : copier un `nbt::Compound` ne le duplique **pas** en profondeur — `Tag` porte des `shared_ptr` vers ses compounds et listes filles. Un « état avant » obtenu par affectation était modifié en même temps que l'original, donc `//undo` réécrivait l'état déjà modifié. Les instantanés sont désormais **sérialisés** avant toute modification. Avertissement ajouté dans `nbt.hpp` : le premier test d'annulation l'a attrapé.
  - Facade UI dans `ui_mapeditor.cpp` : panneau repliable « Opérations WorldEdit », double-clic sur un chunk = **pos1** (centre du chunk, au plus haut bloc non-air), Maj+double-clic = **pos2**, saisie manuelle des trois coordonnées, repères colorés sur la grille, volume de la sélection, compteurs de presse-papier et d'historique. Opérations exécutées en tâche de fond, annulables. Crochet de test `TL_AUTO_WE=1`.
  - Changer de monde **vide l'historique, le presse-papier et pos1/pos2** : ils portent des coordonnées et des chunks de l'ancien monde, un `//undo` les y aurait réécrits.
  - Tests `tests/test_worldedit.cpp` (**ALL TESTS PASSED**) : bornes et volume, refus (monde absent, volume excessif, arguments vides), `//set` à cheval sur deux chunks, `//undo`/`//redo` avec relecture disque, `//replace` exact et joker (y compris la seconde passe qui ne change plus rien), `//copy`/`//paste` avec décalage vérifié bloc par bloc, chunk jamais généré ignoré et non créé, altitude hors sections, annulation coopérative, historique borné à 20 étapes, et **`ChunkView` comparé à `block_at` sur toutes les positions**.
  - **16 suites : ALL TESTS PASSED.** `TeamLauncher.exe` **2 444 Ko** (2 502 656 o), **+77 Ko**. Avec `SDL2.dll` : **3,99 Mo**, plafond de 5 Mo respecté.
  - **Reste du 3quater** : ModDev, ModelViewer.
- [x] **3quater-j — Développement de mods (ModDev)** — **fait le 27/09/2026** :
  - `src/moddev.hpp/.cpp` : génération du squelette de projet (Fabric / Forge / NeoForge / Bedrock), normalisations, détection de la chaîne d'outils, exécution d'une commande avec sortie ligne à ligne. Page `src/ui_moddev.cpp` (page 13, « Mods (dev) »).
  - **CORRECTIF majeur vs C#** : `ModDevPage.cs` **ne générait jamais le wrapper Gradle**. `BuildProjectAsync` et `RunProjectAsync` cherchaient `gradlew.bat` dans le dossier du projet et affichaient « gradlew.bat non trouvé. Crée le projet d'abord. » — or créer le projet ne le produisait pas. **Build et Run ne pouvaient donc jamais fonctionner sur un projet créé par le launcher.** Ici `detect_toolchain` cherche le wrapper, puis `gradle` dans le PATH, puis vérifie le JDK (`JAVA_HOME` puis PATH), et la page affiche les trois états en permanence avec la commande exacte à lancer s'il manque quelque chose.
  - **CORRECTIF** : les versions de Yarn et du loader étaient devinées (`yarn:<mc>+build.1`, `fabric-api:<mc>+`) — des chaînes qui n'existent pas la plupart du temps, donc échec à la résolution des dépendances dès le premier build. Elles sont maintenant **résolues en ligne contre `meta.fabricmc.net`** (déjà dans l'allowlist), avec repli documenté **écrit en commentaire dans `gradle.properties`** plutôt que silencieux.
  - **CORRECTIF** : aucune validation du nom ni du paquet. « Épée Enchantée » produisait une classe Java invalide et un `mod_id` refusé par Fabric. `mod_id_from`, `class_name_from` (désaccentuation incluse) et `valid_package` (segments minuscules, mots réservés du langage rejetés) normalisent, et la page montre l'id et le nom de classe en direct.
  - **CORRECTIF** : le C# écrasait en silence un projet existant. Les fichiers déjà présents sont conservés sauf case « Écraser » cochée, et le compte rendu distingue écrits et conservés.
  - Autres écarts : les dossiers Bedrock vides du C# (que ni une archive ni Git ne conservent) reçoivent un `.gitkeep` ; `settings.gradle`, `.gitignore` et `README.md` sont ajoutés ; le succès d'un build n'est plus déduit d'un emoji dans la sortie mais du **code de sortie** du processus.
  - Console : sortie fusionnée stdout+stderr **ligne à ligne pendant le build** (le C# lisait aussi en continu, mais via deux `Task` concurrentes sans ordre garanti), coloration par niveau, 4 000 lignes au maximum, défilement automatique seulement si on est déjà en bas, champ de commande libre et bouton d'arrêt.
  - Tests `tests/test_moddev.cpp` (**ALL TESTS PASSED**), **hors ligne** : normalisations (accents, chiffre en tête, caractères interdits, longueur), paquets valides/invalides dont les mots réservés, 200 UUID v4 vérifiés (version, variante, aucune collision), fichiers générés pour les quatre chargeurs avec **manifestes relus en JSON**, chemin de classe suivant le paquet, écriture sur disque, conservation d'un fichier modifié à la main puis écrasement sur demande, refus argumentés, détection de la chaîne d'outils, et exécution réelle d'une commande (lignes, code de sortie non nul, exécutable introuvable).
  - **Nettoyage transverse** : la police par défaut d'ImGui ne couvre que Latin-1. Les **52 occurrences de « … » (U+2026)** et les **8 « — » (U+2014)** présentes dans les chaînes de l'interface s'affichaient donc en « ? » sur toutes les pages. Remplacées par `...` et `-`.
  - **17 suites : ALL TESTS PASSED.** `TeamLauncher.exe` **2 563 Ko** (2 625 024 o), **+119 Ko**. Avec `SDL2.dll` : **4,10 Mo**, plafond de 5 Mo respecté.
  - **Reste du 3quater** : ModelViewer.
- [x] **3quater-k — Visualiseur de modèles 3D** — **fait le 27/09/2026**, **3quater TERMINÉ** :
  - `src/model3d.hpp/.cpp` (lecture) + `src/ui_modelviewer.cpp` (page 14, « Modèles 3D »).
  - **CORRECTIF majeur vs C#** : `ModelViewerPage.LoadModel_Click` **n'ouvrait jamais le fichier choisi**. Il retenait le chemin, puis faisait `cubes.Clear(); LoadDefaultModel();` — le même mannequin codé en dur s'affichait quel que soit le fichier. Pire, `ModelViewer3D`, la seule classe sachant analyser un `.bbmodel`, **n'était instanciée nulle part** : ses 355 lignes étaient du code mort. Le fichier est ici réellement lu, et trois formats sont gérés (`.bbmodel`, modèle Java `.json`, géométrie Bedrock `.geo.json`).
  - **Rendu** : algorithme du peintre dans la liste de dessin ImGui, donc **aucune ligne d'OpenGL ajoutée** pour un résultat équivalent au GDI+ du C#. Trois écarts assumés, tous des corrections :
    - Le C# faisait tourner le modèle autour de **l'origine du monde** alors que les coordonnées d'un modèle Minecraft vont de 0 à 16 : il partait en orbite hors du cadre. Rotation autour du centre de la boîte englobante.
    - Zoom figé à 8 : un modèle de 16 unités occupait 128 px quelle que soit la taille du cadre. Ajustement automatique au chargement, bouton de réinitialisation.
    - Les faces arrière étaient dessinées puis recouvertes (6 faces par boîte au lieu de 3). Élimination sur le signe de l'aire projetée, ce qui supprime au passage les scintillements entre faces coplanaires. Sur le mannequin : **36 faces au lieu de 72**.
  - Autres écarts : les « yeux » du mannequin du C# avaient une **profondeur nulle** et étaient donc écartés par sa propre règle des dimensions positives — invisibles ; ils reçoivent une épaisseur. `from`/`to` inversés sont normalisés (le C# obtenait des dimensions négatives et jetait la boîte). Le filtre du sélecteur n'annonce plus `.obj`, format qu'aucun code ne lisait. Les rotations d'éléments restent ignorées, mais **c'est affiché** plutôt que silencieux. Le contournement du C# sur une clé nommée « cubes » avec une espace en tête est abandonné.
  - **Fuite du C# à ne pas reproduire** : `GetBrush`/`GetPen` mettaient les objets GDI+ en cache, mais l'appelant faisait `using var brush = GetBrush(...)` — l'objet mis en cache était donc **disposé à chaque itération puis réutilisé**. Sans objet.
  - Les cinq liens d'outils externes (Blockbench, Mine-imator, Cinema 4D, Blender, Pixel Studio) sont conservés, dans un bandeau repliable.
  - Tests `tests/test_model3d.cpp` (**ALL TESTS PASSED**) : les trois formats, `from`/`to` inversés, boîte plate écartée, couleur explicite / teinte de feuillage / repli, géométrie Bedrock avec et sans l'extension attendue, rotation signalée, neuf entrées dégradées (JSON invalide, types faux, tableaux courts), bornes et centre, mannequin (12 boîtes toutes non dégénérées), et lecture de fichier distinguant **illisible** (`nullopt`) de **inexploitable** (modèle vide).
  - **18 suites : ALL TESTS PASSED.** `TeamLauncher.exe` **2 590 Ko** (2 652 672 o), **+27 Ko**. Avec `SDL2.dll` : **4,13 Mo**, plafond de 5 Mo respecté.
  - **3quater est terminé** : Explorateur, WorldSync, éditeur de cartes, WorldEdit, générateur de ville, ModDev et ModelViewer sont tous portés.
- [ ] **Signature de l'application : ABANDONNÉE** — décision de l'utilisateur le 27/09/2026, il n'a pas de certificat. Ne pas la reproposer. Conséquence à connaître : SmartScreen affichera un avertissement au premier lancement d'un binaire téléchargé.
- [x] **3quater — Outils** : TERMINE le 27/09/2026 — Explorateur (NBT/chunks/mondes, WorldSync, import), Exploration, cartes (editeur + WorldEdit + generateur de ville), ModDev, ModelViewer
- [x] **4 — Services & infra** : TERMINE (4a auth MS, 4a bis session, 4b Lang+Parametres, 4c maintenance, 4d CurseForge, 4e Exploration, 4f Onboarding, 4g raccourci+Discord, 4h partage de packs, 4i AdminService, plus AppTasks / ServerHost / InstanceDetail / mise a jour sans Velopack / durcissement securite)
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

## Mesures réelles — modules 4i → 4l (27/09/2026)

Build unique du **27/09/2026 à 10:38** (`cpp\build-release.bat`) : les quatre modules sont dans le même exécutable, leurs contributions **ne sont pas séparables**. Arbre de travail du 27/09/2026, qui contient aussi d'autres modifications en cours menées en parallèle — l'écart global n'est donc pas imputable uniquement à 4i–4l.

| Composant | Après 4h | Après 4i–4l | Écart |
|---|---|---|---|
| TeamLauncher.exe | 1 940 Ko | **2 064,0 Ko** (2 113 536 o) | **+124 Ko** |
| SDL2.dll | 1 637 Ko | **1 637,0 Ko** (1 676 288 o) | — |
| **Total disque** | 3,49 Mo | **3,61 Mo** (3 789 824 o) | plafond 5 Mo respecté ; cible haute de 3 Mo dépassée de 0,61 Mo |
| RAM idle | 80,4 Mo (mesuré après 4d) | **à mesurer au build final** | lancement de l'exécutable interdit dans cette tâche |
| PDB | — | **absent** (0 fichier `.pdb` dans `cpp/build`) | pas de debug symbols embarqués |

Outils et sources (instantané du 27/09/2026 vers 11:12 ; l'arbre est modifié en parallèle par d'autres chantiers, ces comptages ne sont donc figés nulle part ailleurs) :

| Élément | Relevé |
|---|---|
| Binaires de test | **9** exécutables `TLTest*.exe` dans `cpp/build` (DataStore, GamePort, MsAuth, Lang, Services, Packs, ServerHost, Apptasks, InstanceDetail), datés du 27/09 10:38 |
| Cibles de test déclarées | **10** dans `CMakeLists.txt` (`TL_TEST_TARGETS`) — `TLTestSecurity` est déclaré mais **absent** de `cpp/build` (cible ajoutée après ce build ou non construite) |
| Sources de test | **10** fichiers `cpp/tests/*.cpp` — 2 705 lignes non vides / 2 985 lignes au total |
| Sources | `cpp/src` : **42** `.cpp` (14 966 lignes non vides / 16 520) + **30** `.hpp` (1 539 / 1 922) = **72 fichiers, 16 505 lignes non vides** (18 442 au total) |
| Exécution des tests | **non relancée** dans le cadre de cette tâche (exécutions interdites) → statut des suites **non vérifié** ici |
| Captures 4i / 4j / 4k | `4i-ptero.png` 32 114 o (31,4 Ko), `4j-update.png` 57 069 o (55,7 Ko), `4k-detail.png` 40 866 o (39,9 Ko), `4k-check.png` et `4k-detail3.png` 40 858 o (39,9 Ko) — **aucune capture 4l** : le panneau des tâches n'apparaît que pendant une opération réelle |
| RAM idle / temps de démarrage / pic pendant un import | **à mesurer au build final** |

(mesures ponctuelles ; la RAM idle est affectée ±10 Mo par la machine)

## Étape 5 — cible Linux : inventaire et premiers pas (27/09/2026)

**Blocage matériel constaté** : cette machine n'a ni WSL ni chaîne de compilation Linux. Écrire les backends POSIX sans jamais les compiler n'aurait aucune valeur de vérification. Décision de l'utilisateur le 27/09/2026 : **installer WSL** (`wsl --install`, droits admin + redémarrage), puis reprendre le portage module par module avec compilation et tests réels, comme sous Windows.

### Inventaire des dépendances Windows

56 fichiers `.cpp` dans `cpp/src`, dont **30 touchent une API Windows**. Par ordre de densité :

| Fichier | Occurrences | Ce qu'il faudra côté POSIX |
|---|---|---|
| `http_win.cpp` | 50 | libcurl (ou une pile TLS) — le plus gros morceau |
| ~~`util_hash.cpp`~~ | ~~19~~ | **fait** : SHA-1 et MD5 portables |
| `game_launcher.cpp` | 15 | `posix_spawn` / `fork` + tubes |
| `moddev.cpp` | 11 | idem, via la couche plateforme |
| `ui.cpp` | 8 | sélecteurs de fichiers (portail XDG ou GTK), `ShellExecute` → `xdg-open` |
| `shortcut.cpp` | 8 | fichier `.desktop` au lieu d'`IShellLink` |
| `presence.cpp` | 8 | socket Unix `$XDG_RUNTIME_DIR/discord-ipc-N` (même format de trame) |
| `maintenance.cpp`, `game_installer.cpp`, `admin.cpp` | 6 | `/proc`, `uname`, `sysconf`, lancement de processus |
| `worldsync.cpp`, `secrets.cpp` | 5 | chemins CurseForge ; **DPAPI n'a pas d'équivalent** |
| `datastore.cpp` | 4 | XDG pour les chemins, `getrandom()` pour `BCryptGenRandom` |
| autres (18 fichiers) | 1 à 3 | conversions UTF-8/UTF-16 (sans objet sous Linux), `_stricmp` |

### Fait dans cette passe (vérifiable sous Windows)

- **SHA-1 et MD5 portables** (`src/util_hash.cpp`) : les deux algorithmes passaient par BCrypt. Plutôt que d'écrire une deuxième implémentation pour Linux ou d'ajouter OpenSSL en dépendance, il n'y en a plus qu'**une seule pour les deux plateformes**, ~120 lignes, sans dépendance système.
  - Verrouillée par les **vecteurs officiels des RFC** : SHA-1 sur chaîne vide, « abc », le vecteur à deux blocs de la RFC 3174, 55 / 56 / 64 octets (les cas de bourrage qui cassent les implémentations naïves) et le million de « a » ; MD5 sur la suite complète de la RFC 1321 plus 56 octets. Deux valeurs attendues que j'avais d'abord écrites de mémoire étaient fausses — recalculées, l'implémentation était juste.
  - `bcrypt.lib` reste liée : `datastore.cpp` utilise encore `BCryptGenRandom`. Ce sera `getrandom()` côté POSIX.

### Point dur identifié : le stockage du jeton

`secrets.cpp` chiffre le jeton de rafraîchissement Microsoft avec **DPAPI**, qui lie le secret au compte Windows. Il n'y a pas d'équivalent direct sous Linux. Trois options, **tranché le 28/09/2026** :
1. **libsecret / Secret Service** (GNOME Keyring, KWallet) — la bonne réponse, mais ajoute une dépendance et échoue sur une session sans trousseau.
2. **Fichier en 0600** sous `$XDG_DATA_HOME` — simple, mais c'est du stockage en clair protégé par les seules permissions. À ne faire qu'en le **disant explicitement** dans l'interface.
3. Redemander la connexion à chaque lancement sous Linux — sûr, mais c'est exactement ce que l'utilisateur voulait éviter.

Recommandation : libsecret quand il est disponible, repli sur le fichier 0600 avec avertissement visible.

**Décision 28/09/2026 : on applique la recommandation** — backend `libsecret` prioritaire, repli fichier `0600` sous `$XDG_DATA_HOME` avec avertissement visible dans l'interface (jamais de stockage silencieux en clair). `libsecret-1-dev` installé dans WSL le même jour ; le portage se fait module par module avec compilation et tests réels, `secrets.cpp` viendra avec son backend POSIX. Limite de test connue : pas de trousseau GNOME sous WSL, donc seul le repli 0600 sera exerçable dans WSL.

### Scaffolding CMake (28/09/2026)

- WSL : Ubuntu 26.04.1 LTS (WSL2) + `build-essential cmake ninja-build pkg-config libcurl4-openssl-dev libsecret-1-dev libgl1-mesa-dev` + dev X11 (g++ 15.2, cmake 4.2.3, ninja 1.13.2). Accès root sans mot de passe via `wsl -u root` (le `sudo` interactif de l'utilisateur `team` ne permet pas l'automatisation).
- `CMakeLists.txt` : liens `tl_core` et `TeamLauncher` conditionnés `if(WIN32)` / POSIX (`CURL::libcurl` + `libsecret-1` via pkg-config, `OpenGL::GL`) ; branche release non-MSVC nettoyée (`-fno-exceptions -fno-rtti` retirés — nlohmann/json et les workers utilisent try/throw, comme le rappelle le commentaire `/EHsc` côté MSVC).
- **`cmake -S cpp -B cpp/build-linux -G Ninja` configure avec succès sous WSL** : SDL2 vendue se configure (X11 ON, OpenGL ON, audio en repli OSS/dummy — suffisant pour un launcher), CURL 8.18, libsecret 0.21.7, OpenGL trouvés. `cpp/build-linux/` est couvert par `.gitignore` (`build-*/`).
- La compilation échouera sur les sources Windows-only (attendu) : ordre suggéré des backends POSIX — `util_str`/`datastore` (XDG + `getrandom()`), `shortcut` (`.desktop`, petit), `presence` (socket Unix), `game_launcher`/`moddev` (`posix_spawn`), `ui.cpp` (`xdg-open` + portail XDG), `secrets` (libsecret/0600), `http_win` (libcurl, le plus gros) en dernier.

### Backend 1 fait (28/09/2026) : `datastore` + `util_str`

- `datastore.cpp` : `_stricmp` → `strcasecmp` (via helper `strCaseCmp`), `exeDir()` via `/proc/self/exe`, `dir()` XDG (`$XDG_DATA_HOME` sinon `~/.local/share`, `TL_DATA_DIR` et `--portable` inchangés), `doSave()` via `rename()` atomique, `new_guid()` via `getrandom()` + repli `random_device`. Branche Windows inchangée (build + 17/18 suites OK, voir incident ci-dessous).
- `util_str.hpp` : même API, implémentation POSIX manuelle UTF-8 ↔ UTF-32 (sans iconv), surrogates/hors-plage ignorés.
- `tests/test_datastore.cpp` : section 8 `#ifndef _WIN32` (XDG, repli HOME, format GUID, round-trip UTF y compris astral).
- **Vérifié sous WSL** : `datastore.cpp` + `test_datastore.cpp` compilent (g++ 15.2, `-fsyntax-only`) ; smoke test `/tmp/tl-smoke1` (`SMOKE ALL PASSED`) sur le vrai `datastore.cpp` — XDG, 1000 GUID uniques 32-hexa, round-trip save/load sans `.tmp` résiduel, UTF — avec un stub `secrets` au contrat identique (le vrai backend arrive avec le module `secrets`).
- **Incident non lié** : `TLTestRegion.exe` échoue sur cette machine (`test_region.cpp:423` — `r.-5.-9.mca` réel à 0 chunk parsable). Dépend des données monde locales, aucun fichier region/nbt modifié ici.

### Backend 2 fait (28/09/2026) : `shortcut` (.desktop)

- `shortcut.cpp` : branche Windows inchangée (COM/IShellLink) ; branche POSIX = fichier `.desktop` freedesktop (`Type/Name/Exec/Path/Comment/Terminal=false/Categories=Game;`, quoting si espaces, bit `+x`, `.lnk` d'entrée reécrit en `.desktop`). `desktop_dir()` via `XDG_DESKTOP_DIR` de `~/.config/user-dirs.dirs` (guillemets + `$HOME`, commentaires) sinon `~/Desktop` ; `exe_path()` via `/proc/self/exe`. Pas de clé `Icon` : le seul asset est un `.ico` Windows, invalide ici. `ensure_desktop_shortcut()` écrit `Team Launcher.desktop`, même sémantique « une fois sauf force » (appelants `ui.cpp`/`ui_settings.cpp` inchangés).
- **Vérifié sous WSL** : compile (g++ 15.2) ; smoke test `/tmp/tl-smoke2` (`SMOKE2 ALL PASSED`) sur le vrai `shortcut.cpp` + `datastore.cpp` — parsing user-dirs, contenu .desktop, bit +x, remap .lnk, ensure/force/flag.
- **Windows** : build OK, 17/18 suites (même incident `TLTestRegion` pré-existant, inchangé).

### Backend 3 fait (28/09/2026) : `presence` (socket Unix)

- `presence.cpp` : classe `Pipe` à interface identique des deux côtés (worker/handshake/SET_ACTIVITY inchangés). Branche POSIX = socket Unix `AF_UNIX` sur `$XDG_RUNTIME_DIR/discord-ipc-N` (repli `/tmp`), N 0→9, non-bloquante + `poll()` (mêmes délais que `PeekNamedPipe`), `send(..., MSG_NOSIGNAL)` (pas de SIGPIPE), garde-fou 1 Mio conservé. `GetCurrentProcessId()` → helper `self_pid()` (`getpid()`), `_stricmp` → helper `strCaseCmp()` — ce dernier a cassé le build Windows une fois (`strcasecmp` nu n'existe pas sous Windows), corrigé et rebuildé vert.
- **Vérifié sous WSL** : compile (g++ 15.2) ; smoke test `/tmp/tl-smoke3` (`SMOKE3 ALL PASSED`) bout-en-bout face à un faux serveur Discord — échec propre sans serveur (`connected()==false`), handshake (`client_id` vérifié), `SET_ACTIVITY` (`details` + `pid` vérifiés), `clear` final (`activity: null`) au `shutdown()`.
- **Windows** : build OK, 17/18 suites (même incident `TLTestRegion` pré-existant, inchangé).

### Backend 4 fait (28/09/2026) : `game_launcher` + `moddev` (posix_spawn)

- Nouveau `src/proc.hpp` (header-only, vide sous Windows) : `spawn()` (posix_spawnp + pipes `O_CLOEXEC`, stdin `/dev/null`, dossier via `addchdir_np` glibc avec repli `sh -c 'cd "$0" && shift && exec "$@"'`), `spawn_shell()`, `run_capture()` (capture fusionnée + délai, tue + moissonne, jamais de zombie), `wait_exit()`, `terminate_child()` (SIGKILL), `close_fd()`. Pas de `fork()` (dangereux en multithread).
- `game_launcher.cpp` : `localtime_r`, `sysinfo()` (RAM), `bin/java` + bit `+x` (`javaw.exe` sous Windows), `detect_java_major` via `run_capture` (même regex, 5 s), `find_java` (JAVA_HOME, `/usr/lib/jvm`, `/usr/java`, java du PATH ; pas de balayage `/opt`), URL Adoptium `linux/x64`, séparateurs `/` et `:` dans `build_jvm_args`, `GameProcess` en `void*` inchangé (fds/pid castés, même layout), `start_game`/`wait_game`/`close_game`/`start_game_log_writer` POSIX (`close_game` ne ferme pas les fds des pumps, comme Windows).
- `moddev.cpp` : `widen()`/`which()`/textes `.bat`→sans extension + `JAVA_HOME/bin/java` + `sh -c` dans `run()` (pompe ligne à ligne et annulation identiques), `strCaseCmp`, README Bedrock au chemin générique (pas de Bedrock UWP sous Linux).
- **Vérifié sous WSL** : les deux TU compilent (g++ 15.2) ; smoke test `/tmp/tl-smoke4` (`SMOKE4 ALL PASSED`, 14 points) sur le vrai code — spawn/capture/timeout/kill, `detect_java_major` 21/8/0 sur faux JDK, `find_java` via JAVA_HOME, `moddev::run` + `detect_toolchain` factices, `GameProcess` bout-en-bout (`/bin/echo` + log writer), `sysinfo`, séparateurs, `offline_session`.
- **Windows** : build OK, 17/18 suites (même incident `TLTestRegion` pré-existant, inchangé).
- Hors périmètre (leurs backends viendront) : `game_installer.cpp` (son propre helper spawn + chemins), `util_zip.cpp` (`_stricmp` l.35), `http_win.cpp` — stubbés dans le smoke.

### Backend 5 fait (28/09/2026) : `ui` (sélecteurs + xdg-open)

- Nouveau `src/dialogs.cpp` (extrait de `ui.cpp`, ajouté à `TL_CORE_SOURCES` + exclusion GLOB) : les 7 fonctions Windows déplacées **verbatim** (comportement inchangé), branches POSIX à côté. `wstr_to_utf8()` devient un wrapper de `tl::wide_to_utf8` (portable, plus d'ifdef). `pick_folder()` : signature `wchar_t*` (Windows) / `char*` (POSIX) dans `ui_internal.hpp` + appelant `ui.cpp` adapté.
- POSIX : `zenity` puis `kdialog` en sous-processus (`proc::run_capture`, modal comme `GetOpenFileName`), filtres/titres/défauts conservés, `.zip` ajouté comme `lpstrDefExt`, annulation/outil absent → `nullopt`. `open_in_explorer()` = `xdg-open` fire-and-forget (pas d'attente, comme `ShellExecuteW`). Portail D-Bus documenté comme amélioration future (pas de dépendance ajoutée).
- **Vérifié sous WSL** : compile (g++ 15.2) ; smoke test `/tmp/tl-smoke5` (`SMOKE5 ALL PASSED`) sur le vrai `dialogs.cpp` — faux `zenity` instrumenté (chemins + args `--file-selection`/`*.zip`/`--directory`/titres vérifiés), annulation, `.zip` auto, faux `xdg-open` (chemin avec espace intact), repli nullopt sans outils, `wstr_to_utf8` UTF-32.
- **Windows** : build OK, 17/18 suites (même incident `TLTestRegion` pré-existant, inchangé). Leçon outillage : `/tmp` ne persiste pas entre deux invocations `wsl` de l'outil → build+run regroupés en un seul script désormais.
- Reste UI sous Windows (leurs backends viendront avec `ui.cpp` complet) : `ui_settings.cpp`/`ui_skins.cpp`/`ui_packs.cpp` (leurs `wstr_to_utf8` locaux), `ui_moddev.cpp:222` (`pick_folder` wchar).

### Backend 6 fait (29/09/2026) : `secrets` (libsecret + repli 0600)

- Même API (`dpapi_protect_b64`/`unprotect`, `encrypt_value`, `secure_wipe`) : Windows inchangé ; POSIX = base64 manuel + `protect` qui stocke au trousseau (`org.teamlauncher.Secret`, attribut `key`=uuid) et rend base64(`"ks:<uuid>"`), repli fichier `0600` (`secrets.local` XDG, `{"version":1,"secrets":{...}}`) rendant base64(`"local:<uuid>"`) + flag `insecure_fallback_active()` pour l'avertissement UI. Le blob ne contient jamais le secret sous Linux (référence seule). `+ insecure_fallback_active()=false` sous Windows, `tl_schema()` sans warning via `call_once`.
- **Vérifié sous WSL** (headless → repli local exercé) : smoke `/tmp/tl-smoke6` (`SMOKE6 ALL PASSED`) — vecteurs base64 (+binaire 0-255, rejets), round-trip, préfixe `enc:v1:`, compat clair, fichier en 0600 sans droits groupe/autres, blob illisible conservé, `secure_wipe`.

### Backend 7 fait (29/09/2026) : `http_win` (libcurl)

- Bloc S2 (allowlist/redaction, ~200 lignes) conservé tel quel (portable) ; seul le transport part en `#ifdef` : `dbg()` (erreurs curl), `Easy`/`Slist` RAII, `curl_global_init` once, `do_request` (UA 6.0, `PROTOCOLS_STR`+`REDIR_PROTOCOLS_STR`=http/https, timeouts 15/60 s, `FOLLOWLOCATION` 10, annulation via xfer-callback, corps lu quel que soit le statut comme WinHTTP) et `get_to_file` (200 exigé, progression, retries 500·attempt, pas de partiel). Hôte S2 via `host_from_url()` partagé (pas de parseur ad hoc).
- **Vérifié sous WSL** : smoke `/tmp/tl-smoke7` (`SMOKE7 ALL PASSED`) — allowlist/refus/schémas hors réseau, annulation pré-armée, GET 200/404 réels, chemin POST (404 JSON, sans rien spammer), téléchargement + progression + pas de partiel.

### Backend 8 fait (29/09/2026) : `game_installer` + `util_zip`

- `runtime_root()` XDG, `localtime_r`, `strcasecmp`, `run_jar_installer` via `proc::spawn` + pompes parallèles (exposé au header pour les tests), `rules_allow` sur `kOsName` (windows/linux), séparateurs maven `/`, clé `natives-linux` (+ slash natifs intacts), messages `forge-install.log` au bon slash. `zip_extract_dlls` → `zip_extract_natives` (`.dll`/`.so`, compare portable) + `<cctype>`. Tests `test_game_port` OS-aware (rules kOs/kOther + cas deny-autre ajouté, `TL_DATA_DIR`/`TL_RUNTIME_DIR` aussi sous Linux, zip `.so`).
- **Vérifié sous WSL** : smoke `/tmp/tl-smoke8` (`SMOKE8 ALL PASSED`, 10 points) — maven/rules/natives `.so` + zip-slip, `runtime_root`, `run_jar_installer` (faux java, stdout+stderr).
- **Windows** : inchangé de comportement (branches `#ifdef`), suites au §12.

### Backend 9 fait (29/09/2026) : `maintenance` + `admin`

- `maintenance.cpp` : disque via `statvfs` (détail sans lettre de volume), `wstr_to_utf8_local` → `tl::wide_to_utf8` (partagé, plus d'ifdef), score d'assets inversé (linux+x64 préférés, win/macos/arm pénalisés), message « paquet Linux », `install_staged_and_restart` POSIX = staging (`.tl-update-<pid>`, ménage orphelins) + renames atomiques + `+x` + ménage + relance double-fork/setsid (explication : `rename()` autorisé sur exe mappé, `open(O_TRUNC)` interdit `ETXTBSY` — trouvé au strace après un faux succès à 0 fichier ; le test exige désormais `nWrote > 0`). Header `maintenance.hpp` documenté (bat Windows vs in-process Linux).
- `admin.cpp` : `machine_info()` POSIX — `gethostname`, `PRETTY_NAME` + noyau (`uname`), `model name` de `/proc/cpuinfo`, GPU = 1er PCI classe `03xx` sous `/sys` (NVIDIA/AMD/Intel + IDs, sans pci.ids).
- **Vérifié sous WSL** : smoke `/tmp/tl-smoke9` (`SMOKE9 ALL PASSED`) — health disque `3.9 Go libres`, select linux/win-only, `machine_info` (host/OS/CPU/PCI/RAM), **self-update réel** (staging + rename par-dessus l'exe en cours + relance + flag). `strace` installé dans WSL au passage.
- Tests `test_services.cpp` OS-aware (asset linux ajouté au mix, `parse_release_json` par OS, section raccourci `.desktop`/contenu/`HOME` isolé).

### Backend 10 fait (29/09/2026) : `open_url` groupé

- `proc::open_detached()` (xdg-open fire-and-forget) dans `proc.hpp`, réutilisé par `dialogs.cpp`. `ms_auth::open_browser` + `ui_auth::open_url` (garde allowlist conservée) + `ui_news` (sans garde, comme avant) → `open_detached`. `main.cpp` : console-hide en `#ifdef`. `telemetry.cpp` : `OSVERSIONINFOEXW` en `#ifdef`, label OS = `PRETTY_NAME` sinon `Linux`, `localtime_r`/`gmtime_r` (+ 2 autres `localtime_s` oubliés trouvés par le compilateur : `ms_auth` non gardé idem).
- **Vérifié sous WSL** : les 5 TU compilent ; smoke `/tmp/tl-smoke10` (`SMOKE10 ALL PASSED`) — `open_browser` autorisé/refusé (faux xdg-open), payload telemetry POSTé sur webhook local avec `OS : Ubuntu`. Smoke5 re-joué vert (non-régression `open_detached`).

### Backend 11 fait (29/09/2026) : `ui_servers` (BSD sockets)

- Alias `Socket`/`kBadSocket`/`close_sock`, presse-papiers via ImGui (SDL, portable) au lieu de `CF_TEXT`, `WSAStartup` en `#ifdef`, `ioctlsocket`→`fcntl`, `WSAEWOULDBLOCK`→`EINPROGRESS`, `select(s+1)` (ignoré sous Windows, requis POSIX), `socklen_t`, timeouts `timeval`. `query_slp` exposé dans `ui_internal.hpp` (sorti du namespace anonyme, comme `run_jar_installer`) pour les tests.
- **Vérifié sous WSL** : compile ; smoke `/tmp/tl-smoke11` (`SMOKE11 ALL PASSED`) sur le **vrai** `query_slp` (link ui_servers+imgui+lang+server_host, 2 stubs UI) face à un faux serveur SLP python — handshake/statut complets (joueurs/version/motd), port fermé, adresse vide, annulation.

### Backend 12 fait (29/09/2026) : reste UI + broutilles + build Linux complet

- API générique `pick_file_open`/`pick_files_open`/`pick_file_save` dans `dialogs.cpp` (Windows : commdlg dynamique équivalent ; POSIX : filtres zenity/kdialog + `--multiple`/`--separate-output` + `ensure_ext`) ; `ui_packs`/`ui_settings`/`ui_skins` branchés dessus (branches Windows verbatim conservées). `ui_moddev` littéral `pick_folder` ifdef. `tl::strCaseCmp` centralisé dans `util_str.hpp` (`pack_share` ×2, `worldsync` ×4, `ui.cpp`, `ui_explorer`). `localtime_r` : `backup`, `worldsync`, `ui_skins`, `ui.cpp`, `ui_explorer`, `ui_home`, `ui_instances`(tri `strCaseCmp`), `ui_bedrock` dégradé (pas d'UWP : page « non installé », OPTIMISER neutre, `shell_open` narrow). `poll_game_exit()` (waitpid WNOHANG, sémantique Windows préservée à l'identique). Includes Win32 gardés dans les 5 pages restantes. `test_env.hpp` (shim `_putenv_s` portable) : gardes `#ifdef` retirées dans 6 fichiers de test, `<cmath>` pour `test_worldedit`.
- **Build Linux COMPLET réussi** (`cpp/build-linux`) : `TeamLauncher` 2,5 Mo + 19 suites. Correctifs révélés par le build : flags SDL `/clang:` gardés Clang-only, `SecretSchema` via `call_once` (warnings), 3 `localtime_s` oubliés, `test_env.hpp`, `worldsync` §12b, tests OS-aware (game_port/moddev/services).
- **Bugs prod réels trouvés grâce au portage** (affectaient aussi Windows en latence) :
  1. `worldsync::list_worlds_in` : `is_regular_file(manquant, ec)` pose `ENOENT` sous libstdc++ → `if (ec) break` avortait le scan (ordre ext4 vs alphabétique Windows masquait) — `ec` local désormais. Audit des 22 boucles sœurs : seul ce site avait le piège (les autres testent des entrées existantes).
  2. `install_staged_and_restart` : extraction à 0 fichier traitée comme succès (strace : `ETXTBSY` sur l'exe en cours) — staging + rename + exigence `nWrote > 0`.
  3. `file_time_to_unix()` (datastore.hpp, `clock_cast`) : l'epoch `file_time_type` n'est pas Unix (négatif sous libstdc++, 1601 sous MSVC) — `mtime_of` (backup/worldsync) corrigé des deux côtés.
- **Linux : 19/19 suites vertes.** Corrigés pour ça : `rules`/séparateur/`_putenv_s`/commandes moddev (127 vs -1 documenté)/assets/services-raccourci + `TL_DATA_DIR` isolé partout (les premiers runs avaient écrit dans le HOME WSL `team` — jamais le config Windows réel — session-cache/secrets.local : à nettoyer).
- **Windows : build OK, 17/18** (même incident `TLTestRegion` sur données locales ; `TLTestGamePort.exe` supprimé puis verrouillé par **Bitdefender** — le nom reste inutilisable dans `cpp/build` malgré le déblocage annoncé : quarantaine non relâchée. **Validé quand même** en linkant le même `.obj` vers Temp : `ALL TESTS PASSED` (Java 8 détecté). À régler : restaurer/exclure le nom d'origine dans Bitdefender).

### Rebranchement AppTasks fait (29/09/2026) : launch, news, ptero, ping

Contrat existant (`apptasks_begin` + relais `apptasks_frame` + `apptasks_end` par le worker) étendu aux 4 derniers workers utilisateurs. Télémétrie et daemons (ping_worker de fond, heartbeat admin) exclus volontairement : pas d'action utilisateur, pas d'annulation sensée.
- **Lancement** (`ui.cpp`) : entrée `Lancement de <nom>` + `&g.cancel` branché, miroir progression/statut (`tasks::update`, fraction clampée), clôture dans le worker (Done/Annulée/erreur). `#include "apptasks.hpp"` ajouté.
- **News** (`ui_news`) : champ `taskId` dans `NewsState`, `news_worker(tid)`, fin Done/Annulée (cache offline = succès).
- **Ptero Power/Command** (`ui_servers`) : `PteroTask` += `taskId` + `shared_ptr<atomic>` (contrat de durée de vie), helper `ptero_enqueue_user`, worker avec pré-check d'annulation, fin systématique y compris shutdown (anti use-after-free du relais), Refresh silencieuses inchangées, toasts conservés (sauf annulation).
- **Ping batch** (`ui_servers`) : champs `pingCancel/pingTaskId/pingDone/pingTotal` (déjà prévus) branchés — entrée par lot, progression, marquage `cancelled` du reliquat, fin au drain. `query_slp` + `queue_pings` sortis du namespace anonyme (exposés, sans collision vérifiée).
- **Vérifié** : build Windows OK (17/18 + GamePort bloqué AV, inchangé), Linux 19/19, smoke `/tmp/tl-smoke12` (`SMOKE12 ALL PASSED`) — vrai lot ping → Done 1.0 + résultats (faux serveurs SLP). Leçon : un worker infini doit être arrêté avant la fin du main sous peine de `terminate` (abort vu puis corrigé dans le smoke).

## Installeur Windows (27/09/2026)

`cpp/installer/TeamLauncher.iss` + `cpp/make-installer.ps1`. Inno Setup 6, déjà présent sur la machine.

**Résultat : `TeamLauncher-6.0.0-Setup.exe`, 3 425 Ko.** Il contient toute l'application : l'exécutable (2 665 Ko), `SDL2.dll` (1 637 Ko) et `assets/default.env`, compressés en LZMA2. À comparer au C# : ~46 Mo en AOT, ou un installeur léger mais exigeant le runtime .NET 8.

Ajouté au passage : `cpp/src/app.rc`, ressources Win32 (icône + bloc `VERSIONINFO`). Sans icône, le raccourci et l'entrée « Applications installées » restaient vides, et `make-installer.ps1` s'en sert pour vérifier que le binaire et l'installeur annoncent bien la même version — un build périmé est refusé plutôt qu'emballé.

Corrections par rapport au script du C# (`installer.iss` à la racine) :

1. **Plus de dépendance .NET.** L'ancien embarquait `*.deps.json` et `*.runtimeconfig.json`, et définissait une fonction `IsDotNet8Installed` **jamais appelée** : rien ne vérifiait le runtime, l'installation réussissait sur une machine sans .NET et l'application échouait au premier lancement.
2. **`taskkill /f` retiré.** `InitializeSetup` tuait de force `TeamLauncher.exe` avant même le premier écran, donc y compris quand l'utilisateur annulait ensuite — au risque de perdre une configuration en cours d'écriture. `CloseApplications` demande poliment la fermeture.
3. **`Source: "dist\*.dll"` remplacé par des fichiers nommés un par un** : le joker embarquait tout ce qui traînait dans le dossier de sortie.
4. **Désinstallation propre** : `launcher.log`, écrit à côté de l'exécutable, laissait le dossier d'installation derrière lui.

### Incident du 27/09/2026 — perte de données pendant un test

Le premier script proposait à la désinstallation de supprimer aussi `%LOCALAPPDATA%\TeamLauncher`, avec « Non » comme bouton par défaut (`MB_DEFBUTTON2`). J'ai lancé la désinstallation de test avec `/SUPPRESSMSGBOXES` **en supposant** que la suppression des boîtes de dialogue renverrait ce défaut.

**C'est faux : Inno Setup renvoie `IDYES` pour un `MB_YESNO` supprimé, sans tenir compte du bouton par défaut.** Les données réelles de la machine (réglages, session Microsoft, clé API CurseForge, dossier d'instances) ont été effacées. `DelTree` ne passe pas par la corbeille ; aucun cliché instantané ni point de restauration n'existait. **Irrécupérable.**

Deux garde-fous, vérifiés par un test qui installe, désinstalle en silencieux et contrôle qu'un fichier témoin survit :

1. **`if UninstallSilent then Exit`** — aucune automatisation ne peut détruire des données sans clic humain. C'est le garde-fou qui compte.
2. **Deux confirmations successives**, la seconde énumérant ce qui sera perdu, toutes deux avec « Non » par défaut.

Leçon générale, au-delà de l'installeur : **ne jamais tester une action destructrice sur les données réelles de la machine**, même quand on croit que le mode silencieux choisit l'option sûre. Le test aurait dû tourner avec `TL_DATA_DIR` pointant sur un dossier jetable.

### Non signé

Décision de l'utilisateur du 27/09/2026 : pas de certificat de signature de code. SmartScreen avertira au premier lancement d'un installeur téléchargé. Attendu, ce n'est pas un défaut du script.

### Reste à faire

- **Logo** : l'icône actuelle vient du projet C# et n'est plus la bonne. Le nouveau logo doit remplacer `cpp/assets/TeamLauncher.ico` et servir aussi d'illustration Discord Rich Presence.
- ~~**Installeur Linux**~~ : **fait le 29/09/2026** (voir section suivante).

## Installeur Linux (29/09/2026)

`cpp/installer/linux/` : `teamlauncher.desktop.in` (validé `desktop-file-validate` RC=0), `make-deb.sh`, `install.sh` (+ logo `cpp/assets/teamlauncher.png`, frame 256 px extraite du `.ico` en stdlib python — le rendu est bon).
- Layout unifié : `<root>/teamlauncher/{TeamLauncher,assets/,lib/libSDL2*}`, lien `<prefix>/bin/teamlauncher` (symlink : `/proc/self/exe` résout le réel, `exeDir/assets` reste correct). SDL embarquée via `rpath $ORIGIN/lib` (CMake, cible Linux uniquement) : pas de `LD_LIBRARY_PATH` (qui fuiterait vers le jeu lancé), pas de dépendance libsdl2 système (ABI figée). `Depends: libcurl4, libsecret-1-0, libglib2.0-0, libgl1` (dérivés du `readelf NEEDED`).
- `make-deb.sh` (sans root) : `teamlauncher_6.0.0_amd64.deb`, **1,8 Mo** (binaire 2,5 Mo + SDL 2,6 Mo + assets). Testé : `dpkg -i` OK, `ldd` résout tout (SDL via rpath), `.desktop` valide, symlink OK. AppImage écarté (FUSE + outil externe, besoin couvert par .deb + install.sh).
- `install.sh` : mode utilisateur (`~/.local`, testé : layout + .desktop + icône + rpath OK) et `--system` (`/opt`, root).
- **Jalon : premier run graphique Linux** (WSLg) : la fenêtre s'ouvre et tourne (le « hang » headless était la boucle principale en attente d'input !), capture `TL_SCREENSHOT` OK — onboarding + page Instances + chemin XDG corrects.
- Reste : signature éventuelle du .deb (non signé, comme Windows — décision à prendre), dépôt APT (hors sujet pour l'instant).

## Phase 7 — Démarrage, aide, mises à jour (30/09/2026)

Sept points de la liste v7, tous **déclarés dans `AppSettings` depuis la phase A mais jamais branchés** : `closeBehavior`, `launchAtSystemStart`, `startupGame`, `lastGameId`, `dateFormat`, `updateChannel`, `updateFreqHours`. Même motif que `ideal_ram_gb`, `maxDownloads` ou `backupSpaceMb` avant eux : le champ existait, se sauvegardait, se rechargeait, et ne servait à rien.

Deux fonctions du même acabit ont été trouvées au passage, **déclarées dans `maintenance.hpp` et jamais définies** : `updates::deployment()` et `updates::install_dir_writable()`. Le lien n'a cassé qu'au moment où la page Aide a voulu afficher « installé / portable ». Elles sont maintenant écrites : présence d'`unins000.exe` à côté de l'exécutable sous Windows, préfixe `/usr` ou `/opt` sous Linux ; et pour l'inscriptibilité, une écriture réelle plutôt qu'une lecture de permissions (ACL, virtualisation et montages en lecture seule mentent).

### Nouveaux modules

- **`startup.hpp/.cpp`** — entrée de démarrage de session : valeur `TeamLauncher` sous `HKCU\...\CurrentVersion\Run` (Windows) ou `~/.config/autostart/teamlauncher.desktop` (Linux). Aucun droit d'administrateur, rien hors du profil. Le launcher est lancé avec `--autostart`, que `main()` traduit par « fenêtre réduite » : surgir au premier plan à chaque ouverture de session serait insupportable. **L'état est relu à la source, pas déduit du réglage** — un profil recopié ou un nettoyeur de démarrage peut avoir retiré l'entrée, et afficher « activé » serait un mensonge.
- **`support.hpp/.cpp`** — rapport machine et export zip des journaux (`rapport.txt`, fin de `launcher.log` plafonnée à 4 Mo, `config-expurge.json`). L'expurgation est récursive, tableaux compris (`PteroHosts` est un tableau d'objets qui portent des clés d'API) et **distingue « (masqué) » de « (vide) »** : savoir qu'un réglage n'est pas renseigné est souvent tout le diagnostic.
- **`syscolor.hpp/.cpp`** — barre de titre accordée au système (voir plus bas).
- **`ui_help.cpp`** (page 17) — centre d'aide, ticket, suggestion, salon Discord (réglable, bouton grisé tant qu'il n'est pas configuré : mieux qu'un lien d'invitation inventé), export des journaux, « Quoi de neuf », raccourcis vers le diagnostic et les mises à jour, et sortie explicite.

### Canal bêta

`/releases/latest` **ne renvoie jamais de préversion**, quoi qu'on lui demande : le canal bêta interroge donc `/releases`, qui rend un tableau. `parse_release_json` accepte maintenant les deux formes. Sur un tableau : les brouillons sont toujours écartés, les préversions seulement en stable, et **c'est la plus haute version qui gagne, pas la plus récemment publiée** — GitHub trie par date de publication, donc un correctif sorti après coup sur une ancienne branche passerait devant la version la plus récente.

### Fermeture : défaut inversé

Le champ valait `"minimize"` par défaut depuis la phase A. Sans icône de zone de notification — SDL2 n'en propose pas, SDL3 oui —, une fenêtre qui ne disparaît pas quand on clique sur la croix passe pour une panne. Le défaut est donc passé à `"quit"`, et le mode « réduire » reste offert, avec une bulle qui dit une fois par session où se trouve la sortie, et un bouton « Quitter le launcher » dans la page Aide qui n'apparaît **que** dans ce mode.

`SDL_QUIT` et `SDL_WINDOWEVENT_CLOSE` arrivent tous les deux sur un clic sur la croix : les deux passent par le même arbitre.

### Vérification périodique des mises à jour

`updateFreqHours` pilote enfin quelque chose. La date de dernière tentative (`LastUpdateCheckUnix`) est écrite **avant** la requête : un réseau coupé ne doit pas faire retenter à chaque démarrage. Le résultat n'est pas affiché depuis le fil de fond — le toast vit dans l'état de l'interface, sans verrou — mais déposé et consommé à la frame suivante. Échec réseau : silence. Une vérification que personne n'a demandée n'a pas à interrompre qui que ce soit pour annoncer que le réseau est coupé.

### Barre de titre accordée au système (demande du 30/09/2026)

La barre du haut n'est pas dessinée par le launcher : c'est le système qui la rend. On ne peut donc pas la peindre, seulement lui dire quelles couleurs employer.

- **Windows** : `DwmSetWindowAttribute` (chargée à la demande depuis `dwmapi.dll`, pas de bibliothèque d'import en plus) avec mode sombre immersif, couleur de légende, de texte et de bordure. Les valeurs viennent de `Themes\Personalize\SystemUsesLightTheme` — et **non** `AppsUseLightTheme`, qui gouverne les applications et non les barres de titre, les deux pouvant différer —, de `ColorPrevalence` (« afficher la couleur d'accentuation sur les barres de titre ») et de `DWM\AccentColor`. Quand la prévalence est désactivée, on **rend** la légende à Windows (`0xFFFFFFFF`) au lieu de lui imposer une teinte : c'est le réglage de l'utilisateur, pas le nôtre. Deux inversions d'ordre d'octets à ne pas rater : `AccentColor` est en `0xAABBGGRR`, `COLORREF` en `0x00BBGGRR`.
- **Linux** : la décoration est dessinée par le gestionnaire de fenêtres selon le thème du bureau. Elle est **déjà** native ; une application qui tenterait de la repeindre s'en écarterait. `query()` y renvoie tout de même la préférence clair/sombre.

Relu une fois par seconde : l'utilisateur peut basculer clair/sombre pendant que le launcher tourne.

### Numéro de version retiré de deux endroits

Demande du 30/09/2026 : le titre de la fenêtre (`"Team Launcher v6.0.0"` → `"Team Launcher"`) et le coin haut-gauche de la barre latérale. Il occupait la place la plus visible de l'écran sans rien apporter au quotidien. Il reste lisible page Aide et Paramètres > Intégrations.

### Tests

`tests/test_support.cpp` (`TLTestSupport`) : reconnaissance des clés sensibles **et absence de faux positifs** (`PlayerName`, `MaxRamGb`, `NewsUrl` doivent rester lisibles, sinon le rapport ne sert plus à rien), expurgation récursive, choix de release sur une liste volontairement désordonnée, rapport sans secret, archive contenant bien ses trois pièces et dossier de travail effacé. L'écriture réelle de l'entrée de démarrage est derrière `TL_TEST_AUTOSTART=1` — elle sort du bac à sable du test — et remet l'utilisateur dans l'état où elle l'a trouvé.

**22 suites vertes sous Linux, 21 sous Windows** (`TLTestGamePort.exe` reste en « suppression en attente » sur cette machine, indépendamment du code).

## Phase 5 — Mods et découverte (30/09/2026)

La seule phase à laquelle rien n'avait été fait. Elle reposait sur une brique qui manquait entièrement : **le launcher ne savait pas lire un mod**. Il listait des fichiers `.jar` et c'est tout. Or le nom de fichier ne dit rien — « Sodium 0.5.8 pour 1.20.1 » peut s'appeler `sodium.jar` et avoir été téléchargé pour une autre version.

### `modmeta` — lire ce qu'un mod déclare

Cinq formats de manifeste coexistent selon le chargeur : `fabric.mod.json`, `quilt.mod.json`, `META-INF/mods.toml` (Forge), `META-INF/neoforge.mods.toml` (NeoForge depuis 1.20.5) et `mcmod.info` (≤ 1.12). Tous sont lus. Les analyseurs sont **purs** — ils prennent du texte, pas un chemin — donc testables sans fabriquer d'archive.

Beaucoup de mods multi-chargeurs embarquent **plusieurs** manifestes dans le même jar. L'ordre d'essai n'est donc pas arbitraire : NeoForge avant Forge, Quilt avant Fabric, parce que le plus récent décrit mieux ce que le jar sait faire.

L'analyseur TOML est volontairement minimal : ces fichiers sont écrits à la main par les moddeurs et n'emploient qu'une poignée de constructions. Embarquer une bibliothèque TOML complète aurait coûté plus qu'elle n'aurait rapporté. Ce qui n'est pas compris est ignoré, jamais deviné.

**Comparaison de versions.** Elle devait couvrir les intervalles Maven de Forge (`[1.20,1.21)`), les prédicats semver de Fabric (`>=1.20 <1.21`, `~1.20.1`, `^1.20`), les alternatives (`||`), les jokers (`1.20.x`) et les versions nues (que Fabric lit comme une égalité). Trois pièges :

- `1.9` vs `1.10` : comparaison **numérique**, jamais textuelle.
- `1.0` est plus récent que `1.0-beta1` — un suffixe marque une préversion, pas un incrément.
- La virgule d'un intervalle Maven **n'est pas** un séparateur de contraintes. Couper sur toutes les virgules casserait `[1.20,1.21)` en deux.

Une contrainte sans aucun chiffre (`latest`, une syntaxe d'un chargeur qu'on ne connaît pas) est **acceptée** : bloquer un lancement sur une chaîne qu'on n'a pas comprise serait pire que se taire. Le test l'exigeait, le code ne le faisait pas — c'est le code qui avait tort.

### `modcheck` — le conflit dit avant, pas après

Sept contrôles : mauvais chargeur, version de Minecraft hors plage, dépendance obligatoire absente, dépendance présente mais dans la mauvaise version, **même modId dans deux jars**, archive illisible, et le cas « aucun chargeur sur l'instance ».

Nuances qui évitent les faux positifs, et qui comptent plus que les contrôles eux-mêmes :

- **Quilt charge les mods Fabric.** Ce n'est donc pas un conflit.
- **Forge et NeoForge** ont divergé mais restent souvent compatibles : avertissement, pas blocage.
- `minecraft`, `java`, `fabricloader`, `forge`… sont fournis par la plateforme. Les chercher dans le dossier mods produirait une dépendance manquante à chaque ligne.
- Instance sans chargeur : **un** message, pas quarante. Minecraft n'ouvrira même pas le dossier mods/.
- Une dépendance **facultative** absente n'est pas un problème : c'est une suggestion. C'est de là que viennent les « compléments possibles », tirés de ce que les mods déclarent — pas d'un catalogue inventé.

Le barrage tourne **dans le worker de lancement**, pas sur le fil de l'interface : ouvrir une archive par mod prend le temps qu'il faut, et figer la fenêtre juste après un clic sur Jouer serait inacceptable. En cas de refus, la modale montre tout, propose « Corriger les mods », « Annuler » et — volontairement à droite, sans accent — « Lancer quand même ». Notre analyse peut se tromper, et l'utilisateur garde la main sur sa machine.

`latest` n'est pas une version de Minecraft : la comparer aux contraintes des mods les déclarerait tous incompatibles. On passe alors une chaîne vide, et les autres contrôles restent actifs.

### `modupdate` — la mise à jour, et son changelog

Chemin prévu par Modrinth : SHA-1 de chaque jar → projet → versions compatibles avec le chargeur et la version du jeu. Un mod téléchargé à la main, hors du launcher, est donc reconnu comme les autres.

Trois décisions :

1. **Rien ne part tout seul.** Une requête par projet : déclencher la vérification à l'ouverture de l'onglet enverrait des dizaines d'appels à Modrinth chaque fois qu'on vient juste regarder sa liste de mods.
2. **Le changelog est montré avant le bouton.** Mettre un mod à jour au milieu d'une partie casse des mondes ; le texte de l'auteur est ce qui permet de décider.
3. **« Non reconnu » n'est pas « à jour ».** CurseForge exige une clé et un autre protocole d'empreinte (murmur2) ; ces mods ressortent en inconnus. Les compter comme à jour serait mentir.

`apply()` télécharge sous un nom temporaire, **puis** retire l'ancien : une coupure réseau ne doit pas laisser l'instance sans le mod. L'ordre est le seul garde-fou quand l'ancien et le nouveau portent le même nom, ce qui arrive avec les auteurs qui ne versionnent pas leurs fichiers.

### Comparateur et flux de recommandations

Le comparateur répond à « pourquoi ça marche sur mon autre instance ? ». Il compare par **modId déclaré**, pas par nom de fichier, et trie les différences en tête.

**Défaut trouvé à l'écran, pas par les tests** : ma table indexée par modId écrasait silencieusement les doublons et affichait la mauvaise version (0.5.3 là où le jar principal était en 0.5.8). Les versions sont maintenant accolées — « 0.5.8 + 0.5.3 » — et le doublon se voit.

Le flux de recommandations filtre sur le chargeur et la version de l'instance (facettes Modrinth) et trie par popularité. Un palmarès générique n'aide personne : le mod le plus téléchargé du moment ne sert à rien s'il ne tourne pas sur la version qu'on joue. CurseForge n'offre pas l'équivalent : le bouton est grisé sur cette source, avec la raison écrite.

### Second défaut trouvé à l'écran

Le détail des conflits, déplié par défaut, faisait plusieurs écrans de haut et repoussait hors de vue **tout ce qui suivait dans l'onglet** — les mises à jour, puis la liste des mods elle-même. Il est désormais replié, avec le nombre dans le libellé. Le résumé suffit à savoir s'il faut ouvrir, et le barrage avant lancement déroule tout au moment où cela compte.

### Tests

`tests/test_modcheck.cpp` (`TLTestModCheck`) : comparaison de versions, 25 formes de contraintes, les cinq analyseurs, chaque conflit isolément, les non-conflits (Quilt/Fabric), l'ordre de présentation, et une **vraie archive** fabriquée sur place pour vérifier la lecture de zip et la priorité des manifestes.

Piège rencontré en l'écrivant : `versionRange="[47,)"` contient la séquence qui **termine une chaîne brute C++**. D'où les délimiteurs `R"TOML(` et `R"J(`.

## Phase 6 — reste livré (30/09/2026)

Elle était faite, sauf l'aperçu du contenu d'une sauvegarde. `zip_top_level_dirs` ajouté à `util_zip` : la liste des mondes contenus dans l'archive s'affiche sous chaque ligne, avec un cache indexé sur la date de modification — sans lui, dix archives faisaient dix ouvertures de zip par frame.

La capture d'écran du monde (`icon.png`) n'est pas montrée : elle demanderait de téléverser une texture OpenGL par archive. Les noms des mondes répondent déjà à la question « laquelle restaurer ? ».

## SDK social dans les installeurs (30/09/2026)

- **Windows** : une ligne dans `TeamLauncher.iss`, en `skipifsourcedoesntexist` — un build sans le SDK doit rester empaquetable. Installeur vérifié : **6 464 Ko**, la DLL y est.
- **Linux** : `make-deb.sh` et `install.sh` copient `libdiscord_partner_sdk.so` dans `lib/`, sous le même `rpath $ORIGIN/lib` que la SDL. Le paquet passe de 1,8 à **5,7 Mo**. Vérifié en extrayant le .deb : `ldd` résout les **deux** bibliothèques depuis `$ORIGIN/lib`, et non depuis le système.

**23 suites vertes sous Linux, 22 sous Windows** (`TLTestGamePort.exe` reste en « suppression en attente » sur cette machine, indépendamment du code).

### Crochets de validation ajoutés

`TL_AUTO_DETAIL=<onglet>` et `TL_AUTO_COMPARE=1`, au même titre que `TL_AUTO_PAGE` ou `TL_AUTO_MODAL` : sans eux, le panneau de compatibilité et le comparateur n'étaient atteignables qu'à la souris, donc invérifiables par capture. Les deux défauts ci-dessus ont été trouvés grâce à eux.

## Phase 8 — Architecture et performance (01/10/2026)

Trois chantiers sans rapport entre eux, d'où trois modules indépendants.

### Plugins : des processus, pas des bibliothèques

**La décision structurante du module.** Un plugin n'est pas une bibliothèque native chargée dans le launcher, c'est un dossier avec un `plugin.json` qui déclare des commandes, exécutées comme des **processus séparés**.

Charger du code natif aurait posé trois problèmes dont aucun n'a de bonne réponse : un plugin mal compilé fait planter tout le launcher ; l'ABI C++ interdit de mélanger des binaires construits avec d'autres compilateurs ou d'autres versions, donc chaque mise à jour du launcher casserait tous les plugins ; et surtout un plugin aurait eu accès à la mémoire du processus, donc au jeton de session Microsoft. Un processus séparé répond aux trois d'un coup.

Il n'y a **aucun bac à sable**, et le module ne prétend pas le contraire — un plugin lance des programmes avec les droits de l'utilisateur. Les garde-fous sont donc procéduraux :

- **Rien ne s'active tout seul.** Déposer un dossier dans `plugins/` le rend visible, pas actif.
- **La commande exacte est affichée à côté de la case**, pas derrière un bouton « détails ». On n'autorise pas un nom, on autorise une ligne qu'on a lue.
- **Pas de shell.** Programme et arguments sont passés séparément au système (`CreateProcessW` avec échappement `CommandLineToArgvW`, `posix_spawn` ailleurs). Un `&&` ou un `|` dans un argument reste du texte. Et un `run` qui contient un séparateur de commandes est **refusé à l'analyse** : c'est quelqu'un qui espère un shell.
- **L'autorisation porte sur l'empreinte du manifeste**, donc sur un contenu et non sur un nom. Modifier `plugin.json` la révoque.

Nuance qui a demandé réflexion, et que le test a forcée : **retrouver exactement le manifeste approuvé le réautorise.** J'avais d'abord écrit l'inverse dans le test. En y regardant, ce sont les octets que l'utilisateur a lus et acceptés ; lui redemander son accord pour les mêmes serait du bruit, pas de la sécurité. Retirer l'autorisation, en revanche, efface **toutes** les empreintes du plugin, et aucun retour en arrière ne la ressuscite.

### API HTTP locale

`cpp-httplib` était déjà dans `third_party/` sans être utilisé (la couche sortante passe par WinHTTP/libcurl). Il sert enfin, et il est inclus dans **une seule** unité de compilation — c'est un en-tête énorme.

Quatre règles, toutes nécessaires :

1. **127.0.0.1 uniquement**, jamais `0.0.0.0`.
2. **Un jeton sur chaque requête**, lecture comprise. Sur une machine partagée, « localhost » n'est pas une frontière : tout autre programme de la session peut s'y connecter. La comparaison du jeton est à **durée constante** — un `==` s'arrête au premier octet différent, ce qui laisse le retrouver caractère par caractère sur une boucle locale.
3. **Rien n'est servi depuis l'état vivant.** Le serveur a son fil ; lire `settings.instances` pendant que l'interface le modifie serait un comportement indéfini. L'interface publie un instantané JSON une fois par seconde, le serveur sert cet instantané. L'instantané ne contient **que sept champs choisis** : recopier l'instance entière exposerait tout ce qu'on y ajoutera plus tard sans y repenser.
4. **Aucune action n'est exécutée par le fil du serveur.** `POST /api/launch` dépose l'identifiant dans une file plafonnée, que la boucle vide à la frame suivante.

Piège de `cpp-httplib` : `wait_until_ready()` revient **aussi quand l'écoute a échoué** (elle attend « plus en train de démarrer »). Sans le `is_running()` qui suit, un port déjà pris serait passé pour un démarrage réussi.

### Préchauffage : ce qui est possible et ce qui ne l'est pas

« Précharger la JVM » ne peut pas vouloir dire garder une machine virtuelle allumée : un processus JVM déjà démarré ne peut pas être redirigé vers Minecraft — il faudrait l'avoir lancé avec le bon classpath et les bons arguments, c'est-à-dire connaître déjà l'instance choisie. Le dire dans l'interface plutôt que de laisser croire autre chose.

Ce qui coûte réellement cher, en revanche, se mesure : **`find_java` prend 680 à 910 ms** sur cette machine (plusieurs arborescences parcourues, un `java -version` lancé par candidat), et il n'était **pas** mis en cache — ce coût était payé à chaque partie. Un cache y a été ajouté, indexé sur la version requise et sur le réglage `JavaPath`. Mesuré par le test : **911 ms au premier appel, 2 µs au second.**

Le préchauffage fait donc deux choses pendant qu'on choisit son instance : il appelle `find_java` (qui remplit ce cache), et il lit le début des plus gros `.jar` pour amener leurs pages dans le cache du système. Plafonné à 120 fichiers et 192 Mo — inonder le cache avec des fichiers inutiles en évincerait d'utiles. Annulable entre chaque fichier, et le lancement ne l'attend jamais.

### Défaut trouvé à l'écran

Un plugin au manifeste illisible affichait son erreur **sans son nom** : `parse_manifest` rendait la main avant d'avoir posé le moindre nom. Exactement le cas où le nom est le plus nécessaire. Le repli est désormais posé avant toute analyse.

### Tests

`tests/test_phase8.cpp` (`TLTestPhase8`) : manifestes acceptés et refusés, substitution (y compris le cas où une clé absente **laisse le substituable en clair** — le vider transformerait `{instanceDir}/mods` en `/mods`, qui désigne un dossier bien réel), cycle d'autorisation complet, et l'API locale en **vraies requêtes HTTP** : 401 sans jeton et avec un mauvais jeton, 200 avec le bon, 404 sur un chemin inconnu, 400 sur un corps sans `id`, file consommée une seule fois, et un `POST` sans jeton qui ne dépose rien.

Vérifié aussi contre le launcher en marche, ce que le test unitaire ne couvre pas : le pont publication/consommation. `GET /api/status` et `/api/instances` rendent les vraies données, et un lancement d'une instance inconnue est refusé.

**Trace manquante corrigée au passage** : les messages de l'API passaient par `push_log`, qui n'écrit que dans le panneau de l'application, pas dans `launcher.log`. Une action déclenchée de l'extérieur doit laisser quelque chose à relire — personne ne regarde un panneau au moment où un script agit.

### Crochet ajouté

`TL_WINDOW_SIZE=<largeur>x<hauteur>` : les trois panneaux de cette phase tiennent sur plus d'un écran de 620 pixels, et il n'y avait aucun moyen de les capturer en entier.

**24 suites vertes sous Linux, 23 sous Windows** (`TLTestGamePort.exe` reste en « suppression en attente » sur cette machine, indépendamment du code).

## Diagnostic de crash v2 — nommer le mod coupable (01/10/2026)

Premier jalon de l'API de diagnostic. Décision prise après inventaire : parmi tout ce que le launcher sait faire, l'analyse de crash et l'analyse de mods sont les seules capacités **sans état, à petite charge et sans donnée personnelle**. Ce sont donc les seules réellement exposables, et plus tard hébergeables à coût négligeable. Le monde et la génération de ville, eux, resteront locaux.

### Deux défauts de la v1, dont un grave

**1. « OpenGL » déclenchait le diagnostic « pilote graphique ».** La règle cherchait le mot nu, or la section *System Details* de **tout** rapport de crash le contient (version du pilote, capacités GL). Vérifié sur un rapport réel de cette machine : un plantage dont la vraie cause était un mod était diagnostiqué comme un problème de carte graphique. Et comme la règle passait avant celle des conflits de mods, elle gagnait.

Corrigé en deux temps : les motifs n'acceptent plus que des formulations qui ne peuvent venir que d'un vrai échec graphique (`Pixel format not accelerated`, `GLFW error`, `No OpenGL context`…), et la règle est passée **en dernier** — ses motifs restent les plus susceptibles d'apparaître par accident, une cause plus précise doit gagner.

**2. Le coupable n'était jamais nommé.** « Conflit entre mods » ne sert à rien à quelqu'un qui en a quarante.

### Comment on nomme le mod sans rien savoir de la machine

**Le rapport de crash porte lui-même la liste des mods chargés**, avec identifiant, version et fichier d'origine. On croise cette liste avec les paquets Java cités dans la trace de pile.

Sur le rapport réel qui a servi de référence :

```
	at fr.webscreen.registry.ModItems.registerItems(ModItems.java:35)
	| LCH | webscreen | 2.0.0 | WebDisplay2-2.0.0.jar | None |
```

L'intersection désigne `webscreen`. À noter : **le nom du fichier, « WebDisplay2 », ne contient pas une lettre de « webscreen »** — une correspondance par nom de jar aurait échoué. Seul le croisement identifiant ↔ paquet pouvait trouver.

Conséquence qui compte pour la suite : l'analyse ne lit **aucun fichier** et ne touche pas au réseau. Un log collé depuis n'importe où suffit. C'est ce qui la rend exposable telle quelle par une API.

Trois formats de liste reconnus : la table 1.12 (`| LCH | id | version | source |`), Forge et NeoForge modernes (`Mod List:` puis colonnes séparées par des barres), et Fabric (`Fabric Mods:` puis `id: Nom version`).

### Trois erreurs faites en écrivant, et ce qu'elles ont appris

**Noter en mots plutôt qu'en cadres.** La première version comptait la position du mod en nombre de mots depuis le début. Or les premiers cadres d'une trace sont presque toujours du code de bibliothèque ou de Forge, et chacun pèse une dizaine de mots : `webscreen`, pourtant au **troisième cadre**, arrivait au 29ᵉ mot et récoltait un score de 42. On compte désormais en cadres : 100 au premier, −8 par cadre, plancher à 20.

**Un plancher de 4 caractères sur les identifiants.** Il écartait `jei` — l'un des mods les plus installés au monde — ainsi que `rei`, `ars`, `ic2`. Le plancher est à trois ; ce qui écarte les coïncidences, ce n'est pas la longueur mais le fait que l'identifiant doive figurer dans la liste des mods **de ce rapport précis**.

**« Suspected Mods: » ignoré quand le mod n'était pas dans la pile.** Forge moderne désigne parfois lui-même un coupable, y compris pour une erreur de chargement survenue avant toute pile. Sa parole vaut mieux que notre déduction : on crée maintenant le suspect même sans trace.

### Le garde-fou qui compte le plus

Le troisième rapport réel de la machine est le cas le plus instructif. L'exception visible est un `NoClassDefFoundError` sur une classe d'OptiFine ; trente lignes plus bas se trouve `Caused by: java.lang.OutOfMemoryError: GC overhead limit exceeded`. La classe n'a pas pu être chargée **faute de mémoire**.

Accuser OptiFine aurait envoyé l'utilisateur désinstaller un mod innocent et ne rien régler. D'où la règle : quand la cause est environnementale — mémoire, tas trop grand, session, réseau, pilote, fichier verrouillé — les suspects sont **écartés du verdict**. Le mod cité dans la pile est une victime, pas un coupable.

### Résultat sur les rapports réels

Trois rapports de cette machine, passés par `TL_CRASH_DIR` :

| Rapport | Verdict |
|---|---|
| `crash-...17.44.59` | `mod_error` → **webscreen** (WebDisplay2-2.0.0.jar) |
| `crash-...17.47.57` | `mod_error` → **webscreen** |
| `crash-...18.40.23` | `out_of_memory`, aucun suspect (correct : `Caused by: OutOfMemoryError`) |

Avant cette version, les trois répondaient « problème de pilote graphique ».

### Interface

Le rapport structuré sert deux publics d'un coup : la bulle reçoit la phrase qui tient en 84 pixels (titre + mod en cause), le journal reçoit tout — mod, fichier d'origine et la ligne de pile qui l'accuse, pour qu'on puisse **vérifier plutôt que croire**.

### Tests

`tests/test_crash.cpp` (`TLTestCrash`) : les trois formats de liste, les quatre scénarios (Forge 1.12, Forge moderne avec `Suspected Mods`, Fabric, cascade mémoire), la non-accusation des mods présents mais absents de la pile, les identifiants de cause stables (contrat d'API), et la robustesse sur entrée vide ou tronquée. `TL_CRASH_DIR=<dossier>` passe en plus tous les rapports d'un dossier réel en affichant le verdict.

**25 suites vertes sous Linux, 24 sous Windows.**

### Reste à faire pour l'API de diagnostic

- `POST /v1/crash/analyze` et `POST /v1/mods/check` : exposer les deux cerveaux (le travail d'analyse est fait, il manque le transport et le modèle de clés).
- Événements sortants : que le launcher pousse le crash déjà analysé vers une URL, sans que le joueur ait à faire quoi que ce soit.

## API de diagnostic — clés d'application et événements sortants (01/10/2026)

Suite directe du diagnostic v2. L'analyse était faite ; il manquait de quoi la distribuer.

### Pourquoi un jeton unique ne suffisait plus

Celui de la phase 8 convient à un script qu'on écrit soi-même. Dès qu'on **distribue** l'accès, il montre trois limites : on ne peut pas révoquer une application sans couper toutes les autres, on ne sait pas laquelle appelle, et un simple widget d'affichage reçoit le pouvoir de lancer une partie.

D'où `apikeys` — une clé par application, avec trois décisions :

- **Le secret n'est jamais conservé.** On range son empreinte SHA-1. Lire `config.json` ne permet donc pas de s'authentifier, et le secret n'est montré qu'une fois, à la création. Le perdre oblige à en régénérer un : c'est voulu.
- **Des portées, pas un interrupteur.** `diag` analyse sans rien lire de la machine, `read` expose les instances, `control` lance une partie.
- **Chaque appel laisse une trace** (compteur, date de dernier usage). Sans cela, on ne sait pas quelle clé révoquer.

`sha1_hex_of()` a été ajouté à `util_hash` : la primitive SHA-1 existait, mais seulement derrière une lecture de fichier.

Détail qui compte : la vérification de clé **compare à durée constante**. Un `==` s'arrête au premier octet différent, ce qui laisse mesurer combien de caractères sont bons — assez, sur une boucle locale, pour retrouver une clé octet par octet.

Le jeton du propriétaire, lui, garde tous les droits : c'est l'outil de l'utilisateur sur sa propre machine, pas une clé distribuée.

### Les deux capacités exposées

```
POST /v1/diag/crash   log brut ou {"log":"..."} → cause, action, mod en faute
POST /v1/diag/mods    {loader, mcVersion, mods:[...]} → conflits, manques, doublons
GET  /v1              auto-description : ce que la clé présentée permet
```

Elles ne lisent **aucun fichier**, ne touchent pas au réseau et ne voient aucune donnée de l'utilisateur : tout arrive dans le corps de la requête. C'est précisément ce qui les rend distribuables, et un jour hébergeables, contrairement au monde ou à la génération de ville.

Deux choix d'ergonomie d'API :

- `/v1/diag/crash` accepte le **log brut** autant que du JSON. Imposer un échappement JSON sur 400 Ko de texte pour rien aurait été une barrière gratuite.
- `/v1/diag/mods` accepte des **manifestes bruts** autant que des descripteurs déjà analysés. Lire les cinq formats est justement ce que l'appelant vient chercher ; le lui faire réimplémenter aurait vidé l'API de son intérêt.

Un refus de portée rend **403 et non 401** : l'appelant doit savoir qu'il ne s'agit pas de se réauthentifier mais de demander un droit qu'on ne lui a pas accordé.

Les anciens chemins `/api/*` restent servis. Rien de ce qui marchait hier ne cesse de marcher.

### Les événements sortants

C'est le point qui change la nature du support. Aujourd'hui, un joueur qui plante doit penser à chercher son log, le coller quelque part et attendre. Avec un abonnement, le destinataire reçoit le crash **et sa cause déjà analysée** — le mod en faute nommé dans le message — à la seconde où il arrive. Le joueur n'a rien fait, et le destinataire n'a rien à savoir des rapports de crash.

Et cela vaut **même si personne n'écrit de client** : recevoir un POST, c'est dix lignes.

Trois règles de sûreté :

- **HTTPS obligatoire**, sauf vers une adresse de bouclage (on développe souvent son récepteur en local). Le corps porte le nom de l'instance et la version du jeu, le secret voyage en en-tête : en clair sur le réseau, les deux seraient lisibles.
- **L'hôte est ajouté à l'allowlist sortante** à l'enregistrement, jamais implicitement. Le refus par défaut de la couche HTTP (barrière S2) reste la règle.
- **Jamais sur le fil de l'interface.** Une URL qui ne répond pas gèlerait la fenêtre pendant le délai d'expiration — juste après un crash, le pire moment.

Deux garde-fous de fonctionnement : la file est plafonnée à 64 (un jeu qui planterait en boucle ne doit pas la faire enfler), et un abonnement qui échoue **dix fois d'affilée se désactive** plutôt que de retenter à chaque crash. L'utilisateur le réactive quand il a corrigé l'adresse.

L'URL est affichée **expurgée** dans les réglages : beaucoup d'adresses de réception portent un secret dans leur chemin (Discord, Slack), et une capture d'écran de support le donnerait à tout le monde.

### Tests

`TLTestPhase8` couvre désormais, en plus des plugins et de l'API locale :

- **Clés** : la portée accordée passe et les autres non ; un secret inconnu n'est ni authentifié ni « portée refusée » (deux cas distincts) ; révoquer une clé n'affecte pas les autres ; le secret **n'apparaît nulle part** dans la configuration sérialisée ; aller-retour JSON.
- **Diagnostic par HTTP** : un vrai log envoyé en texte brut ressort avec `webscreen` et son jar ; une clé `read` reçoit 403 sur `/v1/diag/crash` et une clé `diag` reçoit 403 sur `/v1/status` ; corps illisible → 400 et non 500 ; clé révoquée refusée **sans redémarrage** ; le jeton du propriétaire garde tout ; `/api/status` répond toujours.
- **Événements** : un vrai récepteur HTTP est lancé dans le test, et on vérifie que le message d'essai **puis** le crash analysé arrivent, que le secret est bien dans l'en-tête et **pas dans le corps**, et qu'un type sans abonné n'envoie rien du tout.

**25 suites vertes sous Linux, 24 sous Windows.**

### Ce qui reste pour en faire un produit

- Le **formulaire** de délivrance et le processus de validation (hors launcher).
- Décider entre clés locales et service hébergé. Le découpage naturel reste celui du 01/10 : `diag` est hébergeable (petites charges, aucune donnée personnelle), le monde et la ville non.
- Une documentation publique des deux points d'accès.

### Documentation publique (01/10/2026)

`cpp/docs/API.md` — la référence à joindre au formulaire de délivrance.

Elle a été **vérifiée en l'exécutant** contre le launcher en marche, et
non relue : chaque exemple de réponse vient d'un appel réel. Deux
corrections en ont découlé, trouvées en rédigeant :

1. **`severity` était rendu en français** (« Bloquant »). Un appelant
   aurait dû comparer des chaînes traduisibles. L'API rend désormais une
   clé stable (`error` / `warning` / `info`) et le libellé lisible à côté,
   sous `severityLabel`.
2. **`game_start` et `game_stop` étaient proposés dans l'interface mais
   jamais émis.** Une case à cocher pour un événement qui ne part pas est
   un défaut, pas une fonctionnalité à venir. Les deux sont maintenant
   émis — `game_start` seulement quand le processus du jeu tourne
   vraiment (pas au clic sur Jouer, qui peut encore échouer), et
   `game_stop` uniquement pour les fins normales, un arrêt anormal partant
   en `crash` avec son diagnostic. Jamais les deux pour un même plantage.

Une troisième correction est venue du même exercice : `/v1/diag/mods`
sans champ `loader` répondait « aucun chargeur sur cette instance ».
Diagnostic juste pour le launcher, trompeur pour un appelant qui a
simplement oublié le champ — c'est maintenant un 400 qui le dit.

Le score de l'exemple documenté a aussi été corrigé : 60 sur le rapport
réel complet, là où la version abrégée du test donnait 84.

## `tl_diagd` — service de diagnostic hébergeable (01/10/2026)

Décision prise après inventaire : de tout ce que le launcher sait faire,
seules l'analyse de crash et l'analyse de mods sont **sans état, à petite
charge et sans donnée personnelle**. Ce sont donc les seules hébergeables.
Le monde et la génération de ville restent locaux : ils demandent de
transférer des gigaoctets et de tenir un monde en mémoire.

Le constat qui a rendu la chose évidente, vérifié avant d'écrire une
ligne :

```
crash_analyzer.cpp → std uniquement
modcheck.cpp       → std uniquement
modmeta.cpp        → std + nlohmann + miniz
```

Aucune dépendance au launcher. Pas de SDL, pas de SDK Discord, pas de
`DataStore`, pas de curl. Le service pèse **506 Ko sous Windows, 563 Ko
sous Linux**, et ne lie délibérément **pas** `tl_core` : le faire aurait
traîné la SDL, le SDK Discord, libcurl et le stockage de configuration
pour rien, avec une surface d'attaque sans rapport avec ce qu'il fait.

### Le module partagé, écrit en premier

`diagapi` — « un corps de requête entre, un corps de réponse sort », sans
HTTP ni clé. Les gestionnaires de `localapi` ont été réécrits pour
l'appeler.

C'est la seule décision structurante du lot : deux copies de ces
gestionnaires auraient fini par rendre **deux réponses différentes à la
même question**, et c'est le genre de divergence qu'on ne remarque que le
jour où un appelant s'en plaint. `tests/test_diagapi.cpp` vaut donc pour
les deux programmes à la fois.

### Ce que le service fait, et ne fait pas

Deux routes d'analyse, l'auto-description, et `/healthz` **sans clé** —
une sonde de supervision ne doit pas en réclamer une, et cette route ne
révèle rien.

**Pas de TLS.** Le service s'installe derrière un reverse proxy qui le
termine. Embarquer OpenSSL aurait ajouté une grosse dépendance pour
refaire moins bien ce qu'un proxy fait déjà, et pour devoir en suivre les
mises à jour de sécurité nous-mêmes.

**Pas de base de données.** Un fichier JSON de clés, rechargé quand sa
date de modification change : ajouter ou révoquer ne demande pas de
redémarrage. Vérifié en révoquant une clé en cours de service — la
requête suivante a reçu un 401.

Garde-fou qui compte : **un fichier de clés devenu illisible ne révoque
personne.** Les clés en mémoire sont conservées et l'erreur part au
journal. Couper l'accès à tout le monde parce qu'une virgule manque
serait le pire comportement possible.

### La discipline, vérifiée plutôt qu'affirmée

**Le corps des requêtes n'est jamais écrit sur disque.** Un rapport de
crash contient le nom de compte Windows dans les chemins de fichiers,
parfois des noms de serveurs, parfois des dossiers personnels.

Ce n'est pas une intention : après avoir fait analyser le vrai rapport de
la machine, le journal a été fouillé pour `webscreen`,
`NullPointerException`, `WebDisplay2`, `fr.webscreen`, `darkm` et
`SecurityCraft`. **Aucun présent.** Il ne contient que la route, le code,
l'identifiant de clé, la taille et la clé de verdict :

```
/v1/diag/crash 200 key=ak_06995fc00006 bytes=14203 cause=mod_error
```

Même un plantage interne ne renvoie que `{"error":"erreur interne"}` : le
détail d'une exception pourrait porter un fragment du corps.

### Plafond de débit

`ratePerMin` par clé, compté par minute civile, `429` + `Retry-After`
au-delà. Mesuré : avec `ratePerMin: 30`, 35 appels dans la même minute
donnent 29 × `200` et 6 × `429`.

### Délivrance des clés

`tl_diagd --new-key "Nom"` imprime le secret (une seule fois) et la ligne
JSON à coller. Le service ne range que l'empreinte SHA-1 ; un secret
perdu se remplace, il ne se retrouve pas.

Volontairement rudimentaire : il y aura cinq clés, pas cinq mille. Un
système de comptes coûterait plus qu'il ne rapporterait, et la délivrance
à la main laisse le droit de dire non sans avoir à coder une politique.

### Documentation

`cpp/docs/DIAGD.md` — exploitation : lancement, proxy, unité systemd
durcie (`ProtectSystem=strict`, `ReadOnlyPaths`), délivrance de clés et
vérification d'installation.

### Tests

`tests/test_diagapi.cpp` (`TLTestDiagApi`) : log brut et enveloppe JSON,
un log qui **commence par une accolade sans être du JSON**, corps vide
rendu en `200 found=false` (rien à analyser n'est pas une faute de
l'appelant), les cinq formats de manifeste devinés, descripteurs déjà
analysés, et surtout : **toute réponse est du JSON, même en erreur** — un
appelant ne doit jamais avoir à lire du HTML.

Vérifié en plus à la main contre le service en marche : `/healthz`, `401`
sans clé, le vrai rapport de crash rendant `webscreen` /
`WebDisplay2-2.0.0.jar`, le plafond de débit et la révocation à chaud.

**26 suites vertes sous Linux, 25 sous Windows.**
