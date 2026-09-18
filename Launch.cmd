@echo off
cd /d "%~dp0"
if not exist "build\bin\Release\VisionRestoration.exe" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
  if errorlevel 1 exit /b 1
)
start "" "%~dp0build\bin\Release\VisionRestoration.exe" --games
