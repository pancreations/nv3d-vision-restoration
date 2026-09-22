param([ValidateRange(4,30)][int]$Seconds=12,[switch]$Live)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing -TypeDefinition @'
using System.Windows.Forms;
public class AiDesktopMotionProbe : Form {
 protected override bool ShowWithoutActivation { get { return true; } }
}
'@
# Keep a small patch changing on every attached display so the capture test
# measures fresh desktop frames. Do not focus a window or inject user input.
$forms=@()
$process=$null
$ownsProcess=$false
function Read-LiveSample {
 $lines=Get-Content -LiteralPath (Join-Path $root 'reports\session.log') -Tail 4
 foreach($line in $lines){
  if($line -match '^t=([\d.]+).*\| out=1 .*capture: kind=3 frames=(\d+).*depth: maps=(\d+)'){
   $sample=[pscustomobject]@{Time=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture);Frames=[long]$Matches[2];Maps=[long]$Matches[3];Status=$line}
  }
 }
 if(!$sample){throw 'No running AI desktop sample found in session.log'}
 return $sample
}
try {
 $app=Join-Path $root 'build\bin\Release\VisionRestoration.exe'
 if($Live){
  $process=Get-Process VisionRestoration -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $app} | Select-Object -First 1
  if(!$process){throw 'Start AI desktop before using -Live.'}
 }
 foreach($screen in [Windows.Forms.Screen]::AllScreens){
  $form=New-Object AiDesktopMotionProbe
  $form.Text='AI desktop frame-rate check'
  $form.StartPosition='Manual'
  $form.Location=New-Object Drawing.Point(($screen.Bounds.Left+100),($screen.Bounds.Top+100))
  $form.Size=New-Object Drawing.Size(480,270)
  $form.TopMost=!$Live
  $forms+=$form
  $form.Show()
 }
 if(!$Live){$process=Start-Process -FilePath $app -ArgumentList "--screen-test $Seconds --display 0" -WindowStyle Hidden -PassThru;$ownsProcess=$true}
 $timer=[Diagnostics.Stopwatch]::StartNew()
 $baseline=$null
 while(!$process.HasExited -and $timer.Elapsed.TotalSeconds -lt ($Seconds+20)){
  $value=[int]($timer.ElapsedMilliseconds % 255)
  foreach($form in $forms){$form.BackColor=[Drawing.Color]::FromArgb($value,70,255-$value)}
  [Windows.Forms.Application]::DoEvents()
  if($Live -and !$baseline -and $timer.Elapsed.TotalSeconds -ge 3){$baseline=Read-LiveSample}
  if($Live -and $timer.Elapsed.TotalSeconds -ge $Seconds){break}
  Start-Sleep -Milliseconds 5
 }
 if($Live){
  if($process.HasExited){throw 'AI desktop exited during the test.'}
  $end=Read-LiveSample
  $elapsed=$end.Time-$baseline.Time
  if($elapsed -le 0 -or $end.Frames -lt $baseline.Frames -or $end.Maps -lt $baseline.Maps){throw 'AI desktop restarted or stopped producing diagnostics during the test.'}
  $result=[pscustomobject]@{SampleSeconds=$elapsed;DesktopFps=($end.Frames-$baseline.Frames)/$elapsed;DepthUpdatesPerSecond=($end.Maps-$baseline.Maps)/$elapsed;FinalStatus=$end.Status}
  $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'reports\ai-desktop-live-rate.json')
  $result | Format-List
  return
 }
 if(!$process.HasExited){throw 'Screen test timed out; inspect the app error report.'}
 if($process.ExitCode -ne 0){throw "Screen test failed: $($process.ExitCode)"}
 $report=Join-Path $root 'reports\screen-test.txt'
 Copy-Item -LiteralPath $report -Destination (Join-Path $root 'reports\ai-desktop-motion-rate.txt')
 Get-Content -LiteralPath $report
} finally {
 if($ownsProcess -and $process -and !$process.HasExited){Stop-Process -Id $process.Id -ErrorAction SilentlyContinue}
 foreach($form in $forms){$form.Close();$form.Dispose()}
}
