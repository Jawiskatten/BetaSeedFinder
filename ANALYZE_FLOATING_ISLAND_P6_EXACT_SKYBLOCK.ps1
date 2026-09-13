param(
    [string]$RunDir = '',
    [ValidateRange(3,8)][int]$ChunkRadius = 4,
    [ValidateRange(16,120)][int]$IsolationRadius = 48,
    [ValidateRange(0,1000)][int]$LiquidTicks = 96,
    [ValidateRange(0,1000000)][int]$MaxSeeds = 0,
    [ValidateRange(1,1000)][int]$ProgressEvery = 10
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($RunDir)) {
    $last = Join-Path $ProjectRoot 'out\floating_island_spawn_p6\LAST_RUN.txt'
    if (-not (Test-Path $last -PathType Leaf)) { throw 'No P6 LAST_RUN.txt found. Pass -RunDir explicitly.' }
    $RunDir = (Get-Content -LiteralPath $last -Raw).Trim()
}
$RunDir = [IO.Path]::GetFullPath($RunDir)
$input = Join-Path $RunDir 'verified_all.csv'
if (-not (Test-Path $input -PathType Leaf)) { throw "verified_all.csv not found: $input" }
if ($IsolationRadius -gt ($ChunkRadius * 16 - 8)) {
    throw "IsolationRadius=$IsolationRadius is too large for ChunkRadius=$ChunkRadius. Max is $($ChunkRadius*16-8)."
}

$build = Join-Path $ProjectRoot 'build\beta173-exact-skyblock-oracle'
$mcRepo = Join-Path $build 'mc_b1.7.3_release'
$classes = Join-Path $build 'classes'
$srcRoot = Join-Path $build 'oracle-src'
$srcDir = Join-Path $srcRoot 'net\minecraft\src'
$javaSrc = Join-Path $srcDir 'Beta173SkyblockOracle.java'
New-Item -ItemType Directory -Force -Path $build,$classes,$srcDir | Out-Null

function Find-Javac {
    $cmd = Get-Command javac.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cmd = Get-Command javac -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:JAVA_HOME) { $candidates.Add((Join-Path $env:JAVA_HOME 'bin\javac.exe')) }
    foreach ($pattern in @(
        'C:\Program Files\Java\jdk*\bin\javac.exe',
        'C:\Program Files\Eclipse Adoptium\jdk*\bin\javac.exe',
        'C:\Program Files\Microsoft\jdk*\bin\javac.exe',
        'C:\Program Files\Zulu\zulu*\bin\javac.exe',
        'C:\Program Files\JetBrains\*\jbr\bin\javac.exe',
        "$env:USERPROFILE\.jdks\*\bin\javac.exe"
    )) {
        foreach ($f in Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue) { $candidates.Add($f.FullName) }
    }
    foreach ($c in $candidates) { if (Test-Path $c -PathType Leaf) { return $c } }
    return $null
}

if (-not (Test-Path (Join-Path $mcRepo '.git'))) {
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
    if (-not $git) { throw 'Git is required once to fetch the compiled Beta 1.7.3 server classes.' }
    Write-Host 'Fetching compiled Beta 1.7.3 server world-generator classes (one-time cache)...'
    & $git.Source clone --depth 1 --filter=blob:none --sparse 'https://github.com/jacobo-mc/mc_b1.7.3_release.git' $mcRepo
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone mc_b1.7.3_release.' }
    Push-Location $mcRepo
    try {
        & $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft_server'
        if ($LASTEXITCODE -ne 0) { throw 'Failed to sparse-checkout Beta server classes.' }
    } finally { Pop-Location }
}

$betaBin = Join-Path $mcRepo '1.7.3-LTS\bin\minecraft_server'
if (-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)) {
    throw "Beta server class cache is incomplete: $betaBin"
}

$ref = 'e669609f189ba6966bd81060bfa5e660bd2fde48'
$raw = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$ref/tools/Beta173SkyblockOracle.java"
Write-Host 'Downloading exact population oracle source...'
Invoke-WebRequest -UseBasicParsing $raw -OutFile $javaSrc

$javac = Find-Javac
if (-not $javac) {
    throw @'
Could not find javac (a JDK compiler). The oracle runs against the real Beta classes and needs to compile one small Java helper.
Install/use any JDK 8+ or set JAVA_HOME to a JDK, then rerun this same command. Your existing analysis data is untouched.
'@
}
$java = Join-Path (Split-Path $javac -Parent) 'java.exe'
if (-not (Test-Path $java -PathType Leaf)) { throw "java.exe not found beside javac: $javac" }

Write-Host "Compiling exact Beta 1.7.3 oracle with $javac ..."
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $javaSrc
if ($LASTEXITCODE -ne 0) { throw 'Beta173SkyblockOracle.java compilation failed.' }

# The MCP class directory intentionally contains only .class files. Beta's Block/Stat
# bootstrap initializes StringTranslate, which unconditionally opens these two resources.
# The dedicated server normally gets them from minecraft_server.jar; our sparse class
# directory does not. Empty files are sufficient because the oracle never needs localized
# strings, and Properties falls back to the untranslated key.
$langDir = Join-Path $classes 'lang'
New-Item -ItemType Directory -Force -Path $langDir | Out-Null
[IO.File]::WriteAllText((Join-Path $langDir 'en_US.lang'), '')
[IO.File]::WriteAllText((Join-Path $langDir 'stats_US.lang'), '')

$out = Join-Path $RunDir 'exact_skyblock_analysis_v2'
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host ''
Write-Host '=================================================================='
Write-Host ' EXACT BETA 1.7.3 SKYBLOCK / TREE / WATER / LAVA ANALYSIS V2'
Write-Host '=================================================================='
Write-Host 'Actual Beta 1.7.3 terrain/population is run in memory.'
Write-Host 'Primary outputs remain deliberately SEPARATE:'
Write-Host '  tree_spawns.csv'
Write-Host '  tree_at_0_0.csv'
Write-Host '  good_skyblock_islands.csv'
Write-Host '  tiny_skyblock_islands.csv'
Write-Host '  waterfalls.csv'
Write-Host '  lavafalls.csv'
Write-Host '  water_and_lava_falls.csv'
Write-Host ''
Write-Host 'V2 fixes the missing /lang resources that caused BiomeGenBase initialization to fail.'
Write-Host 'It also performs a one-seed preflight before touching the full dataset.'
Write-Host "Input=$input"
Write-Host "Output=$out"
Write-Host "ChunkRadius=$ChunkRadius IsolationRadius=$IsolationRadius LiquidTicks=$LiquidTicks MaxSeeds=$MaxSeeds"
Write-Host ''

$cp = "$classes;$betaBin"

# Fail fast on JVM/bootstrap/classpath problems instead of silently producing empty lists.
$preflight = Join-Path $build 'preflight-output-v2'
Remove-Item -Recurse -Force $preflight -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $preflight | Out-Null
Write-Host 'Running one-seed exact-generation preflight...'
& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173SkyblockOracle `
    --input $input `
    --output $preflight `
    --chunk-radius $ChunkRadius `
    --isolation-radius $IsolationRadius `
    --liquid-ticks $LiquidTicks `
    --max-seeds 1 `
    --progress-every 1
if ($LASTEXITCODE -ne 0) { throw 'Exact Beta SkyBlock oracle preflight process failed.' }
$preflightMaster = Join-Path $preflight 'actual_features_all.csv'
if (-not (Test-Path $preflightMaster -PathType Leaf)) { throw 'Preflight did not create actual_features_all.csv.' }
$preflightRow = Import-Csv -LiteralPath $preflightMaster | Select-Object -First 1
if ($null -eq $preflightRow) { throw 'Preflight produced no result row.' }
if ($preflightRow.status -ne 'OK') {
    throw "Exact Beta preflight failed: status=$($preflightRow.status) seed=$($preflightRow.seed) error=$($preflightRow.error)"
}
Write-Host "PRECHECK OK seed=$($preflightRow.seed) floating=$($preflightRow.floating) foot=$($preflightRow.footprint) tree=$($preflightRow.tree_count) water=$($preflightRow.waterfall) lava=$($preflightRow.lavafall)"
Remove-Item -Recurse -Force $preflight -ErrorAction SilentlyContinue
Write-Host ''

& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173SkyblockOracle `
    --input $input `
    --output $out `
    --chunk-radius $ChunkRadius `
    --isolation-radius $IsolationRadius `
    --liquid-ticks $LiquidTicks `
    --max-seeds $MaxSeeds `
    --progress-every $ProgressEvery
if ($LASTEXITCODE -ne 0) { throw 'Exact Beta SkyBlock oracle failed.' }

Write-Host ''
$master = Join-Path $out 'actual_features_all.csv'
if (Test-Path $master -PathType Leaf) {
    $rows = Import-Csv -LiteralPath $master
    Write-Host 'STATUS COUNTS:'
    $rows | Group-Object status | Sort-Object Count -Descending | Format-Table Count,Name -AutoSize
    $okCount = @($rows | Where-Object status -eq 'OK').Count
    if ($okCount -eq 0) { throw 'Oracle finished with zero OK rows; refusing to present empty feature lists as real results.' }
}

$summary = Join-Path $out 'EXACT_SUMMARY.txt'
if (Test-Path $summary) { Get-Content -LiteralPath $summary }
Write-Host ''
Write-Host 'Top exact tree-at-origin results:'
$tree0 = Join-Path $out 'tree_at_0_0.csv'
if (Test-Path $tree0) {
    Import-Csv $tree0 | Select-Object -First 15 seed,actual_feet_y,component_blocks,footprint,span_x,span_z,nearest_elevated,tree_count | Format-Table -AutoSize
}
Write-Host 'Top exact SkyBlock islands:'
$sky = Join-Path $out 'good_skyblock_islands.csv'
if (Test-Path $sky) {
    Import-Csv $sky | Select-Object -First 15 seed,component_blocks,footprint,span_x,span_z,min_y,max_y,nearest_elevated,skyblock_score | Format-Table -AutoSize
}
Write-Host 'Top exact waterfalls:'
$water = Join-Path $out 'waterfalls.csv'
if (Test-Path $water) {
    Import-Csv $water | Select-Object -First 15 seed,water_drop,water_x,water_y,water_z,component_blocks,footprint,nearest_elevated | Format-Table -AutoSize
}
Write-Host 'Top exact lavafalls:'
$lava = Join-Path $out 'lavafalls.csv'
if (Test-Path $lava) {
    Import-Csv $lava | Select-Object -First 15 seed,lava_drop,lava_x,lava_y,lava_z,component_blocks,footprint,nearest_elevated | Format-Table -AutoSize
}