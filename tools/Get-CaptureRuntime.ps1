param([string]$Destination)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $Destination){$Destination=Join-Path $projectRoot 'build/bin/Release/runtime'}
$cache=Join-Path $projectRoot 'build/reshade-capture-6.8.0'
New-Item -ItemType Directory -Path $cache -Force | Out-Null
$archive=Join-Path $cache 'ReShade_Setup_6.8.0_Addon.exe'
$expected='AFE4C8F13048306307983B8B3D41D5BF00A86820440B0E57DEA10950E1176445'
if(-not (Test-Path -LiteralPath $archive)){
    Invoke-WebRequest -UseBasicParsing -Uri 'https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe' -OutFile $archive
}
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected){throw 'Official ReShade package checksum differs from the tested version; no runtime was installed.'}
$extractor=Join-Path $projectRoot 'third_party/7zip-26.03/x64/7za.exe'
if(-not (Test-Path -LiteralPath $extractor)){$extractor=Join-Path $projectRoot 'third_party/reshade-main/tools/7za.exe'}
if(-not (Test-Path -LiteralPath $extractor)){throw 'Run tools/Get-Dependencies.ps1 first to obtain the archive extractor.'}
# Extract the archive payload without executing the setup program.
& $extractor x -y "-o$cache" $archive ReShade32.dll ReShade64.dll | Out-Null
if($LASTEXITCODE -gt 1){throw 'ReShade runtime extraction failed.'}
foreach($entry in @(@('x86','ReShade32.dll'),@('x64','ReShade64.dll'))){
    $target=Join-Path $Destination $entry[0]
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $cache $entry[1]) -Destination (Join-Path $target 'ReShade.dll')
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/reshade-main/LICENSE.md') -Destination (Join-Path $Destination 'ReShade-LICENSE.md')
Write-Output "Official ReShade add-on runtimes prepared in $Destination. No game installation was changed."
