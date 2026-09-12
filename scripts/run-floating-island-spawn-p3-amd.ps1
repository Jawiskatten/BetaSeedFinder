param(
    [UInt64]$Count = 100000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateSet(4,6,8,10,12)][int]$Radius = 4,
    [int]$Batch = 0,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,10000)][int]$Top = 250,
    [ValidateRange(100,60000)][int]$ProgressMs = 1000,
    [ValidateRange(1000,600000)][int]$CheckpointMs = 5000,
    [string]$Seed = '',
    [string]$ExistingOutput = '',
    [switch]$DesktopFriendly,
    [int]$GpuYieldMs = -1,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ($GpuYieldMs -lt -1 -or $GpuYieldMs -gt 50) { throw 'GpuYieldMs must be -1 (automatic) or 0..50.' }
if ($GpuYieldMs -eq -1) { $GpuYieldMs = if ($DesktopFriendly) { 1 } else { 0 } }
if ($Batch -le 0) {
    if ($DesktopFriendly) {
        # Keep individual compute bursts short enough that Windows/DWM gets
        # frequent scheduling opportunities on a display-attached GPU.
        $Batch = switch ($Radius) { 4 {1024} 6 {768} 8 {512} 10 {384} 12 {256} }
    } else {
        $Batch = switch ($Radius) { 4 {8192} 6 {6144} 8 {4096} 10 {3072} 12 {2048} }
    }
}
if ($Batch -lt 1 -or $Batch -gt 32768) { throw 'Batch must be 1..32768.' }
$runMode = if ($DesktopFriendly) { 'DESKTOP' } else { 'MAX-SPEED' }

. (Join-Path $PSScriptRoot 'cursed-spawn-origin-p1-common.ps1')
Write-Host ("FloatingIslandSpawn P3 preflight: sand -> air -> upper component -> collision push; radius={0}." -f $Radius)
Write-Host ("GPU scheduling: {0} | batch={1} | host yield={2} ms between synchronized batches." -f $runMode,$Batch,$GpuYieldMs)

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' $Radius
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'FloatingIslandSpawn P3 requires the current modern BetaSeedFinder GPU headers.' }
$apiDefine = '-DSKYBLOCK_COARSE_API_MODERN=1'

$nativeDir = Join-Path $ProjectRoot ("build\floating-island-spawn-p3-full-r{0}\native" -f $Radius)
New-Item -ItemType Directory -Force -Path $nativeDir | Out-Null
$sourceOriginal = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnGpuFinderP3.cpp'
if (-not (Test-Path $sourceOriginal -PathType Leaf)) { throw "P3 source missing: $sourceOriginal" }

# Build a deterministic compile copy. Older local installs may still have the
# ambiguous P3 runSelfTest name; FIX_P3_SELFTEST_AND_VERIFY.ps1 may already have
# renamed it. Accept both states so upgrades do not depend on local history.
$source = Join-Path $nativeDir ("FloatingIslandSpawnGpuFinderP3_compile_yield{0}.cpp" -f $GpuYieldMs)
$sourceText = [System.IO.File]::ReadAllText($sourceOriginal)

# The checked-in P3 source includes P1 with a path relative to its own source
# directory. Our generated compile copy lives under build\..., so that relative
# include would point at the wrong directory. Rewrite only the compile copy to
# the real absolute P1 source path.
$p1Source = Join-Path $ProjectRoot 'native\highest_pillar_spawn\HighestPillarSpawnGpuFinder.cpp'
if (-not (Test-Path $p1Source -PathType Leaf)) { throw "P1 dependency missing: $p1Source" }
$p1IncludeOld = '#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"'
if (-not $sourceText.Contains($p1IncludeOld)) { throw 'P3 compile patch could not find the P1 include line.' }
$p1IncludePath = [System.IO.Path]::GetFullPath($p1Source).Replace('\','/')
$p1IncludeNew = '#include "' + $p1IncludePath + '"'
$sourceText = $sourceText.Replace($p1IncludeOld, $p1IncludeNew)

$declOld = 'static int runSelfTest(Config c) {'
$declNew = 'static int runSelfTestP3(Config c) {'
$callOld = 'if (c.selfTest) return runSelfTest(c);'
$callNew = 'if (c.selfTest) return runSelfTestP3(c);'
if ($sourceText.Contains($declOld)) {
    if (-not $sourceText.Contains($callOld)) { throw 'P3 compile patch found old self-test declaration but not its call.' }
    $sourceText = $sourceText.Replace($declOld, $declNew).Replace($callOld, $callNew)
} elseif (-not $sourceText.Contains($declNew)) {
    throw 'P3 compile patch could not find either local self-test declaration.'
}

# A huge batch keeps the display GPU continuously occupied for hundreds of ms.
# In Desktop mode we use much smaller batches and deliberately leave a tiny
# host-side gap after hipDeviceSynchronize(). That gives the Windows compositor,
# browser, Discord, etc. a real scheduling window without changing search math.
$includeMarker = '#include <climits>'
if (-not $sourceText.Contains('#include <thread>')) {
    if (-not $sourceText.Contains($includeMarker)) { throw 'P3 compile patch could not find include insertion point.' }
    $sourceText = $sourceText.Replace($includeMarker, "$includeMarker`r`n#include <thread>")
}
$launchMarker = '            launchGateBatch(b, c, c.startIndex + completed, batchCount, gate);'
if (-not $sourceText.Contains($launchMarker)) { throw 'P3 compile patch could not find main GPU batch launch.' }
$yieldLine = "            if ($GpuYieldMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds($GpuYieldMs));"
$sourceText = $sourceText.Replace($launchMarker, "$launchMarker`r`n$yieldLine")
[System.IO.File]::WriteAllText($source, $sourceText, [System.Text.UTF8Encoding]::new($false))

$worker = Join-Path $nativeDir ("FloatingIslandSpawnGpuFinderAMD_P3_full_r{0}_yield{1}.exe" -f $Radius,$GpuYieldMs)
$sigFile = "$worker.signature.txt"
$archKey = ($arches -join ',')
$sig = Get-NativeSignature $source $nativeSourceDir "AMD|$archKey|$api" ("FloatingIslandSpawnP3|full|r$Radius|yield$GpuYieldMs|desktop-v3")
$old = if (Test-Path $sigFile) { Get-Content $sigFile -Raw } else { '' }
if (-not (Test-Path $worker) -or $old -ne $sig) {
    Write-Host "Compiling FloatingIslandSpawn P3 AMD GPU finder for $archKey..."
    $archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
    & $hipcc -O3 -std=c++17 -x hip @archArgs $apiDefine "-I$generated" "-I$nativeSourceDir" $source -o $worker
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $worker -Force -ErrorAction SilentlyContinue
        throw 'FloatingIslandSpawn P3 AMD compilation failed.'
    }
    Set-Content $sigFile $sig -Encoding UTF8 -NoNewline
} else {
    Write-Host "Using cached FloatingIslandSpawn P3 full/r$Radius AMD worker (yield=$GpuYieldMs ms)."
}

Write-Host 'Running known-seed floating-island self-test...'
& $worker --self-test --batch 4 --seed-mode unique48 --terrain-threads 64
if ($LASTEXITCODE -ne 0) {
    Remove-Item $worker -Force -ErrorAction SilentlyContinue
    throw 'FloatingIslandSpawn P3 self-test failed.'
}
if ($SelfTest) { exit 0 }

$outputRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p3'
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
Set-Content (Join-Path $output 'HOW_TO_RESUME.txt') 'Run RESUME_FLOATING_ISLAND_SPAWN_P3_LAST_RUN.bat for desktop mode, or the MAX_SPEED resume BAT overnight.' -Encoding UTF8

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
    Write-Host 'TARGET: the largest genuine floating terrain component that obstructs the normal sand spawn and pushes the player onto its top.'
    Write-Host 'Hard sequence: spawn-valid sand -> at least one air block -> upper solid mass intersects initial player AABB -> player ends on that mass.'
    Write-Host 'Every accepted hit is flood-filled after the GPU gate. If its component reaches the verification boundary it is rejected, not guessed.'
    Write-Host 'Primary ranking is total floating component blocks. Footprint is the second tiebreak.'
    Write-Host ("GPU mode: {0}; batch={1}; yield={2} ms. Search results are identical across modes." -f $runMode,$Batch,$GpuYieldMs)
    Write-Host "Saving this run to: $output"
    & $worker @workerArgs
    $workerExitCode = $LASTEXITCODE
    if ($workerExitCode -ne 0) { throw "FloatingIslandSpawn P3 AMD search exited with code $workerExitCode." }
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
