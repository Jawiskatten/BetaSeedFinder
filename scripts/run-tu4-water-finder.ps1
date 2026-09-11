param(
    [int]$Batch = 131072,
    [int]$TopExact = 16,
    [UInt64]$StartAttempt = 0,
    [UInt64]$MaxAttempts = 0,
    [UInt64]$Sequence = 0,
    [switch]$UseSequence,
    [switch]$Rebuild,
    [switch]$ContinueAfterHit,
    [Nullable[long]]$VerifySeed = $null
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build\native\amd\TU4WaterFinder.exe'

if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-tu4-water-finder.ps1') -ProjectRoot $root
}

$argsList = @(
    '--batch', $Batch,
    '--top-exact', $TopExact,
    '--start-attempt', $StartAttempt,
    '--max-attempts', $MaxAttempts
)
if ($UseSequence) {
    $argsList += @('--sequence', $Sequence)
}
if ($ContinueAfterHit) {
    $argsList += '--continue-after-hit'
}
if ($null -ne $VerifySeed) {
    $argsList += @('--verify-seed', $VerifySeed.Value)
}

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
