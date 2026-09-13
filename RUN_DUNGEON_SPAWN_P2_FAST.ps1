param(
    [UInt64]$Count = 100000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(100000,1000000000)][UInt64]$ScoutChunk = 25000000,
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
$BranchRef = '7bb55250801c2fcdb148d2b03361c8836746a50a'
$RawBase = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$BranchRef"

$helper = Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1'
if (-not (Test-Path $helper -PathType Leaf)) {
    $helperDir = Split-Path $helper -Parent
    New-Item -ItemType Directory -Force -Path $helperDir | Out-Null
    Invoke-WebRequest -UseBasicParsing "$RawBase/scripts/cursed-spawn-origin-p1-common.ps1" -OutFile $helper
}
. $helper

$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot

$build = Join-Path $ProjectRoot 'build\dungeon-spawn-p2-fast'
$srcDir = Join-Path $build 'source'
$genDir = Join-Path $build 'generated-origin-only'
$classes = Join-Path $build 'classes'
$oracleSrcDir = Join-Path $build 'oracle-src\net\minecraft\src'
New-Item -ItemType Directory -Force -Path $build,$srcDir,$genDir,$classes,$oracleSrcDir | Out-Null

$scoutSource = Join-Path $srcDir 'DungeonSpawnScoutP2RngFirst.cpp'
$oracleSource = Join-Path $oracleSrcDir 'Beta173DungeonSpawnOracle.java'
Invoke-WebRequest -UseBasicParsing "$RawBase/native/dungeon_spawn/DungeonSpawnScoutP2RngFirst.cpp" -OutFile $scoutSource
Invoke-WebRequest -UseBasicParsing "$RawBase/tools/Beta173DungeonSpawnOracle.java" -OutFile $oracleSource

# Generate the normal exact headers, then collapse X/Z to a single origin lattice
# column. The P2 scout never queries any X/Z except (0,0); dungeon geometry and
# RNG are handled separately. This preserves the exact 17-node vertical density
# column while avoiding the 5x5 chunk lattice used by P1.
$baseGenerated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
foreach ($name in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
    Copy-Item -Force (Join-Path $baseGenerated $name) (Join-Path $genDir $name)
}
$corePath = Join-Path $genDir 'coarse_exact_core.hpp'
$gpuPath = Join-Path $genDir 'coarse_exact_gpu.hpp'
$configPath = Join-Path $genDir 'skyblock_p14_config.hpp'
$core = [IO.File]::ReadAllText($corePath)
$core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', 'static constexpr int SIZE = 1;', 1)
$core = [regex]::Replace($core, 'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;', 'static constexpr int FROM_COARSE = 0;', 1)
[IO.File]::WriteAllText($corePath,$core,[Text.UTF8Encoding]::new($false))

$gpu = [IO.File]::ReadAllText($gpuPath)
$oldCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
'@
$newCoordinates = @'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    // Vanilla Beta chunk-local density point at the origin samples climate (1,1).
    climateX = static_cast<double>(coarseOffsetX * 4 + x * 3 + 1);
    climateZ = static_cast<double>(coarseOffsetZ * 4 + z * 3 + 1);
'@
if (-not $gpu.Contains($oldCoordinates)) { throw 'Could not patch generated GPU header to vanilla origin climate coordinates.' }
$gpu = $gpu.Replace($oldCoordinates,$newCoordinates)
[IO.File]::WriteAllText($gpuPath,$gpu,[Text.UTF8Encoding]::new($false))
$config = [IO.File]::ReadAllText($configPath)
$config = [regex]::Replace($config, 'static constexpr int CHUNK_RADIUS\s*=\s*\d+\s*;', 'static constexpr int CHUNK_RADIUS = 0;', 1)
[IO.File]::WriteAllText($configPath,$config,[Text.UTF8Encoding]::new($false))

$curseDir = Join-Path $ProjectRoot 'native\cursed_spawn_origin'
$curseSource = Join-Path $curseDir 'CursedSpawnOriginGpuFinder.cpp'
if (-not (Test-Path $curseSource -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path $curseDir | Out-Null
    Invoke-WebRequest -UseBasicParsing "$RawBase/native/cursed_spawn_origin/CursedSpawnOriginGpuFinder.cpp" -OutFile $curseSource
}

$scoutExe = Join-Path $build 'DungeonSpawnScoutP2Fast_AMD.exe'
Write-Host "Compiling dungeon P2 fast GPU scout for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" "-I$curseDir" $scoutSource -o $scoutExe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'DungeonSpawnScoutP2 fast AMD compilation failed.' }

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
    )) { foreach($f in Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue){$candidates.Add($f.FullName)} }
    foreach($c in $candidates){if(Test-Path $c -PathType Leaf){return $c}}
    return $null
}

$javac = Find-Javac
if (-not $javac) { throw 'A JDK (javac) is required for the exact Beta dungeon verifier.' }
$java = Join-Path (Split-Path $javac -Parent) 'java.exe'
if (-not (Test-Path $java -PathType Leaf)) { throw "java.exe not found beside javac: $javac" }

$mcRepo = Join-Path $ProjectRoot 'build\beta173-exact-skyblock-oracle\mc_b1.7.3_release'
if (-not (Test-Path (Join-Path $mcRepo '.git'))) { $mcRepo = Join-Path $build 'mc_b1.7.3_release' }
if (-not (Test-Path (Join-Path $mcRepo '.git'))) {
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
    if (-not $git) { throw 'Git is required once to fetch the Beta 1.7.3 server classes.' }
    Write-Host 'Fetching Beta 1.7.3 compiled server classes (one-time cache)...'
    & $git.Source clone --depth 1 --filter=blob:none --sparse 'https://github.com/jacobo-mc/mc_b1.7.3_release.git' $mcRepo
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone mc_b1.7.3_release.' }
    Push-Location $mcRepo
    try { & $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft_server'; if($LASTEXITCODE-ne 0){throw 'Sparse checkout failed.'} }
    finally { Pop-Location }
}
$betaBin = Join-Path $mcRepo '1.7.3-LTS\bin\minecraft_server'
if (-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)) { throw "Incomplete Beta class cache: $betaBin" }

Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
Write-Host 'Compiling exact Beta dungeon verifier...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $oracleSource
if ($LASTEXITCODE -ne 0) { throw 'Beta173DungeonSpawnOracle.java compilation failed.' }
$langDir = Join-Path $classes 'lang'
New-Item -ItemType Directory -Force -Path $langDir | Out-Null
[IO.File]::WriteAllText((Join-Path $langDir 'en_US.lang'),'')
[IO.File]::WriteAllText((Join-Path $langDir 'stats_US.lang'),'')
$cp = "$classes;$betaBin"

$outputRoot = Join-Path $ProjectRoot 'out\dungeon_spawn_p2_fast'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRun = Join-Path $outputRoot 'LAST_RUN.txt'
if ($Resume) {
    if (-not (Test-Path $lastRun -PathType Leaf)) { throw 'No dungeon P2 fast LAST_RUN.txt exists.' }
    $output = (Get-Content $lastRun -Raw).Trim()
    if (-not (Test-Path $output -PathType Container)) { throw "Dungeon P2 run directory missing: $output" }
} else {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output = Join-Path $outputRoot "run_$stamp"
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $output = [IO.Path]::GetFullPath($output)
    [IO.File]::WriteAllText($lastRun,$output,[Text.Encoding]::ASCII)
}
$output = [IO.Path]::GetFullPath($output)
$scoutDir = Join-Path $output 'scout_candidates'
$verifyDir = Join-Path $output 'exact_verify'
New-Item -ItemType Directory -Force -Path $scoutDir,$verifyDir | Out-Null
$checkpoint = Join-Path $output 'checkpoint.txt'

if ($Resume) {
    $kv=@{}
    foreach($line in Get-Content $checkpoint){if($line-match '^(.*?)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
    $StartIndex=[UInt64]$kv['START_INDEX']; $Count=[UInt64]$kv['COUNT']; $RandomKey=[UInt64]$kv['RANDOM_KEY']; $SeedMode=$kv['SEED_MODE']; $completed=[UInt64]$kv['COMPLETED']
} else {
    $completed=[UInt64]0
    if ($null -eq $RandomKey) {
        $bytes = New-Object byte[] 8
        $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
        try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
        $RandomKey=[BitConverter]::ToUInt64($bytes,0)
    }
}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 DUNGEON SPAWN P2 FAST'
Write-Host '=================================================================='
Write-Host 'Stage A: RNG-only filter rejects ~95.6% before terrain generation.'
Write-Host 'Stage B: exact origin-only 17-node terrain column checks sand + collision.'
Write-Host 'Stage C: actual Beta Java caves + dungeon generator verifies survivors.'
Write-Host 'Target/coverage is identical to P1: first dungeon attempt in chunk (-1,-1), lake-free subset.'
Write-Host "Output=$output"
Write-Host "Count=$Count StartIndex=$StartIndex Completed=$completed RandomKey=$RandomKey Batch=$Batch"
Write-Host ''

$invariant=[Globalization.CultureInfo]::InvariantCulture
while($completed -lt $Count) {
    [UInt64]$n=[Math]::Min([double]$ScoutChunk,[double]($Count-$completed))
    [UInt64]$index=$StartIndex+$completed
    Write-Host "--- FAST SCOUT start=$index count=$n ---"
    & $scoutExe --output $scoutDir --count $n.ToString($invariant) --start-index $index.ToString($invariant) `
        --random-key ([UInt64]$RandomKey).ToString($invariant) --seed-mode $SeedMode --batch $Batch `
        --terrain-threads $TerrainThreads --top 1 --progress-ms 1000 --checkpoint-ms 5000
    if ($LASTEXITCODE -ne 0) { throw "Dungeon P2 fast GPU scout failed at start=$index" }

    $candidateFile = Join-Path $scoutDir ("candidates_{0}.csv" -f $index)
    if (-not (Test-Path $candidateFile -PathType Leaf)) { throw "Scout did not create $candidateFile" }
    $candidateCount = [Math]::Max(0,(Get-Content $candidateFile | Measure-Object -Line).Lines-1)
    Write-Host "GPU candidates in chunk: $candidateCount"

    if ($candidateCount -gt 0) {
        Write-Host 'Running exact Beta caves/population/dungeon verifier...'
        & $java '-Xmx3G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173DungeonSpawnOracle `
            --input $candidateFile --output $verifyDir --progress-every 100
        if ($LASTEXITCODE -ne 0) { throw "Exact dungeon verifier failed at start=$index" }
        $summary = Join-Path $verifyDir 'SUMMARY.txt'
        if (Test-Path $summary) { Get-Content $summary }
    }

    $completed += $n
    @(
        'VERSION=DungeonSpawnP2Fast'
        "START_INDEX=$StartIndex"
        "COUNT=$Count"
        "COMPLETED=$completed"
        "NEXT_INDEX=$($StartIndex+$completed)"
        "RANDOM_KEY=$RandomKey"
        "SEED_MODE=$SeedMode"
    ) | Set-Content -LiteralPath $checkpoint -Encoding ASCII
    Write-Host "CHECKPOINT $completed/$Count"
}

Write-Host ''
Write-Host 'SEARCH COMPLETE.'
$summary = Join-Path $verifyDir 'SUMMARY.txt'
if (Test-Path $summary) { Get-Content $summary }
$inside = Join-Path $verifyDir 'spawn_inside_dungeon.csv'
if (Test-Path $inside) {
    Write-Host ''
    Write-Host 'TOP ACTUAL DUNGEON SPAWNS:'
    Import-Csv $inside | Select-Object -First 25 seed,dungeon_x,dungeon_y,dungeon_z,actual_feet_y,chest_count,spawn_on_spawner | Format-Table -AutoSize
}
Write-Host "RESULT_DIR=$output"
