$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1')

$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })

$sourceDir = Join-Path $ProjectRoot 'native\floating_island_spawn'
New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
$source = Join-Path $sourceDir 'FloatingIslandSpawnScoutP5Wave.cpp'
$raw = 'https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/floating-island-spawn-p5-wave/native/floating_island_spawn/FloatingIslandSpawnScoutP5Wave.cpp?v=p5wave1'
Write-Host 'Downloading current P5 wave scout source...'
Invoke-WebRequest -UseBasicParsing $raw -OutFile $source

$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p5-wave'
$outRoot = Join-Path $ProjectRoot 'out\floating_island_spawn_p5\wave_benchmark'
New-Item -ItemType Directory -Force -Path $build,$outRoot | Out-Null

$reference = Join-Path $ProjectRoot 'build\floating-island-spawn-p4-optimized\native\FloatingIslandSpawnScoutP4_AMD.exe'
if (-not (Test-Path $reference -PathType Leaf)) {
    throw 'P4 reference scout not found. Run the P4 optimized search once first.'
}

$configs = @(
    [pscustomobject]@{Name='L32_U32'; Lanes=32; Perm='u32'; Define=''},
    [pscustomobject]@{Name='L32_U16'; Lanes=32; Perm='u16'; Define='-DP5_PERM_U16=1'},
    [pscustomobject]@{Name='L32_U8';  Lanes=32; Perm='u8';  Define='-DP5_PERM_U8=1'},
    [pscustomobject]@{Name='L64_U16'; Lanes=64; Perm='u16'; Define='-DP5_PERM_U16=1'},
    [pscustomobject]@{Name='L64_U8';  Lanes=64; Perm='u8';  Define='-DP5_PERM_U8=1'}
)

foreach ($cfg in $configs) {
    $exe = Join-Path $build ("FloatingIslandSpawnScoutP5_{0}_AMD.exe" -f $cfg.Name)
    Write-Host ("Compiling P5 {0} for {1}..." -f $cfg.Name,$archKey)
    $defs = @("-DP5_LANES=$($cfg.Lanes)")
    if ($cfg.Define) { $defs += $cfg.Define }
    & $hipcc -O3 -std=c++17 -x hip @archArgs @defs "-I$nativeSourceDir" $source -o $exe
    if ($LASTEXITCODE -ne 0) {
        Remove-Item $exe -Force -ErrorAction SilentlyContinue
        throw "P5 compilation failed: $($cfg.Name)"
    }
    $cfg | Add-Member -NotePropertyName Exe -NotePropertyValue $exe
    Write-Host ("Self-test P5 {0}..." -f $cfg.Name)
    & $exe --self-test
    if ($LASTEXITCODE -ne 0) { throw "P5 self-test failed: $($cfg.Name)" }
}

# Same stream used by the previous P4 tuning benchmark. It is known to contain
# 89 P4 candidates. P5 must match the exact CSV rows, not merely the count.
[UInt64]$count = 1048576
[UInt64]$randomKey = 4683806932147175118
[int]$batch = 262144
$refCsv = Join-Path $outRoot 'reference_p4.csv'
Remove-Item $refCsv -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== Building exact P4 reference candidate set ==='
$refSw = [System.Diagnostics.Stopwatch]::StartNew()
& $reference `
    --candidate-out $refCsv `
    --count $count `
    --start-index 0 `
    --random-key $randomKey `
    --seed-mode unique48 `
    --batch $batch `
    --terrain-threads 32 `
    --yield-ms 0 `
    --progress-ms 60000
$refExit = $LASTEXITCODE
$refSw.Stop()
if ($refExit -ne 0) { throw 'P4 reference run failed.' }
$refLines = @(Get-Content -LiteralPath $refCsv)
$refCandidates = [Math]::Max(0, $refLines.Count - 1)
if ($refCandidates -ne 89) {
    Write-Warning "Expected 89 candidates from the known P4 stream, got $refCandidates. Exact row comparison will still be authoritative."
}

$results = @()
foreach ($cfg in $configs) {
    $csv = Join-Path $outRoot ("p5_{0}.csv" -f $cfg.Name)
    Remove-Item $csv -Force -ErrorAction SilentlyContinue
    Write-Host ''
    Write-Host ("=== P5 WAVE {0}: lanes={1} perm={2} count={3} ===" -f $cfg.Name,$cfg.Lanes,$cfg.Perm,$count)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $cfg.Exe `
        --candidate-out $csv `
        --count $count `
        --start-index 0 `
        --random-key $randomKey `
        --seed-mode unique48 `
        --batch $batch `
        --yield-ms 0 `
        --progress-ms 60000
    $exit = $LASTEXITCODE
    $sw.Stop()
    if ($exit -ne 0) { throw "P5 run failed: $($cfg.Name)" }

    $lines = @(Get-Content -LiteralPath $csv)
    if ($lines.Count -ne $refLines.Count) {
        throw "P5 correctness failure $($cfg.Name): line count $($lines.Count) != P4 $($refLines.Count)."
    }
    for ($i = 0; $i -lt $refLines.Count; ++$i) {
        if ($lines[$i] -ne $refLines[$i]) {
            throw ("P5 correctness failure {0} at CSV line {1}.`nP4: {2}`nP5: {3}" -f $cfg.Name,($i+1),$refLines[$i],$lines[$i])
        }
    }

    $seconds = $sw.Elapsed.TotalSeconds
    $rate = [double]$count / $seconds
    $results += [pscustomobject]@{
        Variant = $cfg.Name
        Lanes = $cfg.Lanes
        Perm = $cfg.Perm
        Seconds = [Math]::Round($seconds,3)
        SeedsPerSecond = [Math]::Round($rate,1)
        Candidates = $lines.Count - 1
        ExactMatchP4 = $true
    }
}

Write-Host ''
Write-Host '=== P5 WAVE RESULTS ==='
Write-Host ("P4 reference wall rate: {0:N1} seeds/s" -f ([double]$count / $refSw.Elapsed.TotalSeconds))
$results | Sort-Object SeedsPerSecond -Descending | Format-Table -AutoSize
$best = $results | Sort-Object SeedsPerSecond -Descending | Select-Object -First 1
Write-Host ("BEST P5 variant={0} rate={1:N1} seeds/s exactMatchP4={2}" -f $best.Variant,$best.SeedsPerSecond,$best.ExactMatchP4)
Write-Host ("Speedup vs this P4 reference run: {0:N2}x" -f ($best.SeedsPerSecond / ([double]$count / $refSw.Elapsed.TotalSeconds)))

$results | Sort-Object SeedsPerSecond -Descending | Export-Csv -LiteralPath (Join-Path $outRoot 'p5_wave_results.csv') -NoTypeInformation -Encoding ASCII
Write-Host "Saved: $outRoot"
