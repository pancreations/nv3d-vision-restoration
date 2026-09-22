param([Parameter(Mandatory=$true)][int]$GameProcessId)
$ErrorActionPreference='Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LiveGameOverlay {
 public delegate bool EnumProc(IntPtr w,IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr w,StringBuilder name,int count);
 [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr w,int index);
 [DllImport("user32.dll")] public static extern IntPtr SetWindowLongPtrW(IntPtr w,int index,IntPtr value);
 [DllImport("user32.dll")] public static extern bool SetLayeredWindowAttributes(IntPtr w,uint key,byte alpha,uint flags);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr w,IntPtr after,int x,int y,int cx,int cy,uint flags);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr w);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 public static IntPtr Find(uint pid){IntPtr result=IntPtr.Zero;EnumWindows((w,p)=>{uint owner;GetWindowThreadProcessId(w,out owner);var name=new StringBuilder(256);GetClassName(w,name,256);if(owner==pid&&name.ToString()=="VisionRestorationOutput")result=w;return true;},IntPtr.Zero);return result;}
}
'@
$app=Get-Process VisionRestoration | Where-Object { $_.Path -eq (Join-Path (Split-Path $PSScriptRoot) 'build\bin\Release\VisionRestoration.exe') } | Select-Object -First 1
if(!$app){throw 'Running workspace app not found'}
$game=Get-Process -Id $GameProcessId
if(!$game.MainWindowHandle -or $game.Id -eq $app.Id){throw 'Game window not found'}
$window=[LiveGameOverlay]::Find($app.Id)
if($window -eq [IntPtr]::Zero){throw 'Stereo output window not found on this desktop'}
$original=[LiveGameOverlay]::GetWindowLongPtrW($window,-20).ToInt64()
$foreground=[LiveGameOverlay]::GetForegroundWindow()
$record=[pscustomobject]@{Time=(Get-Date -Format o);Output=$window.ToInt64();OriginalStyle=$original;Game=$game.Id;GameWindow=$game.MainWindowHandle.ToInt64();OriginalForeground=$foreground.ToInt64()}
$record | ConvertTo-Json | Set-Content (Join-Path (Split-Path $PSScriptRoot) 'reports\live-game-overlay.json')
try {
    [void][LiveGameOverlay]::SetWindowLongPtrW($window,-20,[IntPtr]($original -bor 0x080800A0))
    if(![LiveGameOverlay]::SetLayeredWindowAttributes($window,0,255,2)){throw 'Could not make output opaque'}
    [void][LiveGameOverlay]::SetForegroundWindow($game.MainWindowHandle)
    if(![LiveGameOverlay]::SetWindowPos($window,[IntPtr](-1),0,0,0,0,0x0033)){throw 'Could not keep stereo output on top'}
    if([LiveGameOverlay]::GetForegroundWindow() -eq $game.MainWindowHandle){"Applied opaque click-through stereo overlay; input focus: $($game.ProcessName)."}
    else {'Applied opaque click-through stereo overlay. Click the game image once to give the game input focus.'}
} catch {
    [void][LiveGameOverlay]::SetWindowLongPtrW($window,-20,[IntPtr]$original)
    [void][LiveGameOverlay]::SetForegroundWindow($foreground)
    throw
}
