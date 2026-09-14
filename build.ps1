param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=$PSScriptRoot
cmake -S $projectRoot -B "$projectRoot/build" -G 'Visual Studio 17 2022' -A x64
if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
cmake --build "$projectRoot/build" --config $Configuration --target VisionRestoration vision_tests vision_platform_tests vision_firmware VisionStereoSpout VisionGameHook vision_rp2040_tests vision_rp2040_host_tests vision_rp2040_probe -- /m:1
if($LASTEXITCODE -ne 0){throw 'Build failed'}
ctest --test-dir "$projectRoot/build" -C $Configuration --output-on-failure
if($LASTEXITCODE -ne 0){throw 'Tests failed'}
