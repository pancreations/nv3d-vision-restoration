# Downloads a Depth Anything V2 model (ONNX export by onnx-community) into models/ for the
# whole-screen conversion. Small is fetched by Get-Dependencies.ps1; Base and Large are the
# stronger, slower options. Weights only, no code; each keeps its own license:
#   small  Apache-2.0        about 50 MB   (fp16)
#   base   CC-BY-NC-4.0      about 195 MB  (fp16)
#   large  CC-BY-NC-4.0      about 670 MB  (fp16)
param(
    [ValidateSet('small','base','large')][string]$Size = 'base',
    [switch]$Fp32,   # the float32 export instead of fp16 (twice the size, same picture)
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$dir = Join-Path $PSScriptRoot '..\models'
$null = New-Item -ItemType Directory -Force $dir
$dir = (Resolve-Path $dir).Path
$file = Join-Path $dir "depth-anything-v2-$Size-$(if ($Fp32) { 'fp32' } else { 'fp16' }).onnx"
$url = "https://huggingface.co/onnx-community/depth-anything-v2-$Size/resolve/main/onnx/$(if ($Fp32) { 'model.onnx' } else { 'model_fp16.onnx' })"
if (-not $Force -and (Test-Path $file)) { Write-Host "Present: $file"; exit 0 }
Write-Host "Downloading Depth Anything V2 $Size from $url"
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
Write-Host "  $file"
Write-Host "  SHA-256 $((Get-FileHash $file -Algorithm SHA256).Hash)"
Write-Host 'Pick it under Sources & games > Whole screen (AI depth) > Model.'
