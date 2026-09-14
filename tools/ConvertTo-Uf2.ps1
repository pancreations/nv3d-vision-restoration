param([Parameter(Mandatory=$true)][string]$Binary,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
# RP2040 UF2: 256-byte payloads, flash at 0x10000000, family 0xE48BFF56.
$bytes=[System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Binary))
if($bytes.Length -lt 256 -or $bytes.Length -gt 2MB){throw 'Binary must fit RP2040-Zero 2 MB flash and include boot2'}
$blocks=[int][Math]::Ceiling($bytes.Length/256.0)
$stream=New-Object System.IO.MemoryStream
$writer=New-Object System.IO.BinaryWriter($stream)
try {
    for($block=0;$block -lt $blocks;$block++) {
        $writer.Write([uint32]0x0A324655);$writer.Write([uint32]0x9E5D5157L)
        $writer.Write([uint32]0x2000);$writer.Write([uint32](0x10000000+$block*256))
        $writer.Write([uint32]256);$writer.Write([uint32]$block);$writer.Write([uint32]$blocks);$writer.Write([uint32]0xE48BFF56L)
        $payload=New-Object byte[] 476
        [Array]::Copy($bytes,$block*256,$payload,0,[Math]::Min(256,$bytes.Length-$block*256))
        $writer.Write($payload);$writer.Write([uint32]0x0AB16F30)
    }
    $writer.Flush();[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Output),$stream.ToArray())
} finally {$writer.Dispose();$stream.Dispose()}
Write-Output "UF2 packaged: $Output ($blocks blocks). Not flashed."
