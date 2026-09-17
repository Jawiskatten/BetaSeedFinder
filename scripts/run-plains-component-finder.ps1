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

# P20 keeps the fast P18 biome scout but changes the exact record metric to one
# connected DRY Plains landmass. It applies the validated Beta 1.7.3 terrain
# density test at y=63 and removes ocean/sea columns before flood filling.
$dryPatch = Join-Path $root 'scripts\patch-plains-component-p20-dry-mask.ps1'
if (-not (Test-Path $dryPatch -PathType Leaf)) {
    throw "Missing dry-Plains patch: $dryPatch"
}
& $dryPatch -ProjectRoot $root

if ($Rebuild -or -not (Test-Path $exe -PathType Leaf)) {
    & (Join-Path $root 'scripts\build-plains-component-finder.ps1') -ProjectRoot $root
}

# Fixed objective for this finder: radius 400 square = 800x800 blocks,
# coordinates [-400,+399] relative to the requested center.
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

Write-Host 'Dry Plains search: exact 800x800 square (-400..399), largest 4-neighbour-connected PLAINS land area; ocean/sea water cannot connect it.' -ForegroundColor Cyan
Write-Host "Batch=$Batch TopExact=$TopExact Center=($CenterX,$CenterZ)"

Push-Location $root
try {
    & $exe @argsList
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
