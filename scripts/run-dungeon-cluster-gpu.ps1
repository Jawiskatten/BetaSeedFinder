param(
    [UInt64]$Start = 0,
    [UInt64]$Count = 1000000,
    [int]$Batch = 2048,
    [int]$Threads = 8,
    [int]$ChunkX = 0,
    [int]$ChunkZ = 0,
    [int]$MinDungeons = 2,
    [int]$Top = 100,
    [string]$Arch = 'gfx1101',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

$gpuExe = Join-Path $root 'build\native\amd\DungeonClusterGpuScout.exe'
if ($Rebuild -or -not (Test-Path $gpuExe -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'build-dungeon-cluster-gpu.ps1') -ProjectRoot $root -Arch $Arch | Out-Host
}
if (-not (Test-Path $gpuExe -PathType Leaf)) {
    throw "GPU scout executable missing after build: $gpuExe"
}

$requestedBatch = $Batch
$maxSafeBatch = 4096
if ($Batch -gt $maxSafeBatch) {
    Write-Warning ("Batch {0} is too large for the V2 display-GPU kernel; clamping to {1} to avoid Windows TDR/driver reset." -f $Batch,$maxSafeBatch)
    $Batch = $maxSafeBatch
}
if ($Batch -lt 1) { throw '-Batch must be >= 1' }

if ($Rebuild -or -not (Test-Path 'build\java\classes\beta173\DungeonClusterFinder173.class')) {
    & (Join-Path $PSScriptRoot 'build-java.ps1') -ProjectRoot $root | Out-Null
}

$stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$outDir = Join-Path $root ("out\dungeon_cluster_gpu\run_{0}" -f $stamp)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$candidates = Join-Path $outDir 'gpu_candidates.csv'
$exact = Join-Path $outDir 'exact_connected_clusters.csv'

Write-Host ''
Write-Host 'Dungeon Cluster GPU Pipeline V2' -ForegroundColor Cyan
Write-Host 'Stage 0: GPU no-lake failure-prefix two-block cave-doorway scout.'
Write-Host 'Stage 1: exact Java BetaChunk173 + caves + lakes + sequential dungeon generation.'
Write-Host 'IMPORTANT: V2 coverage is limited to population streams with no pre-dungeon lake trigger.' -ForegroundColor Yellow
Write-Host ("start={0} count={1} batch={2} requestedBatch={3} chunk=({4},{5})" -f $Start,$Count,$Batch,$requestedBatch,$ChunkX,$ChunkZ)
Write-Host 'Display-GPU safety: each HIP dispatch is capped at 4096 seeds to avoid Windows TDR.' -ForegroundColor DarkYellow
Write-Host "Output=$outDir"
Write-Host ''

$gpuArgs = @('--candidate-out',$candidates,'--start',$Start.ToString(),'--count',$Count.ToString(),'--batch',$Batch.ToString(),'--chunk-x',$ChunkX.ToString(),'--chunk-z',$ChunkZ.ToString(),'--progress-ms','1000')
$sw = [Diagnostics.Stopwatch]::StartNew()
& $gpuExe @gpuArgs
if ($LASTEXITCODE -ne 0) { throw "Dungeon GPU scout exited with code $LASTEXITCODE" }
$sw.Stop()
$gpuSeconds = $sw.Elapsed.TotalSeconds

$lineCount = 0
if (Test-Path $candidates -PathType Leaf) { $lineCount = @(Get-Content -LiteralPath $candidates).Count }
$candidateCount = [Math]::Max(0, $lineCount - 1)
$candidateRate = if ($Count -eq 0) { 0.0 } else { 100.0 * $candidateCount / [double]$Count }

Write-Host ''
Write-Host ("GPU stage complete: candidates={0}/{1} ({2:N4}%) wall={3:N2}s" -f $candidateCount,$Count,$candidateRate,$gpuSeconds)

if ($candidateCount -eq 0) {
    Write-Host 'No GPU candidates; exact verifier skipped.'
    Write-Host "RESULT_DIR=$outDir"
    exit 0
}

Write-Host ''
Write-Host 'Exact-verifying GPU survivors...' -ForegroundColor Cyan
$verifyArgs = @('-cp','build/java/classes','beta173.DungeonClusterFinder173','--seed-file',$candidates,'--threads',$Threads.ToString(),'--chunk-x',$ChunkX.ToString(),'--chunk-z',$ChunkZ.ToString(),'--min-dungeons',$MinDungeons.ToString(),'--top',$Top.ToString(),'--csv',$exact)
$verifySw = [Diagnostics.Stopwatch]::StartNew()
& java @verifyArgs
if ($LASTEXITCODE -ne 0) { throw "Exact dungeon candidate verification exited with code $LASTEXITCODE" }
$verifySw.Stop()

$summary = @(
    'DungeonClusterGpuPipelineV2'
    "start=$Start"
    "count=$Count"
    "gpu_candidates=$candidateCount"
    "gpu_candidate_rate_percent=$candidateRate"
    "gpu_seconds=$gpuSeconds"
    "requested_batch=$requestedBatch"
    "effective_batch=$Batch"
    "verify_seconds=$($verifySw.Elapsed.TotalSeconds)"
    "chunk_x=$ChunkX"
    "chunk_z=$ChunkZ"
    'coverage=no-lake-trigger two-block cave-doorway first-success necessary-condition scout'
) -join [Environment]::NewLine
[IO.File]::WriteAllText((Join-Path $outDir 'summary.txt'), $summary + [Environment]::NewLine, [Text.Encoding]::ASCII)

Write-Host ''
Write-Host ("PIPELINE COMPLETE gpu={0:N2}s exact={1:N2}s" -f $gpuSeconds,$verifySw.Elapsed.TotalSeconds)
Write-Host "CANDIDATES=$candidates"
Write-Host "EXACT_RESULTS=$exact"
Write-Host "RESULT_DIR=$outDir"
