@echo off
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: MSYS2 UCRT64 g++ is missing. Install it first.
  exit /b 1
)
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v2.cpp -o deadbush15_v2.exe
if errorlevel 1 exit /b 1
deadbush15_v2.exe --self-test
if errorlevel 1 exit /b 1
echo Deadbush15 v2 compiled and validated.
