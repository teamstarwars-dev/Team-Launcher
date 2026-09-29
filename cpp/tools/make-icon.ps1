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
    [string]$Output,
    # -Round : le logo est une pastille ronde sur fond opaque. On recadre sur
    # le cercle et on rend transparent tout ce qui deborde. Sans cela l'icone
    # est un CARRE NOIR dans la barre des taches et le menu Demarrer, avec en
    # prime une large marge vide (le cercle n'occupe que ~77 % de l'image
    # fournie).
    [switch]$Round
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

    # Zone source a reprendre. En mode -Round on mesure l'etendue reelle du
    # cercle (ligne et colonne mediennes) plutot que de supposer qu'il
    # remplit l'image : le logo fourni a 23 % de marge noire de chaque cote.
    $sx = 0; $sy = 0; $sw = $src.Width; $sh = $src.Height
    if ($Round) {
        $probe = New-Object System.Drawing.Bitmap $src
        try {
            $cx = [int]($probe.Width / 2); $cy = [int]($probe.Height / 2)
            $lit = { param($p) ($p.R + $p.G + $p.B) -gt 40 }
            $x0 = $cx; $x1 = $cx; $y0 = $cy; $y1 = $cy
            for ($x = 0; $x -lt $probe.Width; $x++) {
                if (& $lit $probe.GetPixel($x, $cy)) {
                    if ($x -lt $x0) { $x0 = $x }; if ($x -gt $x1) { $x1 = $x }
                }
            }
            for ($y = 0; $y -lt $probe.Height; $y++) {
                if (& $lit $probe.GetPixel($cx, $y)) {
                    if ($y -lt $y0) { $y0 = $y }; if ($y -gt $y1) { $y1 = $y }
                }
            }
            # Cercle centre : on prend le plus grand demi-diametre des deux
            # axes, plus 1,5 % de marge pour ne pas rogner l'anticrenelage.
            $r = [math]::Max(($x1 - $x0), ($y1 - $y0)) / 2.0 * 1.015
            $sx = [int]([math]::Max(0, $cx - $r))
            $sy = [int]([math]::Max(0, $cy - $r))
            $sw = [int]([math]::Min($probe.Width - $sx, 2 * $r))
            $sh = [int]([math]::Min($probe.Height - $sy, 2 * $r))
            Write-Host "Cercle detecte : ${sw}x${sh} a ($sx,$sy) sur $($src.Width)x$($src.Height)"
        } finally { $probe.Dispose() }
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
            if ($Round) {
                # Decoupe elliptique : les bords sont anticreneles par GDI+,
                # donc pas de marche d'escalier sur le pourtour.
                $path = New-Object System.Drawing.Drawing2D.GraphicsPath
                $path.AddEllipse(0, 0, $s, $s)
                $g.SetClip($path)
                $path.Dispose()
            }
            $g.DrawImage($src,
                (New-Object System.Drawing.Rectangle 0, 0, $s, $s),
                (New-Object System.Drawing.Rectangle $sx, $sy, $sw, $sh),
                [System.Drawing.GraphicsUnit]::Pixel)
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
