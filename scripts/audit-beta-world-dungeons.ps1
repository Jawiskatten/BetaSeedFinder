param(
    [Parameter(Mandatory = $true)]
    [string]$WorldPath,
    [long]$Seed = 501789,
    [int]$ChunkRadius = 3,
    [int]$CenterChunkX = 0,
    [int]$CenterChunkZ = 0,
    [int]$PopulationChunkX = 0,
    [int]$PopulationChunkZ = 0,
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

if ($Rebuild -or -not (Test-Path 'build\java\classes\beta173\BetaWorldDungeonAudit173.class')) {
    & (Join-Path $PSScriptRoot 'build-java.ps1') -ProjectRoot $root | Out-Null
}

$argsList = @(
    '-cp', 'build/java/classes',
    'beta173.BetaWorldDungeonAudit173',
    '--world', $WorldPath,
    '--seed', $Seed,
    '--chunk-radius', $ChunkRadius,
    '--center-chunk-x', $CenterChunkX,
    '--center-chunk-z', $CenterChunkZ,
    '--pop-chunk-x', $PopulationChunkX,
    '--pop-chunk-z', $PopulationChunkZ
)

& java @argsList
if ($LASTEXITCODE -ne 0) {
    throw "BetaWorldDungeonAudit173 exited with code $LASTEXITCODE"
}
