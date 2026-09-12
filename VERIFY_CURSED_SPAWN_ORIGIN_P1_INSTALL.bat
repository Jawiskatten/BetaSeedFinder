@echo off
setlocal
cd /d "%~dp0"
set "FAIL=0"
for %%F in (
  "native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp"
  "scripts\cursed-spawn-origin-p1-common.ps1"
  "scripts\run-cursed-spawn-origin-p1-amd.ps1"
  "scripts\resume-cursed-spawn-origin-p1-amd.ps1"
  "RUN_CURSED_SPAWN_ORIGIN_P1_AMD.bat"
) do (
  if not exist "%%~F" (
    echo MISSING: %%~F
    set "FAIL=1"
  )
)
findstr /l /c:"SPAWN_X=0" "native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp" >nul || set "FAIL=1"
findstr /l /c:"originSandNoise[seedIndex]" "scripts\cursed-spawn-origin-p1-common.ps1" >nul || set "FAIL=1"
findstr /l /c:"originStoneNoise[seedIndex]" "scripts\cursed-spawn-origin-p1-common.ps1" >nul || set "FAIL=1"
if "%FAIL%"=="1" (
  echo CursedSpawnOrigin P1 install verification FAILED.
  exit /b 1
)
echo CursedSpawnOrigin P1 install verification PASS.
exit /b 0
