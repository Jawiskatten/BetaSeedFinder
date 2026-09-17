param(
    [int]$Batch = 131072,
    [int]$TopExact = 1,
    [int]$CenterX = 0,
    [int]$CenterZ = 0,
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
$exe = Join-Path $root 'build\native\amd\PlainsComponentFinder.exe'
$source = Join-Path $root 'native\src\plains_component_finder.cpp'
$generator = Join-Path $root 'scripts\make-plains-component-finder.ps1'

# P22 is generated directly now. This removes the fragile P21 self-patching
# chain entirely. Rebuild always regenerates the dedicated Plains source; an old
# P18/P20 source is also migrated automatically even without -Rebuild.
$needGenerate = $Rebuild -or -not (Test-Path $source -PathType Leaf)
if (-not $needGenerate) {
    $existing = [System.IO.File]::ReadAllText($source)
    $needGenerate = -not $existing.Contains('P22_PLAINS_SEASONAL_SHAPE')
}
if ($needGenerate) {
    if (-not (Test-Path $generator -PathType Leaf)) {
        throw "Missing Plains generator: $generator"
    }
    & $generator -ProjectRoot $root
}

# Some valid local P17 histories retain legacy runCoverage calls after the helper
# itself disappeared. Keep the small idempotent compatibility shim.
$coverageFix = Join-Path $root 'scripts\fix-plains-component-coverage-shim.ps1'
if (-not (Test-Path $coverageFix -PathType Leaf)) {
    throw "Missing Plains compatibility fixer: $coverageFix"
}
& $coverageFix -ProjectRoot $root

# Exact Beta 1.7.3 y=63 terrain mask: sea/ocean columns are removed before the
# CPU component flood fill, so water cannot connect separate land masses.
$dryPatch = Join-Path $root 'scripts\patch-plains-component-p20-dry-mask.ps1'
if (-not (Test-Path $dryPatch -PathType Leaf)) {
    throw "Missing dry-land patch: $dryPatch"
}
& $dryPatch -ProjectRoot $root

if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-plains-component-finder.ps1') -ProjectRoot $root
}

# Fixed objective: 800x800 square, X/Z -400..399 around the requested center.
$argsList = @(
    '--target', '400',
    '--batch', [string]$Batch,
    '--top-exact', [string]$TopExact,
    '--center-x', [string]$CenterX,
    '--center-z', [string]$CenterZ,
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
    $argsList += @('--verify-seed', [string]$VerifySeed)
}

Write-Host 'P22 Plains+Seasonal search: exact 800x800 square (-400..399).' -ForegroundColor Cyan
Write-Host 'Allowed connected land: PLAINS or SEASONAL_FOREST. Ocean/sea water breaks connectivity.'
Write-Host 'Ranking: connected area weighted 70% base + up to 20% bbox fill + up to 10% aspect balance.'
Write-Host "Batch=$Batch TopExact=$TopExact Center=($CenterX,$CenterZ)"

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
