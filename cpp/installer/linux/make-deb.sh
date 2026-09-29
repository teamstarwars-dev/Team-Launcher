#!/bin/bash
# Construit teamlauncher_<ver>_amd64.deb depuis build-linux (sans root).
# Layout : /opt/teamlauncher/{TeamLauncher,assets/,lib/libSDL2*} +
#          /usr/bin/teamlauncher (symlink : /proc/self/exe résout le réel,
#          donc exeDir/assets restent corrects) + .desktop + icône + doc.
set -euo pipefail

HERE="$(dirname "$(readlink -f "$0")")"
CPP="$HERE/../.."
VER="$(grep -m1 -oP 'project\(TeamLauncher VERSION \K[0-9.]+' "$CPP/CMakeLists.txt")"
ARCH=amd64
BUILD="$CPP/build-linux"
BIN="$BUILD/TeamLauncher"

[ -x "$BIN" ] || { echo "ERREUR : $BIN absent — compiler d'abord (build-linux)"; exit 1; }
[ -f "$CPP/assets/teamlauncher.png" ] || { echo "ERREUR : assets/teamlauncher.png absent"; exit 1; }
command -v dpkg-deb >/dev/null || { echo "ERREUR : dpkg-deb introuvable"; exit 1; }

# La SDL embarquée voyage avec le binaire (rpath $ORIGIN/lib) : pas de
# dépendance libsdl2 système (ABI figée à la version compilée).
SDL_REAL="$(ls "$BUILD/third_party/SDL"/libSDL2-2.0.so.0.*.* 2>/dev/null | head -1)"
[ -n "$SDL_REAL" ] || { echo "ERREUR : libSDL2 introuvable dans $BUILD/third_party/SDL"; exit 1; }

PKG=/tmp/teamlauncher-deb
rm -rf "$PKG"
mkdir -p "$PKG/opt/teamlauncher/assets" "$PKG/opt/teamlauncher/lib" \
         "$PKG/usr/bin" "$PKG/usr/share/applications" \
         "$PKG/usr/share/icons/hicolor/256x256/apps" \
         "$PKG/usr/share/doc/teamlauncher" "$PKG/DEBIAN"

cp "$BIN" "$PKG/opt/teamlauncher/TeamLauncher"
cp "$CPP/assets/default.env" "$CPP/assets/README.txt" "$PKG/opt/teamlauncher/assets/"
cp "$SDL_REAL" "$PKG/opt/teamlauncher/lib/"
ln -s "$(basename "$SDL_REAL")" "$PKG/opt/teamlauncher/lib/libSDL2-2.0.so.0"
ln -s libSDL2-2.0.so.0 "$PKG/opt/teamlauncher/lib/libSDL2-2.0.so"
ln -s /opt/teamlauncher/TeamLauncher "$PKG/usr/bin/teamlauncher"
sed "s|@PREFIX@|/usr|" "$HERE/teamlauncher.desktop.in" > "$PKG/usr/share/applications/teamlauncher.desktop"
cp "$CPP/assets/teamlauncher.png" "$PKG/usr/share/icons/hicolor/256x256/apps/teamlauncher.png"
cp "$CPP/../LICENSE.txt" "$PKG/usr/share/doc/teamlauncher/copyright" 2>/dev/null || echo "Team Launcher v$VER (portage C++)" > "$PKG/usr/share/doc/teamlauncher/copyright"

# Dépendances directes du binaire (readelf NEEDED hors libc de base).
cat > "$PKG/DEBIAN/control" <<EOF
Package: teamlauncher
Version: $VER
Architecture: $ARCH
Maintainer: TeamLauncher
Section: games
Priority: optional
Depends: libcurl4, libsecret-1-0, libglib2.0-0, libgl1
Recommends: xdg-utils, zenity | kdialog
Description: Lanceur Minecraft léger
 Portage C++/ImGui du Team Launcher (v6) : instances, mods, skins,
 serveurs, Bedrock — sans runtime .NET, ~5 Mo.
EOF

OUT="$CPP/teamlauncher_${VER}_${ARCH}.deb"
dpkg-deb --build "$PKG" "$OUT" >/dev/null
echo "OK : $OUT ($(du -h "$OUT" | cut -f1))"
dpkg-deb -c "$OUT" | head -20
