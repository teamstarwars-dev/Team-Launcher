#!/bin/bash
# Compile Team Launcher pour Linux — pendant de build-release.bat.
#
# Par defaut : build-linux/ en Release avec le SDL EMBARQUE, car c'est ce
# qu'attendent installer/linux/make-deb.sh et install.sh (ils recopient
# build-linux/third_party/SDL/libSDL2-*.so a cote du binaire, rpath
# $ORIGIN/lib, pour figer l'ABI d'un paquet telecharge hors depot).
#
#   ./build-linux.sh              # build de production, SDL embarque
#   ./build-linux.sh --system-sdl # SDL du systeme : bien plus rapide, pour
#                                 # iterer. NE PAS empaqueter ce build.
#   ./build-linux.sh --dev        # --system-sdl + build hors /mnt (rapide)
#   ./build-linux.sh --clean      # repart de zero
#
# Sous WSL, compiler dans /mnt/... passe par le pont 9p et coute tres cher :
# --dev place les objets dans ~/tl-build. Les scripts d'empaquetage, eux,
# lisent build-linux/ : c'est voulu.

set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
SYSTEM_SDL=OFF
BUILD="$HERE/build-linux"
CLEAN=0

for a in "$@"; do
    case "$a" in
        --system-sdl) SYSTEM_SDL=ON ;;
        --dev)        SYSTEM_SDL=ON; BUILD="$HOME/tl-build" ;;
        --clean)      CLEAN=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "option inconnue : $a (voir --help)" >&2; exit 2 ;;
    esac
done

# --- dependances ------------------------------------------------------------
# Echouer ici avec la commande exacte vaut mieux qu'une erreur CMake obscure.
missing=()
for t in cmake ninja clang clang++; do
    command -v "$t" >/dev/null || missing+=("$t")
done
for p in libcurl libsecret-1; do
    pkg-config --exists "$p" 2>/dev/null || missing+=("$p")
done
if [ "$SYSTEM_SDL" = ON ]; then
    pkg-config --exists sdl2 2>/dev/null || missing+=("sdl2")
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "ERREUR : manquant : ${missing[*]}" >&2
    echo "  sudo apt install build-essential cmake ninja-build clang pkg-config \\" >&2
    echo "       libsdl2-dev libcurl4-openssl-dev libsecret-1-dev libgl1-mesa-dev" >&2
    exit 1
fi

[ $CLEAN -eq 1 ] && rm -rf "$BUILD"

cmake -S "$HERE" -B "$BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DTL_SYSTEM_SDL="$SYSTEM_SDL"

ninja -C "$BUILD"

BIN="$BUILD/TeamLauncher"
if [ ! -x "$BIN" ]; then
    echo "ERREUR : $BIN absent apres compilation" >&2
    exit 1
fi

echo
echo "OK : $BIN ($(du -h "$BIN" | cut -f1))"
if [ "$SYSTEM_SDL" = ON ]; then
    echo "SDL du systeme : build de developpement, NE PAS empaqueter."
    echo "Pour un paquet : ./build-linux.sh --clean puis installer/linux/make-deb.sh"
fi
