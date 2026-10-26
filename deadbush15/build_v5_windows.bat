@echo off
setlocal
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
where g++.exe >nul 2>nul || (echo ERROR: Install MSYS2 UCRT64 GCC first. & exit /b 1)
g++ -O3 -march=native -std=c++20 -pthread deadbush15_v5.cpp -o deadbush15_v5.exe || exit /b 1
deadbush15_v5.exe --self-test
exit /b %ERRORLEVEL%
