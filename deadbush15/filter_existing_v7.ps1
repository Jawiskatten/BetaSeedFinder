# Reuse the existing 100M-seed GPU search output without rescanning.
$ErrorActionPreference='Stop'
Set-Location $PSScriptRoot
if (!(Test-Path .\gpu_v4_candidates.csv)){throw 'Missing gpu_v4_candidates.csv'}
if (!(Test-Path .\badlands_filter_v7.exe)){throw 'Build V7 first: powershell -ExecutionPolicy Bypass -File .\build_v7.ps1'}
.\badlands_filter_v7.exe --self-test
if ($LASTEXITCODE -ne 0){throw 'Biome calibration failed'}
.\badlands_filter_v7.exe --input gpu_v4_candidates.csv --output v7_badlands.csv --rejects v7_rejected.csv
if ($LASTEXITCODE -ne 0){throw 'Biome filter failed'}
if (!(Test-Path .\deadbush15_v5.exe)){throw 'Need V5 ranking program (deadbush15_v5.exe)'}
.\deadbush15_v5.exe --rank --csv v7_badlands.csv --min-run 15 --top 30 --output v7_shortlist.csv
if ($LASTEXITCODE -ne 0){throw 'Ranker failed'}
Import-Csv .\v7_shortlist.csv | Select-Object -First 10 | Format-Table -AutoSize
