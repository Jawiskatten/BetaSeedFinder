@echo off
setlocal
cd /d "%~dp0"
set "LAST=%~dp0out\highest_pillar_spawn_p1\LAST_RUN.txt"
if not exist "%LAST%" (
  echo No LAST_RUN.txt found.
  pause
  exit /b 1
)
set /p OUT=<"%LAST%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-highest-pillar-spawn-p1-amd.ps1" -Count 100000000 -Radius 4 -Top 250 -ExistingOutput "%OUT%"
set "EC=%ERRORLEVEL%"
echo.
if not "%EC%"=="0" echo FAILED with exit code %EC%.
pause
exit /b %EC%
