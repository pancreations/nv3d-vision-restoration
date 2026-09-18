param(
    [string]$Binaries,
    [string]$RuntimeRoot,
    [string]$SetupTool,
    [ValidateSet('d3d11','d3d12')][string[]]$Api=@('d3d11','d3d12'),
    [ValidateSet('sequential','sbs','katanga')][string[]]$Mode=@('sequential','sbs','katanga'),
    [switch]$RightFirst
)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if(-not $Binaries){$Binaries=Join-Path $projectRoot 'build/bin/Release'}
if(-not $RuntimeRoot){$RuntimeRoot=Join-Path $Binaries 'runtime'}
if(-not $SetupTool){$SetupTool=Join-Path $projectRoot 'build/bin/Release/vision_stereo_setup.exe'}
$runRoot=Join-Path $projectRoot ('build/capture-regression-'+[guid]::NewGuid().ToString('N'))
foreach($backend in $Api){foreach($layout in $Mode){
    if($layout -eq 'katanga' -and $backend -ne 'd3d11'){continue}
    $fixture=Join-Path $runRoot "$backend-$layout"
    New-Item -ItemType Directory -Path $fixture -Force | Out-Null
    $exe=Join-Path $fixture 'vision_capture_probe.exe'
    Copy-Item -LiteralPath (Join-Path $Binaries 'vision_capture_probe.exe') -Destination $exe
    $order=@();if($RightFirst){$order=@('right-first')}
    & $SetupTool capture $exe $RuntimeRoot $layout @order
    if($LASTEXITCODE -ne 0){throw "Capture connection failed: $fixture"}
    & $exe $backend $layout @order 2>&1 | Tee-Object -FilePath (Join-Path $fixture 'result.log')
    if($LASTEXITCODE -ne 0){throw "Capture regression failed: $fixture"}
    & $SetupTool uncapture $exe
    if($LASTEXITCODE -ne 0){throw "Capture disconnect failed: $fixture"}
    if(Test-Path -LiteralPath (Join-Path $fixture 'dxgi.dll')){throw 'Owned loader was not removed'}
}}
Write-Output "All requested real capture-adapter regressions passed: $runRoot"
