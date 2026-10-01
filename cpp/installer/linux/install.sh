#!/bin/bash
# Installe Team Launcher sans root (défaut : ~/.local) ou système (--system).
# Même layout que le .deb : <root>/teamlauncher/{TeamLauncher,assets/,lib/} +
# lien <prefix>/bin/teamlauncher (rpath $ORIGIN/lib : pas de LD_LIBRARY_PATH).
set -euo pipefail

PREFIX="$HOME/.local"
SYSTEM=0
SRC=""
for a in "$@"; do
    case "$a" in
        --system) SYSTEM=1 ;;
        --prefix=*) PREFIX="${a#--prefix=}" ;;
        --src=*) SRC="${a#--src=}" ;;
        -h|--help)
            echo "usage: $0 [--system] [--prefix=DIR] [--src=CPP_DIR]"; exit 0 ;;
    esac
done
if [ $SYSTEM -eq 1 ]; then PREFIX=/opt; fi
if [ -z "$SRC" ]; then SRC="$(dirname "$(readlink -f "$0")")/../.."; fi
[ -x "$SRC/build-linux/TeamLauncher" ] || { echo "ERREUR : $SRC/build-linux/TeamLauncher absent"; exit 1; }
if [ $SYSTEM -eq 1 ] && [ "$(id -u)" != 0 ]; then echo "ERREUR : --system requiert root"; exit 1; fi

ROOT="$PREFIX/teamlauncher"
BIN_DIR="$PREFIX/bin"
APPS_DIR=$([ $SYSTEM -eq 1 ] && echo "/usr/share/applications" || echo "$HOME/.local/share/applications")
ICON_DIR=$([ $SYSTEM -eq 1 ] && echo "/usr/share/icons/hicolor/256x256/apps" || echo "$HOME/.local/share/icons/hicolor/256x256/apps")

mkdir -p "$ROOT/assets" "$ROOT/lib" "$BIN_DIR" "$APPS_DIR" "$ICON_DIR"
cp "$SRC/build-linux/TeamLauncher" "$ROOT/"
cp "$SRC/assets/README.txt" "$ROOT/assets/"
# Facultatif : voir make-deb.sh.
[ -f "$SRC/assets/default.env" ] && cp "$SRC/assets/default.env" "$ROOT/assets/"
SDL_REAL="$(ls "$SRC/build-linux/third_party/SDL"/libSDL2-2.0.so.0.*.* 2>/dev/null | head -1)"
[ -n "$SDL_REAL" ] || { echo "ERREUR : libSDL2 introuvable"; exit 1; }
cp "$SDL_REAL" "$ROOT/lib/"
ln -sf "$(basename "$SDL_REAL")" "$ROOT/lib/libSDL2-2.0.so.0"
ln -sf libSDL2-2.0.so.0 "$ROOT/lib/libSDL2-2.0.so"
# SDK social Discord (amis, messages, vocal), a cote de la SDL sous le meme
# rpath $ORIGIN/lib. Facultatif : sans lui tout fonctionne, la page Amis
# expliquant ce qui manque — on avertit plutot que d'echouer.
SOCIAL_SO="$SRC/third_party/discord_social_sdk/lib/release/libdiscord_partner_sdk.so"
if [ -f "$SOCIAL_SO" ]; then
    cp "$SOCIAL_SO" "$ROOT/lib/"
else
    echo "ATTENTION : SDK social absent — installation sans amis/vocal"
fi
ln -sf "$ROOT/TeamLauncher" "$BIN_DIR/teamlauncher"
sed "s|@PREFIX@|$PREFIX|" "$(dirname "$(readlink -f "$0")")/teamlauncher.desktop.in" > "$APPS_DIR/teamlauncher.desktop"
cp "$SRC/assets/teamlauncher.png" "$ICON_DIR/teamlauncher.png"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS_DIR" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache "$ICON_DIR/../.." 2>/dev/null || true
echo "OK : $BIN_DIR/teamlauncher (lance-le depuis le menu ou le terminal)"
