#!/bin/bash
# Mesure les protections mémoire du binaire Linux produit.
#
# À lancer après chaque changement de CMakeLists ou d'options de
# compilation. Le point de cette vérification : une protection se perd en
# silence. Rien n'échoue, rien ne ralentit — elle n'est simplement plus
# là. C'est ce qui s'était produit ici : le canari de pile était absent
# d'un binaire Release, et personne ne pouvait le savoir sans mesurer.
#
# Usage : tools/check-hardening.sh [chemin-du-binaire]
set -u

BIN="${1:-$(dirname "$0")/../build-linux/TeamLauncher}"
[ -x "$BIN" ] || { echo "binaire introuvable : $BIN"; exit 2; }

fail=0
check() { # nom attendu obtenu
    if [ "$2" = "$3" ]; then
        printf '  OK      %-26s %s\n' "$1" "$3"
    else
        printf '  ECHEC   %-26s attendu=%s obtenu=%s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

echo "Durcissement : $BIN"

# PIE : l'exécutable doit être relogeable, sinon l'ASLR ne s'applique
# qu'aux bibliothèques et le binaire lui-même reste à une adresse fixe.
check "PIE" "DYN" "$(readelf -h "$BIN" | awk '/Type:/{print $2}')"

# RELRO complet = GNU_RELRO + BIND_NOW. Sans BIND_NOW, la table de
# liaison reste inscriptible toute la vie du processus.
relro=partiel
readelf -lW "$BIN" | grep -q GNU_RELRO || relro=aucun
if [ "$relro" = partiel ] && readelf -dW "$BIN" | grep -qE 'BIND_NOW|FLAGS.*NOW'; then
    relro=complet
fi
check "RELRO" "complet" "$relro"

# Canari de pile : au moins un symbole __stack_chk.
canari=non
[ "$(readelf -sW "$BIN" 2>/dev/null | grep -c '__stack_chk')" -gt 0 ] && canari=oui
check "Canari de pile" "oui" "$canari"

# Pile non exécutable.
check "Pile non exécutable" "RW" "$(readelf -lW "$BIN" | awk '/GNU_STACK/{print $7}')"

# _FORTIFY_SOURCE : présence de variantes _chk.
fortify=non
[ "$(readelf -sW "$BIN" 2>/dev/null | grep -c '_chk@')" -gt 0 ] && fortify=oui
check "FORTIFY_SOURCE" "oui" "$fortify"

# CET : l'instrumentation (endbr64) ne suffit pas, il faut que la
# propriété soit DÉCLARÉE — elle est un ET sur toutes les entrées
# statiques, et un seul objet non instrumenté la fait disparaître du
# binaire entier sans le moindre avertissement.
cet=non
readelf -nW "$BIN" 2>/dev/null | grep -q 'IBT' && cet=oui
check "CET déclaré (IBT)" "oui" "$cet"

echo
if [ "$fail" -eq 0 ]; then
    echo "Toutes les protections attendues sont présentes."
    exit 0
fi
echo "$fail protection(s) manquante(s)."
exit 1
