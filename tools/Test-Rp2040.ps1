param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
if(-not $SkipBuild) {
    cmake -S $projectRoot -B "$projectRoot/build" -G 'Visual Studio 17 2022' -A x64
    if($LASTEXITCODE -ne 0){throw 'CMake configuration failed'}
    cmake --build "$projectRoot/build" --config Release --target vision_rp2040_tests vision_rp2040_host_tests -- /m:1
    if($LASTEXITCODE -ne 0){throw 'RP2040 simulation build failed'}
}
$testExe=Join-Path $projectRoot 'build/bin/Release/vision_rp2040_tests.exe'
if(-not (Test-Path -LiteralPath $testExe)){throw 'Simulation executable missing; run without -SkipBuild'}
$reportDir=Join-Path $projectRoot 'reports'
[System.IO.Directory]::CreateDirectory($reportDir) | Out-Null
$reportPath=Join-Path $reportDir 'rp2040-simulation.txt'
$result = & $testExe 2>&1
$testExit=$LASTEXITCODE
if($testExit -eq 0) {
    $hostExe=Join-Path $projectRoot 'build/bin/Release/vision_rp2040_host_tests.exe'
    if(-not (Test-Path -LiteralPath $hostExe)){throw 'Host/waveform tests missing; run without -SkipBuild'}
    $hostResult = & $hostExe 2>&1
    $testExit=$LASTEXITCODE
    $result=@($result)+@($hostResult)
}
$lines=@('RP2040 PRE-HARDWARE SIMULATION',('Recorded: '+[DateTimeOffset]::Now.ToString('o')),
    'No USB device, GPIO or display access. Not optical calibration.','') + @($result | ForEach-Object {"$_"})
$lines | Set-Content -LiteralPath $reportPath -Encoding UTF8
$result | Write-Output
if($testExit -ne 0){throw "Simulation failed; see $reportPath"}
Write-Output "Report: $reportPath"
