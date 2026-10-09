param([string]$GameDirectory='F:\SteamLibrary\steamapps\common\ELDEN RING\Game')
$ErrorActionPreference='Stop'
$game=(Resolve-Path -LiteralPath $GameDirectory).Path
if(Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue){throw 'Close Elden Ring first.'}
$exe=Join-Path $game 'eldenring.exe'
$expectedExe='1A3547101327F65D0C76DA2F9190AC0AA66871EA42BAE2AECC61E11A8B597891'
if((Get-FileHash -LiteralPath $exe).Hash -ne $expectedExe){throw 'This experimental build supports only the inspected Elden Ring 2.7.1.0 executable. No installation changes made.'}
$folder=Join-Path $game 'VisionEldenSBS'
foreach($name in @('VisionEldenSBS','dxgi.dll','ReShade.ini','Launch Elden Ring SBS.cmd')){
    if(Test-Path -LiteralPath (Join-Path $game $name)){throw "Existing $name must be removed through its own uninstaller first; no files were overwritten."}
}
foreach($name in @('VisionEldenSBS.addon64','ReShade64.dll','VisionEldenSBS.ini','Launch.ps1','Uninstall.ps1','README.md','THIRD-PARTY.md','LICENSE','ReShade-LICENSE.md','MinHook-LICENSE.txt','FreecamMod-LICENSE.txt')){
    if(-not(Test-Path -LiteralPath (Join-Path $PSScriptRoot $name) -PathType Leaf)){throw "Package is incomplete: $name"}
}
$graphics=Join-Path $env:APPDATA 'EldenRing\GraphicsConfig.xml'
[xml]$settings=Get-Content -LiteralPath $graphics -Encoding Unicode -Raw
$changes=@{ScreenMode='BORDERLESS';Antialiasing='DISABLE'}
foreach($name in $changes.Keys){if(-not $settings.config.SelectSingleNode($name)){throw "Missing graphics setting: $name"}}
$null=New-Item -ItemType Directory -Path $folder
$null=New-Item -ItemType Directory -Path (Join-Path $folder 'NoEffects')
$before=Join-Path $folder 'Before'
$null=New-Item -ItemType Directory -Path $before
Copy-Item -LiteralPath $graphics -Destination (Join-Path $before 'GraphicsConfig.xml')
if((Get-FileHash -LiteralPath $graphics).Hash -ne (Get-FileHash -LiteralPath (Join-Path $before 'GraphicsConfig.xml')).Hash){throw 'Graphics backup verification failed'}
foreach($name in @('VisionEldenSBS.addon64','VisionEldenSBS.ini','Launch.ps1','Uninstall.ps1','README.md','THIRD-PARTY.md','LICENSE','ReShade-LICENSE.md','MinHook-LICENSE.txt','FreecamMod-LICENSE.txt')){
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $folder $name)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ReShade64.dll') -Destination (Join-Path $game 'dxgi.dll')
@'
[ADDON]
AddonPath=.\VisionEldenSBS
DisabledAddons=Generic Depth

[GENERAL]
EffectSearchPaths=.\VisionEldenSBS\NoEffects
PresetPath=.\VisionEldenSBS\Empty.ini

[INPUT]
KeyOverlay=36,0,0,0

[OVERLAY]
TutorialProgress=4
'@ | Set-Content -LiteralPath (Join-Path $game 'ReShade.ini') -Encoding ASCII
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0VisionEldenSBS\Launch.ps1"
if errorlevel 1 pause
'@ | Set-Content -LiteralPath (Join-Path $game 'Launch Elden Ring SBS.cmd') -Encoding ASCII
foreach($name in $changes.Keys){$settings.config.SelectSingleNode($name).InnerText=$changes[$name]}
$settings.Save($graphics)
$record=[ordered]@{
    GameDirectory=$game
    GameSHA256=$expectedExe
    GraphicsPath=$graphics
    GraphicsChanges=$changes
    ReShadeSHA256=(Get-FileHash -LiteralPath (Join-Path $game 'dxgi.dll')).Hash
    AddonSHA256=(Get-FileHash -LiteralPath (Join-Path $folder 'VisionEldenSBS.addon64')).Hash
    Installed=(Get-Date).ToString('o')
}
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $folder 'install.json') -Encoding UTF8
Write-Output "Installed experimental camera SBS: $game"
Write-Output 'Launch with Launch Elden Ring SBS.cmd. In Vision Restoration use Capture window, Elden Ring, Side by side.'
