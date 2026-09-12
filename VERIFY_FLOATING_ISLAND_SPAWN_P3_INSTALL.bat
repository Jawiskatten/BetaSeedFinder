@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0native\floating_island_spawn\FloatingIslandSpawnGpuFinderP3.cpp" (
  echo Missing native\floating_island_spawn\FloatingIslandSpawnGpuFinderP3.cpp
  pause
  exit /b 1
)
if not exist "%~dp0scripts\run-floating-island-spawn-p3-amd.ps1" (
  echo Missing scripts\run-floating-island-spawn-p3-amd.ps1
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-floating-island-spawn-p3-amd.ps1" -SelfTest -Radius 4
set "EC=%ERRORLEVEL%"
echo.
if "%EC%"=="0" (
  echo VERIFY OK
) else (
  echo VERIFY FAILED with exit code %EC%
)
pause
exit /b %EC%
