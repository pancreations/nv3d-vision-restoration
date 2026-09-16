param([ValidateRange(1,60)][int]$Seconds=15,[switch]$ApplyOpaque)
$ErrorActionPreference='Stop'
# A reversible live diagnostic: scope the change to this app's output window and
# restore its exact layered attributes even if sampling fails.
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class VisionOpacityProbe {
 public delegate bool EnumProc(IntPtr window, IntPtr data);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr data);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder name, int count);
 [DllImport("user32.dll", SetLastError=true)] public static extern bool GetLayeredWindowAttributes(IntPtr window, out uint key, out byte alpha, out uint flags);
 [DllImport("user32.dll", SetLastError=true)] public static extern bool SetLayeredWindowAttributes(IntPtr window, uint key, byte alpha, uint flags);
}
'@
$app=Get-Process VisionRestoration | Where-Object { $_.Path -eq (Join-Path (Split-Path $PSScriptRoot) 'build\bin\Release\VisionRestoration.exe') } | Select-Object -First 1
if(!$app){throw 'Running workspace app not found'}
$script:outputHandle=[IntPtr]::Zero
[VisionOpacityProbe]::EnumWindows({param($window,$data)
    [uint32]$owner=0
    [void][VisionOpacityProbe]::GetWindowThreadProcessId($window,[ref]$owner)
    if($owner -eq $app.Id){
        $name=New-Object Text.StringBuilder 256
        [void][VisionOpacityProbe]::GetClassName($window,$name,256)
        if($name.ToString() -eq 'VisionRestorationOutput'){$script:outputHandle=$window}
    }
    return $true
},[IntPtr]::Zero) | Out-Null
if($outputHandle -eq [IntPtr]::Zero){throw 'App output window not found'}
[uint32]$key=0;[byte]$alpha=0;[uint32]$flags=0
if(![VisionOpacityProbe]::GetLayeredWindowAttributes($outputHandle,[ref]$key,[ref]$alpha,[ref]$flags)){throw 'Output is not a layered overlay'}
if($ApplyOpaque){
    if(![VisionOpacityProbe]::SetLayeredWindowAttributes($outputHandle,$key,255,2)){throw 'Opacity change failed'}
    'Applied opaque output to the running workspace app; mouse pass-through and capture exclusion unchanged.'
    return
}
$report=Join-Path (Split-Path $PSScriptRoot) 'reports\overlay-opacity-ab.log'
$session=Join-Path (Split-Path $PSScriptRoot) 'reports\session.log'
function Sample([string]$label){
    "$label $(Get-Date -Format o)" | Tee-Object -FilePath $report -Append
    Get-Content $session -Tail 1 | Tee-Object -FilePath $report -Append
    Start-Sleep -Seconds $Seconds
    Get-Content $session -Tail 1 | Tee-Object -FilePath $report -Append
}
Sample "baseline alpha=$alpha"
try {
    if(![VisionOpacityProbe]::SetLayeredWindowAttributes($outputHandle,$key,255,2)){throw 'Opacity change failed'}
    Sample 'opaque alpha=255'
} finally {
    if(![VisionOpacityProbe]::SetLayeredWindowAttributes($outputHandle,$key,$alpha,$flags)){throw 'Could not restore overlay attributes'}
}
Sample "restored alpha=$alpha"
