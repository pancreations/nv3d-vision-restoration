param([switch]$Launch)
$ErrorActionPreference = 'Stop'
$gameDir = 'D:\SteamLibrary\steamapps\common\Schedule I'
$exe = Join-Path $gameDir 'Schedule I.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Schedule I installation was not found.' }
$gameProcess = Get-Process -Name 'Schedule I' -ErrorAction SilentlyContinue
if ($gameProcess) {
    if ($gameProcess.Path -ne $exe) { throw 'Another Schedule I installation is running.' }
    $null = $gameProcess.CloseMainWindow()
    Start-Sleep -Seconds 2
    if (Get-Process -Name 'Schedule I' -ErrorAction SilentlyContinue) { throw 'Please close Schedule I; installation stopped.' }
}
$mods = Join-Path $gameDir 'Mods'
New-Item -ItemType Directory -Force -Path $mods | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'package\ScheduleIStereoSBS.dll') -Destination (Join-Path $mods 'ScheduleIStereoSBS.dll') -Force
if ($Launch) { Start-Process -FilePath $exe -WorkingDirectory $gameDir -WindowStyle Hidden }
