param([string]$Drive='')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$firmwareDirectory=Join-Path $projectRoot 'build/rp2040'
$firmware=Join-Path $firmwareDirectory 'VisionEmitter.uf2'
$probe=Join-Path $projectRoot 'build/bin/Release/vision_rp2040_probe.exe'

# Only the RP2040 ROM's UF2 disk is a flash target. Never use a COM port or
# the NVIDIA RAM-firmware loader, and never guess between multiple boards.
$candidates=@([System.IO.DriveInfo]::GetDrives() | Where-Object {
    $_.DriveType -eq [System.IO.DriveType]::Removable -and $_.IsReady -and $_.VolumeLabel -eq 'RPI-RP2'
})
if($Drive){
    if($Drive -notmatch '^[A-Za-z]:[\\/]?$'){throw 'Drive must be a drive letter such as H:.'}
    $targetRoot=$Drive.Substring(0,2)+'\'
    $candidates=@($candidates | Where-Object { $_.Name -eq $targetRoot })
}
if($candidates.Count -ne 1){
    throw "Expected exactly one RPI-RP2 removable drive; found $($candidates.Count). Unplug the RP2040-Zero, hold BOOT while reconnecting USB, then release BOOT. If multiple boards are in BOOT mode, use -Drive to select this board. Nothing flashed."
}
$target=$candidates[0].RootDirectory.FullName
$infoPath=Join-Path $target 'INFO_UF2.TXT'
$info=Get-Content -LiteralPath $infoPath -Raw
# The RP2040 ROM calls its model "Raspberry Pi RP2" in INFO_UF2.TXT.
if($info -notmatch '(?m)^Model:\s*Raspberry Pi RP2\s*$' -or $info -notmatch '(?m)^Board-ID:\s*RPI-RP2\s*$'){
    throw 'The boot disk does not identify the expected RP2040 chip. Nothing flashed.'
}
if(-not (Test-Path -LiteralPath $probe)){throw 'Build vision_rp2040_probe before flashing so the USB connection can be checked.'}
$existing=@(& $probe --list 2>&1)
if($LASTEXITCODE -ne 0){throw 'USB discovery failed before flashing. Nothing flashed.'}
if(($existing -join "`n") -match 'RP2040 candidate cafe:3d02'){
    throw 'Disconnect other Vision RP2040 emitters before flashing so the runtime check identifies this board. Nothing flashed.'
}

# This independently checks the UF2 payload against the linked binary,
# chip family, flash addresses, boot2 checksum and reset vectors.
& "$PSScriptRoot/Test-FirmwareArtifact.ps1" -Directory $firmwareDirectory
$hash=(Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash
$reportDirectory=Join-Path $projectRoot 'reports'
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$report=Join-Path $reportDirectory ('rp2040-flash-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.txt')
@("RP2040 flash attempt: $(Get-Date -Format o)","Target: $target",$info,"UF2: $firmware","SHA256: $hash") |
    Set-Content -LiteralPath $report -Encoding UTF8
Write-Output "Programming VisionEmitter.uf2 on $target; the board should reboot automatically."
Copy-Item -LiteralPath $firmware -Destination (Join-Path $target 'VisionEmitter.uf2')
Add-Content -LiteralPath $report -Value 'UF2 copy completed; checking runtime USB and clock.'

$runtimeFound=$false
$deadline=[DateTime]::UtcNow.AddSeconds(30)
do {
    Start-Sleep -Milliseconds 500
    $listing=@(& $probe --list 2>&1)
    if($LASTEXITCODE -eq 0 -and ($listing -join "`n") -match 'RP2040 candidate cafe:3d02'){$runtimeFound=$true;break}
} while([DateTime]::UtcNow -lt $deadline)
$listing | Tee-Object -FilePath $report -Append
if(-not $runtimeFound){throw "UF2 was copied, but the runtime device is not accessible through WinUSB. Check Windows for Vision RP2040 Emitter v1; problem code 28 means its driver is missing. Report: $report"}
& $probe --clock 2>&1 | Tee-Object -FilePath $report -Append
$probeExit=$LASTEXITCODE
if($probeExit -ne 0){throw "Firmware was copied; USB/clock validation needs attention (probe exit $probeExit). Report: $report"}
'PASS: firmware copied and runtime USB/clock probe passed. IR output and glasses response have not been tested.' |
    Tee-Object -FilePath $report -Append
Write-Output "Report: $report"
