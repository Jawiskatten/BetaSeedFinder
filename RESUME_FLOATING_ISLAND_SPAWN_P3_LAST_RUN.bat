@echo off
setlocal
cd /d "%~dp0"
set "LAST=%~dp0out\floating_island_spawn_p3\LAST_RUN.txt"
if not exist "%LAST%" (
  echo No LAST_RUN.txt found.
  pause
  exit /b 1
)
set /p OUT=<"%LAST%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-floating-island-spawn-p3-amd.ps1" -Count 100000000 -Radius 4 -Top 250 -DesktopFriendly -ExistingOutput "%OUT%"
set "EC=%ERRORLEVEL%"
echo.
if not "%EC%"=="0" echo FAILED with exit code %EC%.
pause
exit /b %EC%
