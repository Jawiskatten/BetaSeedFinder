# V11: exhaust the existing Badlands-compatible XZ >= 15 candidate pool
# using actual vanilla Java 26.3 generated chunks. No manual seed visits.
param(
    [switch]$AcceptEula,
    [int]$MaxSeeds = 100,
    [int]$MinRun = 15,
    [string]$ExcludeSeeds = "124767051,178096580"
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $AcceptEula) {
    throw "Read and agree to https://aka.ms/MinecraftEULA, then rerun with -AcceptEula."
}
if ($MaxSeeds -lt 1 -or $MinRun -lt 1 -or $MinRun -gt 64) {
    throw "Invalid MaxSeeds or MinRun."
}
if (!(Test-Path .\terrain_rank_v9.exe)) { throw 'Run build_v9.ps1 first.' }
if (!(Test-Path .\v7_badlands.csv)) { throw 'Missing v7_badlands.csv; no GPU re-scan will be performed.' }
if (!(Test-Path .\auto_verify_v10.py)) { throw 'Pull the newest deadbush15 branch first.' }
if (!(Test-Path .\scan_real_world_v11.py)) { throw 'Pull V11 full-chunk scanner first.' }

$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$ranked = 'v11_all_xz15_ranked.csv'
Write-Host "Re-ranking existing V7 biome-compatible XZ rows, no RNG scan..."
& .\terrain_rank_v9.exe --input v7_badlands.csv --output $ranked --min-run $MinRun --top 10000
if ($LASTEXITCODE -ne 0) { throw "Ranking failed, exit $LASTEXITCODE" }
$potential = @(Import-Csv $ranked)
Write-Host ("Candidate rows found: " + $potential.Count)

Write-Host "Running actual 26.3 worldgen with NO approximate-terrain rejection..."
Write-Host "Each world is read for ALL generated dead-bush rows, even off the guessed line."
Write-Host "Already completed rows in v10_verified.csv are automatically skipped."
Write-Host "Manually rejected seeds skipped: $ExcludeSeeds"
$verifyArgs = @(
    '--csv', $ranked, '--output', 'v10_verified.csv',
    '--download-server', '--accept-eula',
    '--min-run', [string]$MinRun, '--min-strict', '0',
    '--max-seeds', [string]$MaxSeeds,
    '--exclude-seeds', $ExcludeSeeds,
    '--target', '15'
)
& py -3 auto_verify_v10.py @verifyArgs
if ($LASTEXITCODE -ne 0) { throw "Actual-world verification failed, exit $LASTEXITCODE" }
Write-Host ""
Write-Host "Actual generated-world results:"
if (Test-Path .\v10_verified.csv) {
    Import-Csv .\v10_verified.csv |
        Sort-Object { [int]($_.actual_world_best_run) } -Descending |
        Select-Object -First 15 seed_signed, run, world_verification, actual_badlands_same_y, actual_world_best_run, actual_world_best_x, actual_world_best_y, actual_world_best_z |
        Format-Table -AutoSize
}
Write-Host "Complete: v10_verified.csv (only actual saved-block scores count)."
