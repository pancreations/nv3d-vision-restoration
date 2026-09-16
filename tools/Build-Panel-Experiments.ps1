param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$panelRoot=Split-Path -Parent $PSScriptRoot
$panelBuild=Join-Path $panelRoot 'build\panel-experiments'
cmake -S $panelRoot -B $panelBuild -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'Panel configuration failed'}
cmake --build $panelBuild --config $Configuration --target VisionRestoration vision_tests vision_panel_tests vision_platform_tests vision_rp2040_tests vision_rp2040_host_tests -- /m:1
if($LASTEXITCODE -ne 0){throw 'Panel build failed'}
ctest --test-dir $panelBuild -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Panel tests failed'}
$panelBin=Join-Path $panelBuild "bin\$Configuration"
# This nested build uses executable-local profiles/reports. Seed copies once so the
# old executable can keep using its original files (it cannot read version 10).
$panelProfiles=Join-Path $panelBin 'profiles'
New-Item -ItemType Directory -Force -Path $panelProfiles | Out-Null
Get-ChildItem -LiteralPath (Join-Path $panelRoot 'profiles') -Filter '*.ini' -File | ForEach-Object {
    $panelCopy=Join-Path $panelProfiles $_.Name
    if(-not (Test-Path -LiteralPath $panelCopy)){Copy-Item -LiteralPath $_.FullName -Destination $panelCopy}
}
Write-Output "Panel experiment build: $panelBin\VisionRestoration.exe"
