@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "gpu_p20_benchmark\Build-NvidiaBackend.ps1"
if errorlevel 1 (
  echo.
  echo NVIDIA BACKEND BUILD FAILED.
  pause
  exit /b 1
)
echo.
echo NVIDIA BACKEND BUILD COMPLETE.
pause
