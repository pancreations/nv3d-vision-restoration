# Downloads the third-party build dependencies from their official sources into third_party/.
# This repository redistributes none of them; each keeps its own license (see THIRD_PARTY_NOTICES.md).
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Join-Path $PSScriptRoot '..\third_party'
$null = New-Item -ItemType Directory -Force $root
$root = (Resolve-Path $root).Path
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'   # bsdtar: reads both .zip and .7z
$temp = Join-Path ([IO.Path]::GetTempPath()) ('vision-deps-' + [guid]::NewGuid())
$null = New-Item -ItemType Directory $temp

# Target folder names are the paths CMakeLists.txt expects. Flat archives have no top-level folder.
$deps = @(
    @{ Name = 'Dear ImGui v1.91.9b'; Url = 'https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.zip'; Target = 'imgui-1.91.9b'; Check = 'imgui.cpp' }
    @{ Name = 'ReShade v6.8.0 add-on API headers'; Url = 'https://github.com/crosire/reshade/archive/refs/tags/v6.8.0.zip'; Target = 'reshade-main'; Check = 'include\reshade.hpp' }
    @{ Name = 'MinHook v1.3.4'; Url = 'https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip'; Target = 'minhook-1.3.4'; Check = 'include\MinHook.h' }
    @{ Name = 'libusb 1.0.30 (Windows binaries)'; Url = 'https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z'; Target = 'libusb'; Check = 'VS2022\MS64\dll\libusb-1.0.dll'; Flat = $true }
    # Whole-screen AI depth: ONNX Runtime's DirectML build and DirectML itself, as NuGet packages (zip archives).
    @{ Name = 'ONNX Runtime 1.22.1 (DirectML build)'; Url = 'https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.22.1'; Target = 'onnxruntime'; Check = 'build\native\include\onnxruntime_c_api.h'; Flat = $true; File = 'Microsoft.ML.OnnxRuntime.DirectML.1.22.1.nupkg' }
    @{ Name = 'DirectML 1.15.4'; Url = 'https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4'; Target = 'directml'; Check = 'bin\x64-win\DirectML.dll'; Flat = $true; File = 'Microsoft.AI.DirectML.1.15.4.nupkg' }
    @{ Name = '7-Zip 26.03 portable extractor'; Url = 'https://github.com/ip7z/7zip/releases/download/26.03/7z2603-extra.7z'; Target = '7zip-26.03'; Check = 'x64\7za.exe'; Flat = $true }
)
# Depth network for the whole-screen conversion (weights, not code): Depth Anything V2 Small,
# Apache-2.0, ONNX export by onnx-community. The fp32 export is onnx/model.onnx in the same repository.
$modelDir = Join-Path $PSScriptRoot '..\models'
$modelFile = Join-Path $modelDir 'depth-anything-v2-small-fp16.onnx'
$modelUrl = 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model_fp16.onnx'

try {
    foreach ($dep in $deps) {
        $target = Join-Path $root $dep.Target
        if (-not $Force -and (Test-Path (Join-Path $target $dep.Check))) { Write-Host "Present: $($dep.Name)"; continue }
        Write-Host "Downloading $($dep.Name)"
        $archive = Join-Path $temp $(if ($dep.File) { $dep.File } else { [IO.Path]::GetFileName($dep.Url) })
        Invoke-WebRequest -UseBasicParsing -Uri $dep.Url -OutFile $archive
        Write-Host "  SHA-256 $((Get-FileHash $archive -Algorithm SHA256).Hash)"
        if (Test-Path $target) { Remove-Item -Recurse -Force $target }
        $extract = Join-Path $temp ([guid]::NewGuid())
        $null = New-Item -ItemType Directory $extract
        & $tar -xf $archive -C $extract
        if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
        if ($dep.Flat) { Move-Item $extract $target }
        else { Move-Item (Get-ChildItem $extract -Directory | Select-Object -First 1).FullName $target }
        if (-not (Test-Path (Join-Path $target $dep.Check))) { throw "$($dep.Name) did not unpack as expected." }
    }
    $sevenZipSource = Join-Path $root '7zip-26.03/7z2603-src.7z'
    if ($Force -or -not (Test-Path -LiteralPath $sevenZipSource)) {
        Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/ip7z/7zip/releases/download/26.03/7z2603-src.7z' -OutFile $sevenZipSource
    }
    $null = New-Item -ItemType Directory -Force $modelDir
    if (-not $Force -and (Test-Path $modelFile)) { Write-Host 'Present: Depth Anything V2 Small (ONNX, fp16)' }
    else {
        Write-Host 'Downloading Depth Anything V2 Small (ONNX, fp16, about 50 MB)'
        Invoke-WebRequest -UseBasicParsing -Uri $modelUrl -OutFile $modelFile
        Write-Host "  SHA-256 $((Get-FileHash $modelFile -Algorithm SHA256).Hash)"
    }
} finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Dependencies ready. Run .\build.ps1 next.'
Write-Host 'Not downloaded: the NVIDIA emitter firmware (extract it from your own nvstusb.sys, see docs/EMITTER.md)'
Write-Host 'and the optional libwdi installer (without it, bind WinUSB to the emitter with Zadig).'
