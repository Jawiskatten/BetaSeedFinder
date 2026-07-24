@echo off
setlocal
cd /d "%~dp0"

if not exist config mkdir config >nul 2>nul
if not exist config\gpu_backend.properties (
  > config\gpu_backend.properties echo # backend=auto ^| amd ^| nvidia ^| legacy
  >> config\gpu_backend.properties echo backend=auto
)

echo Checking project files...
call compile_project.bat
if errorlevel 1 (
    pause
    exit /b 1
)

rem P59 safety check: compile the backend doctor explicitly if an older bin folder
rem or compile script left it out.
if not exist "bin\GpuBackendDoctor.class" (
    echo Repairing missing GPU backend classes...
    javac -sourcepath src -d bin src\GpuBackendKind.java src\GpuBackendLocator.java src\GpuBackendDoctor.java
    if errorlevel 1 (
        echo Could not compile GPU backend classes.
        pause
        exit /b 1
    )
)

echo.
echo Resolving GPU backend...
java -cp bin GpuBackendDoctor > "%TEMP%\bsf_gpu_backend.txt" 2>&1
if errorlevel 1 (
    type "%TEMP%\bsf_gpu_backend.txt"
    echo.
    echo No usable GPU backend was found.
    echo Put an AMD worker at backend\amd\gpu_p20_benchmark.exe
    echo or a NVIDIA worker at backend\nvidia\gpu_p20_benchmark.exe
    pause
    exit /b 1
)
type "%TEMP%\bsf_gpu_backend.txt"

echo.
echo Starting GUI...
java -cp bin GuiMain

echo.
pause
