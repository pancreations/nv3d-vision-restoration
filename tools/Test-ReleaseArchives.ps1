param([string]$NvidiaPackage, [string]$NvidiaSys, [switch]$PublicOnly)

$ErrorActionPreference = 'Stop'

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$distRoot = Join-Path $projectRoot 'dist'
$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(VisionRestoration VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Could not read version.' }
$version = $Matches[1]
$publicZip = Join-Path $distRoot "Vision-Restoration-Portable-v$version.zip"
$privateZip = Join-Path $distRoot "Vision-Restoration-PRIVATE-Personal-Assets-v$version.zip"
$verifyRoot = Join-Path $distRoot 'verify-archive'

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Open-Archive([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Archive is missing: $Path" }
    return [IO.Compression.ZipFile]::OpenRead($Path)
}

function Get-EntryNames($Archive) {
    return @($Archive.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName.Replace('\', '/') })
}

function Assert-Has([string[]]$Names, [string]$Name) {
    if ($Names -notcontains $Name) { throw "Required archive entry is missing: $Name" }
}

function Test-InternalManifest($Archive) {
    $manifest = $Archive.GetEntry('SHA256SUMS.txt')
    if (-not $manifest) { throw 'Archive has no internal SHA256SUMS.txt.' }
    $reader = [IO.StreamReader]::new($manifest.Open(), [Text.Encoding]::UTF8)
    try { $lines = @($reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ }) } finally { $reader.Dispose() }
    foreach ($line in $lines) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw "Invalid manifest line: $line" }
        $expected = $Matches[1]
        $name = $Matches[2]
        $entry = $Archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $name } | Select-Object -First 1
        if (-not $entry) { throw "Manifest entry is missing from archive: $name" }
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $actual = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() } finally { $sha.Dispose(); $stream.Dispose() }
        if ($actual -ne $expected) { throw "Manifest hash mismatch: $name" }
    }
    return $lines.Count
}

function Test-OuterManifest([string]$ManifestPath, [string]$ArchivePath) {
    $expectedName = Split-Path -Leaf $ArchivePath
    $line = Get-Content -LiteralPath $ManifestPath | Where-Object { $_ -match "  $([Regex]::Escape($expectedName))$" }
    if (-not $line -or $line -notmatch '^([0-9a-f]{64})  ') { throw "No valid outer hash for $expectedName" }
    $actual = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Matches[1]) { throw "Outer hash mismatch: $expectedName" }
}

function Test-NoMachinePaths($Archive) {
    foreach ($entry in $Archive.Entries | Where-Object { $_.Name -and [IO.Path]::GetExtension($_.Name) -in @('.txt', '.ini', '.cmd', '.bat', '.ps1', '.html', '.json') }) {
        $reader = [IO.StreamReader]::new($entry.Open(), [Text.Encoding]::UTF8, $true)
        try { $content = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($content -match '(?i)[A-Z]:\\Users\\[^\\]+' -or $content -match [Regex]::Escape($projectRoot)) {
            throw "Machine-specific absolute path found in archive entry: $($entry.FullName)"
        }
    }
}

$public = Open-Archive $publicZip
try {
    $publicNames = Get-EntryNames $public
    if (-not ($public.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq 'models/' })) { throw 'Public archive is missing the included empty models folder.' }
    foreach ($name in @(
        'VisionRestoration.exe', 'VisionDepth.exe', 'onnxruntime.dll', 'DirectML.dll',
        'runtime/x86/VisionStereo11.dll', 'runtime/x86/VisionStereoLoader.dll',
        'runtime/x64/VisionStereo11.dll', 'runtime/x64/VisionStereoLoader.dll',
        'vision_firmware.exe', 'Prepare NVIDIA Firmware.cmd', 'Prepare NVIDIA Firmware.ps1',
        'tools/7zip/7za.exe', 'licenses/7zip/7z2603-src.7z',
        'firmware/VisionEmitter.uf2', 'Vision Restoration Quick Start.pdf', 'Vision Restoration README.pdf', 'README.md', 'Launch.cmd',
        'runtime/x64/VisionStereoCapture.addon64', 'runtime/x86/VisionStereoCapture.addon32',
        'runtime/x64/ReShade.dll', 'runtime/x86/ReShade.dll'
    )) { Assert-Has $publicNames $name }
    $publicBad = $publicNames | Where-Object {
        $_ -match '(?i)(^|/)(emitter\.fw|nvstusb[^/]*\.sys|d3d11\.dll|dxgi\.dll|d3dx\.ini|d3dxdm\.ini)$' -or
        $_ -match '(?i)\.onnx$' -or $_ -match '(?i)(^|/)(profiles|reports)/'
    }
    if ($publicBad) { throw "Restricted or saved data found in public archive: $($publicBad -join ', ')" }
    Test-NoMachinePaths $public
    $publicManifestCount = Test-InternalManifest $public
} finally { $public.Dispose() }

if (-not $PublicOnly) {
$private = Open-Archive $privateZip
try {
    $privateNames = Get-EntryNames $private
    foreach ($name in @(
        'emitter.fw',
        'models/depth-anything-v2-small-fp16.onnx',
        'models/depth-anything-v2-base-fp16.onnx',
        'models/depth-anything-v2-large-fp16.onnx',
        'private-assets/geo11-0.7.11/x32/d3d11.dll',
        'private-assets/geo11-0.7.11/x64/d3d11.dll',
        'README - PRIVATE ASSETS - DO NOT UPLOAD.txt',
        'Vision Restoration Quick Start.pdf'
    )) { Assert-Has $privateNames $name }
    $privateBad = $privateNames | Where-Object { $_ -match '(?i)(^|/)nvstusb[^/]*\.sys$' }
    if ($privateBad) { throw "Original NVIDIA driver found in private archive: $($privateBad -join ', ')" }
    $privateStale = $privateNames | Where-Object { $_ -match '(?i)(^|/)(profiles|reports)/' -or $_ -match '(?i)\.(log|dmp|pdb|ilk)$' }
    if ($privateStale) { throw "Saved data or stale artifacts found in private archive: $($privateStale -join ', ')" }
    $modelCount = @($privateNames | Where-Object { $_ -match '(?i)\.onnx$' }).Count
    if ($modelCount -ne 3) { throw "Private archive contains $modelCount models instead of 3." }
    Test-NoMachinePaths $private
    $privateManifestCount = Test-InternalManifest $private
} finally { $private.Dispose() }
}

Test-OuterManifest (Join-Path $distRoot 'SHA256SUMS.txt') $publicZip
if (-not $PublicOnly) { Test-OuterManifest (Join-Path $distRoot 'PRIVATE-SHA256SUMS.txt') $privateZip }
if ((Get-Content -LiteralPath (Join-Path $distRoot 'SHA256SUMS.txt') -Raw) -match 'PRIVATE') {
    throw 'The private archive appears in the public SHA256SUMS.txt.'
}

if (-not [IO.Path]::GetFullPath($verifyRoot).StartsWith([IO.Path]::GetFullPath($distRoot).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe verification directory.' }
if (Test-Path -LiteralPath $verifyRoot) { Remove-Item -LiteralPath $verifyRoot -Recurse -Force }
[IO.Compression.ZipFile]::ExtractToDirectory($publicZip, $verifyRoot)
try {
    if ($NvidiaPackage) {
        & (Join-Path $PSScriptRoot 'Test-FirmwarePreparation.ps1') -PortableDirectory $verifyRoot -NvidiaPackage $NvidiaPackage -NvidiaSys $NvidiaSys
    }
    $gpuTest = Start-Process -FilePath (Join-Path $verifyRoot 'VisionRestoration.exe') -ArgumentList '--gpu-test' -WorkingDirectory $verifyRoot -WindowStyle Hidden -PassThru -Wait
    if ($gpuTest.ExitCode -ne 0) { throw "Extracted public archive GPU test failed with exit code $($gpuTest.ExitCode)." }
    $uiTest = Start-Process -FilePath (Join-Path $verifyRoot 'VisionRestoration.exe') -ArgumentList '--smoke-test' -WorkingDirectory $verifyRoot -WindowStyle Hidden -PassThru -Wait
    if ($uiTest.ExitCode -ne 0) { throw "Extracted public archive UI smoke test failed with exit code $($uiTest.ExitCode)." }
} finally {
    for ($attempt = 0; $attempt -lt 20 -and (Test-Path -LiteralPath $verifyRoot); $attempt++) {
        try { Remove-Item -LiteralPath $verifyRoot -Recurse -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 250 }
    }
}
if (Test-Path -LiteralPath $verifyRoot) { throw 'Extracted archive test directory remained locked.' }

Write-Host "PASS: public archive ($($publicNames.Count) files; $publicManifestCount internal hashes)"
if (-not $PublicOnly) { Write-Host "PASS: private archive ($($privateNames.Count) files; $privateManifestCount internal hashes; 3 models)" }
Write-Host 'PASS: public ZIP extracted GPU/UI smoke tests'
Write-Host 'PASS: separate public/private outer checksum manifests'
