@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\run-floating-island-spawn-p5-wave-amd.ps1" -Count 100000000 -MaxSpeed
set ERR=%ERRORLEVEL%
echo.
if not "%ERR%"=="0" echo FAILED with exit code %ERR%.
pause
exit /b %ERR%
