param(
    [ValidateSet('x64','x86')][string[]]$Architecture=@('x64','x86'),
    [ValidateSet('katanga_vr','sbs','tab','sbs_reversed','tab_reversed')][string[]]$Mode=@('katanga_vr','sbs','tab','sbs_reversed','tab_reversed'),
    [switch]$SeparateFactory
)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$runRoot=Join-Path $projectRoot ('build/geo11-direct-regression-'+[guid]::NewGuid().ToString('N'))
$setup=Join-Path $projectRoot 'build/bin/Release/vision_stereo_setup.exe'
foreach($arch in $Architecture){
    $binaries=Join-Path $projectRoot "build/stereo-runtime-$arch/Release"
    $upstream=Join-Path $projectRoot ('build/geo11-upstream/'+$(if($arch -eq 'x86'){'x32'}else{'x64'}))
    $runtime=Join-Path $runRoot "runtime/$arch"
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    # Production runtime: capture mode must not connect to the emitter host.
    Copy-Item -LiteralPath (Join-Path $binaries 'VisionStereo11.dll') -Destination $runtime
    Copy-Item -LiteralPath (Join-Path $binaries 'VisionStereoLoader.dll') -Destination $runtime
    foreach($packing in $Mode){
        $fixture=Join-Path $runRoot "$arch-$packing"
        New-Item -ItemType Directory -Path $fixture -Force | Out-Null
        foreach($name in @('d3d11.dll','d3dxdm.ini',$(if($arch -eq 'x86'){'nvapi.dll'}else{'nvapi64.dll'}))){Copy-Item -LiteralPath (Join-Path $upstream $name) -Destination $fixture}
        $exe=Join-Path $fixture 'vision_geo11_probe.exe'
        Copy-Item -LiteralPath (Join-Path $binaries 'vision_geo11_probe.exe') -Destination $exe
        # This synthetic draw has no game shaders needing fixes. Upstream's
        # demo help overlay independently crashes on process detach; omit it.
        [IO.File]::WriteAllText((Join-Path $fixture 'd3dx.ini'),"[Logging]`ncalls=1`n[Device]`nforce_stereo=2`n[System]`n")
        $provider=Join-Path $fixture 'd3dxdm.ini'
        $text=[regex]::Replace([IO.File]::ReadAllText($provider),'(?m)^direct_mode\s*=\s*\w+',"direct_mode = $packing")
        [IO.File]::WriteAllText($provider,$text)
        $before=(Get-FileHash -LiteralPath $provider).Hash
        $renderer=(Get-FileHash -LiteralPath (Join-Path $fixture 'd3d11.dll')).Hash
        & $setup capture $exe (Join-Path $runRoot 'runtime') katanga
        if($LASTEXITCODE -ne 0){throw "Geo11 direct connection failed: $fixture"}
        if(Test-Path (Join-Path $fixture 'dxgi.dll')){throw 'Geo11 incorrectly loaded ReShade'}
        if((Get-FileHash (Join-Path $fixture 'VisionGeo11.dll')).Hash -ne $renderer){throw 'Existing renderer bytes changed'}
        $arguments=@($fixture,'--capture');if($SeparateFactory){$arguments+='--factory'}
        & (Join-Path $binaries 'vision_debug_run.exe') $exe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $fixture 'result.log')
        if($LASTEXITCODE -ne 0){throw "Geo11 direct capture failed: $fixture"}
        if((Get-FileHash -LiteralPath $provider).Hash -ne $before){throw 'Provider output configuration changed'}
        & $setup uncapture $exe
        if($LASTEXITCODE -ne 0){throw "Geo11 capture removal failed: $fixture"}
        if((Get-FileHash (Join-Path $fixture 'd3d11.dll')).Hash -ne $renderer){throw 'Renderer restoration mismatch'}
    }
}
Write-Output "Real Geo11 direct-eye capture passed: $runRoot"
