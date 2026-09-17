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

if (-not (Test-Path $source -PathType Leaf)) {
    & (Join-Path $root 'scripts\make-plains-component-finder.ps1') -ProjectRoot $root
}

# P18 is generated from a locally-patched P17 source. Some valid local histories
# retain legacy runCoverage calls after the helper itself has disappeared. Patch
# that generated source before every run/build; the fixer is idempotent.
$coverageFix = Join-Path $root 'scripts\fix-plains-component-coverage-shim.ps1'
if (-not (Test-Path $coverageFix -PathType Leaf)) {
    throw "Missing Plains compatibility fixer: $coverageFix"
}
& $coverageFix -ProjectRoot $root

# P20 applies the exact Beta 1.7.3 y=63 terrain mask. Ocean/sea columns are
# removed before connected-component measurement, so water cannot act as a bridge.
$dryPatch = Join-Path $root 'scripts\patch-plains-component-p20-dry-mask.ps1'
if (-not (Test-Path $dryPatch -PathType Leaf)) {
    throw "Missing dry-Plains patch: $dryPatch"
}
& $dryPatch -ProjectRoot $root

# P21 treats PLAINS + SEASONAL_FOREST as one allowed land region and ranks
# connected components using both area and shape/compactness. It is idempotent.
$shapePatch = Join-Path $root 'scripts\patch-plains-component-p21-plains-seasonal-shape.ps1'
if (-not (Test-Path $shapePatch -PathType Leaf)) {
    throw "Missing Plains+Seasonal shape patch: $shapePatch"
}
& $shapePatch -ProjectRoot $root

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

Write-Host 'Plains+Seasonal search: exact 800x800 square (-400..399), dry 4-neighbour PLAINS or SEASONAL_FOREST land, compactness-aware ranking.' -ForegroundColor Cyan
Write-Host 'Shape score = connected area weighted by bbox fill + aspect ratio; ocean/sea water always breaks connectivity.'
Write-Host "Batch=$Batch TopExact=$TopExact Center=($CenterX,$CenterZ)"

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
