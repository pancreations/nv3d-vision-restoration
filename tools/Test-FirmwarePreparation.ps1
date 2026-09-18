param(
    [Parameter(Mandatory = $true)][string]$PortableDirectory,
    [Parameter(Mandatory = $true)][string]$NvidiaPackage,
    [string]$NvidiaSys
)

$ErrorActionPreference = 'Stop'
$portableRoot = (Resolve-Path -LiteralPath $PortableDirectory).Path.TrimEnd('\')
$packagePath = (Resolve-Path -LiteralPath $NvidiaPackage).Path
$helper = Join-Path $portableRoot 'Prepare NVIDIA Firmware.ps1'
$testRoot = Join-Path $portableRoot ('firmware test ' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Invoke-Preparation([string]$InputPath, [string]$OutputPath, [bool]$ExpectSuccess) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $helper -InputFile $InputPath -OutputDirectory $OutputPath -ToolDirectory $portableRoot
    if (($LASTEXITCODE -eq 0) -ne $ExpectSuccess) { throw "Unexpected helper result for $InputPath (exit $LASTEXITCODE)." }
}

try {
    $copiedPackage = Join-Path $testRoot 'NVIDIA package with spaces.exe'
    Copy-Item -LiteralPath $packagePath -Destination $copiedPackage
    $exeOutput = Join-Path $testRoot 'exe output'
    Invoke-Preparation $copiedPackage $exeOutput $true
    $exeFirmware = Join-Path $exeOutput 'emitter.fw'
    if ((Get-Item -LiteralPath $exeFirmware).Length -ne 7026) { throw 'Unexpected extracted firmware size.' }
    $expectedHash = (Get-FileHash -LiteralPath $exeFirmware -Algorithm SHA256).Hash
    Invoke-Preparation $copiedPackage $exeOutput $true
    if ((Get-FileHash -LiteralPath $exeFirmware -Algorithm SHA256).Hash -ne $expectedHash) { throw 'Repeat preparation changed firmware.' }

    if ($NvidiaSys) {
        $sysOutput = Join-Path $testRoot 'sys output'
        Invoke-Preparation (Resolve-Path -LiteralPath $NvidiaSys).Path $sysOutput $true
        if ((Get-FileHash -LiteralPath (Join-Path $sysOutput 'emitter.fw') -Algorithm SHA256).Hash -ne $expectedHash) { throw 'EXE and SYS preparation results differ.' }
    }

    $malformedSys = Join-Path $testRoot 'not a firmware driver.sys'
    Copy-Item -LiteralPath (Join-Path $portableRoot 'VisionRestoration.exe') -Destination $malformedSys
    $badOutput = Join-Path $testRoot 'bad output'
    Invoke-Preparation $malformedSys $badOutput $false
    if (Test-Path -LiteralPath (Join-Path $badOutput 'emitter.fw')) { throw 'Invalid driver created output firmware.' }
    Invoke-Preparation (Join-Path $portableRoot 'licenses/7zip/NOTICE.txt') $badOutput $false

    $conflictOutput = Join-Path $testRoot 'conflict output'
    New-Item -ItemType Directory -Path $conflictOutput | Out-Null
    $existing = Join-Path $conflictOutput 'emitter.fw'
    Copy-Item -LiteralPath (Join-Path $portableRoot 'licenses/7zip/NOTICE.txt') -Destination $existing
    $beforeHash = (Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash
    Invoke-Preparation $copiedPackage $conflictOutput $false
    if ((Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash -ne $beforeHash) { throw 'Existing different firmware was overwritten.' }
    Write-Host 'PASS: downloaded EXE, repeat operation, optional SYS, spaces in paths, invalid inputs, and existing-firmware preservation.'
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolvedTestRoot.StartsWith($portableRoot + '\firmware test ', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe firmware test cleanup target.' }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
