param(
    [ValidateSet('x64','x86')][string[]]$Architecture=@('x64','x86'),
    [ValidateSet('sbs','tab','sbs_reversed','tab_reversed','katanga_vr')][string[]]$Mode=@('sbs','tab','sbs_reversed','tab_reversed','katanga_vr'),
    [switch]$SeparateFactory,
    [switch]$DepthControls,
    [switch]$ContinueOnFailure
)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runRoot=Join-Path $projectRoot ('build/shared-geo11-integration-'+[guid]::NewGuid().ToString('N'))
$setup=Join-Path $projectRoot 'build/bin/Release/vision_stereo_setup.exe'
$failures=@()
foreach($arch in $Architecture){
    $binaries=Join-Path $projectRoot "build/stereo-runtime-$arch/Release"
    $upstream=Join-Path $projectRoot ('build/geo11-upstream/'+$(if($arch -eq 'x86'){'x32'}else{'x64'}))
    $runtime=Join-Path $runRoot "runtime/$arch"
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries 'VisionStereo11Test.dll') -Destination (Join-Path $runtime 'VisionStereo11.dll')
    Copy-Item -LiteralPath (Join-Path $binaries 'VisionStereoLoader.dll') -Destination $runtime
    foreach($packing in $Mode){
        $fixture=Join-Path $runRoot "$arch-$packing"
        New-Item -ItemType Directory -Path $fixture -Force | Out-Null
        Get-ChildItem -LiteralPath $upstream | Copy-Item -Destination $fixture -Recurse
        $exe=Join-Path $fixture ('FixtureDX11-'+$arch+'-'+$packing+'-'+[guid]::NewGuid().ToString('N')+'.exe')
        Copy-Item -LiteralPath (Join-Path $binaries 'vision_geo11_probe.exe') -Destination $exe
        # Only the isolated test fixture selects a mode; the connector must
        # preserve it. These are upstream sample files, never game installs.
        $provider=Join-Path $fixture 'd3dxdm.ini'
        $text=[IO.File]::ReadAllText($provider)
        $text=[regex]::Replace($text,'(?m)^direct_mode\s*=\s*\w+',"direct_mode = $packing")
        [IO.File]::WriteAllText($provider,$text)
        $before=(Get-FileHash -LiteralPath $provider -Algorithm SHA256).Hash
        & $setup connect $exe (Join-Path $runRoot 'runtime')
        if($LASTEXITCODE -ne 0){throw "Shared connection failed: $arch $packing"}
        if((Get-FileHash -LiteralPath $provider -Algorithm SHA256).Hash -ne $before){throw 'Connection changed provider settings'}
        $probeArguments=@($fixture,'--hook-output')
        if($SeparateFactory){$probeArguments+='--factory'}
        if($DepthControls){$probeArguments+='--depth-controls'}
        & (Join-Path $binaries 'vision_debug_run.exe') $exe @probeArguments 2>&1 | Tee-Object -FilePath (Join-Path $fixture 'result.log')
        if($LASTEXITCODE -ne 0){
            $failures+="$arch $packing"
            if(-not $ContinueOnFailure){throw "Actual Geo11 integration failed: $arch $packing; logs: $fixture"}
            Write-Output "FAILED: $arch $packing; preserving its fixture and continuing to the remaining independent cases."
            continue
        }
        $afterProbe=(Get-FileHash -LiteralPath $provider -Algorithm SHA256).Hash
        if(-not $DepthControls -and $afterProbe -ne $before){throw 'Hook changed provider settings during rendering'}
        & $setup disconnect $exe
        if($LASTEXITCODE -ne 0){throw "Shared disconnection failed: $arch $packing"}
        if((Get-FileHash -LiteralPath $provider -Algorithm SHA256).Hash -ne $afterProbe){throw 'Removal changed provider settings'}
    }
}
if($failures.Count){throw "Actual Geo11 integration failures: $($failures -join ', '); logs: $runRoot"}
Write-Output "All requested actual Geo11 integrations passed. Logs: $runRoot"
