param(
    [ValidateSet('x64','x86')][string[]]$Architecture=@('x64','x86'),
    [ValidateSet('sbs','tab','sbs_reversed','tab_reversed','katanga_vr')][string[]]$Mode=@('sbs','tab','sbs_reversed','tab_reversed','katanga_vr')
)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runRoot=Join-Path $projectRoot ('build/shared-geo11-output-'+[guid]::NewGuid().ToString('N'))
foreach($arch in $Architecture){
    $binaries=Join-Path $projectRoot "build/stereo-runtime-$arch/Release"
    $debugger=Join-Path $binaries 'vision_debug_run.exe'
    foreach($packing in $Mode){
        $fixture=Join-Path $runRoot "$arch-$packing"
        New-Item -ItemType Directory -Path $fixture -Force | Out-Null
        foreach($name in @('VisionStereo11Test.dll','vision_output_tests.exe')){
            Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $fixture
        }
        & $debugger (Join-Path $fixture 'vision_output_tests.exe') $packing 2>&1 | Tee-Object -FilePath (Join-Path $fixture 'result.log')
        if($LASTEXITCODE -ne 0){throw "Geo11 output regression failed: $arch $packing; logs: $fixture"}
    }
}
Write-Output "All requested shared Geo11 output regressions passed. Logs: $runRoot"
