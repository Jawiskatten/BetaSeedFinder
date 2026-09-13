$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$worker = Join-Path $ProjectRoot 'build\floating-island-spawn-p4-optimized\native\FloatingIslandSpawnScoutP4_AMD.exe'
if (-not (Test-Path $worker -PathType Leaf)) {
    throw 'P4 scout worker not found. Run the P4 optimized search/self-test once first.'
}

# Same deterministic seed stream for every configuration so candidate counts
# must match. Any mismatch means a correctness problem, not benchmark noise.
[UInt64]$count = 1048576
[UInt64]$randomKey = 4683806932147175118
$testRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p4\tuning'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null

$configs = @()
foreach ($threads in @(32,64)) {
    foreach ($batch in @(32768,65536,131072,262144)) {
        $configs += [pscustomobject]@{ Threads=$threads; Batch=$batch }
    }
}

$results = @()
$expectedCandidates = $null
foreach ($cfg in $configs) {
    $candidateFile = Join-Path $testRoot ("candidates_t{0}_b{1}.csv" -f $cfg.Threads,$cfg.Batch)
    Remove-Item $candidateFile -Force -ErrorAction SilentlyContinue

    Write-Host ''
    Write-Host ("=== P4 TUNE threads={0} batch={1} count={2} ===" -f $cfg.Threads,$cfg.Batch,$count)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $worker `
        --candidate-out $candidateFile `
        --count $count `
        --start-index 0 `
        --random-key $randomKey `
        --seed-mode unique48 `
        --batch $cfg.Batch `
        --terrain-threads $cfg.Threads `
        --yield-ms 0 `
        --progress-ms 60000
    $exit = $LASTEXITCODE
    $sw.Stop()
    if ($exit -ne 0) { throw "P4 tuning run failed: threads=$($cfg.Threads) batch=$($cfg.Batch) exit=$exit" }

    $candidateCount = 0
    if (Test-Path $candidateFile -PathType Leaf) {
        $candidateCount = @(Import-Csv -LiteralPath $candidateFile).Count
    }
    if ($null -eq $expectedCandidates) { $expectedCandidates = $candidateCount }
    elseif ($candidateCount -ne $expectedCandidates) {
        throw "Candidate mismatch: expected $expectedCandidates, got $candidateCount for threads=$($cfg.Threads) batch=$($cfg.Batch)."
    }

    $seconds = $sw.Elapsed.TotalSeconds
    $rate = if ($seconds -gt 0) { [double]$count / $seconds } else { 0.0 }
    $results += [pscustomobject]@{
        Threads = $cfg.Threads
        Batch = $cfg.Batch
        Seconds = [Math]::Round($seconds,3)
        SeedsPerSecond = [Math]::Round($rate,1)
        Candidates = $candidateCount
    }
}

Write-Host ''
Write-Host '=== P4 SCOUT TUNING RESULTS (wall-clock) ==='
$results | Sort-Object SeedsPerSecond -Descending | Format-Table -AutoSize
$best = $results | Sort-Object SeedsPerSecond -Descending | Select-Object -First 1
Write-Host ("BEST threads={0} batch={1} wallRate={2:N1} seeds/s candidates={3}" -f $best.Threads,$best.Batch,$best.SeedsPerSecond,$best.Candidates)

$csv = Join-Path $testRoot 'tuning_results.csv'
$results | Sort-Object SeedsPerSecond -Descending | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding ASCII
Write-Host "Saved: $csv"
