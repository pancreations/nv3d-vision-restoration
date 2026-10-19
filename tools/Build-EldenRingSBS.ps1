param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source=Join-Path $projectRoot 'adapters/eldenring_sbs'
$build=Join-Path $projectRoot 'build/eldenring-sbs'
cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'Elden Ring SBS configuration failed'}
cmake --build $build --config $Configuration -- /m:1
if($LASTEXITCODE -ne 0){throw 'Elden Ring SBS build failed'}
ctest --test-dir $build -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Elden Ring SBS checks failed'}
Write-Output "Built and tested: $build/$Configuration/VisionEldenSBS.addon64"
$package=Join-Path $projectRoot 'dist/EldenRing-SBS-experimental'
$null=New-Item -ItemType Directory -Path $package -Force
$runtime=Join-Path $projectRoot 'build/reshade-capture-6.8.0/ReShade64.dll'
if(-not(Test-Path -LiteralPath $runtime)){& (Join-Path $PSScriptRoot 'Get-CaptureRuntime.ps1')}
Copy-Item -LiteralPath (Join-Path $build "$Configuration/VisionEldenSBS.addon64"),$runtime -Destination $package
foreach($name in @('VisionEldenSBS.ini','Install.ps1','Launch.ps1','Uninstall.ps1','README.md','THIRD-PARTY.md','VALIDATION.md')){
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination $package
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $package
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/reshade-main/LICENSE.md') -Destination (Join-Path $package 'ReShade-LICENSE.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'third_party/minhook-1.3.4/LICENSE.txt') -Destination (Join-Path $package 'MinHook-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $source 'FreecamMod-LICENSE.txt') -Destination $package
Write-Output "Installable package: $package"
