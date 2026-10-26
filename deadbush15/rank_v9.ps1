param(
    [string]$InputCsv = 'v7_badlands.csv',
    [string]$OutputCsv = 'v9_terrain_ranked.csv',
    [int]$MinRun = 7,
    [int]$Top = 30
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
if (!(Test-Path .\terrain_rank_v9.exe)) { throw 'Build V9 first using build_v9.ps1' }
if (!(Test-Path $InputCsv)) { throw "Input not found: $InputCsv" }
& .\terrain_rank_v9.exe --input $InputCsv --output $OutputCsv --min-run $MinRun --top $Top
if ($LASTEXITCODE -ne 0) { throw "V9 ranking failed (exit $LASTEXITCODE)" }
Import-Csv $OutputCsv | Select-Object -First 15 | Format-Table -AutoSize
Write-Host "Results: $OutputCsv"
Write-Host 'These are approximate height-ranked patterns, not verified dead bushes.'