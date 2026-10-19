param([string]$Executable=(Join-Path $PSScriptRoot '../build/bin/Release/VisionRestoration.exe'))
$ErrorActionPreference='Stop'
$appPath=(Resolve-Path -LiteralPath $Executable).Path
$programs=[Environment]::GetFolderPath('Programs')
if(-not $programs){throw 'The current user Start menu could not be located.'}
$shortcutPath=Join-Path $programs 'Vision Restoration.lnk'
$shortcutShell=New-Object -ComObject WScript.Shell
$shortcut=$shortcutShell.CreateShortcut($shortcutPath)
if((Test-Path -LiteralPath $shortcutPath) -and $shortcut.TargetPath -and $shortcut.TargetPath -ne $appPath){
    throw "An existing Vision Restoration shortcut points elsewhere: $($shortcut.TargetPath)"
}
$shortcut.TargetPath=$appPath
$shortcut.WorkingDirectory=Split-Path -Parent $appPath
$shortcut.Arguments=''
$shortcut.Description='NV3D Vision Restoration'
$shortcut.IconLocation="$appPath,0"
$shortcut.Save()
$saved=$shortcutShell.CreateShortcut($shortcutPath)
if($saved.TargetPath -ne $appPath -or $saved.IconLocation -ne "$appPath,0"){
    throw 'Start menu shortcut verification failed.'
}
Write-Output "Installed Start menu shortcut: $shortcutPath"
