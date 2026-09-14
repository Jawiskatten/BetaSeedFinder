param(
    [UInt64]$Count = 100000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(100000,1000000000)][UInt64]$ScoutChunk = 10000000,
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,64)][int]$MinDrop = 15,
    [switch]$Resume
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
# Dependency snapshot: P1 GPU scout + SOLID-only four-chunk prefilter + full client oracle.
$BranchRef = '9fe4de547eda5e181bbee2cc5940066a97ef2949'
$RawBase = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$BranchRef"
$BetaCommit = '740c583901e1ff1150e9ef37e37dab5bc0e4f807'

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

$build = Join-Path $ProjectRoot 'build\ghost-floor-p2-fast'
$srcDir = Join-Path $build 'source'
$genDir = Join-Path $build 'generated-chunk-local'
$classes = Join-Path $build 'classes'
$oracleSrcDir = Join-Path $build 'oracle-src\net\minecraft\src'
New-Item -ItemType Directory -Force -Path $build,$srcDir,$genDir,$classes,$oracleSrcDir | Out-Null

$scoutSource = Join-Path $srcDir 'GhostFloorScoutP1.cpp'
$prefilterSource = Join-Path $oracleSrcDir 'Beta173GhostFloorFastPrefilter.java'
$oracleSource = Join-Path $oracleSrcDir 'Beta173GhostFloorOracle.java'
Invoke-WebRequest -UseBasicParsing "$RawBase/native/ghost_floor/GhostFloorScoutP1.cpp" -OutFile $scoutSource
Invoke-WebRequest -UseBasicParsing "$RawBase/tools/Beta173GhostFloorFastPrefilter.java" -OutFile $prefilterSource
Invoke-WebRequest -UseBasicParsing "$RawBase/tools/Beta173GhostFloorOracle.java" -OutFile $oracleSource

# Same exact GPU terrain setup as P1. P2's speedup is downstream: the full
# 289-chunk client startup is now run only after an exact four-chunk cave/lake
# prefilter proves a deep SOLID landing is plausible.
$baseGenerated = Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
foreach ($name in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')) {
    Copy-Item -Force (Join-Path $baseGenerated $name) (Join-Path $genDir $name)
}
$corePath = Join-Path $genDir 'coarse_exact_core.hpp'
$gpuPath = Join-Path $genDir 'coarse_exact_gpu.hpp'
$configPath = Join-Path $genDir 'skyblock_p14_config.hpp'
$core = [IO.File]::ReadAllText($corePath)
$core = [regex]::Replace($core, 'static constexpr int SIZE\s*=\s*\d+\s*;', 'static constexpr int SIZE = 5;', 1)
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
    climateX = static_cast<double>(coarseOffsetX * 4 + x * 3 + 1);
    climateZ = static_cast<double>(coarseOffsetZ * 4 + z * 3 + 1);
'@
if (-not $gpu.Contains($oldCoordinates)) { throw 'Could not patch generated GPU header to vanilla chunk-local climate coordinates.' }
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

$scoutExe = Join-Path $build 'GhostFloorScoutP2Fast_AMD.exe'
Write-Host "Compiling ghost-floor P2 GPU scout for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" "-I$curseDir" $scoutSource -o $scoutExe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Ghost-floor P2 AMD compilation failed.' }

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
if (-not $javac) { throw 'A JDK (javac) is required for the exact Beta client verifiers.' }
$java = Join-Path (Split-Path $javac -Parent) 'java.exe'
if (-not (Test-Path $java -PathType Leaf)) { throw "java.exe not found beside javac: $javac" }

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Command git -ErrorAction SilentlyContinue }
if (-not $git) { throw 'Git is required once to fetch the exact Beta 1.7.3 client classes.' }
$mcRepo = Join-Path $ProjectRoot 'build\beta173-exact-client-oracle\mc_b1.7.3_release'
if (-not (Test-Path (Join-Path $mcRepo '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $mcRepo -Parent) | Out-Null
    Write-Host 'Fetching Beta 1.7.3 client classes (one-time cache)...'
    & $git.Source clone --filter=blob:none --no-checkout 'https://github.com/jacobo-mc/mc_b1.7.3_release.git' $mcRepo
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone mc_b1.7.3_release.' }
}
Push-Location $mcRepo
try {
    & $git.Source sparse-checkout init --cone | Out-Host
    & $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft' | Out-Host
    & $git.Source fetch --depth 1 origin $BetaCommit | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Failed to fetch exact Beta source commit $BetaCommit" }
    & $git.Source checkout --detach $BetaCommit | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Failed to checkout exact Beta source commit $BetaCommit" }
    & $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft' | Out-Host
} finally { Pop-Location }
$betaBin = Join-Path $mcRepo '1.7.3-LTS\bin\minecraft'
if (-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)) { throw "Incomplete Beta client class cache: $betaBin" }

Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
Write-Host 'Compiling SOLID-only fast prefilter + full Beta CLIENT startup verifier...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $prefilterSource $oracleSource
if ($LASTEXITCODE -ne 0) { throw 'Ghost-floor Java verifier compilation failed.' }

$langDir = Join-Path $classes 'lang'
$achievementDir = Join-Path $classes 'achievement'
New-Item -ItemType Directory -Force -Path $langDir,$achievementDir | Out-Null
foreach ($name in @('en_US.lang','stats_US.lang')) {
    $src = Join-Path (Join-Path $betaBin 'lang') $name
    $dst = Join-Path $langDir $name
    if (Test-Path $src -PathType Leaf) { Copy-Item -Force $src $dst } else { [IO.File]::WriteAllText($dst,'') }
}
$achievementSrc = Join-Path $betaBin 'achievement\map.txt'
$achievementDst = Join-Path $achievementDir 'map.txt'
if (Test-Path $achievementSrc -PathType Leaf) { Copy-Item -Force $achievementSrc $achievementDst } else { [IO.File]::WriteAllText($achievementDst,'') }
$cp = "$classes;$betaBin"

$outputRoot = Join-Path $ProjectRoot 'out\ghost_floor_p2_fast'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRun = Join-Path $outputRoot 'LAST_RUN.txt'
if ($Resume) {
    if (-not (Test-Path $lastRun -PathType Leaf)) { throw 'No ghost-floor P2 LAST_RUN.txt exists.' }
    $output = (Get-Content $lastRun -Raw).Trim()
    if (-not (Test-Path $output -PathType Container)) { throw "Ghost-floor P2 run directory missing: $output" }
} else {
    $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output = Join-Path $outputRoot "run_$stamp"
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    $output = [IO.Path]::GetFullPath($output)
    [IO.File]::WriteAllText($lastRun,$output,[Text.Encoding]::ASCII)
}
$output = [IO.Path]::GetFullPath($output)
$scoutDir = Join-Path $output 'scout_candidates'
$prefilterDir = Join-Path $output 'fast_four_chunk_prefilter'
$verifyDir = Join-Path $output 'exact_client_verify'
New-Item -ItemType Directory -Force -Path $scoutDir,$prefilterDir,$verifyDir | Out-Null
$checkpoint = Join-Path $output 'checkpoint.txt'

if ($Resume) {
    $kv=@{}
    foreach($line in Get-Content $checkpoint){if($line-match '^(.*?)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
    $StartIndex=[UInt64]$kv['START_INDEX']; $Count=[UInt64]$kv['COUNT']; $RandomKey=[UInt64]$kv['RANDOM_KEY']; $SeedMode=$kv['SEED_MODE']; $completed=[UInt64]$kv['COMPLETED']
    if ($kv.ContainsKey('MIN_DROP')) { $MinDrop=[int]$kv['MIN_DROP'] }
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
Write-Host ' BETA 1.7.3 GHOST FLOOR / FREEFALL P2 FAST - SOLID ONLY'
Write-Host '=================================================================='
Write-Host 'GPU: same ~1.4M/s exact lake + sand-gate scout as P1.'
Write-Host 'FAST JAVA: only 4 exact Beta chunks; caves included; isolates population (-1,-1).'
Write-Host 'FULL JAVA: 17x17 client Building-terrain startup ONLY for proven SOLID-drop candidates.'
Write-Host 'Wet lake masks are discarded before any Java world generation.'
Write-Host "Full-startup shortlist: first obstacle SOLID and drop >= $MinDrop blocks"
Write-Host "Output=$output"
Write-Host "Count=$Count StartIndex=$StartIndex Completed=$completed RandomKey=$RandomKey Batch=$Batch"
Write-Host ''

$invariant=[Globalization.CultureInfo]::InvariantCulture
while($completed -lt $Count) {
    [UInt64]$n=[Math]::Min([double]$ScoutChunk,[double]($Count-$completed))
    [UInt64]$index=$StartIndex+$completed
    Write-Host "--- GHOST P2 GPU SCOUT start=$index count=$n ---"
    & $scoutExe --output $scoutDir --count $n.ToString($invariant) --start-index $index.ToString($invariant) `
        --random-key ([UInt64]$RandomKey).ToString($invariant) --seed-mode $SeedMode --batch $Batch `
        --terrain-threads $TerrainThreads --top 1 --progress-ms 1000 --checkpoint-ms 5000
    if ($LASTEXITCODE -ne 0) { throw "Ghost-floor P2 GPU scout failed at start=$index" }

    $candidateFile = Join-Path $scoutDir ("candidates_{0}.csv" -f $index)
    if (-not (Test-Path $candidateFile -PathType Leaf)) { throw "Scout did not create $candidateFile" }
    $candidateCount = [Math]::Max(0,(Get-Content $candidateFile | Measure-Object -Line).Lines-1)
    Write-Host "GPU predicted gate-carve candidates: $candidateCount"

    if ($candidateCount -gt 0) {
        Write-Host 'Running FAST exact 4-chunk cave + isolated lake prefilter (SOLID landing required)...'
        & $java '-Xmx2G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173GhostFloorFastPrefilter `
            --input $candidateFile --output $prefilterDir --min-drop $MinDrop --progress-every 100
        if ($LASTEXITCODE -ne 0) { throw "Fast ghost-floor prefilter failed at start=$index" }

        $prefilterSummary = Join-Path $prefilterDir ("PREFILTER_SUMMARY_{0}.txt" -f $index)
        if (Test-Path $prefilterSummary) { Get-Content $prefilterSummary }
        $shortlistFile = Join-Path $prefilterDir ("shortlist_{0}.csv" -f $index)
        if (-not (Test-Path $shortlistFile -PathType Leaf)) { throw "Fast prefilter did not create $shortlistFile" }
        $shortlistCount = [Math]::Max(0,(Get-Content $shortlistFile | Measure-Object -Line).Lines-1)
        Write-Host "FULL client-startup SOLID shortlist in chunk: $shortlistCount"

        if ($shortlistCount -gt 0) {
            Write-Host 'Running authoritative Beta CLIENT 17x17 startup verifier on SOLID shortlist...'
            & $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173GhostFloorOracle `
                --input $shortlistFile --output $verifyDir --progress-every 1
            if ($LASTEXITCODE -ne 0) { throw "Full ghost-floor verifier failed at start=$index" }
            $summary = Join-Path $verifyDir 'SUMMARY.txt'
            if (Test-Path $summary) { Get-Content $summary }
        }
    }

    $completed += $n
    @(
        'VERSION=GhostFloorP2FastSolid'
        "START_INDEX=$StartIndex"
        "COUNT=$Count"
        "COMPLETED=$completed"
        "NEXT_INDEX=$($StartIndex+$completed)"
        "RANDOM_KEY=$RandomKey"
        "SEED_MODE=$SeedMode"
        "MIN_DROP=$MinDrop"
        "BETA_COMMIT=$BetaCommit"
    ) | Set-Content -LiteralPath $checkpoint -Encoding ASCII
    Write-Host "CHECKPOINT $completed/$Count"
}

Write-Host ''
Write-Host 'SEARCH COMPLETE.'
$summary = Join-Path $verifyDir 'SUMMARY.txt'
if (Test-Path $summary) { Get-Content $summary }
$top = Join-Path $verifyDir 'top_longest_drop.csv'
if (Test-Path $top) {
    Write-Host ''
    Write-Host 'TOP AUTHORITATIVE FREEFALLS:'
    Import-Csv $top | Select-Object -First 25 seed,pre_gate_y,gate_destroyed,post_feet_y,post_air_drop,post_obstacle_type,ghost_floor,lethal_estimate | Format-Table -AutoSize
}
Write-Host "RESULT_DIR=$output"
