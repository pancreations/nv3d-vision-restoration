param([string]$ToolchainRoot='', [string]$Python3='')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
if(-not $ToolchainRoot){$ToolchainRoot=Join-Path $projectRoot 'third_party'}
$compiler=Join-Path $ToolchainRoot 'bin/arm-none-eabi-gcc.exe'
if(-not (Test-Path -LiteralPath $compiler)){throw 'ARM compiler missing. See firmware/rp2040/README.md.'}
$sdk=Join-Path $projectRoot 'third_party/pico-sdk-2.2.0'
$tiny=Join-Path $projectRoot 'third_party/tinyusb-86ad6e56c1700e85f1c5678607a762cfe3aa2f47'
$arguments=@('-S',"$projectRoot/firmware/rp2040",'-B',"$projectRoot/build/rp2040",'-G','Ninja',
    "-DPICO_SDK_PATH=$sdk","-DPICO_TINYUSB_PATH=$tiny","-DPICO_TOOLCHAIN_PATH=$ToolchainRoot",
    '-DCMAKE_BUILD_TYPE=Release','-DPICO_BOARD=waveshare_rp2040_zero')
if($Python3){$arguments+="-DPython3_EXECUTABLE=$Python3"}
cmake @arguments
if($LASTEXITCODE -ne 0){throw 'Firmware CMake configuration failed'}
cmake --build "$projectRoot/build/rp2040" --parallel 4
if($LASTEXITCODE -ne 0){throw 'Firmware build failed'}
& "$PSScriptRoot/ConvertTo-Uf2.ps1" -Binary "$projectRoot/build/rp2040/VisionEmitter.bin" -Output "$projectRoot/build/rp2040/VisionEmitter.uf2"
Get-FileHash -Algorithm SHA256 -LiteralPath "$projectRoot/build/rp2040/VisionEmitter.uf2"
