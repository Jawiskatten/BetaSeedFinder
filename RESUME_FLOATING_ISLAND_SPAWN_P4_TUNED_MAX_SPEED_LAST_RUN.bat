@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\run-floating-island-spawn-p4-optimized-amd.ps1" -Resume -MaxSpeed -ScoutBatch 262144
set "ec=%errorlevel%"
if not "%ec%"=="0" (
  echo.
  echo FAILED with exit code %ec%.
  pause
)
exit /b %ec%
