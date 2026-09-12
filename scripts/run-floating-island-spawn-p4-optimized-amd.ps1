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

if ($ScoutBatch -le 0) { $ScoutBatch = if ($MaxSpeed) { 131072 } else { 4096 } }
if ($ScoutChunk -le 0) { $ScoutChunk = if ($MaxSpeed) { 10000000 } else { 2000000 } }
if ($GpuYieldMs -lt 0) { $GpuYieldMs = if ($MaxSpeed) { 0 } else { 1 } }
if ($ScoutBatch -lt 1 -or $ScoutBatch -gt 1048576) { throw 'ScoutBatch must be 1..1048576.' }
if ($ScoutChunk -lt 1) { throw 'ScoutChunk must be positive.' }
if ($GpuYieldMs -lt 0 -or $GpuYieldMs -gt 50) { throw 'GpuYieldMs must be 0..50.' }

$modeName = if ($MaxSpeed) { 'MAX-SPEED' } else { 'DESKTOP' }
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P4 requires the current modern BetaSeedFinder GPU headers.' }
$apiDefine = '-DSKYBLOCK_COARSE_API_MODERN=1'

$buildRoot = Join-Path $ProjectRoot 'build\floating-island-spawn-p4-optimized'
$nativeBuild = Join-Path $buildRoot 'native'
New-Item -ItemType Directory -Force -Path $nativeBuild | Out-Null

# Generate the exact r4 P14 headers once using the proven P3 path, then derive a
# one-column origin-only specialization. The scout therefore evaluates exactly
# the same Beta noise/RNG math as P3 but stores/evaluates only one X/Z density
# column per seed instead of the whole r4 lattice.
$fullR4Generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
$originGenerated = Join-Path $buildRoot 'origin-generated-include'
New-Item -ItemType Directory -Force -Path $originGenerated | Out-Null
foreach ($name in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
    Copy-Item -Force (Join-Path $fullR4Generated $name) (Join-Path $originGenerated $name)
}
$corePath = Join-Path $originGenerated 'coarse_exact_core.hpp'
$core = [System.IO.File]::ReadAllText($corePath)
$core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', 'static constexpr int SIZE = 1;', 1)
$core = [regex]::Replace($core, 'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;', 'static constexpr int FROM_COARSE = 0;', 1)
if ($core -notmatch 'static constexpr int SIZE = 1;' -or $core -notmatch 'static constexpr int FROM_COARSE = 0;') {
    throw 'Could not create P4 one-column origin terrain specialization.'
}
[System.IO.File]::WriteAllText($corePath, $core, [System.Text.UTF8Encoding]::new($false))
$configPath = Join-Path $originGenerated 'skyblock_p14_config.hpp'
$configText = [System.IO.File]::ReadAllText($configPath)
$configText = [regex]::Replace($configText, 'static constexpr int CHUNK_RADIUS\s*=\s*\d+\s*;', 'static constexpr int CHUNK_RADIUS = 0;', 1)
[System.IO.File]::WriteAllText($configPath, $configText, [System.Text.UTF8Encoding]::new($false))

function Compile-P4Worker(
    [string]$Source,
    [string]$GeneratedInclude,
    [string]$ExeName,
    [string]$Variant
) {
    $worker = Join-Path $nativeBuild $ExeName
    $sigFile = "$worker.signature.txt"
    $sig = Get-NativeSignature $Source $nativeSourceDir "AMD|$archKey|$api" $Variant
    # Include generated specialization contents in the cache key; Get-NativeSignature
    # intentionally only knows the canonical native headers.
    foreach ($g in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
        $p = Join-Path $GeneratedInclude $g
        $item = Get-Item $p
        $sig += "`n$($item.FullName)|$($item.Length)|$($item.LastWriteTimeUtc.Ticks)"
    }
    $old = if (Test-Path $sigFile) { Get-Content $sigFile -Raw } else { '' }
    if (-not (Test-Path $worker) -or $old -ne $sig) {
        Write-Host "Compiling $ExeName for $archKey..."
        & $hipcc -O3 -std=c++17 -x hip @archArgs $apiDefine "-I$GeneratedInclude" "-I$nativeSourceDir" $Source -o $worker
        if ($LASTEXITCODE -ne 0) {
            Remove-Item $worker -Force -ErrorAction SilentlyContinue
            throw "Compilation failed: $ExeName"
        }
        Set-Content $sigFile $sig -Encoding UTF8 -NoNewline
    } else {
        Write-Host "Using cached $ExeName."
    }
    return $worker
}

$scoutSource = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnScoutP4.cpp'
$verifySource = Join-Path $ProjectRoot 'native\floating_island_spawn\FloatingIslandSpawnVerifyP4.cpp'
if (-not (Test-Path $scoutSource -PathType Leaf)) { throw "Missing P4 scout source: $scoutSource" }
if (-not (Test-Path $verifySource -PathType Leaf)) { throw "Missing P4 verifier source: $verifySource" }

$scoutWorker = Compile-P4Worker $scoutSource $originGenerated 'FloatingIslandSpawnScoutP4_AMD.exe' 'P4|origin-only|size1|v1'
$r4Worker = Compile-P4Worker $verifySource $fullR4Generated 'FloatingIslandSpawnVerifyP4_r4_AMD.exe' 'P4|verify|r4|ground-shortcut|bulkcopy|v1'

Write-Host 'Running P4 origin-scout self-test...'
& $scoutWorker --self-test
if ($LASTEXITCODE -ne 0) { throw 'P4 origin scout self-test failed.' }
Write-Host 'Running P4 r4 verifier self-test...'
& $r4Worker --self-test
if ($LASTEXITCODE -ne 0) { throw 'P4 r4 verifier self-test failed.' }
if ($SelfTest) { Write-Host 'P4 install/self-test OK.'; exit 0 }

function Get-P4Verifier([int]$Radius) {
    if ($Radius -eq 4) { return $r4Worker }
    if ($Radius -notin @(8,12)) { throw "Unsupported P4 verifier radius: $Radius" }
    $generated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' $Radius
    $worker = Compile-P4Worker $verifySource $generated ("FloatingIslandSpawnVerifyP4_r{0}_AMD.exe" -f $Radius) ("P4|verify|r{0}|ground-shortcut|bulkcopy|v1" -f $Radius)
    Write-Host ("Running P4 r{0} verifier self-test..." -f $Radius)
    & $worker --self-test
    if ($LASTEXITCODE -ne 0) { throw ("P4 r{0} verifier self-test failed." -f $Radius) }
    return $worker
}

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

$outputRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p4'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRunFile = Join-Path $outputRoot 'LAST_RUN.txt'

if ($Resume) {
    if (-not (Test-Path $lastRunFile -PathType Leaf)) { throw 'No P4 LAST_RUN.txt exists.' }
    # Read directly in PowerShell to avoid cmd.exe BOM/path corruption.
    $output = (Get-Content -LiteralPath $lastRunFile -Raw).Trim([char]0xFEFF, [char]0x200B, ' ', "`r", "`n", "`t")
    if (-not (Test-Path $output -PathType Container)) { throw "P4 last run directory not found: $output" }
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
    if (-not (Test-Path $checkpoint -PathType Leaf)) { throw "P4 checkpoint missing: $checkpoint" }
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
if ($completed -gt $Count) { throw 'P4 checkpoint completed count exceeds requested count.' }

function Save-P4Checkpoint {
    $text = @(
        'VERSION=FloatingIslandSpawnP4Optimized',
        "START_INDEX=$StartIndex",
        "COMPLETED=$completed",
        "NEXT_INDEX=$($StartIndex + $completed)",
        "COUNT=$Count",
        "RANDOM_KEY=$runRandomKey",
        "SEED_MODE=$SeedMode",
        "MODE=$modeName"
    ) -join "`r`n"
    [System.IO.File]::WriteAllText($checkpoint, $text + "`r`n", [System.Text.Encoding]::ASCII)
}
Save-P4Checkpoint

Write-Host ''
Write-Host 'FloatingIslandSpawn P4 OPTIMIZED'
Write-Host 'Stage 1: exact ONE-COLUMN origin scout (sand -> 1/2 air -> collision push).'
Write-Host 'Stage 2: full r4 terrain only for scout hits; bulk density copy + early ground-connected rejection.'
Write-Host 'Stage 3: only genuinely unresolved horizontal-boundary cases expand to r8, then r12.'
Write-Host ("Mode={0} scoutBatch={1} scoutChunk={2} yield={3}ms threads=32" -f $modeName,$ScoutBatch,$ScoutChunk,$GpuYieldMs)
Write-Host ("SeedMode={0} randomKey={1} resumeCompleted={2}/{3}" -f $SeedMode,$runRandomKey,$completed,$Count)
Write-Host "Output=$output"

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
    Write-Host ("=== P4 SCOUT chunk start={0} count={1} ===" -f $absoluteStart,$chunkCount)
    & $scoutWorker `
        --candidate-out $candidateFile `
        --count $chunkCount.ToString($invariant) `
        --start-index $absoluteStart.ToString($invariant) `
        --random-key $runRandomKey.ToString($invariant) `
        --seed-mode $SeedMode `
        --batch $ScoutBatch.ToString($invariant) `
        --terrain-threads 32 `
        --yield-ms $GpuYieldMs.ToString($invariant) `
        --progress-ms 1000
    if ($LASTEXITCODE -ne 0) { throw 'P4 scout chunk failed.' }

    if (Has-CandidateRows $candidateFile) {
        Write-Host 'P4 exact r4 verification of compacted scout hits...'
        & $r4Worker --input $candidateFile --output $output --boundary-out $boundary4 --batch 1024 --terrain-threads 64 --top $Top
        if ($LASTEXITCODE -ne 0) { throw 'P4 r4 verification failed.' }

        if (Has-CandidateRows $boundary4) {
            Write-Host 'P4 boundary-only survivors exist; expanding only those to r8...'
            $r8Worker = Get-P4Verifier 8
            & $r8Worker --input $boundary4 --output $output --boundary-out $boundary8 --batch 256 --terrain-threads 64 --top $Top
            if ($LASTEXITCODE -ne 0) { throw 'P4 r8 verification failed.' }

            if (Has-CandidateRows $boundary8) {
                Write-Host 'P4 r8 still unresolved; expanding only those to r12...'
                $r12Worker = Get-P4Verifier 12
                & $r12Worker --input $boundary8 --output $output --boundary-out $boundary12 --batch 128 --terrain-threads 64 --top $Top
                if ($LASTEXITCODE -ne 0) { throw 'P4 r12 verification failed.' }
            }
        }
    } else {
        Write-Host 'No scout candidates in this chunk; skipping all full-window terrain work.'
    }

    $completed += $chunkCount
    Save-P4Checkpoint
    Write-Host ("P4 checkpoint completed={0}/{1}" -f $completed,$Count)
}

[System.IO.File]::WriteAllText((Join-Path $output 'COMPLETED.txt'), ("Completed {0:o}`r`n" -f (Get-Date)), [System.Text.Encoding]::ASCII)
Write-Host ''
Write-Host 'P4 SEARCH COMPLETE.'
Write-Host "RESULT_DIR=$output"
