# Recupere les dependances tierces du portage C++ sur leurs versions exactes.
# third_party/ n'est pas dans le depot (106 Mo) : lancer ce script une fois
# apres un clone frais, puis cpp\build-release.bat.
#
#   powershell -ExecutionPolicy Bypass -File cpp\fetch-deps.ps1
#
# -Force reclone les dependances deja presentes.

param([switch]$Force)

$ErrorActionPreference = 'Stop'

# git ecrit ses messages de progression ("Cloning into...", avertissement
# "detached HEAD") sur STDERR. Sous Windows PowerShell 5.1, une commande native
# qui ecrit sur stderr leve une NativeCommandError quand $ErrorActionPreference
# vaut 'Stop' : le clone reussissait mais le script s'arretait quand meme.
# On relache donc la preference le temps de l'appel, et on se fie au code de
# sortie de git, seul indicateur fiable ici.
function Invoke-Git {
  param([string[]]$GitArgs)
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & git @GitArgs 2>&1 | Out-Null
    return $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $saved
  }
}

$root = Join-Path $PSScriptRoot 'third_party'
New-Item -ItemType Directory -Force $root | Out-Null

# Versions epinglees : elles correspondent aux mesures du journal de portage
# (docs/ETAPE1-estimations.md). Ne pas les bouger sans remesurer taille et RAM.
$deps = @(
  @{ Name = 'imgui';         Url = 'https://github.com/ocornut/imgui.git';      Tag = 'v1.91.8' }
  @{ Name = 'SDL';           Url = 'https://github.com/libsdl-org/SDL.git';     Tag = 'release-2.30.9' }
  @{ Name = 'cpp-httplib';   Url = 'https://github.com/yhirose/cpp-httplib.git'; Tag = 'v0.18.3' }
  @{ Name = 'nlohmann_json'; Url = 'https://github.com/nlohmann/json.git';      Tag = 'v3.11.3' }
  @{ Name = 'miniz';         Url = 'https://github.com/richgel999/miniz.git';   Tag = '3.1.2' }
)

$failed = @()

foreach ($d in $deps) {
  $dest = Join-Path $root $d.Name
  if (Test-Path $dest) {
    if (-not $Force) { Write-Host ("OK   {0,-14} deja present" -f $d.Name); continue }
    Remove-Item -LiteralPath $dest -Recurse -Force
  }

  # GitHub etrangle parfois plusieurs clones d'affilee (SDL fait 75 Mo) :
  # on reessaie plutot que d'abandonner toute la recuperation.
  $ok = $false
  foreach ($try in 1..3) {
    Write-Host ("---> {0,-14} {1}{2}" -f $d.Name, $d.Tag,
                $(if ($try -gt 1) { " (tentative $try/3)" } else { "" }))
    # --depth 1 : on ne veut que l'arbre du tag, pas l'historique amont.
    $rc = Invoke-Git @('clone', '--depth', '1', '--branch', $d.Tag, $d.Url, $dest)
    if ($rc -eq 0) { $ok = $true; break }
    # Ne jamais laisser un dossier a moitie clone derriere soi.
    if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Start-Sleep -Seconds (2 * $try)
  }
  # Une dependance en echec n'interrompt pas les suivantes : on releve tout
  # a la fin, l'utilisateur relance le script pour completer.
  if (-not $ok) { $failed += $d.Name; Write-Warning "echec du clone de $($d.Name)" }
}

# miniz : le build amont genere miniz_export.h ; on le synthetise (build
# statique, aucun symbole exporte) - cf. journal de portage, module 2.
$exp = Join-Path $root 'miniz\miniz_export.h'
if (-not (Test-Path $exp)) {
  @'
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H
/* Genere par cpp/fetch-deps.ps1 : miniz est compile en statique dans tl_core,
   aucun symbole n'a besoin d'etre exporte. */
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT
#endif
'@ | Set-Content $exp -Encoding UTF8
  Write-Host "---> miniz_export.h genere"
}

# stb : un seul en-tete, versionne dans le depot (0,3 Mo) - rien a faire.
$stb = Join-Path $root 'stb\stb_image.h'
if (-not (Test-Path $stb)) {
  Write-Warning "third_party/stb/stb_image.h manquant (normalement versionne)."
}

if ($failed.Count -gt 0) {
  Write-Host ""
  Write-Warning ("Manquantes : " + ($failed -join ', ') +
                 " - relance le script pour completer.")
  exit 1
}

Write-Host "`nDependances pretes. Build : cpp\build-release.bat"
