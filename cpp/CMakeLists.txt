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
# clang x64 définit __MMX__ par défaut → blocs MMX SDL incompatibles clang-cl/MSVC headers
if(TARGET SDL2)
  target_compile_options(SDL2 PRIVATE -U__MMX__ -U__3dNOW__)
  # SDL_cpuinfo.h force __SSE3__ côté MSVC → code SSE3 présent mais cible sans sse3
  target_compile_options(SDL2 PRIVATE /clang:-msse3)
endif()

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
      $<$<CONFIG:Release>:-Os -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -Wall -Wextra>
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
  src/ms_auth.cpp
  src/launch_flow.cpp
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
target_link_libraries(tl_core PUBLIC tl_headers winhttp bcrypt crypt32 shell32)
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
foreach(core_src IN ITEMS datastore http_win util_hash util_zip game_installer game_launcher lang backup crash_analyzer maintenance telemetry curseforge pack_import ms_auth launch_flow)
  list(REMOVE_ITEM TL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/${core_src}.cpp)
endforeach()
add_executable(TeamLauncher ${TL_SOURCES})

target_include_directories(TeamLauncher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(TeamLauncher PRIVATE
  imgui
  SDL2::SDL2
  tl_headers
  tl_core
  opengl32
  comdlg32
  shell32
)
target_compile_definitions(TeamLauncher PRIVATE
  TL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  TL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  TL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  # TL_VERSION_STRING vient de tl_core (PUBLIC) — ne pas le redefinir ici
  _CRT_SECURE_NO_WARNINGS
  $<$<CONFIG:Release>:NDEBUG>
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(TeamLauncher PRIVATE _CRT_USE_BUILTIN_OFFSETOF)
endif()

tl_release_flags(TeamLauncher)

# ---- Copie SDL2.dll à côté de l'exé (link dynamique) ----
add_custom_command(TARGET TeamLauncher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL2::SDL2>
    $<TARGET_FILE_DIR:TeamLauncher>
  COMMENT "Copie SDL2.dll"
)

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
