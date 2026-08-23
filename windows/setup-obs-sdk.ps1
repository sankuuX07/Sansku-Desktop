<#
.SYNOPSIS
    Acquires the OBS Studio plugin SDK headers and generates obs.lib.

.DESCRIPTION
    1. Sparse-clones only the 'libobs/' directory from obsproject/obs-studio (depth 1).
    2. Copies headers into windows/obs-sdk/libobs/.
    3. Generates obs.lib from the installed obs.dll via dumpbin + lib.exe.

    Safe to re-run. All output is gitignored.

.PARAMETER ObsInstallDir
    OBS Studio installation directory. Defaults to C:\Program Files\obs-studio.

.PARAMETER ObsSdkDir
    Output path for the SDK files. Defaults to obs-sdk/ next to this script.

.EXAMPLE
    .\setup-obs-sdk.ps1
    .\setup-obs-sdk.ps1 -ObsInstallDir "D:\OBS"
#>
param(
    [string]$ObsInstallDir = "C:\Program Files\obs-studio",
    [string]$ObsSdkDir     = "$PSScriptRoot\obs-sdk"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "============================================================"
Write-Host "  SanskyStream M14 -- OBS SDK Setup"
Write-Host "============================================================"

# 1. Validate OBS installation
$ObsDll = Join-Path $ObsInstallDir "bin\64bit\obs.dll"
if (-not (Test-Path $ObsDll)) {
    Write-Error "OBS Studio not found at '$ObsInstallDir'. Install OBS or pass -ObsInstallDir."
}
$ver = (Get-Item $ObsDll).VersionInfo.ProductVersion
Write-Host "[1/4] Found OBS Studio $ver at '$ObsInstallDir'."

# 2. Sparse-clone libobs headers
Write-Host "[2/4] Fetching libobs headers from GitHub (sparse, depth 1)..."

$TempDir = Join-Path $env:TEMP "obs-sparse-$(Get-Random)"
if (Test-Path $TempDir) { Remove-Item -Recurse -Force $TempDir }
New-Item -ItemType Directory -Path $TempDir | Out-Null

try {
    Push-Location $TempDir
    git init --quiet 2>&1 | Out-Null
    git remote add origin "https://github.com/obsproject/obs-studio.git" 2>&1 | Out-Null
    git sparse-checkout init --cone 2>&1 | Out-Null
    git sparse-checkout set libobs 2>&1 | Out-Null
    Write-Host "  Fetching -- this downloads ~3 MB of headers..."
    git fetch --depth 1 origin HEAD --quiet 2>&1 | Out-Null
    git checkout FETCH_HEAD --quiet 2>&1 | Out-Null

    $FetchedDir = Join-Path $TempDir "libobs"
    if (-not (Test-Path $FetchedDir)) {
        Write-Error "Sparse checkout failed -- libobs directory not found."
    }

    $DestDir = Join-Path $ObsSdkDir "libobs"
    if (Test-Path $DestDir) { Remove-Item -Recurse -Force $DestDir }
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
    Copy-Item -Path "$FetchedDir\*" -Destination $DestDir -Recurse
    Write-Host "  Headers copied to '$DestDir'."
}
finally {
    Pop-Location
    Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
}

# 3. Locate MSVC tools
Write-Host "[3/4] Locating MSVC dumpbin.exe and lib.exe..."

$MsvcBinDir = $null
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    $VsWhere = "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
}
if (Test-Path $VsWhere) {
    $VsPath = & $VsWhere -latest -property installationPath 2>$null
    if ($VsPath) {
        $MsvcVers = Get-ChildItem "$VsPath\VC\Tools\MSVC" -ErrorAction SilentlyContinue |
                    Sort-Object Name -Descending | Select-Object -First 1
        if ($MsvcVers) {
            $Cand = Join-Path $MsvcVers.FullName "bin\Hostx64\x64"
            if (Test-Path "$Cand\dumpbin.exe") { $MsvcBinDir = $Cand }
        }
    }
}
if (-not $MsvcBinDir) {
    $Db = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($Db) { $MsvcBinDir = Split-Path $Db.Source }
    else { Write-Error "dumpbin.exe not found. Run from a VS Developer PowerShell." }
}
$DumpbinExe = Join-Path $MsvcBinDir "dumpbin.exe"
$LibExe     = Join-Path $MsvcBinDir "lib.exe"
Write-Host "  dumpbin: $DumpbinExe"
Write-Host "  lib:     $LibExe"

# 4. Generate obs.lib
Write-Host "[4/4] Generating obs.lib from obs.dll..."

$BinDir  = Join-Path $ObsSdkDir "bin"
New-Item -ItemType Directory -Path $BinDir -Force | Out-Null
$ExpFile = Join-Path $BinDir "obs.exports"
$DefFile = Join-Path $BinDir "obs.def"
$LibFile = Join-Path $ObsSdkDir "obs.lib"

& $DumpbinExe /EXPORTS $ObsDll > $ExpFile 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "dumpbin failed." }

$Exports = Get-Content $ExpFile |
    Where-Object { $_ -match '^\s+\d+\s+[\dA-Fa-f]+\s+[\dA-Fa-f]+\s+\w' } |
    ForEach-Object { ($_ -split '\s+')[4] } |
    Where-Object { $_ -ne "" }

"LIBRARY obs`r`nEXPORTS`r`n" + (($Exports | ForEach-Object { "    $_" }) -join "`r`n") |
    Set-Content -Path $DefFile -Encoding ASCII

Write-Host "  Exporting $($Exports.Count) symbols."
& $LibExe /DEF:$DefFile /OUT:$LibFile /MACHINE:X64 /NOLOGO 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Error "lib.exe failed." }

Remove-Item $ExpFile, $DefFile -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "============================================================"
Write-Host "  OBS SDK ready:"
Write-Host "    Headers : $ObsSdkDir\libobs\"
Write-Host "    Library : $LibFile"
Write-Host ""
Write-Host "  Next steps:"
Write-Host "    cmake -B build -G 'Visual Studio 18 2026' -A x64 ."
Write-Host "    cmake --build build --config Release --target sansky-source"
Write-Host "    .\install-plugin.ps1   (run as Administrator)"
Write-Host "============================================================"
