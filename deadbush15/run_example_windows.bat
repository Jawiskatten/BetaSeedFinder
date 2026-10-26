@echo off
if not exist deadbush15.exe call build_windows.bat
if not exist deadbush15.exe exit /b 1
rem Reference index 75; run a reproducible first batch for 15 bushes.
deadbush15.exe --self-test
if errorlevel 1 (
  echo ERROR: Reference self-test failed, search aborted.
  pause
  exit /b 1
)
deadbush15.exe --search --start 0 --count 1000000 --index 75 --threads 8 --radius 1 --mode flat --report 8 --target 15 --output candidates.csv
pause
