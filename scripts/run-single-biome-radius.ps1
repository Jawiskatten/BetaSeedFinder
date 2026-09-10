param(
    [int]$Target = 432,
    [int]$Batch = 16384,
    [int]$TopExact = 1,
    [int]$CenterX = 0,
    [int]$CenterZ = 0,
    [UInt64]$StartAttempt = 0,
    [UInt64]$MaxAttempts = 0,
    [UInt64]$Sequence = 0,
    [switch]$UseSequence,
    [switch]$Rebuild,
    [switch]$ContinueAfterHit
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\native\amd\SingleBiomeRadiusFinder.exe'

if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-single-biome-radius.ps1') -ProjectRoot $root
}

$argsList = @(
    '--target', $Target,
    '--batch', $Batch,
    '--top-exact', $TopExact,
    '--center-x', $CenterX,
    '--center-z', $CenterZ,
    '--start-attempt', $StartAttempt,
    '--max-attempts', $MaxAttempts
)
if ($UseSequence) {
    $argsList += @('--sequence', $Sequence)
}
if ($ContinueAfterHit) {
    $argsList += '--continue-after-hit'
}

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
