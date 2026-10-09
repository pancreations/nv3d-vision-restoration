param([string]$GameDir = 'D:\SteamLibrary\steamapps\common\Schedule I')
$ErrorActionPreference = 'Stop'
$compiler = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe'
$runtime = 'C:\Program Files\dotnet\shared\Microsoft.NETCore.App\6.0.36'
$interop = Join-Path $GameDir 'MelonLoader\Il2CppAssemblies'
if (!(Test-Path $interop)) { throw 'Run Schedule I with MelonLoader once to generate assemblies.' }
$out = Join-Path $PSScriptRoot 'package'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$refs = @(Get-ChildItem $runtime -Filter '*.dll') + @(Get-ChildItem $interop -Filter '*.dll') + @(Get-ChildItem (Join-Path $GameDir 'MelonLoader\net6') -Filter '*.dll')
$refs = $refs | Where-Object { try { [Reflection.AssemblyName]::GetAssemblyName($_.FullName) | Out-Null; $true } catch { $false } }
$argsList = @('/nologo','/target:library','/langversion:latest','/nostdlib+','/optimize+',('/out:' + (Join-Path $out 'ScheduleIStereoSBS.dll')))
$argsList += $refs | ForEach-Object { '/reference:' + $_.FullName }
$argsList += Join-Path $PSScriptRoot 'StereoMod.cs'
$response = Join-Path $out 'compile.rsp'
$argsList | ForEach-Object { '"' + $_ + '"' } | Set-Content -LiteralPath $response -Encoding utf8
& $compiler ('@' + $response)
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
Copy-Item (Join-Path $PSScriptRoot 'README.md') $out -Force
Write-Host "Built $out\ScheduleIStereoSBS.dll"
