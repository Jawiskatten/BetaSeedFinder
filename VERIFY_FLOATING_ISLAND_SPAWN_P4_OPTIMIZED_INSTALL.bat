@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-floating-island-spawn-p4-optimized-amd.ps1" -SelfTest
set "EC=%ERRORLEVEL%"
echo.
if "%EC%"=="0" echo P4 OPTIMIZED VERIFY OK.
if not "%EC%"=="0" echo P4 OPTIMIZED VERIFY FAILED with exit code %EC%.
pause
exit /b %EC%
