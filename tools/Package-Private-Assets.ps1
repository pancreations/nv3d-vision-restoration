param(
    [ValidateSet('Release')][string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$distRoot = Join-Path $projectRoot 'dist'
$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(VisionRestoration VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read the Vision Restoration version from CMakeLists.txt.'
}
$version = $Matches[1]
$stage = Join-Path $distRoot 'stage-private-assets'
$privateZip = Join-Path $distRoot "Vision-Restoration-PRIVATE-Personal-Assets-v$version.zip"
$privateSums = Join-Path $distRoot 'PRIVATE-SHA256SUMS.txt'

function Assert-UnderDist([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $dist = [IO.Path]::GetFullPath($distRoot).TrimEnd('\')
    if (-not $full.StartsWith($dist + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside dist: $full"
    }
}

function Reset-Directory([string]$Path) {
    Assert-UnderDist $Path
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required private asset is missing: $Source" }
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function New-HashManifest([string]$Root) {
    $resolved = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $lines = Get-ChildItem -LiteralPath $resolved -Recurse -File |
        Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($resolved.Length + 1).Replace('\', '/')
            '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
        }
    [IO.File]::WriteAllLines((Join-Path $resolved 'SHA256SUMS.txt'), $lines, [Text.UTF8Encoding]::new($false))
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
Reset-Directory $stage
foreach ($path in @($privateZip, $privateSums)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

# Recreate the prepared firmware from the locally supplied NVIDIA driver when
# available, and require it to match the already verified personal copy.
$firmwareTool = Join-Path $projectRoot "build/bin/$Configuration/vision_firmware.exe"
$driver = Join-Path $projectRoot 'STUFF/extracted/NV3DVisionUSB.Driver/nvstusb64.sys'
$knownFirmware = Join-Path $projectRoot 'STUFF/emitter.fw'
$stagedFirmware = Join-Path $stage 'emitter.fw'
if ((Test-Path -LiteralPath $firmwareTool -PathType Leaf) -and (Test-Path -LiteralPath $driver -PathType Leaf)) {
    & $firmwareTool $driver $stagedFirmware
    if ($LASTEXITCODE -ne 0) { throw 'Firmware extraction from the local NVIDIA driver failed.' }
    if (Test-Path -LiteralPath $knownFirmware -PathType Leaf) {
        $generatedHash = (Get-FileHash -LiteralPath $stagedFirmware -Algorithm SHA256).Hash
        $knownHash = (Get-FileHash -LiteralPath $knownFirmware -Algorithm SHA256).Hash
        if ($generatedHash -ne $knownHash) { throw 'Freshly extracted emitter firmware does not match the verified personal copy.' }
    }
} else {
    Copy-Required $knownFirmware $stagedFirmware
}

foreach ($model in @(
    'depth-anything-v2-small-fp16.onnx',
    'depth-anything-v2-base-fp16.onnx',
    'depth-anything-v2-large-fp16.onnx'
)) {
    Copy-Required (Join-Path $projectRoot "models/$model") (Join-Path $stage "models/$model")
}

$geo11Source = Join-Path $projectRoot 'build/geo11-upstream'
if (-not (Test-Path -LiteralPath $geo11Source -PathType Container)) { throw "Required private Geo11 assets are missing: $geo11Source" }
$geo11Destination = Join-Path $stage 'private-assets/geo11-0.7.11'
New-Item -ItemType Directory -Path $geo11Destination -Force | Out-Null
Get-ChildItem -LiteralPath $geo11Source -Force | Copy-Item -Destination $geo11Destination -Recurse -Force

Copy-Required (Join-Path $projectRoot 'docs/PRIVATE-ASSETS-NOTICE.txt') (Join-Path $stage 'README - PRIVATE ASSETS - DO NOT UPLOAD.txt')
Copy-Required (Join-Path $projectRoot 'docs/MODEL-LICENSES.txt') (Join-Path $stage 'MODEL-LICENSES.txt')
$quickStart = Join-Path $distRoot 'Vision Restoration Quick Start.pdf'
Copy-Required $quickStart (Join-Path $stage 'Vision Restoration Quick Start.pdf')

if ((Get-Item -LiteralPath $stagedFirmware).Length -ne 7026) { throw 'Prepared emitter firmware has an unexpected size.' }
if ((Get-ChildItem -LiteralPath (Join-Path $stage 'models') -Filter '*.onnx' -File).Count -ne 3) { throw 'Private archive does not contain exactly three AI models.' }
foreach ($architecture in @('x32', 'x64')) {
    if (-not (Test-Path -LiteralPath (Join-Path $geo11Destination "$architecture/d3d11.dll") -PathType Leaf)) {
        throw "Private archive is missing the Geo11 $architecture renderer."
    }
}

New-HashManifest $stage
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $privateZip, [IO.Compression.CompressionLevel]::Optimal, $false)
$archiveHash = '{0}  {1}' -f (Get-FileHash -LiteralPath $privateZip -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $privateZip)
[IO.File]::WriteAllLines($privateSums, @($archiveHash), [Text.UTF8Encoding]::new($false))

Remove-Item -LiteralPath $stage -Recurse -Force
Write-Host ''
Write-Host 'PRIVATE PERSONAL archive created. DO NOT UPLOAD:'
Get-Item -LiteralPath $privateZip, $privateSums | Select-Object FullName, Length, LastWriteTime
