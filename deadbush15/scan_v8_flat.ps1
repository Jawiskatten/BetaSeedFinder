# Deadbush15 V8: GPU flat-pattern scout -> normal-world Badlands filter -> shortlist.
# Experimental optimization of candidate quality (NOT a faster GPU kernel or full terrain verifier).
param(
    [long]$Start = 300000000,
    [long]$Count = 2000000,
    [int]$Locations = 4,
    [int]$Report = 7
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
if ($Start -lt 0 -or $Count -lt 1 -or $Locations -lt 1 -or $Locations -gt 16 -or $Report -lt 1 -or $Report -gt 15) {
    throw 'Invalid options; use Start>=0, Count>=1, Locations=1..16 and Report=1..15.'
}
foreach ($exe in @('deadbush15_v4.exe','badlands_filter_v7.exe','deadbush15_v5.exe')) {
    if (!(Test-Path (Join-Path $PSScriptRoot $exe))) { throw "Missing $exe; build existing V4, V7 and V5 tools first" }
}
$prefix = "v8_flat_s${Start}_n${Count}_l${Locations}_r${Report}"
$raw = "$prefix.csv"
$checkpoint = "$prefix.progress"
$filtered = "$prefix.badlands.csv"
$rejected = "$prefix.rejected.csv"
$shortlist = "$prefix.shortlist.csv"

Write-Host 'Verifying GPU/CPU decoration RNG parity with known normal-world 14-bush reference...'
& .\deadbush15_v4.exe --gpu-self-test
if ($LASTEXITCODE -ne 0) { throw "GPU parity self-test failed (exit $LASTEXITCODE)" }
Write-Host "Scanning $Count seeds, $Locations positions/seed in FLAT mode (not XZ)..."
$gpuArgs = @('--gpu-search','--start',[string]$Start,'--count',[string]$Count,
  '--batch','8192','--locations',[string]$Locations,
  '--spread-span','1000000','--mode','flat','--prefilter',[string]$Report,
  '--report',[string]$Report,'--target','15','--output',$raw,'--checkpoint',$checkpoint)
if (Test-Path $checkpoint) {
    Write-Host 'Resuming matching checkpoint if one exists...'
    $gpuArgs += '--resume'
}
& .\deadbush15_v4.exe @gpuArgs
if ($LASTEXITCODE -ne 0) { throw "GPU flat search failed (exit $LASTEXITCODE)" }

Write-Host 'Filtering reported flat-compatible rows by NORMAL-world Badlands biomes...'
& .\badlands_filter_v7.exe --input $raw --output $filtered --rejects $rejected
if ($LASTEXITCODE -ne 0) { throw "Badlands filter failed (exit $LASTEXITCODE)" }
Write-Host 'Ranking actual reported flat rows (still NOT terrain verified)...'
& .\deadbush15_v5.exe --rank --csv $filtered --min-run $Report --top 30 --output $shortlist
if ($LASTEXITCODE -ne 0) { throw "V5 ranker failed (exit $LASTEXITCODE)" }

Import-Csv $shortlist | Select-Object -First 10 | Format-Table -AutoSize
Write-Host "Raw=$raw"
Write-Host "Biome-compatible=$filtered"
Write-Host "Ranked=$shortlist"
Write-Host 'All hits are RNG and biome candidates, NOT world-generated bushes.'
