$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$p4 = Join-Path $root 'build\floating-island-spawn-p4-optimized\native\FloatingIslandSpawnScoutP4_AMD.exe'
if (-not (Test-Path $p4 -PathType Leaf)) { throw 'P4 reference worker missing. Run P4 once first.' }

# Ensure the production P5 worker exists and is current.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\run-floating-island-spawn-p5-wave-amd.ps1') -SelfTest -MaxSpeed
if ($LASTEXITCODE -ne 0) { throw 'P5 production self-test/compile failed.' }
$p5 = Join-Path $root 'build\floating-island-spawn-p5-wave\production\FloatingIslandSpawnScoutP5_L32_U8_AMD.exe'
if (-not (Test-Path $p5 -PathType Leaf)) { throw 'P5 production worker missing after self-test.' }

[UInt64]$count = 8388608
[UInt64]$key = 4683806932147175118
$out = Join-Path $root 'out\floating_island_spawn_p5\validation_8m'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$p4csv = Join-Path $out 'p4.csv'
$p5csv = Join-Path $out 'p5.csv'
Remove-Item $p4csv,$p5csv -Force -ErrorAction SilentlyContinue

Write-Host '=== P4 exact-origin reference over 8,388,608 deterministic seeds ==='
$sw4 = [Diagnostics.Stopwatch]::StartNew()
& $p4 --candidate-out $p4csv --count $count --start-index 0 --random-key $key --seed-mode unique48 --batch 262144 --terrain-threads 32 --yield-ms 0 --progress-ms 60000
$e4 = $LASTEXITCODE
$sw4.Stop()
if ($e4 -ne 0) { throw "P4 validation run failed: $e4" }

Write-Host '=== P5 wave scout over the identical seed stream ==='
$sw5 = [Diagnostics.Stopwatch]::StartNew()
& $p5 --candidate-out $p5csv --count $count --start-index 0 --random-key $key --seed-mode unique48 --batch 262144 --yield-ms 0 --progress-ms 60000
$e5 = $LASTEXITCODE
$sw5.Stop()
if ($e5 -ne 0) { throw "P5 validation run failed: $e5" }

$a = @(Get-Content -LiteralPath $p4csv)
$b = @(Get-Content -LiteralPath $p5csv)
$exact = $a.Count -eq $b.Count
if ($exact) {
    for ($i = 0; $i -lt $a.Count; ++$i) {
        if ($a[$i] -cne $b[$i]) {
            $exact = $false
            Write-Host "FIRST MISMATCH line=$($i+1)"
            Write-Host "P4: $($a[$i])"
            Write-Host "P5: $($b[$i])"
            break
        }
    }
}
$p4n = [Math]::Max(0,$a.Count-1)
$p5n = [Math]::Max(0,$b.Count-1)
$r4 = [double]$count / $sw4.Elapsed.TotalSeconds
$r5 = [double]$count / $sw5.Elapsed.TotalSeconds
Write-Host ''
Write-Host '=== 8M VALIDATION RESULT ==='
Write-Host ("P4 candidates={0} wallRate={1:N1}/s time={2:N2}s" -f $p4n,$r4,$sw4.Elapsed.TotalSeconds)
Write-Host ("P5 candidates={0} wallRate={1:N1}/s time={2:N2}s" -f $p5n,$r5,$sw5.Elapsed.TotalSeconds)
Write-Host ("Exact CSV match: {0}" -f $exact)
Write-Host ("Speedup: {0:N2}x" -f ($r5/$r4))
if (-not $exact) { throw 'P5 FAILED exact 8M validation against P4.' }
Write-Host 'P5 PASSED exact 8M validation against P4.'
