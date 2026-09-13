param(
    [UInt64]$Count = 100000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(1,10000)][int]$Top = 250,
    [switch]$MaxSpeed,
    [switch]$Resume,
    [switch]$SelfTest,
    [int]$ScoutBatch = 0,
    [int]$ScoutChunk = 0,
    [int]$GpuYieldMs = -1
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'cursed-spawn-origin-p1-common.ps1')

# RX 7800 XT tuning: P5 L32/U8 peaks around batch 262144.  Larger batches lose
# throughput sharply.  Desktop mode uses short launches plus a tiny host yield.
if ($ScoutBatch -le 0) { $ScoutBatch = if ($MaxSpeed) { 262144 } else { 65536 } }
if ($ScoutChunk -le 0) { $ScoutChunk = if ($MaxSpeed) { 25000000 } else { 5000000 } }
if ($GpuYieldMs -lt 0) { $GpuYieldMs = if ($MaxSpeed) { 0 } else { 1 } }
if ($ScoutBatch -lt 1 -or $ScoutBatch -gt 4194304) { throw 'ScoutBatch must be 1..4194304.' }
if ($ScoutChunk -lt 1) { throw 'ScoutChunk must be positive.' }
if ($GpuYieldMs -lt 0 -or $GpuYieldMs -gt 50) { throw 'GpuYieldMs must be 0..50.' }

$modeName = if ($MaxSpeed) { 'MAX-SPEED' } else { 'DESKTOP' }
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P5 verifier requires the current modern BetaSeedFinder GPU headers.' }
$apiDefine = '-DSKYBLOCK_COARSE_API_MODERN=1'

$buildRoot = Join-Path $ProjectRoot 'build\floating-island-spawn-p5-wave'
$nativeBuild = Join-Path $buildRoot 'production'
New-Item -ItemType Directory -Force -Path $nativeBuild | Out-Null

$scoutSource = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnScoutP5Wave.cpp'
$verifySource = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnVerifyP4.cpp'
if (-not (Test-Path $scoutSource -PathType Leaf)) { throw "Missing P5 scout source: $scoutSource" }
if (-not (Test-Path $verifySource -PathType Leaf)) { throw "Missing exact verifier source: $verifySource" }

function Compile-P5Scout {
    $worker = Join-Path $nativeBuild 'FloatingIslandSpawnScoutP5_L32_U8_AMD.exe'
    $sigFile = "$worker.signature.txt"
    $sig = Get-NativeSignature $scoutSource $nativeSourceDir "AMD|$archKey|p5-wave" 'P5|L32|U8|production-v1'
    $old = if (Test-Path $sigFile) { Get-Content $sigFile -Raw } else { '' }
    if (-not (Test-Path $worker -PathType Leaf) -or $old -ne $sig) {
        Write-Host "Compiling production P5 L32/U8 scout for $archKey..."
        & $hipcc -O3 -std=c++17 -x hip @archArgs '-DP5_LANES=32' '-DP5_PERM_U8=1' "-I$nativeSourceDir" $scoutSource -o $worker
        if ($LASTEXITCODE -ne 0) {
            Remove-Item $worker -Force -ErrorAction SilentlyContinue
            throw 'P5 scout compilation failed.'
        }
        [System.IO.File]::WriteAllText($sigFile, $sig, [System.Text.UTF8Encoding]::new($false))
    } else {
        Write-Host 'Using cached production P5 L32/U8 scout.'
    }
    return $worker
}

function Compile-ExactVerifier([int]$Radius) {
    if ($Radius -notin @(4,8,12)) { throw "Unsupported verifier radius: $Radius" }
    $generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' $Radius
    $worker = Join-Path $nativeBuild ("FloatingIslandSpawnVerifyP5_r{0}_AMD.exe" -f $Radius)
    $sigFile = "$worker.signature.txt"
    $sig = Get-NativeSignature $verifySource $nativeSourceDir "AMD|$archKey|$api" ("P5|exact-verify|r{0}|v1" -f $Radius)
    foreach ($g in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
        $p = Join-Path $generated $g
        $item = Get-Item $p
        $sig += "`n$($item.FullName)|$($item.Length)|$($item.LastWriteTimeUtc.Ticks)"
    }
    $old = if (Test-Path $sigFile) { Get-Content $sigFile -Raw } else { '' }
    if (-not (Test-Path $worker -PathType Leaf) -or $old -ne $sig) {
        Write-Host ("Compiling exact r{0} verifier for {1}..." -f $Radius,$archKey)
        & $hipcc -O3 -std=c++17 -x hip @archArgs $apiDefine "-I$generated" "-I$nativeSourceDir" $verifySource -o $worker
        if ($LASTEXITCODE -ne 0) {
            Remove-Item $worker -Force -ErrorAction SilentlyContinue
            throw ("Verifier r{0} compilation failed." -f $Radius)
        }
        [System.IO.File]::WriteAllText($sigFile, $sig, [System.Text.UTF8Encoding]::new($false))
    } else {
        Write-Host ("Using cached exact r{0} verifier." -f $Radius)
    }
    return $worker
}

$scoutWorker = Compile-P5Scout
$r4Worker = Compile-ExactVerifier 4
Write-Host 'Running production P5 scout self-test...'
& $scoutWorker --self-test
if ($LASTEXITCODE -ne 0) { throw 'P5 scout self-test failed.' }
Write-Host 'Running exact r4 verifier self-test...'
& $r4Worker --self-test
if ($LASTEXITCODE -ne 0) { throw 'Exact r4 verifier self-test failed.' }
if ($SelfTest) { Write-Host 'P5 production install/self-test OK.'; exit 0 }

function Has-CandidateRows([string]$Path) {
    if (-not (Test-Path $Path -PathType Leaf)) { return $false }
    $firstTwo = @(Get-Content -LiteralPath $Path -TotalCount 2)
    return $firstTwo.Count -ge 2
}
function New-RandomUInt64 {
    $bytes = New-Object byte[] 8
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return [BitConverter]::ToUInt64($bytes, 0)
}

$outputRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p5'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRunFile = Join-Path $outputRoot 'LAST_RUN.txt'

if ($Resume) {
    if (-not (Test-Path $lastRunFile -PathType Leaf)) { throw 'No P5 LAST_RUN.txt exists.' }
    $output = (Get-Content -LiteralPath $lastRunFile -Raw).Trim([char]0xFEFF, [char]0x200B, ' ', "`r", "`n", "`t")
    if (-not (Test-Path $output -PathType Container)) { throw "P5 last run directory not found: $output" }
} else {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output = Join-Path $outputRoot ("run_{0}" -f $stamp)
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    [System.IO.File]::WriteAllText($lastRunFile, [System.IO.Path]::GetFullPath($output), [System.Text.Encoding]::ASCII)
}
$output = [System.IO.Path]::GetFullPath($output)
$checkpoint = Join-Path $output 'checkpoint.txt'

[UInt64]$completed = 0
[UInt64]$runRandomKey = 0
if ($Resume) {
    if (-not (Test-Path $checkpoint -PathType Leaf)) { throw "P5 checkpoint missing: $checkpoint" }
    foreach ($line in Get-Content -LiteralPath $checkpoint) {
        if ($line -like 'COMPLETED=*') { $completed = [UInt64]($line.Substring(10)) }
        elseif ($line -like 'RANDOM_KEY=*') { $runRandomKey = [UInt64]($line.Substring(11)) }
        elseif ($line -like 'COUNT=*') { $Count = [UInt64]($line.Substring(6)) }
        elseif ($line -like 'START_INDEX=*') { $StartIndex = [UInt64]($line.Substring(12)) }
        elseif ($line -like 'SEED_MODE=*') { $SeedMode = $line.Substring(10) }
    }
} else {
    $runRandomKey = if ($null -ne $RandomKey) { [UInt64]$RandomKey } else { New-RandomUInt64 }
}
if ($completed -gt $Count) { throw 'P5 checkpoint completed count exceeds requested count.' }

function Save-P5Checkpoint {
    $text = @(
        'VERSION=FloatingIslandSpawnP5WaveProduction',
        "START_INDEX=$StartIndex",
        "COMPLETED=$completed",
        "NEXT_INDEX=$($StartIndex + $completed)",
        "COUNT=$Count",
        "RANDOM_KEY=$runRandomKey",
        "SEED_MODE=$SeedMode",
        "MODE=$modeName",
        "SCOUT_BATCH=$ScoutBatch",
        'SCOUT_LANES=32',
        'SCOUT_PERM_BYTES=1'
    ) -join "`r`n"
    [System.IO.File]::WriteAllText($checkpoint, $text + "`r`n", [System.Text.Encoding]::ASCII)
}
Save-P5Checkpoint

Write-Host ''
Write-Host 'FloatingIslandSpawn P5 WAVE PRODUCTION'
Write-Host 'Stage 1: one independent seed per GPU lane, exact origin scout, uint8 permutation.'
Write-Host 'Stage 2: full exact r4 terrain only for scout hits; re-gates every hit against P4 math.'
Write-Host 'Stage 3: only unresolved non-ground boundary components expand to r8 then r12.'
Write-Host ("Mode={0} scoutBatch={1} scoutChunk={2} yield={3}ms lanes=32 permBytes=1" -f $modeName,$ScoutBatch,$ScoutChunk,$GpuYieldMs)
Write-Host ("SeedMode={0} randomKey={1} resumeCompleted={2}/{3}" -f $SeedMode,$runRandomKey,$completed,$Count)
Write-Host "Output=$output"

$runStart = Get-Date
$invariant = [Globalization.CultureInfo]::InvariantCulture
while ($completed -lt $Count) {
    [UInt64]$left = $Count - $completed
    [UInt64]$chunkCount = [Math]::Min([UInt64]$ScoutChunk, $left)
    [UInt64]$absoluteStart = $StartIndex + $completed
    $tag = $absoluteStart.ToString($invariant)
    $candidateFile = Join-Path $output ("candidates_{0}.csv" -f $tag)
    $boundary4 = Join-Path $output ("boundary_r4_{0}.csv" -f $tag)
    $boundary8 = Join-Path $output ("boundary_r8_{0}.csv" -f $tag)
    $boundary12 = Join-Path $output ("boundary_r12_{0}.csv" -f $tag)

    Write-Host ''
    Write-Host ("=== P5 WAVE SCOUT chunk start={0} count={1} ===" -f $absoluteStart,$chunkCount)
    & $scoutWorker `
        --candidate-out $candidateFile `
        --count $chunkCount.ToString($invariant) `
        --start-index $absoluteStart.ToString($invariant) `
        --random-key $runRandomKey.ToString($invariant) `
        --seed-mode $SeedMode `
        --batch $ScoutBatch.ToString($invariant) `
        --yield-ms $GpuYieldMs.ToString($invariant) `
        --progress-ms 1000
    if ($LASTEXITCODE -ne 0) { throw 'P5 wave scout chunk failed.' }

    if (Has-CandidateRows $candidateFile) {
        Write-Host 'P5 exact r4 verification of compacted scout hits...'
        & $r4Worker --input $candidateFile --output $output --boundary-out $boundary4 --batch 1024 --terrain-threads 64 --top $Top
        if ($LASTEXITCODE -ne 0) { throw 'P5 exact r4 verification failed.' }

        if (Has-CandidateRows $boundary4) {
            Write-Host 'P5 r4 boundary survivor(s); expanding only those to r8...'
            $r8Worker = Compile-ExactVerifier 8
            & $r8Worker --input $boundary4 --output $output --boundary-out $boundary8 --batch 256 --terrain-threads 64 --top $Top
            if ($LASTEXITCODE -ne 0) { throw 'P5 exact r8 verification failed.' }

            if (Has-CandidateRows $boundary8) {
                Write-Host 'P5 r8 still unresolved; expanding only those to r12...'
                $r12Worker = Compile-ExactVerifier 12
                & $r12Worker --input $boundary8 --output $output --boundary-out $boundary12 --batch 128 --terrain-threads 64 --top $Top
                if ($LASTEXITCODE -ne 0) { throw 'P5 exact r12 verification failed.' }
            }
        }
    } else {
        Write-Host 'No P5 scout candidates in this chunk; skipping all full-window terrain work.'
    }

    $completed += $chunkCount
    Save-P5Checkpoint
    Write-Host ("P5 checkpoint completed={0}/{1}" -f $completed,$Count)
}

$elapsed = (Get-Date) - $runStart
$summary = @(
    'FloatingIslandSpawn P5 WAVE completed',
    "count=$Count",
    "start_index=$StartIndex",
    "random_key=$runRandomKey",
    "seed_mode=$SeedMode",
    "mode=$modeName",
    "elapsed_seconds=$([Math]::Round($elapsed.TotalSeconds,3))",
    "completed_utc=$([DateTime]::UtcNow.ToString('o'))"
) -join "`r`n"
[System.IO.File]::WriteAllText((Join-Path $output 'COMPLETED.txt'), $summary + "`r`n", [System.Text.Encoding]::ASCII)
Write-Host ''
Write-Host ("P5 SEARCH COMPLETE in {0:N2}s." -f $elapsed.TotalSeconds)
Write-Host "RESULT_DIR=$output"
