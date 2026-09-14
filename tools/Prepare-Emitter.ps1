# SPDX-License-Identifier: GPL-3.0-or-later
# Brings the original NVIDIA 3D Vision USB emitter from boot state (0955:7003) to
# runtime state (0955:0007) after a plug-in. Required every time the emitter is
# unplugged, because the firmware lives in RAM only.
#   1. Uploads the RAM firmware through the app (--emitter-check <fw>).
#   2. Restarts the device through PnP (elevated pnputil), which performs the USB
#      port reset libnvstusb does after loading. RAM survives a port reset.
#   3. Connects to the runtime device and expects State: Ready.
# No NVIDIA driver is installed. WinUSB must already be bound to both product IDs (Zadig, one-time).
param(
    [string]$Firmware = (Join-Path $PSScriptRoot '..\STUFF\emitter.fw')
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$exe = Join-Path $root 'build\bin\Release\VisionRestoration.exe'
$report = Join-Path $root 'reports\emitter-check.txt'
if (-not (Test-Path $exe)) { throw "Build first: $exe not found." }

function Get-Emitter { Get-PnpDevice -PresentOnly | Where-Object InstanceId -match 'VID_0955&PID_(7003|0007)' }

$dev = Get-Emitter
if (-not $dev) { throw 'No NVIDIA emitter (0955:7003 or 0955:0007) is plugged in.' }
Write-Host "Found: $($dev.InstanceId) status=$($dev.Status)"

if ($dev.InstanceId -match 'PID_7003') {
    if (-not (Test-Path $Firmware)) { throw "Firmware file not found: $Firmware (extract it with vision_firmware.exe from nvstusb64.sys)." }
    Write-Host 'Boot state: uploading RAM firmware...'
    $p = Start-Process $exe -ArgumentList '--emitter-check', "`"$Firmware`"" -Wait -PassThru
    Get-Content $report | Write-Host
    if ($p.ExitCode -ne 0) { throw 'Firmware upload failed. Unplug/replug the emitter and retry.' }
    Write-Host 'Restarting device (UAC prompt for pnputil)...'
    $r = Start-Process pnputil.exe -ArgumentList '/restart-device', "`"$($dev.InstanceId)`"" -Verb RunAs -Wait -PassThru
    if ($r.ExitCode -ne 0) { throw "pnputil exit $($r.ExitCode). Restart the device manually in Device Manager (Disable, then Enable)." }
    $dev = $null
    for ($i = 0; $i -lt 20 -and -not $dev; $i++) { Start-Sleep -Milliseconds 500; $dev = Get-Emitter | Where-Object InstanceId -match 'PID_0007' }
    if (-not $dev) { throw 'Device did not re-enumerate as 0955:0007. Unplug/replug and retry.' }
    Write-Host "Re-enumerated: $($dev.InstanceId) status=$($dev.Status)"
}
if ($dev.Status -ne 'OK') { throw "0955:0007 has no working driver (status $($dev.Status)). Bind WinUSB to it with Zadig once." }

Write-Host 'Runtime state: connect check...'
$p = Start-Process $exe -ArgumentList '--emitter-check' -Wait -PassThru
Get-Content $report | Write-Host
if ($p.ExitCode -ne 0) { throw 'Emitter did not reach Ready.' }
Write-Host 'Emitter ready. Launch the app and run the 120 Hz glasses test. USB ready is not optical confirmation.'
