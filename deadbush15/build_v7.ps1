# Build pinned Minecraft 26.3 Cubiomes and Deadbush15 V7 on Windows.
# Run this PowerShell script from any working directory.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$msysBin = 'C:\msys64\ucrt64\bin'
$bash = 'C:\msys64\usr\bin\bash.exe'
$gcc = Join-Path $msysBin 'gcc.exe'
$gpp = Join-Path $msysBin 'g++.exe'
$cmake = Join-Path $msysBin 'cmake.exe'
$ninja = Join-Path $msysBin 'ninja.exe'

if (!(Test-Path $bash)) { throw 'MSYS2 not found at C:\msys64. Install MSYS2 first.' }
$env:PATH = "$msysBin;$env:PATH"

# Check exact UCRT64 paths, not whichever cmake/ninja is first on PATH.
$missingPackages = @()
if (!(Test-Path $gcc) -or !(Test-Path $gpp)) { $missingPackages += 'mingw-w64-ucrt-x86_64-gcc' }
if (!(Test-Path $cmake)) { $missingPackages += 'mingw-w64-ucrt-x86_64-cmake' }
if (!(Test-Path $ninja)) { $missingPackages += 'mingw-w64-ucrt-x86_64-ninja' }
if ($missingPackages.Count -gt 0) {
    Write-Host "Installing MSYS2 packages: $($missingPackages -join ', ')"
    & $bash -lc ('pacman -S --needed ' + ($missingPackages -join ' '))
    if ($LASTEXITCODE -ne 0) { throw 'Installing required MSYS2 packages failed' }
}
foreach ($exe in @($gcc, $gpp, $cmake, $ninja)) {
    if (!(Test-Path $exe)) { throw "Required MSYS2 executable missing: $exe" }
}

$vendor = Join-Path $PSScriptRoot 'vendor\cubiomes'
if (!(Test-Path (Join-Path $vendor '.git'))) {
    New-Item -ItemType Directory -Force (Split-Path $vendor) | Out-Null
    git clone https://github.com/xpple/cubiomes.git $vendor
    if ($LASTEXITCODE -ne 0) { throw 'Cloning Cubiomes failed' }
}
git -C $vendor checkout --detach b99f6e472018cab903a096697117aab9d9170a19
if ($LASTEXITCODE -ne 0) { throw 'Pinned Cubiomes revision unavailable' }

$build = Join-Path $vendor 'build-d15'
& $cmake -S $vendor -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$gcc" "-DCMAKE_CXX_COMPILER=$gpp" "-DCMAKE_MAKE_PROGRAM=$ninja" '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -fwrapv'
if ($LASTEXITCODE -ne 0) { throw 'Cubiomes configuration failed' }

& $cmake --build $build --target cubiomes_static --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Cubiomes compilation failed' }

$lib = Join-Path $build 'libcubiomes_static.a'
if (!(Test-Path $lib)) { throw "Static archive absent at $lib" }
# Cubiomes rng.h declares global lerp(), which conflicts with std::lerp in C++20.
# This filter uses only C++17 features, so compile against the C API as C++17.
& $gpp -O3 -std=c++17 -fwrapv -I $vendor (Join-Path $PSScriptRoot 'badlands_filter_v7.cpp') $lib -lm -o (Join-Path $PSScriptRoot 'badlands_filter_v7.exe')
if ($LASTEXITCODE -ne 0) { throw 'Biome filter compilation failed' }

& (Join-Path $PSScriptRoot 'badlands_filter_v7.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw 'Published reference seed calibration FAILED; do not run biome filtering' }
Write-Host 'Deadbush15 V7 build and calibration passed.'
