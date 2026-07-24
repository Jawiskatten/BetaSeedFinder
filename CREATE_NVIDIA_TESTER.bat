@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\Create-NvidiaTester.ps1" -ProjectRoot "%CD%"
if errorlevel 1 (
  echo.
  echo NVIDIA TESTER PACKAGE CREATION FAILED.
  pause
  exit /b 1
)
echo.
echo NVIDIA tester created successfully.
pause
