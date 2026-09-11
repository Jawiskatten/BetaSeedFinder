param(
    [string]$ProjectRoot = "",
    [int[]]$SeedsPerBlock = @(1, 2, 4, 8, 16),
    [int[]]$BatchSizes = @(65536, 131072, 262144, 524288, 1048576, 2097152),
    [UInt64]$BenchmarkAttempts = 33554432,
    [int]$Repeats = 2,
    [int]$GateRadius = 384,
    [UInt64]$Sequence = 11562480669681820327,
    [string]$GpuArch = 'gfx1101',
    [switch]$Quick,
    [switch]$NoApply
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$root = (Resolve-Path $ProjectRoot).Path
$sourcePath = Join-Path $root 'native\src\single_biome_radius.cpp'
$runnerPath = Join-Path $root 'scripts\run-single-biome-radius.ps1'
$buildDir = Join-Path $root 'build\native\amd'
$tuneDir = Join-Path $buildDir 'tuning'
$productionExe = Join-Path $buildDir 'SingleBiomeRadiusFinder.exe'
$resultsCsv = Join-Path $tuneDir 'gpu-tuning-results.csv'
$summaryPath = Join-Path $tuneDir 'gpu-tuning-best.txt'

if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Source file not found: $sourcePath"
}
if (-not (Test-Path $runnerPath -PathType Leaf)) {
    throw "Runner file not found: $runnerPath"
}

New-Item -ItemType Directory -Force -Path $tuneDir | Out-Null

# P3 is required because the knobs below belong to the grouped Tundra scout.
$originalSource = [System.IO.File]::ReadAllText($sourcePath)
if (-not $originalSource.Contains('TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P3 is not applied to native\src\single_biome_radius.cpp. Run the P3 patch first.'
}
if (-not $originalSource.Contains('SQUARE_TARGET_864_V2')) {
    throw 'Exact 864x864 square target patch is not present.'
}

if ($Quick) {
    $BenchmarkAttempts = 8388608
    $Repeats = 1
    $SeedsPerBlock = @(2, 4, 8, 16)
    $BatchSizes = @(131072, 262144, 524288, 1048576)
}

if ($Repeats -lt 1) { throw '-Repeats must be at least 1.' }
if ($BenchmarkAttempts -lt 1048576) { throw '-BenchmarkAttempts is too small; use at least 1048576.' }
if ($GateRadius -lt 0 -or $GateRadius -gt 432) { throw '-GateRadius must be 0..432.' }
foreach ($spb in $SeedsPerBlock) {
    if ($spb -lt 1 -or $spb -gt 16 -or (64 % $spb) -ne 0) {
        throw "SeedsPerBlock value $spb is invalid. It must divide 64 and be <=16."
    }
    if ((64 / $spb) -lt 3) {
        throw "SeedsPerBlock=$spb leaves fewer than 3 lanes per seed, which cannot initialize climate state safely."
    }
}
foreach ($batch in $BatchSizes) {
    if ($batch -lt 1024) { throw "Batch size $batch is too small." }
}

function Import-VisualStudioEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere -PathType Leaf)) {
        throw 'Visual Studio 2022 C++ Build Tools were not found.'
    }

    $installation = (& $vswhere `
        -latest `
        -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if (-not $installation) { throw 'Visual Studio 2022 C++ Build Tools were not found.' }

    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    $tempCommand = Join-Path ([IO.Path]::GetTempPath()) ('BiomeTune-vsenv-' + [Guid]::NewGuid().ToString('N') + '.cmd')
    @(
        '@echo off'
        ('call "{0}" -no_logo -arch=x64 -host_arch=x64 >nul' -f $devCmd)
        'if errorlevel 1 exit /b 1'
        'set'
    ) | Set-Content -LiteralPath $tempCommand -Encoding ASCII

    try {
        $environment = & $tempCommand
        $exitCode = $LASTEXITCODE
    }
    finally {
        Remove-Item -LiteralPath $tempCommand -Force -ErrorAction SilentlyContinue
    }
    if ($exitCode -ne 0) { throw "Visual Studio environment setup failed: $exitCode" }

    foreach ($line in $environment) {
        if ($line -isnot [string]) { continue }
        $i = $line.IndexOf('=')
        if ($i -gt 0) {
            [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process')
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is unavailable after Visual Studio environment setup.'
    }
}

function Find-Hipcc {
    $roots = @($env:HIP_PATH, $env:HIP_SDK_DIR) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique

    foreach ($configuredRoot in $roots) {
        foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
            $candidate = Join-Path $configuredRoot ('bin\' + $name)
            if (Test-Path $candidate -PathType Leaf) { return $candidate }
        }
    }

    $base = 'C:\Program Files\AMD\ROCm'
    if (Test-Path $base -PathType Container) {
        foreach ($versionDir in (Get-ChildItem $base -Directory | Sort-Object Name -Descending)) {
            foreach ($name in @('hipcc.exe','hipcc.bat','hipcc.bin.exe','hipcc')) {
                $candidate = Join-Path $versionDir.FullName ('bin\' + $name)
                if (Test-Path $candidate -PathType Leaf) { return $candidate }
            }
        }
    }
    throw 'AMD HIP SDK hipcc was not found.'
}

function Make-TuningSource([int]$Spb, [int]$FixedGate) {
    $text = $originalSource

    $spbPattern = 'static constexpr int SEARCH_SEEDS_PER_BLOCK = \d+; // TUNDRA_P3_GROUPED_SEARCH'
    if ([regex]::Matches($text, $spbPattern).Count -ne 1) {
        throw 'Could not locate the P3 SEARCH_SEEDS_PER_BLOCK constant.'
    }
    $text = [regex]::Replace(
        $text,
        $spbPattern,
        "static constexpr int SEARCH_SEEDS_PER_BLOCK = $Spb; // TUNDRA_P3_GROUPED_SEARCH",
        1
    )

    # Benchmark the steady-state search with a fixed record gate. This avoids
    # one lucky exact record changing the workload for one variant but not another.
    $gatePattern = 'bestExact >= 0 \? std::min\(o\.target, bestExact \+ 1\) : 0,'
    if ([regex]::Matches($text, $gatePattern).Count -ne 1) {
        throw 'Could not locate the dynamic record-gate launch argument.'
    }
    $text = [regex]::Replace($text, $gatePattern, "$FixedGate,", 1)

    $text = [regex]::Replace(
        $text,
        'P3 scout: TUNDRA-only \| \d+ seeds/wave',
        "P3 scout: TUNDRA-only | $Spb seeds/block",
        1
    )
    return $text
}

function Compile-Variant([int]$Spb) {
    $variantSource = Join-Path $tuneDir ("single_biome_radius_spb{0}.cpp" -f $Spb)
    $variantExe = Join-Path $tuneDir ("SingleBiomeRadiusFinder_spb{0}.exe" -f $Spb)
    $variantLog = Join-Path $tuneDir ("compile_spb{0}.log" -f $Spb)

    $text = Make-TuningSource -Spb $Spb -FixedGate $GateRadius
    [System.IO.File]::WriteAllText($variantSource, $text, [System.Text.UTF8Encoding]::new($false))

    $args = @(
        $variantSource,
        '-O3','-std=c++17',
        ("--offload-arch={0}" -f $GpuArch),
        ('-I' + (Join-Path $root 'native\src')),
        '-ffp-contract=off','-fno-fast-math','-fno-associative-math',
        '-Wno-unused-result',
        '-o',$variantExe
    )

    Write-Host ("  compiling SPB={0}..." -f $Spb) -NoNewline
    Remove-Item $variantExe -Force -ErrorAction SilentlyContinue
    Remove-Item $variantLog -Force -ErrorAction SilentlyContinue
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $script:Hipcc @args 2>&1 | Tee-Object -FilePath $variantLog | Out-Null
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    if ($exitCode -ne 0 -or -not (Test-Path $variantExe -PathType Leaf)) {
        Write-Host ' FAILED' -ForegroundColor Red
        return $null
    }
    Write-Host ' ok' -ForegroundColor Green
    return $variantExe
}

function Run-Benchmark(
    [string]$Exe,
    [int]$Spb,
    [int]$Batch,
    [int]$Repeat,
    [string]$Phase
) {
    $logPath = Join-Path $tuneDir ("bench_{0}_spb{1}_batch{2}_r{3}.txt" -f $Phase,$Spb,$Batch,$Repeat)
    $hitLog = Join-Path $tuneDir ("hits_{0}_spb{1}_batch{2}_r{3}.csv" -f $Phase,$Spb,$Batch,$Repeat)
    Remove-Item $hitLog -Force -ErrorAction SilentlyContinue

    $args = @(
        '--target','432',
        '--batch',[string]$Batch,
        '--top-exact','0',
        '--center-x','0',
        '--center-z','0',
        '--sequence',[string]$Sequence,
        '--start-attempt','0',
        '--max-attempts',[string]$BenchmarkAttempts,
        '--status-seconds','0.20',
        '--log',$hitLog,
        '--continue-after-hit'
    )

    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & $Exe @args 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    $joined = ($output | ForEach-Object { [string]$_ }) -join "`n"
    [System.IO.File]::WriteAllText($logPath, $joined, [System.Text.UTF8Encoding]::new($false))

    if ($exitCode -ne 0) {
        return [pscustomobject]@{
            Phase=$Phase; SeedsPerBlock=$Spb; Batch=$Batch; Repeat=$Repeat; Rate=0.0; Status='run_failed'
        }
    }

    $matches = [regex]::Matches($joined, 'rate=([0-9]+(?:\.[0-9]+)?) seeds/s')
    if ($matches.Count -eq 0) {
        return [pscustomobject]@{
            Phase=$Phase; SeedsPerBlock=$Spb; Batch=$Batch; Repeat=$Repeat; Rate=0.0; Status='no_rate'
        }
    }

    $rate = [double]::Parse(
        $matches[$matches.Count - 1].Groups[1].Value,
        [Globalization.CultureInfo]::InvariantCulture
    )
    return [pscustomobject]@{
        Phase=$Phase; SeedsPerBlock=$Spb; Batch=$Batch; Repeat=$Repeat; Rate=$rate; Status='ok'
    }
}

function Average-Rate($Rows) {
    $good = @($Rows | Where-Object { $_.Status -eq 'ok' -and $_.Rate -gt 0 })
    if ($good.Count -eq 0) { return 0.0 }
    return [double](($good | Measure-Object -Property Rate -Average).Average)
}

function Format-Rate([double]$Rate) {
    return ('{0:N0}' -f $Rate)
}

Get-Process SingleBiomeRadiusFinder -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

Import-VisualStudioEnvironment
$script:Hipcc = Find-Hipcc

Write-Host
Write-Host 'SingleBiome GPU Autotuner' -ForegroundColor Cyan
Write-Host "GPU target:          $GpuArch"
Write-Host "Fixed tuning gate:   $GateRadius"
Write-Host "Attempts / test:     $BenchmarkAttempts"
Write-Host "Repeats:             $Repeats"
Write-Host "Deterministic seq:   $Sequence"
Write-Host

$allRows = New-Object System.Collections.Generic.List[object]

if (Test-Path $productionExe -PathType Leaf) {
    Write-Host 'Warming GPU...' -NoNewline
    $warmArgs = @('--target','432','--batch','262144','--top-exact','0','--sequence',[string]$Sequence,
                  '--max-attempts','2097152','--status-seconds','0.20','--continue-after-hit',
                  '--log',(Join-Path $tuneDir 'warmup-hits.csv'))
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $productionExe @warmArgs *> $null
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }
    Write-Host ' done'
}

Write-Host
Write-Host 'PHASE 1/2 - seeds packed per 64-thread block' -ForegroundColor Cyan
$baselineBatch = 262144
$spbScores = @()
foreach ($spb in $SeedsPerBlock) {
    $exe = Compile-Variant -Spb $spb
    if ($null -eq $exe) {
        $spbScores += [pscustomobject]@{ SeedsPerBlock=$spb; AvgRate=0.0; Exe=$null }
        continue
    }

    $rows = @()
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        Write-Host ("  SPB={0} repeat {1}/{2}..." -f $spb,$repeat,$Repeats) -NoNewline
        $row = Run-Benchmark -Exe $exe -Spb $spb -Batch $baselineBatch -Repeat $repeat -Phase 'spb'
        $rows += $row
        $allRows.Add($row)
        if ($row.Status -eq 'ok') {
            Write-Host (" {0} seeds/s" -f (Format-Rate $row.Rate))
        } else {
            Write-Host (" {0}" -f $row.Status) -ForegroundColor Red
        }
    }
    $avg = Average-Rate $rows
    $spbScores += [pscustomobject]@{ SeedsPerBlock=$spb; AvgRate=$avg; Exe=$exe }
    Write-Host ("    average: {0} seeds/s" -f (Format-Rate $avg)) -ForegroundColor DarkGray
}

$bestSpbRow = $spbScores | Where-Object { $_.AvgRate -gt 0 } | Sort-Object AvgRate -Descending | Select-Object -First 1
if ($null -eq $bestSpbRow) { throw 'Every SeedsPerBlock variant failed.' }
$bestSpb = [int]$bestSpbRow.SeedsPerBlock
$bestExe = [string]$bestSpbRow.Exe

Write-Host
Write-Host ("Best packing: {0} seeds/block @ {1} seeds/s" -f $bestSpb,(Format-Rate $bestSpbRow.AvgRate)) -ForegroundColor Green

Write-Host
Write-Host 'PHASE 2/2 - batch size' -ForegroundColor Cyan
$batchScores = @()
$batchRepeatBase = 100
foreach ($batch in $BatchSizes) {
    $rows = @()
    for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
        Write-Host ("  batch={0} repeat {1}/{2}..." -f $batch,$repeat,$Repeats) -NoNewline
        $row = Run-Benchmark -Exe $bestExe -Spb $bestSpb -Batch $batch -Repeat ($batchRepeatBase + $repeat) -Phase 'batch'
        $rows += $row
        $allRows.Add($row)
        if ($row.Status -eq 'ok') {
            Write-Host (" {0} seeds/s" -f (Format-Rate $row.Rate))
        } else {
            Write-Host (" {0}" -f $row.Status) -ForegroundColor Red
        }
    }
    $avg = Average-Rate $rows
    $batchScores += [pscustomobject]@{ Batch=$batch; AvgRate=$avg }
    Write-Host ("    average: {0} seeds/s" -f (Format-Rate $avg)) -ForegroundColor DarkGray
}

$bestBatchRow = $batchScores | Where-Object { $_.AvgRate -gt 0 } | Sort-Object AvgRate -Descending | Select-Object -First 1
if ($null -eq $bestBatchRow) { throw 'Every batch-size benchmark failed.' }
$bestBatch = [int]$bestBatchRow.Batch
$bestRate = [double]$bestBatchRow.AvgRate

$allRows | Export-Csv -LiteralPath $resultsCsv -NoTypeInformation -Encoding UTF8

$summary = @(
    'SingleBiome GPU tuning result'
    ('Date:               ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
    ('GPU arch:           ' + $GpuArch)
    ('Seeds/block:        ' + $bestSpb)
    ('Batch:              ' + $bestBatch)
    ('Average rate:       ' + (Format-Rate $bestRate) + ' seeds/s')
    ('Benchmark attempts: ' + $BenchmarkAttempts)
    ('Repeats:            ' + $Repeats)
    ('Gate radius:        ' + $GateRadius)
    ('Sequence:           ' + $Sequence)
    ('Results CSV:        ' + $resultsCsv)
)
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host
Write-Host 'WINNER' -ForegroundColor Cyan
Write-Host ("  seeds/block = {0}" -f $bestSpb) -ForegroundColor Green
Write-Host ("  batch       = {0}" -f $bestBatch) -ForegroundColor Green
Write-Host ("  rate        = {0} seeds/s" -f (Format-Rate $bestRate)) -ForegroundColor Green

if (-not $NoApply) {
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $tuneDir 'single_biome_radius.before_tune.cpp') -Force
    Copy-Item -LiteralPath $runnerPath -Destination (Join-Path $tuneDir 'run-single-biome-radius.before_tune.ps1') -Force

    $applied = $originalSource
    $spbPattern = 'static constexpr int SEARCH_SEEDS_PER_BLOCK = \d+; // TUNDRA_P3_GROUPED_SEARCH'
    $applied = [regex]::Replace(
        $applied,
        $spbPattern,
        "static constexpr int SEARCH_SEEDS_PER_BLOCK = $bestSpb; // TUNDRA_P3_GROUPED_SEARCH",
        1
    )
    $applied = [regex]::Replace(
        $applied,
        'P3 scout: TUNDRA-only \| \d+ seeds/wave',
        "P3 scout: TUNDRA-only | $bestSpb seeds/block",
        1
    )
    [System.IO.File]::WriteAllText($sourcePath, $applied, [System.Text.UTF8Encoding]::new($false))

    $runnerText = [System.IO.File]::ReadAllText($runnerPath)
    $batchPattern = '\[int\]\$Batch\s*=\s*\d+'
    if ([regex]::Matches($runnerText, $batchPattern).Count -ge 1) {
        $runnerText = [regex]::Replace($runnerText, $batchPattern, "[int]`$Batch = $bestBatch", 1)
        [System.IO.File]::WriteAllText($runnerPath, $runnerText, [System.Text.UTF8Encoding]::new($false))
    }

    # Tuning binaries use a fixed benchmark gate, so rebuild production from the
    # selected source to restore the real dynamic record gate.
    Write-Host
    Write-Host 'Applying winner and rebuilding production executable...' -ForegroundColor Cyan
    & (Join-Path $root 'scripts\build-single-biome-radius.ps1') -ProjectRoot $root

    Write-Host
    Write-Host 'Applied. Normal runs now use the tuned defaults.' -ForegroundColor Green
} else {
    Write-Host
    Write-Host '-NoApply was set, so source/default batch were left unchanged.' -ForegroundColor Yellow
}

Write-Host "Results: $resultsCsv"
Write-Host "Summary: $summaryPath"
Write-Host
Write-Host 'Start the tuned finder with:'
Write-Host '  .\scripts\run-single-biome-radius.ps1'
