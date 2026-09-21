param(
    [UInt64]$Count = 2000,
    [ValidateRange(32,1024)][int]$WorldSize = 800,
    [ValidateRange(1,10000)][int]$Top = 64,
    [ValidateRange(1,10000)][int]$VerifyTop = 32,
    [ValidateRange(1,8192)][int]$Batch = 1024,
    [ValidateRange(1,32)][int]$VerifyBatch = 4,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,1000000)][int]$MinBlocks = 1,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [switch]$IncludeBoundary
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
$sourceRef = '336b1fc3dc0a872474e21f26982bba2bab83e9ec'

if (($WorldSize % 32) -ne 0) {
    throw 'WorldSize must be divisible by 32. 800 and 864 both work.'
}
if ($VerifyTop -gt $Top) { throw 'VerifyTop cannot be greater than Top.' }

$helper = Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1'
if (-not (Test-Path $helper -PathType Leaf)) { throw "Missing helper: $helper" }
. $helper

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $root

# Reuse the validated chunk-local Beta 1.7.3 terrain headers from the earlier
# needle P1 setup: SIZE=5, FROM_COARSE=0, exact cropped Y nodes, corrected
# chunk-local climate coordinates.
$p1build = Join-Path $root 'build\needle-spawn-p1'
$genDir = Join-Path $p1build 'generated-chunk0'
if (-not (Test-Path (Join-Path $genDir 'coarse_exact_core.hpp') -PathType Leaf)) {
    throw "Missing $genDir. Run RUN_NEEDLE_SPAWN_P1.ps1 once first to create the validated chunk-local headers."
}

$highest = Join-Path $root 'native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
if (-not (Test-Path $highest -PathType Leaf)) {
    $highest = Join-Path $p1build 'source\native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
}
if (-not (Test-Path $highest -PathType Leaf)) { throw 'Missing HighestPillarSpawnGpuFinder.cpp dependency.' }

$build = Join-Path $root 'build\tu4-floating-components'
$sourceNative = Join-Path $build 'source\native'
$finderDir = Join-Path $sourceNative 'tu4_floating_components'
$highestDir = Join-Path $sourceNative 'highest_pillar_spawn'
New-Item -ItemType Directory -Force -Path $finderDir,$highestDir | Out-Null

# Prefer the checked-out GitHub source. If the branch is not pulled locally yet,
# download the exact committed source so this runner still works from one command.
$repoCpp = Join-Path $root 'native\tu4_floating_components\TU4FloatingComponents.cpp'
$buildCpp = Join-Path $finderDir 'TU4FloatingComponents.cpp'
if (Test-Path $repoCpp -PathType Leaf) {
    Copy-Item -Force $repoCpp $buildCpp
} else {
    $rawCpp = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$sourceRef/native/tu4_floating_components/TU4FloatingComponents.cpp"
    Write-Host "Downloading TU4 finder source from commit $sourceRef..."
    Invoke-WebRequest -UseBasicParsing $rawCpp -OutFile $buildCpp
}
Copy-Item -Force $highest (Join-Path $highestDir 'HighestPillarSpawnGpuFinder.cpp')

$exe = Join-Path $build 'TU4FloatingComponents_AMD.exe'
Write-Host "Compiling TU4 floating-component finder for $($arches -join ',')..."
& $hipcc -O3 -std=c++17 -x hip @archArgs `
    '-DSKYBLOCK_COARSE_API_MODERN=1' `
    "-I$genDir" "-I$nativeSourceDir" `
    $buildCpp `
    -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'TU4 floating-component compile failed.' }

if ($null -eq $RandomKey) {
    $bytes = New-Object byte[] 8
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $RandomKey = [BitConverter]::ToUInt64($bytes,0)
}

$outputRoot = Join-Path $root 'out\tu4_floating_components'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$output = [IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"))
New-Item -ItemType Directory -Force -Path $output | Out-Null

Write-Host ''
Write-Host '=================================================================='
Write-Host ' TU4 / BETA 1.7.3 - MOST FLOATING COMPONENTS'
Write-Host '=================================================================='
Write-Host "Area: $WorldSize x $WorldSize blocks, centered on 0,0"
Write-Host 'Scout objective: local detached-footprint component count (fast GPU proxy).'
Write-Host 'Final objective: exact 6-connected solid components not connected to the bottom terrain band.'
Write-Host 'Components touching the analysis boundary are excluded by default.'
Write-Host "Count=$Count Top=$Top VerifyTop=$VerifyTop MinBlocks=$MinBlocks RandomKey=$RandomKey"
Write-Host "Output=$output"
Write-Host ''

$scoutArgs = @(
    '--mode','scout',
    '--output',$output,
    '--count',$Count.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--start-index',$StartIndex.ToString([Globalization.CultureInfo]::InvariantCulture),
    '--random-key',([UInt64]$RandomKey).ToString([Globalization.CultureInfo]::InvariantCulture),
    '--seed-mode',$SeedMode,
    '--world-size',$WorldSize,
    '--batch',$Batch,
    '--terrain-threads',$TerrainThreads,
    '--top',$Top,
    '--verify-top',$VerifyTop
)
& $exe @scoutArgs
if ($LASTEXITCODE -ne 0) { throw 'TU4 scout failed.' }

$candidates = Join-Path $output 'top_candidates.csv'
$verifyArgs = @(
    '--mode','verify',
    '--input',$candidates,
    '--output',$output,
    '--world-size',$WorldSize,
    '--terrain-threads',$TerrainThreads,
    '--top',$Top,
    '--verify-top',$VerifyTop,
    '--verify-batch',$VerifyBatch,
    '--min-blocks',$MinBlocks
)
if ($IncludeBoundary) { $verifyArgs += '--include-boundary' }

Write-Host ''
Write-Host 'Now exact-verifying the strongest proxy worlds...'
& $exe @verifyArgs
if ($LASTEXITCODE -ne 0) { throw 'TU4 exact verification failed.' }

Write-Host ''
Write-Host "RESULT_DIR=$output"
Write-Host "EXACT_RESULTS=$(Join-Path $output 'exact_results.csv')"
Write-Host "BEST_COMPONENTS=$(Join-Path $output 'best_components.csv')"
