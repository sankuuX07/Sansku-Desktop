<#
.SYNOPSIS
    Installs sansky-source.dll into OBS Studio.

.DESCRIPTION
    Copies the compiled OBS plugin DLL from the CMake build output into
    the OBS Studio plugins directory and prints setup instructions.

    Run as Administrator.

.PARAMETER BuildDir
    CMake build directory. Defaults to 'build' next to this script.

.PARAMETER ObsInstallDir
    OBS Studio installation directory. Defaults to C:\Program Files\obs-studio.

.EXAMPLE
    .\install-plugin.ps1
    .\install-plugin.ps1 -ObsInstallDir "D:\OBS" -BuildDir "build-release"
#>
param(
    [string]$BuildDir      = "$PSScriptRoot\build",
    [string]$ObsInstallDir = "C:\Program Files\obs-studio"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Write-Host "============================================================"
Write-Host "  SanskyStream M14 -- Install OBS Plugin"
Write-Host "============================================================"

# 1. Locate the built DLL
$DllPath = Join-Path $BuildDir "obs-plugin\sansky-source.dll"
if (-not (Test-Path $DllPath)) {
    Write-Error (
        "sansky-source.dll not found at '$DllPath'.`n" +
        "Build it first:`n" +
        "  cmake -B build -G 'Visual Studio 18 2026' -A x64 .`n" +
        "  cmake --build build --config Release --target sansky-source"
    )
}
Write-Host "[1/3] Found plugin DLL: $DllPath"

# 2. Verify OBS installation
$ObsBin = Join-Path $ObsInstallDir "bin\64bit\obs64.exe"
if (-not (Test-Path $ObsBin)) {
    Write-Error "OBS Studio not found at '$ObsInstallDir'. Install OBS or pass -ObsInstallDir."
}
Write-Host "[2/3] Found OBS: $ObsBin"

# 3. Copy the DLL
$PluginDir = Join-Path $ObsInstallDir "obs-plugins\64bit"
if (-not (Test-Path $PluginDir)) {
    New-Item -ItemType Directory -Path $PluginDir -Force | Out-Null
}
Copy-Item -Path $DllPath -Destination $PluginDir -Force
Write-Host "[3/3] Installed: $PluginDir\sansky-source.dll"

Write-Host ""
Write-Host "============================================================"
Write-Host "  Plugin installed successfully."
Write-Host ""
Write-Host "  How to use:"
Write-Host "  1. Start SanskyStream.exe (creates the shared memory)"
Write-Host "  2. Open OBS Studio"
Write-Host "  3. In Sources, click '+' -> 'SanskyStream iPhone'"
Write-Host "  4. Name the source and click OK"
Write-Host "  5. The iPhone video will appear in the OBS scene"
Write-Host "  6. Audio is mixed automatically in the OBS audio mixer"
Write-Host ""
Write-Host "  Notes:"
Write-Host "  - SanskyStream.exe must be running for the source to show video."
Write-Host "  - If you close SanskyStream, the source shows black until it restarts."
Write-Host "  - SanskyStream also plays audio locally via WASAPI (monitoring)."
Write-Host "  - OBS controls the stream/recording as normal."
Write-Host "============================================================"
