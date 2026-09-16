param([ValidateSet('Debug','Release')][string]$Configuration='Release',[string]$Destination)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $Destination){$Destination=Join-Path $projectRoot "build/bin/$Configuration/runtime"}
foreach($architecture in @('x64','x86')){
    $platform=if($architecture -eq 'x86'){'Win32'}else{'x64'}
    $buildDirectory=Join-Path $projectRoot "build/stereo-runtime-$architecture"
    cmake -S (Join-Path $projectRoot 'tools/stereo-runtime') -B $buildDirectory -G 'Visual Studio 17 2022' -A $platform
    if($LASTEXITCODE -ne 0){throw 'Stereo runtime configuration failed'}
    cmake --build $buildDirectory --config $Configuration --target VisionStereo11 VisionStereoLoader -- /m:1 /verbosity:minimal
    if($LASTEXITCODE -ne 0){throw 'Stereo runtime build failed'}
    $target=Join-Path $Destination $architecture
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $buildDirectory "$Configuration/VisionStereo11.dll") -Destination $target -Force
    Copy-Item -LiteralPath (Join-Path $buildDirectory "$Configuration/VisionStereoLoader.dll") -Destination $target -Force
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/minhook-1.3.4/LICENSE.txt') -Destination (Join-Path $Destination 'MinHook-LICENSE.txt') -Force
