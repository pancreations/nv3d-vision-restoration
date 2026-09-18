param(
    [string]$InputFile,
    [string]$OutputDirectory = $PSScriptRoot,
    [string]$ToolDirectory = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
$temporaryRoot = $null
try {
    if (-not $InputFile) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = [Windows.Forms.OpenFileDialog]::new()
        try {
            $dialog.Title = 'Select the downloaded NVIDIA USB controller EXE or nvstusb SYS file'
            $dialog.Filter = 'NVIDIA package or driver (*.exe;*.sys)|*.exe;*.sys'
            $dialog.CheckFileExists = $true
            if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 0 }
            $InputFile = $dialog.FileName
        } finally { $dialog.Dispose() }
    }
    $inputPath = (Resolve-Path -LiteralPath $InputFile).Path
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw 'Select a file, not a folder.' }
    $extension = [IO.Path]::GetExtension($inputPath).ToLowerInvariant()
    if ($extension -notin @('.exe', '.sys')) { throw 'Select the NVIDIA download (.exe) or an extracted nvstusb driver (.sys).' }
    $firmwareTool = Join-Path $ToolDirectory 'vision_firmware.exe'
    if (-not (Test-Path -LiteralPath $firmwareTool -PathType Leaf)) { throw 'vision_firmware.exe is missing. Restore the complete portable release.' }

    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('vision-firmware-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $driverPath = $inputPath
    if ($extension -eq '.exe') {
        $extractor = Join-Path $ToolDirectory 'tools/7zip/7za.exe'
        if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) { throw 'The included package extractor is missing. Restore the complete portable release.' }
        $driverDirectory = Join-Path $temporaryRoot 'drivers'
        Write-Host 'Opening the NVIDIA package as an archive. Its installer is NOT being run.'
        & $extractor e $inputPath 'nvstusb*.sys' '-r' "-o$driverDirectory" '-y' '-bd' '-bb0' | Out-Null
        if ($LASTEXITCODE -gt 1) { throw 'Could not open this NVIDIA package. Select the standalone USB controller download linked in the Quick Start PDF.' }
        $drivers = @(Get-ChildItem -LiteralPath $driverDirectory -Filter 'nvstusb*.sys' -File -ErrorAction SilentlyContinue)
        if (-not $drivers.Count) { throw 'This package contains no nvstusb driver. Use the standalone USB controller download in the Quick Start PDF.' }
        $driverPath = ($drivers | Sort-Object @{Expression = { $_.Name -ne 'nvstusb64.sys' }}, Name | Select-Object -First 1).FullName
    }

    $prepared = Join-Path $temporaryRoot 'emitter.fw'
    & $firmwareTool $driverPath $prepared | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'No supported emitter firmware could be extracted. Existing firmware was not changed.' }
    $output = Join-Path $OutputDirectory 'emitter.fw'
    if (Test-Path -LiteralPath $output) {
        if ((Get-FileHash -LiteralPath $prepared -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash) {
            throw 'A different emitter.fw already exists. It was preserved; move it aside yourself before preparing another version.'
        }
        Write-Host 'The correct emitter.fw is already present. No changes were needed.'
    } else {
        New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
        $destination = [IO.File]::Open($output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try {
            $data = [IO.File]::ReadAllBytes($prepared)
            $destination.Write($data, 0, $data.Length)
        } finally { $destination.Dispose() }
        Write-Host "Firmware prepared successfully: $output"
    }
    Write-Host 'Start Vision Restoration. No NVIDIA driver was installed and no USB writes were performed.'
    exit 0
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($temporaryRoot -and (Test-Path -LiteralPath $temporaryRoot)) {
        $resolved = [IO.Path]::GetFullPath($temporaryRoot)
        $tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
        if ($resolved.StartsWith($tempBase + '\vision-firmware-', [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}
