# Mesure les protections mémoire du binaire Windows produit.
#
# Même raison que la version Linux : une protection se perd en silence.
# Control Flow Guard et CET étaient absents du binaire livré sans que
# rien ne le signale — ni avertissement, ni échec, ni ralentissement.
#
# Usage : tools\check-hardening.ps1 [chemin-de-l-exe]

param([string]$Exe = "$PSScriptRoot\..\build\TeamLauncher.exe")

if (-not (Test-Path $Exe)) { Write-Output "binaire introuvable : $Exe"; exit 2 }

$vc = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
if (-not $vc) { Write-Output "VC Build Tools introuvables (dumpbin requis)"; exit 2 }
$dumpbin = Join-Path $vc.FullName "bin\Hostx64\x64\dumpbin.exe"
if (-not (Test-Path $dumpbin)) { Write-Output "dumpbin introuvable : $dumpbin"; exit 2 }

$headers = & $dumpbin /headers $Exe 2>&1 | Out-String

# Chaque entrée est une caractéristique DLL que dumpbin imprime en clair
# quand elle est présente. Toutes sont attendues : aucune n'a de coût
# mesurable, et leur absence ne se voit nulle part ailleurs.
$attendus = @(
    @{ Nom = "ASLR (Dynamic base)";      Motif = "Dynamic base" },
    @{ Nom = "DEP (NX compatible)";      Motif = "NX compatible" },
    @{ Nom = "ASLR haute entropie";      Motif = "High Entropy Virtual Addresses" },
    @{ Nom = "Control Flow Guard";       Motif = "Control Flow Guard" },
    @{ Nom = "CET (pile fantôme)";       Motif = "CET compatible" }
)

Write-Output "Durcissement : $Exe"
$manquants = 0
foreach ($a in $attendus) {
    if ($headers -match [regex]::Escape($a.Motif)) {
        "  OK      {0}" -f $a.Nom
    } else {
        "  ECHEC   {0} — absent du binaire" -f $a.Nom
        $manquants++
    }
}

Write-Output ""
if ($manquants -eq 0) {
    Write-Output "Toutes les protections attendues sont présentes."
    exit 0
}
Write-Output "$manquants protection(s) manquante(s)."
exit 1
