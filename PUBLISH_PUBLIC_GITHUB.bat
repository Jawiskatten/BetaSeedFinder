@echo off
setlocal
cd /d "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\Publish-PublicGitHub.ps1" -ProjectRoot "%CD%"

if errorlevel 1 (
    echo.
    echo PUBLIC GITHUB PUBLISH FAILED OR WAS CANCELLED.
    pause
    exit /b 1
)

echo.
echo PUBLIC GITHUB REPOSITORY PUBLISHED OR UPDATED.
pause
