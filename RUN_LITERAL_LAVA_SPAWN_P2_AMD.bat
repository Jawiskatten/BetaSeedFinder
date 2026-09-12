@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-cursed-spawn-origin-p1-amd.ps1" -Count 100000000 -Radius 4 -Top 250
set "EC=%ERRORLEVEL%"
echo.
if not "%EC%"=="0" echo FAILED with exit code %EC%.
pause
exit /b %EC%
