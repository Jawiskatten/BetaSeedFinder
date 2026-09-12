@echo off
setlocal
cd /d "%~dp0"
echo Verifying Tallest1x1SpawnPillar P2 install...
if not exist "%~dp0native\tallest_pillar_spawn\TallestPillarSpawnGpuFinderP2.cpp" (
  echo MISSING native\tallest_pillar_spawn\TallestPillarSpawnGpuFinderP2.cpp
  exit /b 1
)
if not exist "%~dp0scripts\run-tallest-pillar-spawn-p2-amd.ps1" (
  echo MISSING scripts\run-tallest-pillar-spawn-p2-amd.ps1
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-tallest-pillar-spawn-p2-amd.ps1" -SelfTest -Radius 4
set "EC=%ERRORLEVEL%"
echo.
if "%EC%"=="0" (
  echo VERIFY OK
) else (
  echo VERIFY FAILED with exit code %EC%
)
pause
exit /b %EC%
