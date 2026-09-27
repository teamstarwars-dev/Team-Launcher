# Construit le launcher en Release puis fabrique l'installeur Windows.
#
# Usage :
#   powershell -ExecutionPolicy Bypass -File cpp\make-installer.ps1
#   ... -SkipBuild        : utilise le build deja present
#   ... -Version 6.0.1    : force la version (sinon lue dans CMakeLists.txt)
#
# ASCII uniquement : Windows PowerShell 5.1 lit les .ps1 en ANSI et se
# plante sur les accents (leçon de fetch-deps.ps1).

[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$Version
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root 'build'
$outDir = Join-Path (Split-Path -Parent $root) 'installer-output'

function Fail($msg) {
    Write-Host "ERREUR: $msg" -ForegroundColor Red
    exit 1
}

# --- version ---------------------------------------------------------------
if (-not $Version) {
    $cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
    if ($cmake -match 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        Fail "Version introuvable dans CMakeLists.txt ; passe -Version x.y.z."
    }
}
Write-Host "Version : $Version"

# --- build -----------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host 'Build Release...'
    & (Join-Path $root 'build-release.bat') | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "le build a echoue (code $LASTEXITCODE)." }
}

# --- verifications ---------------------------------------------------------
# Mieux vaut echouer ici avec un message clair qu'expedier un installeur
# auquel il manque la DLL de SDL.
foreach ($f in @('TeamLauncher.exe', 'SDL2.dll')) {
    $p = Join-Path $buildDir $f
    if (-not (Test-Path $p)) { Fail "$f absent de $buildDir." }
}

$exe = Get-Item (Join-Path $buildDir 'TeamLauncher.exe')
$fileVer = $exe.VersionInfo.FileVersion
if ($fileVer -and ($fileVer -notlike "$Version*")) {
    # Le bloc VERSIONINFO vient de app.rc, alimente par CMake : un ecart
    # signale un build perime.
    Fail "l'executable annonce $fileVer, l'installeur $Version. Rebuild."
}

# --- compilateur Inno ------------------------------------------------------
$iscc = $null
foreach ($c in @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe")) {
    if (Test-Path $c) { $iscc = $c; break }
}
if (-not $iscc) {
    Fail "Inno Setup 6 introuvable. Installe-le : winget install JRSoftware.InnoSetup"
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$iss = Join-Path $root 'installer\TeamLauncher.iss'
Write-Host 'Compilation de l''installeur...'
& $iscc "/DMyAppVersion=$Version" "/DBuildDir=$buildDir" "/DOutDir=$outDir" $iss
if ($LASTEXITCODE -ne 0) { Fail "ISCC a echoue (code $LASTEXITCODE)." }

$setup = Join-Path $outDir "TeamLauncher-$Version-Setup.exe"
if (-not (Test-Path $setup)) { Fail "l'installeur attendu est absent : $setup" }

$kb = [math]::Round((Get-Item $setup).Length / 1KB)
Write-Host ''
Write-Host "OK : $setup ($kb Ko)" -ForegroundColor Green
Write-Host 'Non signe : SmartScreen avertira au premier lancement apres telechargement.'
