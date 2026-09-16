@echo off
cd /d "%~dp0"
if not exist "build\panel-experiments\bin\Release\VisionRestoration.exe" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Build-Panel-Experiments.ps1"
  if errorlevel 1 exit /b 1
)
powershell.exe -NoProfile -Command "if (Get-Process -Name VisionRestoration -ErrorAction SilentlyContinue) { Write-Host 'Close the running Vision Restoration app, then launch the panel experiments again.'; exit 1 }"
if errorlevel 1 (
  pause
  exit /b 1
)
start "" "%~dp0build\panel-experiments\bin\Release\VisionRestoration.exe" --calibrate
