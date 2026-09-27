param(
    [Parameter(Mandatory = $true)]
    [long]$Seed,
    [int]$ChunkX = 0,
    [int]$ChunkZ = 0,
    [ValidateRange(0, 4)]
    [int]$ScanChunks = 0,
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
    '--seed', $Seed,
    '--chunk-x', $ChunkX,
    '--chunk-z', $ChunkZ
)

if ($ScanChunks -gt 0) {
    $argsList += @('--scan-chunks', $ScanChunks)
}

Write-Host "Dungeon parity trace: seed=$Seed populationChunk=($ChunkX,$ChunkZ) scanRadius=$ScanChunks" -ForegroundColor Cyan
Write-Host "NOTE: isolated population model; use this trace to locate the first vanilla parity divergence." -ForegroundColor Yellow
& java @argsList
if ($LASTEXITCODE -ne 0) {
    throw "DungeonClusterFinder173 parity trace exited with code $LASTEXITCODE"
}
