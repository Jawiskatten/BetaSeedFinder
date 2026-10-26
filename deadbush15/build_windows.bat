@echo off
where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: g++ not installed. Install MSYS2 MinGW-w64 GCC, then run from its UCRT64 shell.
  exit /b 1
)
g++ -O3 -march=native -std=c++20 -pthread deadbush15.cpp -o deadbush15.exe
if errorlevel 1 exit /b 1
echo Built deadbush15.exe
