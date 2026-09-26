param([ValidateRange(1,60)][int]$Seconds=30,[ValidateRange(0,600)][int]$WaitForOutputSeconds=0)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$exe=Join-Path $root 'build\bin\Release\VisionRestoration.exe'
# Read-only observation. Window titles and screen contents are not collected.
Add-Type @'
using System;
using System.Text;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
public static class OverlayFocusTrace {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
 [StructLayout(LayoutKind.Sequential)] struct Point { public int X,Y; }
 delegate bool EnumProc(IntPtr w,IntPtr p);
 delegate void EventProc(IntPtr hook,uint kind,IntPtr w,int obj,int child,uint thread,uint time);
 [StructLayout(LayoutKind.Sequential)] struct Msg { public IntPtr w; public uint m; public UIntPtr a; public IntPtr b; public uint time; public int x,y; public uint reserved; }
 [DllImport("user32.dll")] static extern IntPtr SetWinEventHook(uint first,uint last,IntPtr module,EventProc callback,uint pid,uint thread,uint flags);
 [DllImport("user32.dll")] static extern bool UnhookWinEvent(IntPtr hook);
 [DllImport("user32.dll")] static extern bool PeekMessage(out Msg message,IntPtr w,uint first,uint last,uint remove);
 [DllImport("user32.dll")] static extern bool TranslateMessage(ref Msg message);
 [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref Msg message);
 static EventProc eventCallback=OnEvent;
 static IntPtr focusHook,windowHook;
 static readonly ConcurrentQueue<string> events=new ConcurrentQueue<string>();
 public static void StartEvents(uint appPid){
  focusHook=SetWinEventHook(3,3,IntPtr.Zero,eventCallback,0,0,2);
  windowHook=SetWinEventHook(0x8002,0x800B,IntPtr.Zero,eventCallback,appPid,0,2);
  if(focusHook==IntPtr.Zero||windowHook==IntPtr.Zero)throw new Exception("Cannot observe window events.");
 }
 static void OnEvent(IntPtr hook,uint kind,IntPtr w,int obj,int child,uint thread,uint time){
  if(obj!=0||child!=0||(kind!=3&&kind!=0x8002&&kind!=0x8003&&kind!=0x800B))return;
  uint pid;GetWindowThreadProcessId(w,out pid);
  if(events.Count<2048)events.Enqueue(String.Format("event timeMs={0} type=0x{1:X} hwnd={2} pid={3} class={4} visible={5}",time,kind,w.ToInt64(),pid,Class(w),IsWindowVisible(w)));
 }
 public static string[] DrainEvents(){
  Msg message;while(PeekMessage(out message,IntPtr.Zero,0,0,1)){TranslateMessage(ref message);DispatchMessage(ref message);}
  var result=new System.Collections.Generic.List<string>();string line;while(events.TryDequeue(out line))result.Add(line);return result.ToArray();
 }
 public static void StopEvents(){if(focusHook!=IntPtr.Zero)UnhookWinEvent(focusHook);if(windowHook!=IntPtr.Zero)UnhookWinEvent(windowHook);}
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback,IntPtr p);
 [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr w,uint cmd);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr w,StringBuilder name,int count);
 [DllImport("user32.dll")] static extern IntPtr GetWindowLongPtr(IntPtr w,int index);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr w);
 [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
 [DllImport("user32.dll")] static extern bool IsIconic(IntPtr w);
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] static extern bool GetLayeredWindowAttributes(IntPtr w,out uint key,out byte alpha,out uint flags);
 [DllImport("dwmapi.dll")] static extern int DwmGetWindowAttribute(IntPtr w,uint attr,out int value,int size);
 static string Class(IntPtr w){var s=new StringBuilder(256);GetClassName(w,s,s.Capacity);return s.ToString();}
 public static IntPtr FindOutput(uint pid){
  IntPtr output=IntPtr.Zero;
  EnumWindows((w,p)=>{uint id;GetWindowThreadProcessId(w,out id);if(id==pid&&Class(w)=="VisionRestorationOutput"){output=w;return false;}return true;},IntPtr.Zero);
  return output;
 }
 public static string Snapshot(IntPtr output){
  IntPtr old=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {return ReadSnapshot(output);} finally {if(old!=IntPtr.Zero)SetThreadDpiAwarenessContext(old);}
 }
 static string ReadSnapshot(IntPtr output){
  IntPtr focus=GetForegroundWindow();uint pid;GetWindowThreadProcessId(focus,out pid);
  Rect area,rect;GetWindowRect(output,out area);GetWindowRect(focus,out rect);
  uint key,flags;byte alpha;GetLayeredWindowAttributes(output,out key,out alpha,out flags);
  var s=new StringBuilder();
  s.AppendFormat("focus={0} pid={1} class={2} rect={3},{4},{5},{6} output={7} visible={8} alpha={9} topmost={10} outputRect={11},{12},{13},{14} above=[",focus.ToInt64(),pid,Class(focus),rect.L,rect.T,rect.R,rect.B,output.ToInt64(),IsWindowVisible(output),alpha,(GetWindowLongPtr(output,-20).ToInt64()&8)!=0,area.L,area.T,area.R,area.B);
  int count=0;
  for(IntPtr w=GetWindow(output,3);w!=IntPtr.Zero&&count++<512;w=GetWindow(w,3)){
   if(!IsWindowVisible(w)||IsIconic(w))continue;
   int cloaked;if(DwmGetWindowAttribute(w,14,out cloaked,4)==0&&cloaked!=0)continue;
   Rect r;if(!GetWindowRect(w,out r)||r.R<=area.L||r.L>=area.R||r.B<=area.T||r.T>=area.B)continue;
   uint id;GetWindowThreadProcessId(w,out id);
   s.AppendFormat(" hwnd={0}/pid={1}/class={2}/rect={3},{4},{5},{6};",w.ToInt64(),id,Class(w),r.L,r.T,r.R,r.B);
  }
  s.Append("] clickTargets=[");
  foreach(int percent in new[]{10,50,90}){
   Point point=new Point {X=area.L+(area.R-area.L)*percent/100,Y=area.T+(area.B-area.T)*percent/100};
   IntPtr target=WindowFromPoint(point);uint targetPid;GetWindowThreadProcessId(target,out targetPid);
   s.AppendFormat(" {0}%:hwnd={1}/pid={2};",percent,target.ToInt64(),targetPid);
  }
  s.Append("]");return s.ToString();
 }
}
'@
$waiting=[Diagnostics.Stopwatch]::StartNew()
do {
 $app=Get-Process VisionRestoration -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $exe} | Select-Object -First 1
 $output=if($app){[OverlayFocusTrace]::FindOutput($app.Id)}else{[IntPtr]::Zero}
 if($output -ne [IntPtr]::Zero){break}
 if($waiting.Elapsed.TotalSeconds -ge $WaitForOutputSeconds){throw 'No running workspace stereo output found before the trace timeout.'}
 Start-Sleep -Milliseconds 100
} while($true)
$report=Join-Path $root ('reports\overlay-focus-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.log')
$timer=[Diagnostics.Stopwatch]::StartNew()
$last=''
$samples=0
$writer=[IO.StreamWriter]::new($report,$false,[Text.UTF8Encoding]::new($false))
try {
 [OverlayFocusTrace]::StartEvents([uint32]$app.Id)
 $writer.WriteLine('Read-only focus/window-order trace; '+(Get-Date -Format o))
 while($timer.Elapsed.TotalSeconds -lt $Seconds -and !$app.HasExited){
  foreach($eventLine in [OverlayFocusTrace]::DrainEvents()){$writer.WriteLine($eventLine)}
  $output=[OverlayFocusTrace]::FindOutput($app.Id)
  $state=[OverlayFocusTrace]::Snapshot($output)
  if($state -ne $last){$writer.WriteLine(('{0:F3} {1}' -f $timer.Elapsed.TotalSeconds,$state));$writer.Flush();$last=$state;$samples++}
  Start-Sleep -Milliseconds 10
 }
} finally {[OverlayFocusTrace]::StopEvents();$writer.Dispose()}
"Recorded $samples window/focus states to $report"
Get-Content -LiteralPath $report -Tail 60
