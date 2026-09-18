@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"

echo Vision Restoration - NVIDIA emitter firmware preparation
echo.
echo Select the downloaded NVIDIA EXE package, or an extracted nvstusb SYS file.
echo NVIDIA's installer will not be run. No driver is installed by this helper.
echo.

powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0Prepare NVIDIA Firmware.ps1" -InputFile "%~1"
set "RESULT=%ERRORLEVEL%"
echo.
pause
exit /b %RESULT%
