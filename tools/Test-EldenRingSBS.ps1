param([switch]$ReverseCameraOrder)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$binaries=Join-Path $projectRoot 'build/eldenring-sbs/Release'
$fixture=Join-Path $projectRoot ('build/eldenring-sbs-dx12-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $fixture
Copy-Item -LiteralPath (Join-Path $binaries 'vision_elden_dx12_test.exe'),(Join-Path $binaries 'VisionEldenSBSTest.addon64') -Destination $fixture
Copy-Item -LiteralPath (Join-Path $projectRoot 'build/reshade-capture-6.8.0/ReShade64.dll') -Destination (Join-Path $fixture 'dxgi.dll')
@'
[Stereo]
EyeSeparation=0.2
ConvergenceShift=0
LockPairPose=1
ReverseCameraOrder=0
'@ | Set-Content -LiteralPath (Join-Path $fixture 'VisionEldenSBS.ini') -Encoding ASCII
if($ReverseCameraOrder){(Get-Content -LiteralPath (Join-Path $fixture 'VisionEldenSBS.ini') -Raw).Replace('ReverseCameraOrder=0','ReverseCameraOrder=1') | Set-Content -LiteralPath (Join-Path $fixture 'VisionEldenSBS.ini') -Encoding ASCII}
@'
[GENERAL]
EffectSearchPaths=.\NoEffects
PresetPath=.\Empty.ini
[OVERLAY]
TutorialProgress=4
'@ | Set-Content -LiteralPath (Join-Path $fixture 'ReShade.ini') -Encoding ASCII
Push-Location -LiteralPath $fixture
try {
    $testArgs=@();if($ReverseCameraOrder){$testArgs=@('reverse')}
    & (Join-Path $fixture 'vision_elden_dx12_test.exe') @testArgs 2>&1 | Tee-Object -FilePath (Join-Path $fixture 'result.log')
    if($LASTEXITCODE -ne 0){throw "DX12 SBS regression failed: $fixture"}
} finally { Pop-Location }
Write-Output "DX12 evidence: $fixture"
