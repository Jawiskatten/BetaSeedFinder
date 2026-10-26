# Reuse existing GPU candidates; -RankOnly ranks the already filtered V7 output.
param([switch]$RankOnly)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
# The V5 executable is dynamically linked to MSYS2 UCRT64 runtime libraries.
# PowerShell launched via powershell -File does not inherit a PATH set in another window.
$ucrtBin = 'C:\msys64\ucrt64\bin'
$env:PATH = "$ucrtBin;$env:PATH"

if (-not $RankOnly) {
    if (!(Test-Path .\gpu_v4_candidates.csv)) { throw 'Missing gpu_v4_candidates.csv' }
    if (!(Test-Path .\badlands_filter_v7.exe)) { throw 'Build V7 first using build_v7.ps1' }
    .\badlands_filter_v7.exe --self-test
    if ($LASTEXITCODE -ne 0) { throw "Biome calibration failed (exit $LASTEXITCODE)" }
    .\badlands_filter_v7.exe --input gpu_v4_candidates.csv --output v7_badlands.csv --rejects v7_rejected.csv
    if ($LASTEXITCODE -ne 0) { throw "Biome filter failed (exit $LASTEXITCODE)" }
}

if (!(Test-Path .\v7_badlands.csv)) { throw 'Missing v7_badlands.csv; run filter_existing_v7.ps1 without -RankOnly first' }
$ranker = Join-Path $PSScriptRoot 'deadbush15_v5.exe'
if (!(Test-Path $ranker)) {
    $compiler = Join-Path $ucrtBin 'g++.exe'
    if (!(Test-Path $compiler)) { throw 'Missing MSYS2 UCRT64 C++ compiler' }
    & $compiler -O3 -march=native -std=c++20 -pthread deadbush15_v5.cpp -o $ranker
    if ($LASTEXITCODE -ne 0) { throw "V5 ranker compilation failed (exit $LASTEXITCODE)" }
}
& $ranker --rank --csv v7_badlands.csv --min-run 15 --top 30 --output v7_shortlist.csv
$code = $LASTEXITCODE
if ($code -ne 0) { throw "Ranker failed (exit $code). Try running $ranker --self-test in this shell." }
if (!(Test-Path .\v7_shortlist.csv)) { throw 'Ranker did not produce v7_shortlist.csv' }
Import-Csv .\v7_shortlist.csv | Select-Object -First 10 | Format-Table -AutoSize
