$ErrorActionPreference='Stop'
$game=Split-Path -Parent $PSScriptRoot
$record=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'install.json') -Raw | ConvertFrom-Json
if((Resolve-Path -LiteralPath $game).Path -ne $record.GameDirectory){throw 'Installation was moved; reinstall the package at its new location.'}
if(Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue){throw 'Elden Ring is already running.'}
if(-not(Get-Process -Name steam -ErrorAction SilentlyContinue)){throw 'Start Steam first, then use this offline game launcher.'}
if((Get-FileHash -LiteralPath (Join-Path $game 'eldenring.exe')).Hash -ne $record.GameSHA256){throw 'Game updated: this SBS camera hook must be revalidated before use.'}
if((Get-FileHash -LiteralPath (Join-Path $game 'dxgi.dll')).Hash -ne $record.ReShadeSHA256){throw 'The graphics loader changed. Reinstall the SBS package before use.'}
$pending=Join-Path $PSScriptRoot 'VisionEldenSBS.pending'
if(Test-Path -LiteralPath $pending){
    $update=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'pending-update.json') -Raw | ConvertFrom-Json
    $addon=Join-Path $PSScriptRoot 'VisionEldenSBS.addon64'
    if((Get-FileHash -LiteralPath $pending).Hash -ne $update.SHA256){throw 'Pending SBS update checksum mismatch.'}
    if((Get-FileHash -LiteralPath $addon).Hash -ne $record.AddonSHA256){throw 'Installed SBS module changed; pending update was not applied.'}
    $backup=Join-Path $PSScriptRoot ('Before\VisionEldenSBS-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.addon64')
    Copy-Item -LiteralPath $addon -Destination $backup
    Copy-Item -LiteralPath $pending -Destination $addon -Force
    if((Get-FileHash -LiteralPath $addon).Hash -ne $update.SHA256){Copy-Item -LiteralPath $backup -Destination $addon -Force;throw 'SBS update verification failed; previous module restored.'}
    $record.AddonSHA256=$update.SHA256
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'install.json') -Encoding UTF8
    # Exact single files inside the validated installation; no recursive move.
    Move-Item -LiteralPath $pending -Destination (Join-Path $PSScriptRoot 'Before\last-applied-update.bin') -Force
    Move-Item -LiteralPath (Join-Path $PSScriptRoot 'pending-update.json') -Destination (Join-Path $PSScriptRoot 'Before\last-applied-update.json') -Force
    Write-Output 'Applied SBS camera-order correction and live settings reload.'
}
$env:VISION_ELDEN_SBS_OFFLINE='1'
$env:SteamAppId='1245620'
$env:SteamGameId='1245620'
# Direct launch is the modded, unprotected session. The original protected
# launcher stays intact. No game executable, save, or Steam setting is patched.
Start-Process -FilePath (Join-Path $game 'eldenring.exe') -WorkingDirectory $game -WindowStyle Normal
