param(
    [ValidateRange(1,60)][int]$MinDrop = 5
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
$BranchRef = '43580fca0ff30267ab35ec14074b0f8668267d9a'
$RawBase = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$BranchRef"

$lastRun = Join-Path $ProjectRoot 'out\sand_wake_freefall_p1\LAST_RUN.txt'
if (-not (Test-Path $lastRun -PathType Leaf)) { throw 'No sand-wake LAST_RUN.txt exists.' }
$run = (Get-Content $lastRun -Raw).Trim()
if (-not (Test-Path $run -PathType Container)) { throw "Run directory missing: $run" }

$build = Join-Path $ProjectRoot 'build\sand-wake-freefall-p1'
$classes = Join-Path $build 'classes'
$oracleSrcDir = Join-Path $build 'oracle-src\net\minecraft\src'
$src = Join-Path $oracleSrcDir 'Beta173SandWakeFreefallOracleV2.java'
New-Item -ItemType Directory -Force -Path $oracleSrcDir,$classes | Out-Null
Invoke-WebRequest -UseBasicParsing "$RawBase/tools/Beta173SandWakeFreefallOracleV2.java" -OutFile $src

$javacCmd = Get-Command javac.exe -ErrorAction SilentlyContinue
if (-not $javacCmd) { $javacCmd = Get-Command javac -ErrorAction SilentlyContinue }
if (-not $javacCmd) {
    $candidates = @(
        'C:\Program Files\Java\jdk*\bin\javac.exe',
        'C:\Program Files\Eclipse Adoptium\jdk*\bin\javac.exe',
        'C:\Program Files\Microsoft\jdk*\bin\javac.exe',
        'C:\Program Files\Zulu\zulu*\bin\javac.exe',
        "$env:USERPROFILE\.jdks\*\bin\javac.exe"
    ) | ForEach-Object { Get-ChildItem -Path $_ -File -ErrorAction SilentlyContinue } | Select-Object -First 1
    if ($candidates) { $javacPath = $candidates.FullName } else { throw 'javac not found.' }
} else { $javacPath = $javacCmd.Source }
$java = Join-Path (Split-Path $javacPath -Parent) 'java.exe'
if (-not (Test-Path $java -PathType Leaf)) { throw "java.exe not found beside javac: $javacPath" }

$betaBin = Join-Path $ProjectRoot 'build\beta173-exact-client-oracle\mc_b1.7.3_release\1.7.3-LTS\bin\minecraft'
if (-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)) { throw "Beta class cache missing: $betaBin" }

Write-Host 'Compiling sand-wake V2 oracle...'
& $javacPath -source 8 -target 8 -encoding UTF-8 -cp "$classes;$betaBin" -d $classes $src
if ($LASTEXITCODE -ne 0) { throw 'V2 oracle compilation failed.' }

$outDir = Join-Path $run 'exact_client_verify_v2'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$best = Join-Path $run 'BEST_AUTHORITATIVE_SAND_WAKE_V2.txt'

$files = @(Get-ChildItem (Join-Path $run 'dry_cave_geometry') -Filter 'candidates_*.csv' -File | Sort-Object Name)
if ($files.Count -eq 0) { throw 'No candidate CSVs found in the last run.' }

Write-Host "Rechecking $($files.Count) candidate file(s) with V2 full-stack forced proof + untouched natural startup..."
foreach ($f in $files) {
    $rows = [Math]::Max(0,(Get-Content $f.FullName | Measure-Object -Line).Lines - 1)
    if ($rows -le 0) { continue }
    Write-Host "--- $($f.Name): $rows rows ---"
    & $java '-Xmx2G' '-Djava.awt.headless=true' -cp "$classes;$betaBin" net.minecraft.src.Beta173SandWakeFreefallOracleV2 `
        --input $f.FullName --output $outDir --best-state $best --min-drop $MinDrop --progress-every 1
    if ($LASTEXITCODE -ne 0) { throw "V2 oracle failed for $($f.FullName)" }
}

Write-Host ''
$summary = Join-Path $outDir 'SUMMARY_V2.txt'
if (Test-Path $summary) { Get-Content $summary }
Write-Host "V2_RESULT_DIR=$outDir"
