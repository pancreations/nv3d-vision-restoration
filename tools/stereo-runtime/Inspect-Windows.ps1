param([Parameter(Mandatory=$true)][int]$ProcessId)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class VisionWindowInfo {
    public long Handle; public long Parent; public bool Visible; public string Class; public string Title;
}
public static class VisionWindows {
    delegate bool EnumProc(IntPtr hwnd,IntPtr param);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback,IntPtr param);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent,EnumProc callback,IntPtr param);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint processId);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd,StringBuilder text,int count);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd,StringBuilder text,int count);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    public static VisionWindowInfo[] Inspect(uint pid) {
        var result=new List<VisionWindowInfo>();
        Action<IntPtr,IntPtr> add=(hwnd,parent)=>{
            var text=new StringBuilder(1024);var cls=new StringBuilder(256);
            GetWindowText(hwnd,text,text.Capacity);GetClassName(hwnd,cls,cls.Capacity);
            result.Add(new VisionWindowInfo{Handle=hwnd.ToInt64(),Parent=parent.ToInt64(),Visible=IsWindowVisible(hwnd),Class=cls.ToString(),Title=text.ToString()});
        };
        EnumWindows((hwnd,param)=>{uint p;GetWindowThreadProcessId(hwnd,out p);if(p==pid){add(hwnd,IntPtr.Zero);EnumChildWindows(hwnd,(child,unused)=>{add(child,hwnd);return true;},IntPtr.Zero);}return true;},IntPtr.Zero);
        return result.ToArray();
    }
}
'@
[VisionWindows]::Inspect($ProcessId) | ConvertTo-Json -Depth 3
