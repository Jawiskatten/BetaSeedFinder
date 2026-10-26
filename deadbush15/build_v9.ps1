# Builds and calibrates the NORMAL-world 26.3 approximate-terrain ranker.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$msys = 'C:\msys64\ucrt64\bin'
$env:PATH = "$msys;$env:PATH"
$compiler = Join-Path $msys 'g++.exe'
$vendor = Join-Path $PSScriptRoot 'vendor\cubiomes'
$lib = Join-Path $vendor 'build-d15\libcubiomes_static.a'
$src = Join-Path $PSScriptRoot 'terrain_rank_v9.cpp'
$exe = Join-Path $PSScriptRoot 'terrain_rank_v9.exe'
if (!(Test-Path $compiler)) { throw 'GCC missing. Install MSYS2 UCRT64 GCC.' }
if (!(Test-Path $lib)) { throw 'Cubiomes static library missing. Build V7 first: powershell -ExecutionPolicy Bypass -File .\build_v7.ps1' }
& $compiler -O3 -std=c++17 -fwrapv -I $vendor $src $lib -lm -o $exe
if ($LASTEXITCODE -ne 0) { throw "V9 compile failed (exit $LASTEXITCODE)" }
& $exe --self-test
if ($LASTEXITCODE -ne 0) { throw "V9 RNG calibration failed (exit $LASTEXITCODE)" }
Write-Host 'V9 built; reference RNG checks passed. Approximate terrain model is not exact.'