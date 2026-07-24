@echo off
setlocal

set "ROCM_DIR="
if defined HIP_SDK_DIR set "ROCM_DIR=%HIP_SDK_DIR%"
if not defined ROCM_DIR (
  for /d %%D in ("C:\Program Files\AMD\ROCm\7.*") do (
    if exist "%%~fD\bin\hipcc.exe" set "ROCM_DIR=%%~fD"
  )
)
if not defined ROCM_DIR set "ROCM_DIR=C:\Program Files\AMD\ROCm\7.1"

if not exist "%ROCM_DIR%\bin\hipcc.exe" (
  echo ERROR: hipcc.exe not found. Install AMD HIP SDK or set HIP_SDK_DIR.
  exit /b 1
)

set "PATH=%ROCM_DIR%\bin;%PATH%"
set "PROJECT_ROOT=%~dp0.."
set "OUT=%PROJECT_ROOT%\backend\amd"
if not exist "%OUT%" mkdir "%OUT%"

echo Building AMD multi-architecture worker...
echo Targets: gfx1030 gfx1031 gfx1032 gfx1100 gfx1101 gfx1102 gfx1200 gfx1201

pushd "%~dp0"
hipcc native\gpu_p20_benchmark.cpp -O3 -std=c++17 ^
  --offload-arch=gfx1030 --offload-arch=gfx1031 --offload-arch=gfx1032 ^
  --offload-arch=gfx1100 --offload-arch=gfx1101 --offload-arch=gfx1102 ^
  --offload-arch=gfx1200 --offload-arch=gfx1201 ^
  -ffp-contract=off -fno-fast-math -fno-associative-math ^
  -o "%OUT%\gpu_p20_benchmark.exe"
if errorlevel 1 (
  popd
  exit /b 1
)
popd

echo.
echo Built: %OUT%\gpu_p20_benchmark.exe
