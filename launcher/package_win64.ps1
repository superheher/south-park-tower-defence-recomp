# Builds the bring-your-own-dump Windows distributable for South Park: Let's Go
# Tower Defense. The zip contains ONLY the engine + its runtime deps + docs -
# NO game data (the player supplies their own dump via the in-engine onboarding).
#
#   powershell -ExecutionPolicy Bypass -File package_win64.ps1
#
param(
  [string]$BuildDir = (Join-Path $PSScriptRoot "..\out\build\win-amd64-relwithdebinfo"),
  [string]$OutZip   = "C:\Temp\SouthParkTD-win64.zip"
)
$ErrorActionPreference = "Stop"

$stage  = Join-Path $env:TEMP ("SouthParkTD-stage-" + [guid]::NewGuid().ToString("N"))
$appdir = Join-Path $stage "SouthParkTD"
New-Item -ItemType Directory -Force -Path $appdir | Out-Null

# Engine + runtime deps loaded from the build dir (verified via loaded-modules).
$fromBuild = @("south_park_td.exe", "rexruntimerd.dll", "TracyClientrd.dll", "SouthPark.ico")
foreach ($f in $fromBuild) {
  $src = Join-Path $BuildDir $f
  if (-not (Test-Path $src)) { throw "Missing build output: $src" }
  Copy-Item $src $appdir
}

# App-local VC++ runtime so a clean machine needs no VC++ redist installed.
$sys = Join-Path $env:WINDIR "System32"
$vc  = @("MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140_ATOMIC_WAIT.dll")
foreach ($f in $vc) {
  $src = Join-Path $sys $f
  if (Test-Path $src) { Copy-Item $src $appdir } else { Write-Warning "VC++ runtime not found: $f" }
}

# Docs.
Copy-Item (Join-Path $PSScriptRoot "INTEGRATION.md") $appdir
$readme = @"
South Park: Let's Go Tower Defense - recompiled, self-onboarding build
=====================================================================

HOW TO PLAY
  1. Run south_park_td.exe
  2. First run: a setup wizard appears. Point it at YOUR OWN copy of the game:
       - an STFS package file (your Xbox 360 console dump), OR
       - a folder of extracted game files (containing default.xex)
     Use the path box or the Browse... button (mouse, keyboard, or gamepad).
  3. Adjust settings if you like, then Play. Your choice is remembered, so the
     next launch goes STRAIGHT INTO THE GAME (the wizard does not show again).

OPEN SETTINGS / CHANGE GAME LATER
  Once a game is chosen, running south_park_td.exe goes straight into the game.
  To get the setup & settings wizard back, double-click *** Setup.bat ***
  (or run "south_park_td.exe --setup" from a terminal).

BRING YOUR OWN GAME
  This package contains NO game data. You must supply your own legally-obtained
  copy. Nothing is downloaded; your copy never leaves this machine.

FRONTENDS (Playnite / Steam / EmuDeck / scripts)
  The engine is also the launcher contract:
    Validate a dump:  south_park_td.exe --validate="<path>"   (JSON to stdout, exit 0/65)
    Launch headless:  south_park_td.exe --game_data_root="<path>" --no_setup
  See INTEGRATION.md for the full cvar / config-file / exit-code contract.
"@
Set-Content -Path (Join-Path $appdir "README.txt") -Value $readme -Encoding ascii

# Setup.bat: a discoverable double-click that re-opens the onboarding/settings
# wizard (the exe alone goes straight into the game once a game is configured).
$setupBat = @'
@echo off
rem Open the South Park setup & settings wizard (choose your game / change settings).
start "" "%~dp0south_park_td.exe" --setup
'@
Set-Content -Path (Join-Path $appdir "Setup.bat") -Value $setupBat -Encoding ascii

# Rebuild the zip fresh (never in-place-update a multi-file zip).
if (Test-Path $OutZip) { Remove-Item $OutZip -Force }
New-Item -ItemType Directory -Force -Path (Split-Path $OutZip) | Out-Null
Compress-Archive -Path (Join-Path $stage "SouthParkTD") -DestinationPath $OutZip
Remove-Item -Recurse -Force $stage

$mb = [math]::Round((Get-Item $OutZip).Length / 1MB, 1)
Write-Host "Built $OutZip ($mb MB)"
Get-ChildItem $appdir -ErrorAction SilentlyContinue  # (stage already removed; informational)
