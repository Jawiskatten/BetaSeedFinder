@echo off
setlocal
cd /d "%~dp0"

if not exist "bin\GuiMain.class" (
    echo ERROR: BetaSeedFinder runtime files are incomplete.
    echo Missing: bin\GuiMain.class
    pause
    exit /b 1
)

if not exist config mkdir config >nul 2>nul
if not exist config\gpu_backend.properties (
    > config\gpu_backend.properties echo # backend=auto ^| amd ^| nvidia ^| legacy
    >> config\gpu_backend.properties echo backend=auto
)

java -cp bin GpuBackendDoctor
if errorlevel 1 (
    echo.
    echo BetaSeedFinder could not find a usable AMD or NVIDIA worker.
    pause
    exit /b 1
)

echo.
java -cp bin GuiMain
if errorlevel 1 (
    echo.
    echo BetaSeedFinder closed with an error.
    pause
    exit /b 1
)
