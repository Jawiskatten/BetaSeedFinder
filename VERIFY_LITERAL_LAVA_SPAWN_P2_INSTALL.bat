@echo off
setlocal
cd /d "%~dp0"
set "FAIL=0"
for %%F in (
  "native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp"
  "scripts\cursed-spawn-origin-p1-common.ps1"
  "scripts\run-cursed-spawn-origin-p1-amd.ps1"
  "RUN_LITERAL_LAVA_SPAWN_P2_AMD.bat"
) do (
  if not exist "%%~F" (
    echo MISSING: %%~F
    set "FAIL=1"
  )
)
findstr /l /c:"PLAYER_FEET_Y=65" "native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp" >nul || set "FAIL=1"
findstr /l /c:"scoreLavaSpawnOriginKernel" "native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp" >nul || set "FAIL=1"
if "%FAIL%"=="1" (
  echo Literal Lava Spawn P2 install verification FAILED.
  exit /b 1
)
echo Literal Lava Spawn P2 install verification PASS.
exit /b 0
