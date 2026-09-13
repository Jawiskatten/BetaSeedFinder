@echo off
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File ".\RUN_DUNGEON_SPAWN_P1.ps1"
pause
