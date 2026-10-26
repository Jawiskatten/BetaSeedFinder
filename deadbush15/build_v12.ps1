$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Build the pinned Cubiomes static library first if it is not ready.
$lib = Join-Path $PSScriptRoot 'vendor\cubiomes\build-d15\libcubiomes_static.a'
if (!(Test-Path $lib)) {
    & (Join-Path $PSScriptRoot 'build_v7.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Pinned Cubiomes build failed' }
}
$gpp = 'C:\msys64\ucrt64\bin\g++.exe'
if (!(Test-Path $gpp)) { throw "MSYS2 UCRT64 g++ not found: $gpp" }
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
& $gpp -O3 -march=native -std=c++17 -fwrapv -static -static-libgcc -static-libstdc++ -I (Join-Path $PSScriptRoot 'vendor') `
    (Join-Path $PSScriptRoot 'deadbush15_v12.cpp') $lib -lm `
    -o (Join-Path $PSScriptRoot 'deadbush15_v12.exe')
if ($LASTEXITCODE -ne 0) { throw 'V12 compilation failed' }
Write-Host 'Built deadbush15_v12.exe'
