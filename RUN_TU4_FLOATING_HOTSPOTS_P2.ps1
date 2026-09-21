param(
    [UInt64]$Count = 5000,
    [ValidateRange(256,1024)][int]$Canvas = 512,
    [ValidateRange(1,10000)][int]$Top = 128,
    [ValidateRange(1,10000)][int]$VerifyTop = 64,
    [ValidateRange(1,4096)][int]$Batch = 512,
    [ValidateRange(1,16)][int]$VerifyBatch = 4,
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
$sourceRef = '2b7dd1c9660a8238e4807e0fe927f4df56573c38'

if (($Canvas % 16) -ne 0) { throw 'Canvas must be divisible by 16.' }
if ($Canvas -lt 256) { throw 'Canvas must be at least 256 blocks so every dynamic window fits.' }
if ($VerifyTop -gt $Top) { throw 'VerifyTop cannot be greater than Top.' }

$helper = Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1'
if (-not (Test-Path $helper -PathType Leaf)) { throw "Missing helper: $helper" }
. $helper

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $root

# Reuse the validated chunk-local Beta terrain headers already produced by the
# needle finder. These preserve the corrected per-chunk lattice/climate behavior.
$p1build = Join-Path $root 'build\needle-spawn-p1'
$genDir = Join-Path $p1build 'generated-chunk0'
if (-not (Test-Path (Join-Path $genDir 'coarse_exact_core.hpp') -PathType Leaf)) {
    throw "Missing $genDir. Run RUN_NEEDLE_SPAWN_P1.ps1 once first."
}

$build = Join-Path $root 'build\tu4-floating-hotspots-p2'
$sourceNative = Join-Path $build 'source\native'
$hotDir = Join-Path $sourceNative 'tu4_floating_hotspots'
$p1Dir = Join-Path $sourceNative 'tu4_floating_components'
$highestDir = Join-Path $sourceNative 'highest_pillar_spawn'
New-Item -ItemType Directory -Force -Path $hotDir,$p1Dir,$highestDir | Out-Null

$hotCpp = Join-Path $hotDir 'TU4FloatingHotspotsP2.cpp'
$p1Cpp = Join-Path $p1Dir 'TU4FloatingComponents.cpp'
$highestCpp = Join-Path $highestDir 'HighestPillarSpawnGpuFinder.cpp'

Write-Host "Downloading exact P2 source from commit $sourceRef..."
Invoke-WebRequest -UseBasicParsing `
    "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$sourceRef/native/tu4_floating_hotspots/TU4FloatingHotspotsP2.cpp" `
    -OutFile $hotCpp
Invoke-WebRequest -UseBasicParsing `
    "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$sourceRef/native/tu4_floating_components/TU4FloatingComponents.cpp" `
    -OutFile $p1Cpp

$highestLocal = Join-Path $root 'native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
if (-not (Test-Path $highestLocal -PathType Leaf)) {
    $highestLocal = Join-Path $p1build 'source\native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
}
if (-not (Test-Path $highestLocal -PathType Leaf)) { throw 'Missing HighestPillarSpawnGpuFinder.cpp dependency.' }
Copy-Item -Force $highestLocal $highestCpp

$exe = Join-Path $build 'TU4FloatingHotspotsP2_AMD.exe'
Write-Host "Compiling P2 dynamic hotspot finder for $($arches -join ',')..."
& $hipcc -O3 -std=c++17 -x hip @archArgs `
    '-DSKYBLOCK_COARSE_API_MODERN=1' `
    "-I$genDir" "-I$nativeSourceDir" `
    $hotCpp `
    -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'P2 hotspot compile failed.' }

if ($null -eq $RandomKey) {
    $bytes = New-Object byte[] 8
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $RandomKey = [BitConverter]::ToUInt64($bytes,0)
}

$outputRoot = Join-Path $root 'out\tu4_floating_hotspots_p2'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$output = [IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"))
New-Item -ItemType Directory -Force -Path $output | Out-Null

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 / TU4 FLOATING-ISLAND HOTSPOT P2'
Write-Host '=================================================================='
Write-Host "Search canvas: $Canvas x $Canvas blocks centered on 0,0"
Write-Host 'Dynamic windows: 64,80,96,112,128,160,192,256 blocks'
Write-Host 'Objective: components^2 / window area (lots of islands packed tightly wins)'
Write-Host 'Exact stage uses true 3D 6-connected floating components.'
Write-Host 'A component is assigned to a hotspot by the center of its bounding box.'
Write-Host 'Exact verifier packs the whole GPU world first, then does ONE host copy per batch.'
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
    '--canvas',$Canvas,
    '--batch',$Batch,
    '--terrain-threads',$TerrainThreads,
    '--top',$Top,
    '--verify-top',$VerifyTop
)
& $exe @scoutArgs
if ($LASTEXITCODE -ne 0) { throw 'P2 hotspot scout failed.' }

$candidates = Join-Path $output 'top_candidates.csv'
$verifyArgs = @(
    '--mode','verify',
    '--input',$candidates,
    '--output',$output,
    '--canvas',$Canvas,
    '--terrain-threads',$TerrainThreads,
    '--top',$Top,
    '--verify-top',$VerifyTop,
    '--verify-batch',$VerifyBatch,
    '--min-blocks',$MinBlocks
)
if ($IncludeBoundary) { $verifyArgs += '--include-boundary' }

Write-Host ''
Write-Host 'Exact-verifying strongest dynamic hotspots...'
& $exe @verifyArgs
if ($LASTEXITCODE -ne 0) { throw 'P2 hotspot exact verification failed.' }

Write-Host ''
Write-Host "RESULT_DIR=$output"
Write-Host "CANDIDATES=$(Join-Path $output 'top_candidates.csv')"
Write-Host "EXACT_HOTSPOTS=$(Join-Path $output 'exact_hotspots.csv')"
