@echo off
setlocal
cd /d "%~dp0"
set /p SEED=Enter signed Beta 1.7.3 seed: 
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-cursed-spawn-origin-p1-amd.ps1" -Seed "%SEED%" -Radius 4 -Top 1
set "EC=%ERRORLEVEL%"
echo.
pause
exit /b %EC%
