@echo off
setlocal
cd /d "%~dp0"
call "gpu_p20_benchmark\build_amd_multiarch.bat"
if errorlevel 1 (
  echo.
  echo AMD BACKEND BUILD FAILED.
  pause
  exit /b 1
)
echo.
echo AMD BACKEND BUILD COMPLETE.
pause
