$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$worker = Join-Path $ProjectRoot 'build\floating-island-spawn-p4-optimized\native\FloatingIslandSpawnScoutP4_AMD.exe'
if (-not (Test-Path $worker -PathType Leaf)) {
    throw 'P4 scout worker not found. Run the P4 optimized search/self-test once first.'
}

# Stabilize measurements over 4M deterministic seeds and only test the winning
# RDNA3 wave size. Candidate counts must match exactly across every batch size.
[UInt64]$count = 4194304
[UInt64]$randomKey = 4683806932147175118
$threads = 32
$batches = @(131072,262144,524288,1048576)
$testRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p4\tuning_batch_ceiling'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

$results = @()
$expectedCandidates = $null
foreach ($batch in $batches) {
    $candidateFile = Join-Path $testRoot ("candidates_t32_b{0}.csv" -f $batch)
    Remove-Item $candidateFile -Force -ErrorAction SilentlyContinue

    Write-Host ''
    Write-Host ("=== P4 BATCH CEILING threads=32 batch={0} count={1} ===" -f $batch,$count)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $worker `
        --candidate-out $candidateFile `
        --count $count `
        --start-index 0 `
        --random-key $randomKey `
        --seed-mode unique48 `
        --batch $batch `
        --terrain-threads $threads `
        --yield-ms 0 `
        --progress-ms 60000
    $exit = $LASTEXITCODE
    $sw.Stop()
    if ($exit -ne 0) { throw "P4 batch-ceiling run failed: batch=$batch exit=$exit" }

    $candidateCount = if (Test-Path $candidateFile -PathType Leaf) { @(Import-Csv -LiteralPath $candidateFile).Count } else { 0 }
    if ($null -eq $expectedCandidates) { $expectedCandidates = $candidateCount }
    elseif ($candidateCount -ne $expectedCandidates) {
        throw "Candidate mismatch: expected $expectedCandidates, got $candidateCount for batch=$batch."
    }

    $seconds = $sw.Elapsed.TotalSeconds
    $rate = if ($seconds -gt 0) { [double]$count / $seconds } else { 0.0 }
    $results += [pscustomobject]@{
        Threads = $threads
        Batch = $batch
        Seconds = [Math]::Round($seconds,3)
        SeedsPerSecond = [Math]::Round($rate,1)
        Candidates = $candidateCount
    }
}

Write-Host ''
Write-Host '=== P4 SCOUT BATCH CEILING RESULTS (wall-clock) ==='
$results | Sort-Object SeedsPerSecond -Descending | Format-Table -AutoSize
$best = $results | Sort-Object SeedsPerSecond -Descending | Select-Object -First 1
Write-Host ("BEST threads=32 batch={0} wallRate={1:N1} seeds/s candidates={2}" -f $best.Batch,$best.SeedsPerSecond,$best.Candidates)

$csv = Join-Path $testRoot 'batch_ceiling_results.csv'
$results | Sort-Object SeedsPerSecond -Descending | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding ASCII
Write-Host "Saved: $csv"
