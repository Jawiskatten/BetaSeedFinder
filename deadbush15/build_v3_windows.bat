@echo off
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where g++ >nul 2>&1
if errorlevel 1 (
 echo ERROR: GCC missing. Install MSYS2 UCRT64 GCC.
 exit /b 1
)
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v3.cpp -o deadbush15_v3.exe
if errorlevel 1 (
 echo ERROR: compiler failed.
 exit /b 1
)
echo Built deadbush15_v3.exe
deadbush15_v3.exe --gpu-self-test
