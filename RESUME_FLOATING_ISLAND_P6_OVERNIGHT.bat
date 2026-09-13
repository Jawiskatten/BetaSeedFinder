@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0RUN_FLOATING_ISLAND_P6_OVERNIGHT.ps1" -Resume
pause
