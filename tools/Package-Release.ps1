param(
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$SkipSmoke
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
$binRoot = Join-Path $projectRoot "build/bin/$Configuration"
$coreStage = Join-Path $distRoot 'stage-portable'
$verifyRoot = Join-Path $distRoot 'verify-portable'
$pdfPath = Join-Path $distRoot 'Vision Restoration Quick Start.pdf'
$readmePdfPath = Join-Path $distRoot 'Vision Restoration README.pdf'
$coreZip = Join-Path $distRoot "Vision-Restoration-Portable-v$version.zip"
$retiredAiZip = Join-Path $distRoot "Vision-Restoration-AI-Depth-Models-v$version.zip"

function Assert-UnderDist([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $dist = [IO.Path]::GetFullPath($distRoot).TrimEnd('\')
    if (-not $full.StartsWith($dist + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside dist: $full"
    }
}

function Reset-Directory([string]$Path) {
    Assert-UnderDist $Path
    if (Test-Path -LiteralPath $Path) { Remove-WithRetry $Path }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Remove-WithRetry([string]$Path) {
    Assert-UnderDist $Path
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if (-not (Test-Path -LiteralPath $Path)) { return }
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq 19) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required release file is missing: $Source" }
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-License([string]$Source, [string]$Stage, [string]$Name) {
    Copy-Required $Source (Join-Path $Stage "licenses/$Name")
}

function New-HashManifest([string]$Stage) {
    $root = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\')
    $lines = Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
            '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
        }
    [IO.File]::WriteAllLines((Join-Path $root 'SHA256SUMS.txt'), $lines, [Text.UTF8Encoding]::new($false))
}

function Assert-ReleaseContents([string]$Stage) {
    $forbiddenNames = @(
        'emitter.fw', 'nvstusb.sys', 'nvstusb32.sys', 'nvstusb64.sys',
        'VisionGeo11.dll', 'd3d11.dll', 'dxgi.dll', 'd3dx.ini', 'd3dxdm.ini'
    )
    $bad = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object {
        $forbiddenNames -contains $_.Name -or $_.Name -like '*.pdb' -or $_.Name -like '*.ilk' -or
        $_.Name -like '*.log' -or $_.Name -like '*.dmp' -or $_.Name -match '^VisionRestoration\.(before|pre-|running)'
    }
    if ($bad) { throw "Forbidden release files found: $($bad.FullName -join ', ')" }
    foreach ($directory in @('profiles', 'reports')) {
        if (Test-Path -LiteralPath (Join-Path $Stage $directory)) { throw "Forbidden release directory found: $directory" }
    }

    $rootPattern = [Regex]::Escape($projectRoot)
    $textFiles = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object { $_.Extension -in @('.txt', '.cmd', '.html', '.ini', '.json', '.ps1') }
    foreach ($file in $textFiles) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        if ($text -match $rootPattern -or $text -match '(?i)[A-Z]:\\Users\\[^\\]+') {
            throw "Machine-specific absolute path found in $($file.FullName)"
        }
    }
}

function Find-Edge {
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles 'Microsoft/Edge/Application/msedge.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe')
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'Microsoft Edge is required to generate the release PDF.'
}

function New-DocumentPdf([string]$Source, [string]$Output) {
    if (Test-Path -LiteralPath $Output) { Remove-Item -LiteralPath $Output -Force }
    # Chromium can reject a profile or local-file source on a mapped/network
    # workspace. Render from the machine-local temporary directory, then copy
    # the finished document into dist.
    $temp = Join-Path ([IO.Path]::GetTempPath()) ('vision-restoration-pdf-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try {
        $localSource = Join-Path $temp 'quick-start.html'
        $localPdf = Join-Path $temp 'quick-start.pdf'
        $profile = Join-Path $temp 'edge-profile'
        Copy-Required $Source $localSource
        $edge = Find-Edge
        $argumentLine = '--headless=new --disable-gpu --no-pdf-header-footer --user-data-dir="' + $profile +
            '" --print-to-pdf="' + $localPdf + '" "' + ([Uri]$localSource).AbsoluteUri + '"'
        $process = Start-Process -FilePath $edge -ArgumentList $argumentLine -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(60000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            throw 'Microsoft Edge PDF generation timed out.'
        }
        for ($attempt = 0; $attempt -lt 50 -and -not (Test-Path -LiteralPath $localPdf); $attempt++) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $localPdf) -or (Get-Item -LiteralPath $localPdf).Length -lt 10000) {
            throw "Microsoft Edge PDF generation failed with exit code $($process.ExitCode)."
        }
        Copy-Required $localPdf $Output
        $signature = [IO.File]::ReadAllBytes($Output)[0..4]
        if ([Text.Encoding]::ASCII.GetString($signature) -ne '%PDF-') { throw 'The quick-start output is not a PDF.' }
    } finally {
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not [IO.Path]::GetFullPath($temp).StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe PDF temporary path.' }
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Copy-VcRuntime([string]$Stage) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer vswhere.exe was not found.' }
    $vs = & $vswhere -latest -products '*' -property installationPath
    if (-not $vs) { throw 'Visual Studio 2022 was not found.' }
    $crt = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Recurse -Directory |
        Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC\d+\.CRT$' -and $_.FullName -notmatch '\\onecore\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $crt) { throw 'The x64 Visual C++ runtime directory was not found.' }
    Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' -File | ForEach-Object {
        Copy-Required $_.FullName (Join-Path $Stage $_.Name)
    }
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
foreach ($path in @($coreStage, $verifyRoot)) { Reset-Directory $path }
foreach ($path in @($coreZip, $retiredAiZip, $pdfPath, (Join-Path $distRoot 'SHA256SUMS.txt'))) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

if (-not $SkipBuild) {
    & (Join-Path $projectRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw 'Application build or tests failed.' }
    & (Join-Path $projectRoot 'tools/Build-Rp2040.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'RP2040 firmware build failed.' }
}

$readmeHtml = Join-Path $distRoot 'README.print.html'
py -3 (Join-Path $projectRoot 'tools/Render-Readme.py') $readmeHtml
if ($LASTEXITCODE -ne 0) { throw 'README HTML generation failed.' }
New-DocumentPdf (Join-Path $projectRoot 'docs/portable-quick-start.html') $pdfPath
New-DocumentPdf $readmeHtml $readmePdfPath

# Main portable application: explicit allowlist only.
New-Item -ItemType Directory -Path (Join-Path $coreStage 'models') -Force | Out-Null
foreach ($name in @(
    'VisionRestoration.exe', 'vision_firmware.exe', 'vision_stereo_setup.exe',
    'vision_usb_recover.exe', 'vision_winusb_installer.exe', 'VisionDepth.exe',
    'libusb-1.0.dll', 'onnxruntime.dll', 'DirectML.dll'
)) {
    Copy-Required (Join-Path $binRoot $name) (Join-Path $coreStage $name)
}
foreach ($architecture in @('x86', 'x64')) {
    $addon = if ($architecture -eq 'x64') { 'VisionStereoCapture.addon64' } else { 'VisionStereoCapture.addon32' }
    foreach ($name in @('VisionStereo11.dll', 'VisionStereoLoader.dll', 'ReShade.dll', $addon)) {
        Copy-Required (Join-Path $binRoot "runtime/$architecture/$name") (Join-Path $coreStage "runtime/$architecture/$name")
    }
}
Copy-Required (Join-Path $projectRoot 'build/rp2040/VisionEmitter.uf2') (Join-Path $coreStage 'firmware/VisionEmitter.uf2')
Copy-Required (Join-Path $projectRoot 'tools/Start Vision Restoration.cmd') (Join-Path $coreStage 'Start Vision Restoration.cmd')
Copy-Required (Join-Path $projectRoot 'tools/Start Vision Restoration.cmd') (Join-Path $coreStage 'Launch.cmd')
Copy-Required (Join-Path $projectRoot 'README.md') (Join-Path $coreStage 'README.md')
Copy-Required (Join-Path $projectRoot 'tools/Prepare NVIDIA Firmware.cmd') (Join-Path $coreStage 'Prepare NVIDIA Firmware.cmd')
Copy-Required (Join-Path $projectRoot 'tools/Prepare NVIDIA Firmware.ps1') (Join-Path $coreStage 'Prepare NVIDIA Firmware.ps1')
Copy-Required (Join-Path $projectRoot 'third_party/7zip-26.03/x64/7za.exe') (Join-Path $coreStage 'tools/7zip/7za.exe')
Copy-License (Join-Path $projectRoot 'third_party/7zip-26.03/License.txt') $coreStage '7zip/License.txt'
Copy-License (Join-Path $projectRoot 'third_party/7zip-26.03/7z2603-src.7z') $coreStage '7zip/7z2603-src.7z'
Copy-License (Join-Path $projectRoot 'third_party/libusb-1.0.30/COPYING') $coreStage '7zip/GNU-LGPL-2.1.txt'
Copy-License (Join-Path $projectRoot 'docs/7ZIP-NOTICE.txt') $coreStage '7zip/NOTICE.txt'
Copy-Required $pdfPath (Join-Path $coreStage 'Vision Restoration Quick Start.pdf')
Copy-Required $readmePdfPath (Join-Path $coreStage 'Vision Restoration README.pdf')
Copy-VcRuntime $coreStage

Copy-License (Join-Path $projectRoot 'LICENSE') $coreStage 'Vision-Restoration-GPL-3.0.txt'
Copy-License (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') $coreStage 'THIRD_PARTY_NOTICES.txt'
Copy-License (Join-Path $projectRoot 'third_party/imgui-1.91.9b/LICENSE.txt') $coreStage 'Dear-ImGui-MIT.txt'
Copy-License (Join-Path $projectRoot 'third_party/libusb-1.0.30/COPYING') $coreStage 'libusb-LGPL-2.1.txt'
Copy-License (Join-Path $projectRoot 'third_party/libwdi/COPYING-LGPL') $coreStage 'libwdi-LGPL-3.0.txt'
Copy-License (Join-Path $projectRoot 'third_party/minhook-1.3.4/LICENSE.txt') $coreStage 'MinHook-BSD-2-Clause.txt'
Copy-License (Join-Path $binRoot 'runtime/ReShade-LICENSE.md') $coreStage 'ReShade-BSD-3-Clause.txt'
Copy-License (Join-Path $projectRoot 'third_party/pico-sdk-2.2.0/LICENSE.TXT') $coreStage 'Pico-SDK-BSD-3-Clause.txt'
Copy-License (Join-Path $projectRoot 'third_party/tinyusb-86ad6e56c1700e85f1c5678607a762cfe3aa2f47/LICENSE') $coreStage 'TinyUSB-MIT.txt'
Copy-License (Join-Path $projectRoot 'docs/MICROSOFT-RUNTIME-NOTICE.txt') $coreStage 'Microsoft-Runtime-Notice.txt'
Copy-License (Join-Path $projectRoot 'third_party/onnxruntime/LICENSE') $coreStage 'ONNX-Runtime-MIT.txt'
Copy-License (Join-Path $projectRoot 'third_party/onnxruntime/ThirdPartyNotices.txt') $coreStage 'ONNX-Runtime-Third-Party-Notices.txt'
Copy-License (Join-Path $projectRoot 'third_party/directml/LICENSE.txt') $coreStage 'DirectML-License.txt'
Copy-License (Join-Path $projectRoot 'third_party/directml/ThirdPartyNotices.txt') $coreStage 'DirectML-Third-Party-Notices.txt'
Copy-License (Join-Path $projectRoot 'docs/MODEL-LICENSES.txt') $coreStage 'AI-Depth-Model-Download-Notices.txt'

Assert-ReleaseContents $coreStage
New-HashManifest $coreStage

if (-not $SkipSmoke) {
    Get-ChildItem -LiteralPath $coreStage -Force | Copy-Item -Destination $verifyRoot -Recurse -Force
    $gpuTest = Start-Process -FilePath (Join-Path $verifyRoot 'VisionRestoration.exe') -ArgumentList '--gpu-test' -WorkingDirectory $verifyRoot -WindowStyle Hidden -PassThru -Wait
    if ($gpuTest.ExitCode -ne 0) { throw "Portable GPU self-test failed with exit code $($gpuTest.ExitCode)." }
    $uiTest = Start-Process -FilePath (Join-Path $verifyRoot 'VisionRestoration.exe') -ArgumentList '--smoke-test','--prepare-smoke' -WorkingDirectory $verifyRoot -WindowStyle Hidden -PassThru -Wait
    if ($uiTest.ExitCode -ne 0) { throw "Portable UI smoke test failed with exit code $($uiTest.ExitCode)." }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($coreStage, $coreZip, [IO.Compression.CompressionLevel]::Optimal, $false)

$archiveHashes = @($coreZip, $readmePdfPath, $pdfPath) | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant(), ((Split-Path -Leaf $_).Replace(' ', '.')) }
[IO.File]::WriteAllLines((Join-Path $distRoot 'SHA256SUMS.txt'), $archiveHashes, [Text.UTF8Encoding]::new($false))

Remove-WithRetry $coreStage
Remove-WithRetry $verifyRoot
Write-Host ''
Write-Host 'Public release archive created:'
Get-Item -LiteralPath $coreZip, (Join-Path $distRoot 'SHA256SUMS.txt') | Select-Object FullName, Length, LastWriteTime
