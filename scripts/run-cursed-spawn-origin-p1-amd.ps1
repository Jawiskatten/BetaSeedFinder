param(
    [UInt64]$Count = 10000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateSet(4,6,8,10,12)][int]$Radius = 4,
    [int]$Batch = 0,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,10000)][int]$Top = 100,
    [ValidateRange(100,60000)][int]$ProgressMs = 1000,
    [ValidateRange(1000,600000)][int]$CheckpointMs = 5000,
    [string]$Seed = '',
    [string]$ExistingOutput = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ($Batch -le 0) {
    $Batch = switch ($Radius) { 4 {8192} 6 {6144} 8 {4096} 10 {3072} 12 {2048} }
}
if ($Batch -lt 1 -or $Batch -gt 32768) { throw 'Batch must be 1..32768.' }

. (Join-Path $PSScriptRoot 'cursed-spawn-origin-p1-common.ps1')
Write-Host ("LavaSpawnOrigin P2 preflight: spawn-valid sand at X=0,Z=0, then a population lava lake must cover player feet Y=65; radius={0}." -f $Radius)

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' $Radius
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'LavaSpawnOrigin P2 requires the current modern BetaSeedFinder GPU headers.' }
$apiDefine = '-DSKYBLOCK_COARSE_API_MODERN=1'
$nativeDir = Join-Path $ProjectRoot ("build\lava-spawn-origin-p2-full-r{0}\native" -f $Radius)
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null
$source = Join-Path $ProjectRoot 'native\cursed_spawn_origin\CursedSpawnOriginGpuFinder.cpp'
if (-not (Test-Path $source -PathType Leaf)) { throw "CursedSpawnOrigin source missing: $source. Extract the overlay into the BetaSeedFinder project root." }
$worker = Join-Path $nativeDir ("LavaSpawnOriginGpuFinderAMD_P2_full_r{0}.exe" -f $Radius)
$sigFile = "$worker.signature.txt"
$archKey = ($arches -join ',')
$sig = Get-NativeSignature $source $nativeSourceDir "AMD|$archKey|$api" ("LavaSpawnOriginP2|full|r$Radius")
$old = if (Test-Path $sigFile) { Get-Content $sigFile -Raw } else { '' }
if (-not (Test-Path $worker) -or $old -ne $sig) {
    Write-Host "Compiling LavaSpawnOrigin P2 AMD GPU finder for $archKey..."
    $archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
    & $hipcc -O3 -std=c++17 -x hip @archArgs $apiDefine "-I$generated" "-I$nativeSourceDir" $source -o $worker
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $worker -Force -ErrorAction SilentlyContinue
        throw 'LavaSpawnOrigin P2 AMD compilation failed.'
    }
    Set-Content $sigFile $sig -Encoding UTF8 -NoNewline
} else {
    Write-Host "Using cached LavaSpawnOrigin P2 full/r$Radius AMD worker."
}

Write-Host 'Running deterministic GPU self-test...'
& $worker --self-test --batch 4 --seed-mode unique48 --terrain-threads 64
if ($LASTEXITCODE -ne 0) {
    Remove-Item $worker -Force -ErrorAction SilentlyContinue
    throw 'LavaSpawnOrigin P2 GPU self-test failed.'
}
if ($SelfTest) { exit 0 }

$outputRoot = Join-Path $ProjectRoot 'out\lava_spawn_origin_p2'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
if (-not [string]::IsNullOrWhiteSpace($ExistingOutput)) {
    $output = [System.IO.Path]::GetFullPath($ExistingOutput)
    if (-not (Test-Path $output -PathType Container)) { throw "Existing run directory not found: $output" }
    $resumeExisting = $true
} else {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output = Join-Path $outputRoot ("r{0}_{1}" -f $Radius, $stamp)
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $output = [System.IO.Path]::GetFullPath($output)
    $resumeExisting = $false
}
Set-Content (Join-Path $outputRoot 'LAST_RUN.txt') $output -Encoding UTF8
Set-Content (Join-Path $output 'IN_PROGRESS.txt') ("Started/resumed {0:o}" -f (Get-Date)) -Encoding UTF8
Set-Content (Join-Path $output 'HOW_TO_RESUME.txt') 'Run RESUME_LITERAL_LAVA_SPAWN_P2_LAST_RUN.bat. It preserves the unique48 sequence and merges the existing leaderboards.' -Encoding UTF8

$invariant = [Globalization.CultureInfo]::InvariantCulture
$workerArgs = @(
    '--output', $output,
    '--count', $Count.ToString($invariant),
    '--start-index', $StartIndex.ToString($invariant),
    '--batch', $Batch.ToString($invariant),
    '--terrain-threads', $TerrainThreads.ToString($invariant),
    '--top', $Top.ToString($invariant),
    '--seed-mode', $SeedMode,
    '--progress-ms', $ProgressMs.ToString($invariant),
    '--checkpoint-ms', $CheckpointMs.ToString($invariant)
)
if ($null -ne $RandomKey) { $workerArgs += @('--random-key', ([UInt64]$RandomKey).ToString($invariant)) }
if (-not [string]::IsNullOrWhiteSpace($Seed)) {
    [Int64]$parsedSeed = 0
    if (-not [Int64]::TryParse($Seed, [Globalization.NumberStyles]::Integer, $invariant, [ref]$parsedSeed)) { throw "Invalid signed 64-bit seed: $Seed" }
    $workerArgs += @('--seed', $parsedSeed.ToString($invariant))
}
if ($resumeExisting) { $workerArgs += '--resume-existing' }

$transcriptStarted = $false
try {
    try { Start-Transcript -Path (Join-Path $output 'console.log') -Append | Out-Null; $transcriptStarted = $true } catch {}
    Write-Host ''
    Write-Host 'Every saved hit has spawn-valid sand at exactly X=0,Z=0 before population.'
    Write-Host 'Hard target: the later lava lake covers the player bounding box at world Y=65.'
    Write-Host "Saving this run to: $output"
    & $worker @workerArgs
    $workerExitCode = $LASTEXITCODE
    if ($workerExitCode -ne 0) { throw "LavaSpawnOrigin P2 AMD search exited with code $workerExitCode." }
    Remove-Item (Join-Path $output 'IN_PROGRESS.txt') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $output 'FAILED.txt') -Force -ErrorAction SilentlyContinue
    Set-Content (Join-Path $output 'COMPLETED.txt') ("Completed {0:o}" -f (Get-Date)) -Encoding UTF8
} catch {
    Set-Content (Join-Path $output 'FAILED.txt') (($_ | Out-String).Trim()) -Encoding UTF8
    throw
} finally {
    if ($transcriptStarted) { try { Stop-Transcript | Out-Null } catch {} }
    Write-Host ''
    Write-Host "RESULT_DIR=$output"
}
