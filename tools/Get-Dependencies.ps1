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
    @{ Name = 'Spout2 SDK 2.007.017'; Url = 'https://github.com/leadedge/Spout2/archive/refs/tags/2.007.017.zip'; Target = 'Spout2-master'; Check = 'SPOUTSDK\SpoutDirectX\SpoutDX\SpoutDX.cpp' }
    @{ Name = 'ReShade v6.8.0 add-on API headers'; Url = 'https://github.com/crosire/reshade/archive/refs/tags/v6.8.0.zip'; Target = 'reshade-main'; Check = 'include\reshade.hpp' }
    @{ Name = 'libusb 1.0.30 (Windows binaries)'; Url = 'https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.7z'; Target = 'libusb'; Check = 'VS2022\MS64\dll\libusb-1.0.dll'; Flat = $true }
)

try {
    foreach ($dep in $deps) {
        $target = Join-Path $root $dep.Target
        if (-not $Force -and (Test-Path (Join-Path $target $dep.Check))) { Write-Host "Present: $($dep.Name)"; continue }
        Write-Host "Downloading $($dep.Name)"
        $archive = Join-Path $temp ([IO.Path]::GetFileName($dep.Url))
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
} finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'Dependencies ready. Run .\build.ps1 next.'
Write-Host 'Not downloaded: the NVIDIA emitter firmware (extract it from your own nvstusb.sys, see docs/EMITTER.md)'
Write-Host 'and the optional libwdi installer (without it, bind WinUSB to the emitter with Zadig).'
