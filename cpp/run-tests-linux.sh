#!/bin/bash
# Lance toutes les suites d'un build Linux et resume le resultat.
#   ./run-tests-linux.sh [dossier-de-build]   (defaut : ~/tl-build)
set -uo pipefail

BUILD="${1:-$HOME/tl-build}"
[ -d "$BUILD" ] || { echo "ERREUR : $BUILD introuvable"; exit 1; }
cd "$BUILD"

fail=0
total=0
for f in TLTest*; do
    [ -x "$f" ] && [ -f "$f" ] || continue
    total=$((total + 1))
    line="$(./"$f" 2>&1 | tail -1)"
    printf '%-24s %s\n' "$f" "$line"
    case "$line" in
        *"ALL TESTS PASSED"*) ;;
        *) fail=$((fail + 1)) ;;
    esac
done

echo
if [ "$fail" -eq 0 ]; then
    echo "OK : $total/$total suites vertes"
else
    echo "ECHEC : $fail suite(s) sur $total"
fi
exit "$fail"
