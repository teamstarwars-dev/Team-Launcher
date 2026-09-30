# build-aot.ps1 - Build TeamLauncher.Avalonia as a static NativeAOT single-file EXE
# Requirements: VS Build Tools 2022 (MSVC + Windows SDK), .NET 8 SDK

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot

# 1. Clean
Write-Host "== Nettoyage =="
Remove-Item "$repoRoot\src\TeamLauncher.Avalonia\obj" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$repoRoot\src\TeamLauncher.Avalonia\bin\Release" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$repoRoot\publish-avalonia\*" -Recurse -Force -ErrorAction SilentlyContinue

# 2. Publish NativeAOT (stops at linking step due to space-in-path bug, but ILC obj + link.rsp are produced)
Write-Host "== Publish AOT (l'erreur de link est attendue) =="
dotnet publish "$repoRoot\src\TeamLauncher.Avalonia\TeamLauncher.Avalonia.csproj" -c Release -r win-x64 -o "$repoRoot\publish-avalonia" 2>&1 | Out-Null

# 3. Fix link.rsp double backslashes (SDK bug with WindowsKits path)
$rspDir = "$repoRoot\src\TeamLauncher.Avalonia\obj\Release\net8.0\win-x64\native"
$rspFile = Join-Path $rspDir "link.rsp"
if (!(Test-Path $rspFile)) { Write-Host "ERROR: link.rsp not found - ILC did not run"; exit 1 }

$content = Get-Content $rspFile -Raw
$content = $content.Replace('\\lib', '\lib').Replace('\\um', '\um')
Set-Content $rspFile $content -NoNewline

# 4. Copy link.rsp to a temp path WITHOUT spaces (bat file reference)
$tempRsp = Join-Path $env:TEMP "link.rsp"
Copy-Item $rspFile $tempRsp -Force

# 5. Find link.exe
$msvcDir = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$linkExe = "$($msvcDir.FullName)\bin\Hostx64\x64\link.exe"

# 6. Link natively via a .bat (cmd handles @responsefile + quoted paths correctly)
Write-Host "== Link AOT =="
$batFile = Join-Path $repoRoot "run-link2.bat"
$batContent = "@echo off`r`nsetlocal`r`ncd /d `"$repoRoot\src\TeamLauncher.Avalonia`"`r`n`"$linkExe`" /NOLOGO /MANIFEST:EMBED @`"$tempRsp`"`r`necho LINK_EXIT_CODE=%ERRORLEVEL%`r`nendlocal"
Set-Content $batFile $batContent -Encoding ASCII

cmd /c `"$batFile`"
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: Native link failed with code $LASTEXITCODE"; exit 1 }

# 7. Copy final single-file EXE to publish
$nativeExe = "$repoRoot\src\TeamLauncher.Avalonia\bin\Release\net8.0\win-x64\native\TeamLauncher.Avalonia.exe"
Remove-Item "$repoRoot\publish-avalonia\*" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $nativeExe "$repoRoot\publish-avalonia\TeamLauncher.Avalonia.exe" -Force
Remove-Item "$repoRoot\publish-avalonia\TeamLauncher.Avalonia.pdb" -Force -ErrorAction SilentlyContinue

# 8. Report
$exePath = "$repoRoot\publish-avalonia\TeamLauncher.Avalonia.exe"
if (Test-Path $exePath) {
    $exe = Get-Item $exePath
    Write-Host ("DONE! Single-file EXE: {0} MB" -f [math]::Round($exe.Length/1MB,1))
} else {
    Write-Host "FAILED: TeamLauncher.Avalonia.exe not found"
    exit 1
}