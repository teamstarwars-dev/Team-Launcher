cmake_minimum_required(VERSION 3.20)
project(TeamLauncher VERSION 6.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(TL_LTO "Enable LTO in Release" ON)
option(TL_FORTIFY "Enable _FORTIFY_SOURCE" ON)
option(TL_UNITY "Unity build for faster compile" OFF)

# Resource dir clang complet (install LLVM partiel = pas de headers builtin → _mm_* externes)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  if(EXISTS "D:/tl-llvm-res/include/emmintrin.h")
    add_compile_options(/clang:-resource-dir=D:/tl-llvm-res)
  else()
    message(WARNING "clang resource dir D:/tl-llvm-res introuvable — intrinsics SSE risquent de ne pas linker")
  endif()
endif()

# ---- SDL2 (dynamic only — jamais statique) ----
#
# Sous Linux on prend le SDL2 du systeme (paquet libsdl2-dev) : c'est ce
# qu'attend une distribution, le .deb declare alors une dependance au lieu
# d'embarquer sa propre copie, et le build est bien plus rapide. Le SDL
# embarque dans third_party sert pour Windows, ou il n'y a pas de gestionnaire
# de paquets, et de repli si le systeme n'en fournit pas.
set(TL_SDL_VENDORED OFF)
if(NOT WIN32)
  find_package(SDL2 QUIET)
endif()

if(NOT TARGET SDL2::SDL2)
  set(TL_SDL_VENDORED ON)
  set(SDL_SHARED ON CACHE BOOL "Build shared SDL2" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "No static SDL2" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  # MMX/assembly SDL casse avec clang-cl x64 (intrinsics MSVC) — inutile pour un launcher
  set(SDL_ASSEMBLY OFF CACHE BOOL "" FORCE)
  set(SDL_MMX OFF CACHE BOOL "" FORCE)
  set(SDL_SSE OFF CACHE BOOL "" FORCE)
  set(SDL_SSE2 OFF CACHE BOOL "" FORCE)
  set(SDL_SSE3 OFF CACHE BOOL "" FORCE)
  set(SDL_SSEMATH OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

  # clang x64 définit __MMX__ par défaut → blocs MMX SDL incompatibles avec
  # les en-tetes MSVC.
  #
  # CORRECTIF : la garde ne testait que « compilateur = Clang », alors que le
  # commentaire disait « Windows uniquement ». Sous Linux, clang recevait donc
  # « /clang:-msse3 », qu'il interprete comme un NOM DE FICHIER — la
  # compilation de SDL echouait. Il faut bien MSVC (donc clang-cl).
  if(TARGET SDL2 AND MSVC AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(SDL2 PRIVATE -U__MMX__ -U__3dNOW__)
    # SDL_cpuinfo.h force __SSE3__ côté MSVC → code SSE3 présent mais cible sans sse3
    target_compile_options(SDL2 PRIVATE /clang:-msse3)
  endif()
endif()
message(STATUS "SDL2 : ${TL_SDL_VENDORED} (ON = copie embarquee, OFF = systeme)")

# ---- Dear ImGui (sources, pas de lib séparée) ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2)
target_compile_definitions(imgui PUBLIC
  # loader GL intégré ImGui (pas de gl3w/glew externe)
)
# clang-cl + UCRT : offsetof() non constexpr → static_assert ImGui échoue
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(imgui PUBLIC _CRT_USE_BUILTIN_OFFSETOF)
  target_compile_options(imgui PRIVATE -Wno-c++11-narrowing)
endif()

# ---- Headers-only deps ----
add_library(tl_headers INTERFACE)
target_include_directories(tl_headers INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/single_include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

# ---- Flags Release communs : taille + gc-sections + LTO ----
function(tl_release_flags tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:/O1 /W4 /WX- /permissive->
    )
    target_link_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /DEBUG:NONE>
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # clang-cl ignore /GL+/LTCG (avertissement 'unused') → LTO reel via -flto=thin
      # (lld-link detecte le bitcode et lance LTO automatiquement — pas de flag link)
      target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/clang:-flto=thin>)
    else()
      target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/GL>)
      target_link_options(${tgt} PRIVATE $<$<CONFIG:Release>:/LTCG>)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:-Os -ffunction-sections -fdata-sections -Wall -Wextra>
      $<$<AND:$<CONFIG:Release>,$<BOOL:${TL_FORTIFY}>>:-D_FORTIFY_SOURCE=2>
    )
    target_link_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:-Wl,--gc-sections>
    )
    if(TL_LTO)
      target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:-flto>)
      target_link_options(${tgt} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    endif()
  endif()
endfunction()

# ---- Core metier (portage C#) : DataStore + HTTP + hashes + zip + jeu ----
set(TL_CORE_SOURCES
  src/datastore.cpp
  src/secrets.cpp
  src/dialogs.cpp
  src/http_win.cpp
  src/util_hash.cpp
  src/util_zip.cpp
  src/game_installer.cpp
  src/game_launcher.cpp
  src/lang.cpp
  src/backup.cpp
  src/crash_analyzer.cpp
  src/maintenance.cpp
  src/telemetry.cpp
  src/curseforge.cpp
  src/pack_import.cpp
  src/modrinth.cpp
  src/content_install.cpp
  src/shortcut.cpp
  src/presence.cpp
  src/pack_share.cpp
  src/ms_auth.cpp
  src/launch_flow.cpp
  src/server_host.cpp
  src/apptasks.cpp
  src/admin.cpp
  src/nbt.cpp
  src/region.cpp
  src/world.cpp
  src/worldsync.cpp
  src/citygen.cpp
  src/worldedit.cpp
  src/moddev.cpp
  src/model3d.cpp
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
)
add_library(tl_core STATIC ${TL_CORE_SOURCES})
target_include_directories(tl_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz
)
# crypt32 : DPAPI + base64 (jetons MsAuth) ; shell32 : ouverture du navigateur
# bcrypt : plus utilise pour SHA-1/MD5 (portables depuis l etape 5), mais
# encore requis par datastore.cpp (BCryptGenRandom). A remplacer par
# getrandom() cote POSIX quand la couche plateforme arrivera.
if(WIN32)
  target_link_libraries(tl_core PUBLIC tl_headers winhttp bcrypt crypt32 shell32 ole32 uuid)
else()
  # POSIX (etape 5) : WinHTTP -> libcurl, DPAPI -> libsecret. Les sources
  # Windows-only (http_win.cpp, secrets.cpp...) ne compilent pas encore sous
  # Linux : seuls les liens sont prets, les backends arrivent module par module.
  find_package(CURL REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SECRET REQUIRED libsecret-1)
  target_link_libraries(tl_core PUBLIC tl_headers CURL::libcurl ${SECRET_LIBRARIES})
  target_include_directories(tl_core PUBLIC ${SECRET_INCLUDE_DIRS})
endif()
# maintenance.cpp / telemetry.cpp rapportent la version du launcher
target_compile_definitions(tl_core PUBLIC TL_VERSION_STRING="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(tl_core PRIVATE /EHsc)
  target_compile_definitions(tl_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(tl_core PUBLIC _CRT_USE_BUILTIN_OFFSETOF)
endif()
tl_release_flags(tl_core)

# ---- Application ----
file(GLOB_RECURSE TL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
foreach(core_src IN ITEMS datastore secrets dialogs http_win util_hash util_zip game_installer game_launcher lang backup crash_analyzer maintenance telemetry curseforge pack_import modrinth content_install shortcut presence pack_share ms_auth launch_flow server_host apptasks admin nbt region world worldsync citygen worldedit moddev model3d)
  list(REMOVE_ITEM TL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/${core_src}.cpp)
endforeach()
# Ressources Win32 (icone + bloc de version). Le .rc n'existe que sous
# Windows : la cible Linux prendra un .desktop a la place.
if(WIN32)
  list(APPEND TL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc)
endif()

add_executable(TeamLauncher ${TL_SOURCES})

target_include_directories(TeamLauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
  target_link_libraries(TeamLauncher PRIVATE
    imgui
    SDL2::SDL2
    tl_headers
    tl_core
    opengl32
    comdlg32
    shell32
  )
else()
  # POSIX (etape 5) : OpenGL systeme au lieu d'opengl32, pas de comdlg32/shell32.
  find_package(OpenGL REQUIRED)
  target_link_libraries(TeamLauncher PRIVATE
    imgui
    SDL2::SDL2
    tl_headers
    tl_core
    OpenGL::GL
  )
  # rpath $ORIGIN/lib : la SDL embarquee voyage à côté du binaire
  # (layout /opt/teamlauncher ou ~/.local/share/teamlauncher) sans
  # LD_LIBRARY_PATH — qui fuiterait sinon vers le jeu lance.
  set_target_properties(TeamLauncher PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/lib")
endif()
target_compile_definitions(TeamLauncher PRIVATE
  TL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  TL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  TL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  # TL_VERSION_STRING vient de tl_core (PUBLIC) — ne pas le redefinir ici
  _CRT_SECURE_NO_WARNINGS
  $<$<CONFIG:Release>:NDEBUG>
)
if(MSVC)
  # /EHsc explicite : nlohmann/json et les workers des pages UI utilisent
  # try/throw. Ne pas dependre du CMAKE_CXX_FLAGS par defaut, qui peut etre
  # vide selon la facon dont le cache a ete genere.
  target_compile_options(TeamLauncher PRIVATE /EHsc)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(TeamLauncher PRIVATE _CRT_USE_BUILTIN_OFFSETOF)
endif()

tl_release_flags(TeamLauncher)

# ---- Copie SDL2.dll à côté de l'exé (link dynamique) ----
# Seulement quand SDL est la copie embarquee : avec le SDL du systeme
# (Linux), la bibliotheque est deja installee et le chargeur la trouve.
if(TL_SDL_VENDORED)
  add_custom_command(TARGET TeamLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:SDL2::SDL2>
      $<TARGET_FILE_DIR:TeamLauncher>
    COMMENT "Copie SDL2.dll"
  )
endif()

# ---- Assets externes (jamais embarqués) ----
add_custom_command(TARGET TeamLauncher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:TeamLauncher>/assets
  COMMENT "Copie assets/"
)

install(TARGETS TeamLauncher RUNTIME DESTINATION .)
install(TARGETS SDL2 RUNTIME DESTINATION .)
install(DIRECTORY assets/ DESTINATION assets)

# ---- Tests unitaires (dev uniquement) ----
option(TL_BUILD_TESTS "Build unit tests" ON)
if(TL_BUILD_TESTS)
  set(TL_TEST_TARGETS
    "tests/test_datastore.cpp|TLTestDataStore"
    "tests/test_game_port.cpp|TLTestGamePort"
    "tests/test_ms_auth.cpp|TLTestMsAuth"
    "tests/test_lang.cpp|TLTestLang"
    "tests/test_services.cpp|TLTestServices"
    "tests/test_packs.cpp|TLTestPacks"
    "tests/test_server_host.cpp|TLTestServerHost"
    "tests/test_apptasks.cpp|TLTestApptasks"
    "tests/test_instance_detail.cpp|TLTestInstanceDetail"
    "tests/test_security.cpp|TLTestSecurity"
    "tests/test_nbt.cpp|TLTestNbt"
    "tests/test_region.cpp|TLTestRegion"
    "tests/test_world.cpp|TLTestWorld"
    "tests/test_worldsync.cpp|TLTestWorldSync"
    "tests/test_citygen.cpp|TLTestCityGen"
    "tests/test_worldedit.cpp|TLTestWorldEdit"
    "tests/test_moddev.cpp|TLTestModDev"
    "tests/test_model3d.cpp|TLTestModel3D"
  )
  foreach(entry IN LISTS TL_TEST_TARGETS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 tsrc)
    list(GET parts 1 ttgt)
    add_executable(${ttgt} ${tsrc})
    target_link_libraries(${ttgt} PRIVATE tl_core)
    if(MSVC)
      target_compile_options(${ttgt} PRIVATE /EHsc)
      target_compile_definitions(${ttgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    tl_release_flags(${ttgt})
  endforeach()
endif()
