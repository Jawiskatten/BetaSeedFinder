param(
    [int]$Batch = 131072,
    [int]$TopExact = 24,
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
    '--batch', [string]$Batch,
    '--top-exact', [string]$TopExact,
    '--start-attempt', [string]$StartAttempt,
    '--max-attempts', [string]$MaxAttempts
)
if ($UseSequence) {
    $argsList += @('--sequence', [string]$Sequence)
}
if ($ContinueAfterHit) {
    $argsList += '--continue-after-hit'
}
if ($null -ne $VerifySeed) {
    # PowerShell boxes Nullable[Int64] values as Int64 when populated, so .Value
    # can silently expand to nothing here. Pass the boxed value itself as text.
    $argsList += @('--verify-seed', [string]$VerifySeed)
}

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
