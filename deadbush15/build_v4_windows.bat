@echo off
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where g++ >nul 2>&1
if errorlevel 1 (
  echo Install MSYS2 UCRT64 GCC first. See README_V4.md
  exit /b 1
)
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v4.cpp -o deadbush15_v4.exe
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo Build succeeded: deadbush15_v4.exe
