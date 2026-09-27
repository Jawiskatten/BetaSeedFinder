param(
    [long]$Start = 0,
    [long]$Count = 1000000,
    [int]$Threads = 16,
    [int]$ChunkX = 0,
    [int]$ChunkZ = 0,
    [int]$MinDungeons = 2,
    [int]$Top = 20,
    [string]$Csv = "out\dungeon_cluster_results.csv",
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

if ($Rebuild -or -not (Test-Path 'build\java\classes\beta173\DungeonClusterFinder173.class')) {
    & (Join-Path $PSScriptRoot 'build-java.ps1') -ProjectRoot $root | Out-Null
}

$argsList = @(
    '-cp', 'build/java/classes',
    'beta173.DungeonClusterFinder173',
    '--start', $Start,
    '--count', $Count,
    '--threads', $Threads,
    '--chunk-x', $ChunkX,
    '--chunk-z', $ChunkZ,
    '--min-dungeons', $MinDungeons,
    '--top', $Top,
    '--csv', $Csv
)

Write-Host "Dungeon cluster search: start=$Start count=$Count threads=$Threads populationChunk=($ChunkX,$ChunkZ)" -ForegroundColor Cyan
& java @argsList
if ($LASTEXITCODE -ne 0) {
    throw "DungeonClusterFinder173 exited with code $LASTEXITCODE"
}
