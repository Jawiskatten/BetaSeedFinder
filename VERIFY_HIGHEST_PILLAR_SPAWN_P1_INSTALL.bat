@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-highest-pillar-spawn-p1-amd.ps1" -Radius 4 -SelfTest
set "EC=%ERRORLEVEL%"
echo.
if "%EC%"=="0" (
  echo HighestPillarSpawn P1 install/self-test OK.
) else (
  echo HighestPillarSpawn P1 verify FAILED with exit code %EC%.
)
pause
exit /b %EC%
