param([string]$Directory='')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
if(-not $Directory){$Directory=Join-Path $projectRoot 'build/rp2040'}
$bin=[System.IO.File]::ReadAllBytes((Join-Path $Directory 'VisionEmitter.bin'))
$uf2=[System.IO.File]::ReadAllBytes((Join-Path $Directory 'VisionEmitter.uf2'))
if($uf2.Length -eq 0 -or $uf2.Length%512 -ne 0){throw 'Invalid UF2 size'}
$blocks=$uf2.Length/512
if($blocks -ne [Math]::Ceiling($bin.Length/256.0)){throw 'UF2 block count mismatch'}
for($i=0;$i -lt $blocks;$i++) {
    $offset=$i*512
    $expected=@([uint32]0x0A324655,[uint32]0x9E5D5157L,[uint32]0x2000,[uint32](0x10000000+$i*256),[uint32]256,[uint32]$i,[uint32]$blocks,[uint32]0xE48BFF56L)
    for($field=0;$field -lt 8;$field++){if([BitConverter]::ToUInt32($uf2,$offset+$field*4) -ne $expected[$field]){throw "UF2 header mismatch at block $i field $field"}}
    if([BitConverter]::ToUInt32($uf2,$offset+508) -ne 0x0AB16F30){throw 'UF2 footer mismatch'}
    for($j=0;$j -lt 476;$j++) {
        $value=0
        if($j -lt 256 -and ($i*256+$j) -lt $bin.Length){$value=$bin[$i*256+$j]}
        if($uf2[$offset+32+$j] -ne $value){throw "UF2 payload or zero-padding mismatch in block $i"}
    }
}
# Independently verify the boot2 checksum (non-reflected CRC32, initial all-ones).
[uint32]$crc=0xffffffffL
for($i=0;$i -lt 252;$i++) {
    $crc=$crc -bxor ([uint32]$bin[$i] -shl 24)
    for($j=0;$j -lt 8;$j++) {
        $top=($crc -band 0x80000000L) -ne 0
        $crc=[uint32](([uint64]$crc -shl 1) -band 0xffffffffL)
        if($top){$crc=$crc -bxor 0x04C11DB7}
    }
}
if([BitConverter]::ToUInt32($bin,252) -ne $crc){throw 'RP2040 boot2 checksum mismatch'}
$stack=[BitConverter]::ToUInt32($bin,256);$reset=[BitConverter]::ToUInt32($bin,260)
if($stack -lt 0x20000000 -or $stack -gt 0x20042000){throw 'Invalid Cortex-M0+ initial stack'}
if(($reset -band 1) -ne 1 -or $reset -lt 0x10000100 -or $reset -ge (0x10000000+$bin.Length)){throw 'Invalid Cortex-M0+ reset vector'}
$report=@('PASS RP2040 UF2 headers, family, addresses, payload, padding and footer',
    'PASS boot2 CRC32 and Cortex-M0+ vectors',"Binary $($bin.Length) bytes; UF2 $($uf2.Length) bytes; $blocks blocks",'Artifact checks only. This check does not flash hardware or establish USB/electrical/optical operation.',
    ('SHA256: '+(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Directory 'VisionEmitter.uf2')).Hash))
$report | Set-Content -LiteralPath (Join-Path $projectRoot 'reports/rp2040-firmware-artifact.txt') -Encoding UTF8
$report | Write-Output
