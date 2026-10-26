# Build the pinned 26.3 Cubiomes library and the native Deadbush15 V7 biome gate.
# PowerShell, run inside the deadbush15 project directory.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
if (!(Test-Path 'C:\msys64\usr\bin\bash.exe')) { throw "MSYS2 not found at C:\msys64" }
if (!(Get-Command cmake -ErrorAction SilentlyContinue) -or !(Get-Command ninja -ErrorAction SilentlyContinue)) {
    & 'C:\msys64\usr\bin\bash.exe' -lc 'pacman -S --needed mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja'
    if ($LASTEXITCODE -ne 0) { throw 'Installing CMake/Ninja failed' }
}
$vendor = Join-Path $PSScriptRoot 'vendor\cubiomes'
if (!(Test-Path (Join-Path $vendor '.git'))) {
    New-Item -ItemType Directory -Force (Split-Path $vendor) | Out-Null
    git clone https://github.com/xpple/cubiomes.git $vendor
    if ($LASTEXITCODE -ne 0) { throw 'Cloning cubiomes failed' }
}
git -C $vendor checkout --detach b99f6e472018cab903a096697117aab9d9170a19
if ($LASTEXITCODE -ne 0) { throw 'Pinned cubiomes revision unavailable' }
$build = Join-Path $vendor 'build-d15'
cmake -S $vendor -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -fwrapv'
if ($LASTEXITCODE -ne 0) { throw 'Cubiomes configuration failed' }
cmake --build $build --target cubiomes_static --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Cubiomes compilation failed' }
$lib = Join-Path $build 'libcubiomes_static.a'
if (!(Test-Path $lib)) { throw "Static archive absent at $lib" }
& 'C:\msys64\ucrt64\bin\g++.exe' -O3 -std=c++20 -fwrapv -I $vendor (Join-Path $PSScriptRoot 'badlands_filter_v7.cpp') $lib -lm -o (Join-Path $PSScriptRoot 'badlands_filter_v7.exe')
if ($LASTEXITCODE -ne 0) { throw 'Biome filter compilation failed' }
& (Join-Path $PSScriptRoot 'badlands_filter_v7.exe') --self-test
if ($LASTEXITCODE -ne 0) { throw 'Published reference seed calibration FAILED; do not run biome filtering' }
