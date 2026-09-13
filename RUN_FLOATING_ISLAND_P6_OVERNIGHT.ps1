param(
    [UInt64]$Count = 100000000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(1,10000)][int]$Top = 500,
    [switch]$Resume,
    [switch]$SelfTestOnly,
    [int]$ScoutBatch = 262144,
    [int]$ScoutChunk = 25000000,
    [int]$GpuYieldMs = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
. (Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1')

$SourceRef = 'floating-island-spawn-p6-vanilla-spawn'
$RepoRaw = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef"

function Has-Rows([string]$Path) {
    if (-not (Test-Path $Path -PathType Leaf)) { return $false }
    $two = @(Get-Content -LiteralPath $Path -TotalCount 2)
    return $two.Count -ge 2
}
function Count-Rows([string]$Path) {
    if (-not (Test-Path $Path -PathType Leaf)) { return 0 }
    $n = 0
    foreach ($line in [System.IO.File]::ReadLines($Path)) { $n++ }
    return [Math]::Max(0, $n - 1)
}
function New-RandomUInt64 {
    $bytes = New-Object byte[] 8
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return [BitConverter]::ToUInt64($bytes, 0)
}
function Get-Signature([string[]]$Files, [string]$Extra) {
    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add($Extra)
    foreach ($f in $Files) {
        if (-not (Test-Path $f -PathType Leaf)) { throw "Signature input missing: $f" }
        $parts.Add((Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash)
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($parts -join '|'))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-','') } finally { $sha.Dispose() }
}

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$api = Get-CoarseGpuApi $nativeSourceDir
if ($api -ne 'modern') { throw 'P6 requires the current modern BetaSeedFinder GPU headers.' }

$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p6-overnight'
$srcDir = Join-Path $build 'source'
$genDir = Join-Path $build 'generated-chunk'
New-Item -ItemType Directory -Force -Path $srcDir,$genDir | Out-Null

Write-Host 'Preparing corrected P6 sources...'
$p5Downloaded = Join-Path $srcDir 'FloatingIslandSpawnScoutP5Wave.cpp'
$p5Patched = Join-Path $srcDir 'FloatingIslandSpawnScoutP6OriginFixed.cpp'
$verifySource = Join-Path $srcDir 'FloatingIslandSpawnVerifyP6ChunkExact.cpp'
Invoke-WebRequest -UseBasicParsing "$RepoRaw/native/floating_island_spawn/FloatingIslandSpawnScoutP5Wave.cpp" -OutFile $p5Downloaded
Invoke-WebRequest -UseBasicParsing "$RepoRaw/native/floating_island_spawn/FloatingIslandSpawnVerifyP6ChunkExact.cpp" -OutFile $verifySource

$p5 = [System.IO.File]::ReadAllText($p5Downloaded)
$oldSample = 'const double terrainValue = slimSimplex2(p, perm, lane, 2.0 * scale, 2.0 * scale) * weight;'
$newSample = 'const double terrainValue = slimSimplex2(p, perm, lane, 1.0 * scale, 1.0 * scale) * weight;'
if (([regex]::Matches($p5, [regex]::Escape($oldSample))).Count -ne 1) {
    throw 'Could not find the single old P5 terrain climate sample to patch.'
}
$p5 = $p5.Replace($oldSample, $newSample)
$p5 = $p5.Replace('Terrain climate is sampled at the coarse-cell center (2,2), while biome', 'Vanilla terrain density at the origin samples climate index (1,1), while biome')
[System.IO.File]::WriteAllText($p5Patched, $p5, [System.Text.UTF8Encoding]::new($false))

# Build a true per-chunk 5x17x5 Beta density lattice.  The retained vertical slice
# is nodes 7..15 plus the exact implicit top node, but X/Z are now one real chunk.
$baseGenerated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
foreach ($name in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
    Copy-Item -Force (Join-Path $baseGenerated $name) (Join-Path $genDir $name)
}
$corePath = Join-Path $genDir 'coarse_exact_core.hpp'
$gpuPath = Join-Path $genDir 'coarse_exact_gpu.hpp'
$configPath = Join-Path $genDir 'skyblock_p14_config.hpp'
$core = [System.IO.File]::ReadAllText($corePath)
$core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', 'static constexpr int SIZE = 5;', 1)
$core = [regex]::Replace($core, 'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;', 'static constexpr int FROM_COARSE = 0;', 1)
[System.IO.File]::WriteAllText($corePath, $core, [System.Text.UTF8Encoding]::new($false))

$gpu = [System.IO.File]::ReadAllText($gpuPath)
$oldCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
'@
$newCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    // Vanilla Beta 1.7.3: var16 = 16 / 5 = 3, center offset = 1.
    // For one chunk, climate indices are 1,4,7,10,13 on both axes.
    climateX = static_cast<double>(coarseOffsetX * 4 + x * 3 + 1);
    climateZ = static_cast<double>(coarseOffsetZ * 4 + z * 3 + 1);
'@
if (-not $gpu.Contains($oldCoordinates)) { throw 'Could not patch chunk-local vanilla climate coordinates.' }
$gpu = $gpu.Replace($oldCoordinates, $newCoordinates)
[System.IO.File]::WriteAllText($gpuPath, $gpu, [System.Text.UTF8Encoding]::new($false))
$config = [System.IO.File]::ReadAllText($configPath).Replace('static constexpr int CHUNK_RADIUS = 4;', 'static constexpr int CHUNK_RADIUS = 0;')
[System.IO.File]::WriteAllText($configPath, $config, [System.Text.UTF8Encoding]::new($false))

# The verifier was downloaded to build/source, so make its embedded include resolve by include path.
$verifyText = [System.IO.File]::ReadAllText($verifySource)
$verifyText = $verifyText.Replace('#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"', '#include "HighestPillarSpawnGpuFinder.cpp"')
[System.IO.File]::WriteAllText($verifySource, $verifyText, [System.Text.UTF8Encoding]::new($false))

$scoutExe = Join-Path $build 'FloatingIslandSpawnScoutP6_L32_U8_AMD.exe'
$verifyExe = Join-Path $build 'FloatingIslandSpawnVerifyP6ChunkExact_AMD.exe'
$highestDir = Join-Path $ProjectRoot 'native\highest_pillar_spawn'
$highestSource = Join-Path $highestDir 'HighestPillarSpawnGpuFinder.cpp'
$commonInputs = @(
    (Join-Path $nativeSourceDir 'gpu_runtime_compat.hpp'),
    (Join-Path $nativeSourceDir 'p20_exact_math.hpp')
)

$scoutSig = Get-Signature (@($p5Patched) + $commonInputs) "P6-scout|$archKey|L32|U8|vanilla-climate-1-1"
$scoutSigFile = "$scoutExe.signature.txt"
$oldScoutSig = if (Test-Path $scoutSigFile) { (Get-Content $scoutSigFile -Raw).Trim() } else { '' }
if (-not (Test-Path $scoutExe -PathType Leaf) -or $oldScoutSig -ne $scoutSig) {
    Write-Host "Compiling corrected P6 wave scout for $archKey..."
    & $hipcc -O3 -std=c++17 -x hip @archArgs '-DP5_LANES=32' '-DP5_PERM_U8=1' "-I$nativeSourceDir" $p5Patched -o $scoutExe | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'P6 scout compilation failed.' }
    [IO.File]::WriteAllText($scoutSigFile, $scoutSig, [Text.Encoding]::ASCII)
} else { Write-Host 'Using cached corrected P6 scout.' }

$verifySig = Get-Signature @($verifySource,$highestSource,$corePath,$gpuPath,$configPath) "P6-verifier|$archKey|chunk-local-v1"
$verifySigFile = "$verifyExe.signature.txt"
$oldVerifySig = if (Test-Path $verifySigFile) { (Get-Content $verifySigFile -Raw).Trim() } else { '' }
if (-not (Test-Path $verifyExe -PathType Leaf) -or $oldVerifySig -ne $verifySig) {
    Write-Host "Compiling P6 chunk-local verifier for $archKey..."
    & $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" "-I$highestDir" $verifySource -o $verifyExe | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'P6 chunk verifier compilation failed.' }
    [IO.File]::WriteAllText($verifySigFile, $verifySig, [Text.Encoding]::ASCII)
} else { Write-Host 'Using cached P6 chunk-local verifier.' }

Write-Host ''
Write-Host 'Running P6 safety self-tests...'
& $scoutExe --self-test
if ($LASTEXITCODE -ne 0) { throw 'Corrected P6 scout self-test failed.' }
& $verifyExe --self-test
if ($LASTEXITCODE -ne 0) { throw 'P6 chunk verifier self-test failed.' }
Write-Host 'P6 self-tests OK: known real pillar passes; known two-block-gap false positive rejects.'
if ($SelfTestOnly) { exit 0 }

$outputRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p6'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRunFile = Join-Path $outputRoot 'LAST_RUN.txt'

if ($Resume) {
    if (-not (Test-Path $lastRunFile -PathType Leaf)) { throw 'No P6 LAST_RUN.txt exists.' }
    $output = (Get-Content -LiteralPath $lastRunFile -Raw).Trim()
    if (-not (Test-Path $output -PathType Container)) { throw "Last P6 run directory missing: $output" }
} else {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output = Join-Path $outputRoot ("run_$stamp")
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    [IO.File]::WriteAllText($lastRunFile, [IO.Path]::GetFullPath($output), [Text.Encoding]::ASCII)
}
$output = [IO.Path]::GetFullPath($output)
$scoutDir = Join-Path $output 'scout_candidates'
$boundaryDir = Join-Path $output 'boundaries'
$unresolvedDir = Join-Path $output 'unresolved_huge'
New-Item -ItemType Directory -Force -Path $scoutDir,$boundaryDir,$unresolvedDir | Out-Null
$checkpoint = Join-Path $output 'checkpoint.txt'
$summaryPath = Join-Path $output 'SUMMARY.txt'

[UInt64]$completed = 0
[UInt64]$runRandomKey = 0
if ($Resume) {
    if (-not (Test-Path $checkpoint -PathType Leaf)) { throw "P6 checkpoint missing: $checkpoint" }
    foreach ($line in Get-Content -LiteralPath $checkpoint) {
        if ($line -like 'COMPLETED=*') { $completed = [UInt64]($line.Substring(10)) }
        elseif ($line -like 'RANDOM_KEY=*') { $runRandomKey = [UInt64]($line.Substring(11)) }
        elseif ($line -like 'COUNT=*') { $Count = [UInt64]($line.Substring(6)) }
        elseif ($line -like 'START_INDEX=*') { $StartIndex = [UInt64]($line.Substring(12)) }
        elseif ($line -like 'SEED_MODE=*') { $SeedMode = $line.Substring(10) }
        elseif ($line -like 'TOP=*') { $Top = [int]($line.Substring(4)) }
    }
} else {
    $runRandomKey = if ($null -ne $RandomKey) { [UInt64]$RandomKey } else { New-RandomUInt64 }
}
if ($completed -gt $Count) { throw 'Checkpoint completed exceeds Count.' }
if ($SeedMode -eq 'unique48' -and ($StartIndex -ge 281474976710656 -or $Count -gt (281474976710656 - $StartIndex))) {
    throw 'unique48 range must stay below 2^48.'
}

function Save-Checkpoint {
    $text = @(
        'VERSION=FloatingIslandSpawnP6OvernightV1',
        "START_INDEX=$StartIndex",
        "COMPLETED=$completed",
        "NEXT_INDEX=$($StartIndex + $completed)",
        "COUNT=$Count",
        "RANDOM_KEY=$runRandomKey",
        "SEED_MODE=$SeedMode",
        "TOP=$Top",
        "SCOUT_BATCH=$ScoutBatch",
        "SCOUT_CHUNK=$ScoutChunk",
        'VERIFY_STAGES=chunkR1,chunkR3,chunkR6,chunkR10'
    ) -join "`r`n"
    [IO.File]::WriteAllText($checkpoint, $text + "`r`n", [Text.Encoding]::ASCII)
}

function Preview-Board([string]$File, [int]$N = 10) {
    if (-not (Test-Path $File -PathType Leaf)) { return @('  none yet') }
    $rows = @(Import-Csv -LiteralPath $File | Select-Object -First $N)
    if ($rows.Count -eq 0) { return @('  none yet') }
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($r in $rows) {
        $out.Add(("  #{0} seed={1} blocks={2} footprint={3} span={4}x{5} Y={6}..{7} feetY={8}" -f $r.rank,$r.seed,$r.component_blocks,$r.footprint_columns,$r.span_x,$r.span_z,$r.min_y,$r.max_y,$r.player_feet_y))
    }
    return @($out)
}

function Update-Summary {
    $verified = Count-Rows (Join-Path $output 'verified_all.csv')
    $candidateCount = 0
    foreach ($f in Get-ChildItem -LiteralPath $scoutDir -Filter 'candidates_*.csv' -File -ErrorAction SilentlyContinue) { $candidateCount += Count-Rows $f.FullName }
    $unresolved = 0
    foreach ($f in Get-ChildItem -LiteralPath $unresolvedDir -Filter '*.csv' -File -ErrorAction SilentlyContinue) { $unresolved += Count-Rows $f.FullName }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('P6 OVERNIGHT FLOATING-ISLAND SEARCH SUMMARY')
    $lines.Add("Updated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    $lines.Add("Output: $output")
    $lines.Add("Processed: $completed / $Count seeds")
    $lines.Add("RandomKey: $runRandomKey   SeedMode: $SeedMode")
    $lines.Add("Raw scout candidates saved: $candidateCount")
    $lines.Add("Verified contained floating hits: $verified")
    $lines.Add("Still touching +/-10 chunk boundary (saved for inspection): $unresolved")
    $lines.Add('')
    $lines.Add('EVERY VERIFIED HIT: verified_all.csv')
    $lines.Add('EVERY RAW SCOUT HIT: scout_candidates\\candidates_*.csv')
    $lines.Add('')
    $lines.Add('TOP LARGEST')
    foreach ($x in Preview-Board (Join-Path $output 'top_largest.csv')) { $lines.Add($x) }
    $lines.Add('')
    $lines.Add('TOP 1x1')
    foreach ($x in Preview-Board (Join-Path $output 'top_1x1.csv')) { $lines.Add($x) }
    $lines.Add('')
    $lines.Add('TOP GEOMETRY OUTLIERS')
    foreach ($x in Preview-Board (Join-Path $output 'top_outlier_geometry.csv')) { $lines.Add($x) }
    $lines.Add('')
    $lines.Add('TOP HIGHEST SPAWNS')
    foreach ($x in Preview-Board (Join-Path $output 'top_highest.csv')) { $lines.Add($x) }
    [IO.File]::WriteAllLines($summaryPath, $lines, [Text.Encoding]::UTF8)
}

Save-Checkpoint
Update-Summary

Write-Host ''
Write-Host '============================================================'
Write-Host ' FLOATING ISLAND SPAWN P6 - OVERNIGHT'
Write-Host '============================================================'
Write-Host 'Corrected vanilla origin climate + chunk-local 5x17x5 terrain.'
Write-Host 'Adaptive verification: chunk radius 1 -> 3 -> 6 -> 10.'
Write-Host 'Every verified hit is appended immediately to verified_all.csv.'
Write-Host 'Leaderboards: largest, 1x1, highest, island-like, tall/thin, long/skinny, flat/wide, geometry outliers, smallest.'
Write-Host "Count=$Count  Start=$StartIndex  RandomKey=$runRandomKey  ScoutChunk=$ScoutChunk"
Write-Host "Output=$output"
Write-Host 'You can stop it in the morning with Ctrl+C. Completed chunks are checkpointed; use -Resume to continue.'
Write-Host ''

$runStart = Get-Date
$invariant = [Globalization.CultureInfo]::InvariantCulture
while ($completed -lt $Count) {
    [UInt64]$left = $Count - $completed
    [UInt64]$chunkCount = if ($left -lt [UInt64]$ScoutChunk) { $left } else { [UInt64]$ScoutChunk }
    [UInt64]$absoluteStart = $StartIndex + $completed
    $tag = $absoluteStart.ToString($invariant)
    $candidateFile = Join-Path $scoutDir ("candidates_$tag.csv")
    $b1 = Join-Path $boundaryDir ("boundary_r1_$tag.csv")
    $b3 = Join-Path $boundaryDir ("boundary_r3_$tag.csv")
    $b6 = Join-Path $boundaryDir ("boundary_r6_$tag.csv")
    $b10 = Join-Path $unresolvedDir ("unresolved_r10_$tag.csv")

    Write-Host ''
    Write-Host ("=== P6 SCOUT start={0} count={1} completed={2}/{3} ===" -f $absoluteStart,$chunkCount,$completed,$Count)
    & $scoutExe `
        --candidate-out $candidateFile `
        --count $chunkCount.ToString($invariant) `
        --start-index $absoluteStart.ToString($invariant) `
        --random-key $runRandomKey.ToString($invariant) `
        --seed-mode $SeedMode `
        --batch $ScoutBatch.ToString($invariant) `
        --yield-ms $GpuYieldMs.ToString($invariant) `
        --progress-ms 1000
    if ($LASTEXITCODE -ne 0) { throw 'P6 scout chunk failed.' }

    if (Has-Rows $candidateFile) {
        Write-Host 'P6 verify stage 1: +/-1 chunk...'
        & $verifyExe --input $candidateFile --output $output --boundary-out $b1 --chunk-radius 1 --batch 512 --terrain-threads 64 --top $Top
        if ($LASTEXITCODE -ne 0) { throw 'P6 chunkR1 verification failed.' }
        if (Has-Rows $b1) {
            Write-Host 'P6 verify stage 2: boundary survivors -> +/-3 chunks...'
            & $verifyExe --input $b1 --output $output --boundary-out $b3 --chunk-radius 3 --batch 128 --terrain-threads 64 --top $Top
            if ($LASTEXITCODE -ne 0) { throw 'P6 chunkR3 verification failed.' }
            if (Has-Rows $b3) {
                Write-Host 'P6 verify stage 3: boundary survivors -> +/-6 chunks...'
                & $verifyExe --input $b3 --output $output --boundary-out $b6 --chunk-radius 6 --batch 32 --terrain-threads 64 --top $Top
                if ($LASTEXITCODE -ne 0) { throw 'P6 chunkR6 verification failed.' }
                if (Has-Rows $b6) {
                    Write-Host 'P6 verify stage 4: huge survivors -> +/-10 chunks...'
                    & $verifyExe --input $b6 --output $output --boundary-out $b10 --chunk-radius 10 --batch 8 --terrain-threads 64 --top $Top
                    if ($LASTEXITCODE -ne 0) { throw 'P6 chunkR10 verification failed.' }
                    if (Has-Rows $b10) { Write-Host "WARNING: huge component(s) still hit +/-10 chunk boundary; saved: $b10" }
                }
            }
        }
    } else {
        Write-Host 'No P6 scout candidates in this chunk.'
    }

    $completed += $chunkCount
    Save-Checkpoint
    Update-Summary
    $elapsed = ((Get-Date) - $runStart).TotalSeconds
    $rate = if ($elapsed -gt 0) { [double]$completed / $elapsed } else { 0.0 }
    Write-Host ("P6 CHECKPOINT completed={0}/{1} averagePipelineRate={2:N0} seeds/s" -f $completed,$Count,$rate)
    Write-Host "Morning summary: $summaryPath"
}

Update-Summary
Write-Host ''
Write-Host 'P6 OVERNIGHT RUN COMPLETE.'
Write-Host "Summary: $summaryPath"
Write-Host "Every verified hit: $(Join-Path $output 'verified_all.csv')"
