@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "scripts\Verify-GithubSource.ps1" -ProjectRoot "%CD%"
if errorlevel 1 (
  echo.
  echo PUBLIC SOURCE COMPONENT VERIFICATION FAILED.
  pause
  exit /b 1
)
echo.
echo Public source components verified.
echo The GitHub packager and CI perform an additional strict clean-tree check.
pause
