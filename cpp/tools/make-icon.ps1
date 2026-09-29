# Fabrique cpp/assets/TeamLauncher.ico a partir de cpp/assets/logo.png.
#
# Pourquoi un script plutot qu'un .ico commite tel quel : le logo evolue, et
# un .ico produit par un convertisseur en ligne embarque souvent une seule
# taille (ou une taille 256 non compressee de 75 Ko, ce qu'etait l'ancienne
# icone heritee du C#). Ici les tailles sont explicites et les grandes sont
# stockees en PNG, ce que Windows accepte depuis Vista.
#
# Usage : powershell -ExecutionPolicy Bypass -File cpp\tools\make-icon.ps1
#
# ASCII uniquement (Windows PowerShell 5.1 lit les .ps1 en ANSI).

[CmdletBinding()]
param(
    [string]$Source,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$assets = Join-Path (Split-Path -Parent $tools) 'assets'
if (-not $Source) { $Source = Join-Path $assets 'logo.png' }
if (-not $Output) { $Output = Join-Path $assets 'TeamLauncher.ico' }

if (-not (Test-Path $Source)) {
    Write-Host "ERREUR: source introuvable : $Source" -ForegroundColor Red
    Write-Host "Depose le logo en PNG carre (512x512 ou plus) a cet emplacement."
    exit 1
}

$src = [System.Drawing.Image]::FromFile($Source)
try {
    if ($src.Width -ne $src.Height) {
        Write-Host "AVERTISSEMENT: image non carree ($($src.Width)x$($src.Height)), elle sera deformee." -ForegroundColor Yellow
    }

    # 16 a 64 : tailles reellement utilisees par l'explorateur, la barre des
    # taches et la fenetre. 128 et 256 pour l'affichage en grandes icones.
    $sizes = @(16, 24, 32, 48, 64, 128, 256)
    $entries = @()

    foreach ($s in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap $s, $s
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Transparent)
            $g.DrawImage($src, 0, 0, $s, $s)
        } finally { $g.Dispose() }

        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $entries += [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
        $ms.Dispose()
    }
} finally { $src.Dispose() }

# --- ecriture du conteneur ICO --------------------------------------------
# En-tete ICONDIR (6 octets) puis une ICONDIRENTRY de 16 octets par image,
# puis les donnees. Une dimension de 256 s'ecrit 0 dans le champ d'un octet.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
try {
    $w.Write([UInt16]0)                 # reserve
    $w.Write([UInt16]1)                 # type 1 = icone
    $w.Write([UInt16]$entries.Count)

    $offset = 6 + 16 * $entries.Count
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
        $w.Write([Byte]$dim)            # largeur
        $w.Write([Byte]$dim)            # hauteur
        $w.Write([Byte]0)               # palette
        $w.Write([Byte]0)               # reserve
        $w.Write([UInt16]1)             # plans
        $w.Write([UInt16]32)            # bits par pixel
        $w.Write([UInt32]$e.Data.Length)
        $w.Write([UInt32]$offset)
        $offset += $e.Data.Length
    }
    foreach ($e in $entries) { $w.Write($e.Data) }
    $w.Flush()
    [System.IO.File]::WriteAllBytes($Output, $out.ToArray())
} finally { $w.Dispose(); $out.Dispose() }

$kb = [math]::Round((Get-Item $Output).Length / 1KB, 1)
Write-Host "OK : $Output ($kb Ko, $($entries.Count) tailles)" -ForegroundColor Green

# Le .rc compile l'icone depuis cpp/src : on y garde une copie a jour.
$srcCopy = Join-Path (Join-Path (Split-Path -Parent $tools) 'src') 'TeamLauncher.ico'
Copy-Item $Output $srcCopy -Force
Write-Host "Copie pour la ressource Win32 : $srcCopy"
