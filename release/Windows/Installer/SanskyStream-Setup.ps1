#Requires -RunAsAdministrator
# ===========================================================================
# SanskyStream 1.0.0 — Windows Installer
# ===========================================================================
# Usage: Right-click this file -> Run with PowerShell (as Administrator)
#        Or: powershell -ExecutionPolicy Bypass -File SanskyStream-Setup.ps1
# ===========================================================================

$ErrorActionPreference = "Stop"

$APP_NAME     = "SanskyStream"
$APP_VERSION  = "1.0.0"
$INSTALL_DIR  = "$env:ProgramFiles\$APP_NAME"
# Resolve installer location robustly: PSScriptRoot is set when running as a .ps1 file;
# fall back to MyInvocation for dot-sourced / interactive use.
$SCRIPT_DIR   = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Definition }
# Portable directory is one level up from the Installer directory, then Windows\Portable
$PORTABLE_DIR = Join-Path (Split-Path -Parent $SCRIPT_DIR) "Portable"
$UNINSTALL_KEY= "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SanskyStream"
$UNINSTALL_PS = Join-Path $INSTALL_DIR "SanskyStream-Uninstall.ps1"

function Write-Step { param([string]$msg) Write-Host "`n[*] $msg" -ForegroundColor Cyan }
function Write-OK   { param([string]$msg) Write-Host "    [OK] $msg" -ForegroundColor Green }
function Write-Fail { param([string]$msg) Write-Host "    [!!] $msg" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "  =================================" -ForegroundColor White
Write-Host "   SanskyStream $APP_VERSION Setup  " -ForegroundColor White
Write-Host "  =================================" -ForegroundColor White
Write-Host ""

# ---- Verify source files exist ----
Write-Step "Verifying source files..."
$requiredFiles = @(
    "SanskyStream_Windows.exe",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
foreach ($f in $requiredFiles) {
    $p = Join-Path $PORTABLE_DIR $f
    if (!(Test-Path $p)) { Write-Fail "Missing required file: $f (expected in $PORTABLE_DIR)" }
}
Write-OK "All required source files present."

# ---- Create install directory ----
Write-Step "Creating install directory: $INSTALL_DIR"
if (!(Test-Path $INSTALL_DIR)) {
    New-Item -ItemType Directory -Path $INSTALL_DIR -Force | Out-Null
}
Write-OK "Directory ready."

# ---- Copy files ----
Write-Step "Copying application files..."
foreach ($f in $requiredFiles) {
    Copy-Item (Join-Path $PORTABLE_DIR $f) (Join-Path $INSTALL_DIR $f) -Force
    Write-OK "Copied $f"
}

# ---- Write uninstaller ----
Write-Step "Writing uninstaller..."
$uninstallScript = @"
#Requires -RunAsAdministrator
`$APP_NAME    = "SanskyStream"
`$INSTALL_DIR = "`$env:ProgramFiles\`$APP_NAME"
`$UNREG_KEY   = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SanskyStream"

Write-Host "Uninstalling SanskyStream..." -ForegroundColor Cyan

# Remove Start Menu shortcut
`$startMenu = [Environment]::GetFolderPath("CommonStartMenu")
`$lnk = "`$startMenu\Programs\SanskyStream.lnk"
if (Test-Path `$lnk) { Remove-Item `$lnk -Force; Write-Host "[OK] Start Menu shortcut removed." }

# Remove Desktop shortcut
`$desktop = [Environment]::GetFolderPath("CommonDesktopDirectory")
`$dlnk = "`$desktop\SanskyStream.lnk"
if (Test-Path `$dlnk) { Remove-Item `$dlnk -Force; Write-Host "[OK] Desktop shortcut removed." }

# Remove registry entry
if (Test-Path `$UNREG_KEY) { Remove-Item `$UNREG_KEY -Force; Write-Host "[OK] Registry entry removed." }

# Remove install directory (after short delay to allow this script to finish)
Start-Sleep -Seconds 1
if (Test-Path `$INSTALL_DIR) {
    Remove-Item `$INSTALL_DIR -Recurse -Force
    Write-Host "[OK] Install directory removed: `$INSTALL_DIR"
}
Write-Host "`nSanskyStream has been uninstalled." -ForegroundColor Green
"@
[System.IO.File]::WriteAllText($UNINSTALL_PS, $uninstallScript, (New-Object System.Text.UTF8Encoding($false)))
Write-OK "Uninstaller written: $UNINSTALL_PS"

# ---- Start Menu shortcut ----
Write-Step "Creating Start Menu shortcut..."
$startMenu = [Environment]::GetFolderPath("CommonStartMenu")
$progDir   = "$startMenu\Programs"
if (!(Test-Path $progDir)) { New-Item -ItemType Directory $progDir -Force | Out-Null }
$wsh     = New-Object -ComObject WScript.Shell
$lnk     = $wsh.CreateShortcut("$progDir\SanskyStream.lnk")
$lnk.TargetPath       = "$INSTALL_DIR\SanskyStream_Windows.exe"
$lnk.WorkingDirectory = $INSTALL_DIR
$lnk.Description      = "SanskyStream — Screen Mirroring Receiver"
$lnk.Save()
Write-OK "Start Menu shortcut created."

# ---- Desktop shortcut ----
Write-Step "Creating Desktop shortcut..."
$desktop = [Environment]::GetFolderPath("CommonDesktopDirectory")
$dlnk    = $wsh.CreateShortcut("$desktop\SanskyStream.lnk")
$dlnk.TargetPath       = "$INSTALL_DIR\SanskyStream_Windows.exe"
$dlnk.WorkingDirectory = $INSTALL_DIR
$dlnk.Description      = "SanskyStream — Screen Mirroring Receiver"
$dlnk.Save()
Write-OK "Desktop shortcut created."

# ---- Windows Firewall rule ----
Write-Step "Adding Windows Firewall rule..."
try {
    $existing = Get-NetFirewallRule -DisplayName "SanskyStream" -ErrorAction SilentlyContinue
    if ($existing) { Remove-NetFirewallRule -DisplayName "SanskyStream" -ErrorAction SilentlyContinue }
    New-NetFirewallRule -DisplayName "SanskyStream" `
        -Direction Inbound `
        -Program "$INSTALL_DIR\SanskyStream_Windows.exe" `
        -Action Allow `
        -Protocol Any `
        -Profile Private,Domain `
        -Description "Allow SanskyStream receiver (TCP 5000, UDP 5001)" | Out-Null
    Write-OK "Firewall rule added (Private + Domain networks)."
} catch {
    Write-Host "    [WARN] Firewall rule could not be added: $_" -ForegroundColor Yellow
    Write-Host "           You may need to allow SanskyStream_Windows.exe manually." -ForegroundColor Yellow
}

# ---- Registry uninstall entry ----
Write-Step "Registering uninstaller..."
if (!(Test-Path $UNINSTALL_KEY)) { New-Item -Path $UNINSTALL_KEY -Force | Out-Null }
Set-ItemProperty $UNINSTALL_KEY "DisplayName"        "$APP_NAME"
Set-ItemProperty $UNINSTALL_KEY "DisplayVersion"     "$APP_VERSION"
Set-ItemProperty $UNINSTALL_KEY "Publisher"          "SanskyStream"
Set-ItemProperty $UNINSTALL_KEY "InstallLocation"    "$INSTALL_DIR"
Set-ItemProperty $UNINSTALL_KEY "UninstallString"    "powershell -ExecutionPolicy Bypass -File `"$UNINSTALL_PS`""
Set-ItemProperty $UNINSTALL_KEY "QuietUninstallString" "powershell -ExecutionPolicy Bypass -NonInteractive -File `"$UNINSTALL_PS`""
Set-ItemProperty $UNINSTALL_KEY "NoModify"           1 -Type DWord
Set-ItemProperty $UNINSTALL_KEY "NoRepair"           1 -Type DWord
Write-OK "Uninstall entry registered in Add/Remove Programs."

# ---- Done ----
Write-Host ""
Write-Host "  =================================" -ForegroundColor Green
Write-Host "   Installation complete!           " -ForegroundColor Green
Write-Host "  =================================" -ForegroundColor Green
Write-Host ""
Write-Host "  SanskyStream $APP_VERSION installed to:" -ForegroundColor White
Write-Host "    $INSTALL_DIR" -ForegroundColor White
Write-Host ""
Write-Host "  Launch from the Desktop or Start Menu shortcut." -ForegroundColor White
Write-Host ""
Write-Host "  To uninstall: Settings > Apps > SanskyStream > Uninstall" -ForegroundColor Gray
Write-Host "             or run: $UNINSTALL_PS" -ForegroundColor Gray
Write-Host ""