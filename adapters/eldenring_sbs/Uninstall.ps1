$ErrorActionPreference='Stop'
if(Get-Process -Name eldenring,start_protected_game -ErrorAction SilentlyContinue){throw 'Close Elden Ring first.'}
$folder=(Resolve-Path -LiteralPath $PSScriptRoot).Path
$record=Get-Content -LiteralPath (Join-Path $folder 'install.json') -Raw | ConvertFrom-Json
$game=(Resolve-Path -LiteralPath (Split-Path -Parent $folder)).Path
if($game -ne $record.GameDirectory -or (Split-Path -Leaf $folder) -ne 'VisionEldenSBS'){throw 'Installation paths do not match.'}
$rootPrefix=$game.TrimEnd('\')+'\'
foreach($item in @(Get-Item -LiteralPath $folder)+@(Get-ChildItem -LiteralPath $folder -Recurse -Force)){
    $resolved=[IO.Path]::GetFullPath($item.FullName)
    if(-not $resolved.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase) -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Unexpected path: $resolved"}
}
$dll=Join-Path $game 'dxgi.dll'
if((Test-Path -LiteralPath $dll) -and (Get-FileHash -LiteralPath $dll).Hash -ne $record.ReShadeSHA256){throw 'dxgi.dll was replaced by another mod; it will not be moved.'}
$installParent=Split-Path -Parent $game
$archive=[IO.Path]::GetFullPath((Join-Path $installParent ('SBS-uninstalled-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))))
if(-not $archive.StartsWith($installParent.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $archive)){throw 'Unexpected archive destination.'}
$null=New-Item -ItemType Directory -Path $archive
$graphics=$record.GraphicsPath
[xml]$original=Get-Content -LiteralPath (Join-Path $folder 'Before\GraphicsConfig.xml') -Encoding Unicode -Raw
[xml]$current=Get-Content -LiteralPath $graphics -Raw
Copy-Item -LiteralPath $graphics -Destination (Join-Path $archive 'GraphicsConfig-before-uninstall.xml')
foreach($property in $record.GraphicsChanges.PSObject.Properties){
    $node=$current.config.SelectSingleNode($property.Name)
    if($node -and $node.InnerText -eq $property.Value){$node.InnerText=$original.config.SelectSingleNode($property.Name).InnerText}
}
$current.Save($graphics)
foreach($name in @('dxgi.dll','ReShade.ini','Launch Elden Ring SBS.cmd')){
    $source=[IO.Path]::GetFullPath((Join-Path $game $name))
    if(-not $source.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected removal path'}
    if(Test-Path -LiteralPath $source){Move-Item -LiteralPath $source -Destination (Join-Path $archive $name)}
}
# Both the recursive source and destination were validated above.
Move-Item -LiteralPath $folder -Destination (Join-Path $archive 'VisionEldenSBS')
Write-Output "SBS uninstalled; files preserved in $archive"
