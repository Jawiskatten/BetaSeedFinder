param(
    [int]$Batch = 0,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,10000)][int]$Top = 100
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $ProjectRoot 'out\lava_spawn_origin_p2'
$lastFile = Join-Path $outputRoot 'LAST_RUN.txt'
if (-not (Test-Path $lastFile)) { throw 'No LavaSpawnOrigin P2 LAST_RUN.txt found.' }
$last = (Get-Content $lastFile -Raw).Trim()
$checkpoint = Join-Path $last 'checkpoint.txt'
if (-not (Test-Path $checkpoint)) { throw "No checkpoint.txt found in $last" }
$data = @{}
foreach ($line in Get-Content $checkpoint) {
    $parts = $line -split '=', 2
    if ($parts.Count -eq 2) { $data[$parts[0]] = $parts[1] }
}
foreach ($key in @('RADIUS','NEXT_INDEX','COUNT','COMPLETED','RANDOM_KEY','SEED_MODE')) {
    if (-not $data.ContainsKey($key)) { throw "Checkpoint missing $key" }
}
$remaining = [UInt64]$data['COUNT'] - [UInt64]$data['COMPLETED']
if ($remaining -eq 0) { Write-Host 'Last run is already complete.'; exit 0 }
$params = @{
    ExistingOutput = $last
    Radius = [int]$data['RADIUS']
    StartIndex = [UInt64]$data['NEXT_INDEX']
    Count = $remaining
    RandomKey = [UInt64]$data['RANDOM_KEY']
    SeedMode = [string]$data['SEED_MODE']
    TerrainThreads = $TerrainThreads
    Top = $Top
}
if ($Batch -gt 0) { $params['Batch'] = $Batch }
Write-Host ("Resuming same run after {0} checked seeds; {1} remain in this segment." -f $data['COMPLETED'], $remaining)
& (Join-Path $PSScriptRoot 'run-cursed-spawn-origin-p1-amd.ps1') @params
