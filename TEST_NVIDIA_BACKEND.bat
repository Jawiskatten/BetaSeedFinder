@echo off
setlocal
cd /d "%~dp0"
set "EXE=backend\nvidia\gpu_p20_benchmark.exe"
set "DATA=gpu_p20_benchmark\data"
set "LOG=backend\nvidia\nvidia_exactness_results.txt"

if not exist "%EXE%" (
  echo ERROR: %EXE% does not exist. Run BUILD_NVIDIA_BACKEND.bat first.
  pause
  exit /b 1
)

> "%LOG%" echo BetaSeedFinder NVIDIA validation
>> "%LOG%" echo =================================

echo [1/5] P20 exactness...
"%EXE%" validate "%DATA%\gpu_p20_reference_1000.bin" >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

echo [2/5] Optimized Stage0 exactness...
"%EXE%" stage0optvalidate "%DATA%\gpu_stage0_reference_1000.bin" >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

echo [3/5] P19 exactness...
"%EXE%" p19validate "%DATA%\gpu_p19_reference_128.bin" >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

echo [4/5] Coarse exactness...
"%EXE%" coarsevalidate "%DATA%\gpu_coarse_reference_64.bin" >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

echo [5/5] MEGA production benchmark...
"%EXE%" pipelineprofile 32768 123456789 3 mega full >> "%LOG%" 2>&1
if errorlevel 1 goto :failed

> backend\nvidia\NVIDIA_BACKEND_VALIDATED.txt echo Validated on %DATE% %TIME%
echo.
echo ALL NVIDIA TESTS PASSED.
echo Results: %LOG%
pause
exit /b 0

:failed
echo.
echo NVIDIA VALIDATION FAILED.
echo Read: %LOG%
pause
exit /b 1
