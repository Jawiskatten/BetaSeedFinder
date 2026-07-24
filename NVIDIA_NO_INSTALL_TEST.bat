@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\Run-NvidiaTester.ps1"
if errorlevel 1 (
  echo.
  echo NVIDIA TEST FAILED.
  pause
  exit /b 1
)
echo.
echo Test complete. Send the ZIP from the return folder to the project owner.
pause
