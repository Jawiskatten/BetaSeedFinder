@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\Create-GithubSource.ps1" -ProjectRoot "%CD%"
if errorlevel 1 (
  echo.
  echo GITHUB SOURCE PACKAGE CREATION FAILED.
  pause
  exit /b 1
)
echo.
echo GitHub source package created successfully.
pause
