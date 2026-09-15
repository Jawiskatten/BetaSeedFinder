param(
    [UInt64]$Count = 10000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,100)][int]$MinDrop = 10,
    [ValidateRange(63,127)][int]$MinTopY = 70,
    [ValidateRange(25,5000)][int]$Top = 250,
    [switch]$Resume
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$ProjectRoot=$PSScriptRoot
$SourceRef='44d417ebcafd981873bdea0dbcc19d517c69b6ba'
$RawBase="https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef"
$BetaCommit='740c583901e1ff1150e9ef37e37dab5bc0e4f807'

$helper=Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1'
if(-not (Test-Path $helper -PathType Leaf)){
    New-Item -ItemType Directory -Force -Path (Split-Path $helper -Parent)|Out-Null
    Invoke-WebRequest -UseBasicParsing "$RawBase/scripts/cursed-spawn-origin-p1-common.ps1" -OutFile $helper
}
. $helper

$hipcc=Get-Hipcc
$arches=@(Get-HipGpuArchitectures $hipcc)
$archArgs=@($arches|ForEach-Object{"--offload-arch=$_"})
$archKey=($arches -join ',')
$nativeSourceDir=Get-BetaGpuNativeSourceDir $ProjectRoot

$build=Join-Path $ProjectRoot 'build\needle-spawn-p1'
$sourceNative=Join-Path $build 'source\native'
$sourceNeedle=Join-Path $sourceNative 'needle_spawn'
$sourceHighest=Join-Path $sourceNative 'highest_pillar_spawn'
$genDir=Join-Path $build 'generated-chunk0'
$oracleSrcDir=Join-Path $build 'oracle-src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $build,$sourceNative,$sourceNeedle,$sourceHighest,$genDir,$oracleSrcDir,$classes|Out-Null

$needleSource=Join-Path $sourceNeedle 'NeedleSpawnGpuFinderP1.cpp'
$highestSource=Join-Path $sourceHighest 'HighestPillarSpawnGpuFinder.cpp'
$oracleSource=Join-Path $oracleSrcDir 'Beta173NeedleSpawnOracle.java'
Invoke-WebRequest -UseBasicParsing "$RawBase/native/needle_spawn/NeedleSpawnGpuFinderP1.cpp" -OutFile $needleSource
Invoke-WebRequest -UseBasicParsing "$RawBase/native/highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp" -OutFile $highestSource
Invoke-WebRequest -UseBasicParsing "$RawBase/tools/Beta173NeedleSpawnOracle.java" -OutFile $oracleSource

# Use the established exact Beta P14 terrain generator but shrink to chunk (0,0).
# The GPU scout only hard-gates +X/+Z through radius 4, all inside this chunk.
# -X/-Z are intentionally left for the exact Java verifier because Beta generates
# boundary density nodes independently for adjacent chunks.
$baseGenerated=Prepare-SkyblockP14LatticeHeaders $ProjectRoot $nativeSourceDir 'full' 4
foreach($name in @('coarse_exact_core.hpp','coarse_exact_gpu.hpp','skyblock_p14_config.hpp')){Copy-Item -Force (Join-Path $baseGenerated $name) (Join-Path $genDir $name)}
$corePath=Join-Path $genDir 'coarse_exact_core.hpp'
$gpuPath=Join-Path $genDir 'coarse_exact_gpu.hpp'
$configPath=Join-Path $genDir 'skyblock_p14_config.hpp'
$core=[IO.File]::ReadAllText($corePath)
$core=[regex]::Replace($core,'static constexpr int SIZE\s*=\s*\d+\s*;','static constexpr int SIZE = 5;',1)
$core=[regex]::Replace($core,'static constexpr int FROM_COARSE\s*=\s*-?\d+\s*;','static constexpr int FROM_COARSE = 0;',1)
[IO.File]::WriteAllText($corePath,$core,[Text.UTF8Encoding]::new($false))
$gpu=[IO.File]::ReadAllText($gpuPath)
$old=@'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = coarseX * 4.0 + 2.0;
    climateZ = coarseZ * 4.0 + 2.0;
'@
$new=@'
    coarseX = static_cast<double>(coarsecore::FROM_COARSE + x + coarseOffsetX);
    coarseZ = static_cast<double>(coarsecore::FROM_COARSE + z + coarseOffsetZ);
    climateX = static_cast<double>(coarseOffsetX * 4 + x * 3 + 1);
    climateZ = static_cast<double>(coarseOffsetZ * 4 + z * 3 + 1);
'@
if($gpu.Contains($old)){$gpu=$gpu.Replace($old,$new);[IO.File]::WriteAllText($gpuPath,$gpu,[Text.UTF8Encoding]::new($false))}elseif(-not $gpu.Contains('coarseOffsetX * 4 + x * 3 + 1')){throw 'Could not confirm corrected chunk-local climate coordinates.'}
$config=[IO.File]::ReadAllText($configPath)
$config=[regex]::Replace($config,'static constexpr int CHUNK_RADIUS\s*=\s*\d+\s*;','static constexpr int CHUNK_RADIUS = 0;',1)
[IO.File]::WriteAllText($configPath,$config,[Text.UTF8Encoding]::new($false))

$exe=Join-Path $build 'NeedleSpawnP1_AMD.exe'
Write-Host "Compiling needle-spawn P1 GPU scout for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" $needleSource -o $exe | Out-Host
if($LASTEXITCODE -ne 0){throw 'Needle P1 AMD compilation failed.'}

function Find-Javac {
    $cmd=Get-Command javac.exe -ErrorAction SilentlyContinue;if($cmd){return $cmd.Source}
    $cmd=Get-Command javac -ErrorAction SilentlyContinue;if($cmd){return $cmd.Source}
    if($env:JAVA_HOME){$p=Join-Path $env:JAVA_HOME 'bin\javac.exe';if(Test-Path $p -PathType Leaf){return $p}}
    foreach($pattern in @('C:\Program Files\Java\jdk*\bin\javac.exe','C:\Program Files\Eclipse Adoptium\jdk*\bin\javac.exe','C:\Program Files\Microsoft\jdk*\bin\javac.exe',"$env:USERPROFILE\.jdks\*\bin\javac.exe")){foreach($f in Get-ChildItem $pattern -File -ErrorAction SilentlyContinue){return $f.FullName}}
    return $null
}
$javac=Find-Javac;if(-not $javac){throw 'JDK/javac required.'}
$java=Join-Path (Split-Path $javac -Parent) 'java.exe'
$git=Get-Command git.exe -ErrorAction SilentlyContinue;if(-not $git){$git=Get-Command git -ErrorAction SilentlyContinue};if(-not $git){throw 'git required.'}
$mcRepo=Join-Path $ProjectRoot 'build\beta173-exact-client-oracle\mc_b1.7.3_release'
if(-not (Test-Path (Join-Path $mcRepo '.git'))){New-Item -ItemType Directory -Force -Path (Split-Path $mcRepo -Parent)|Out-Null;& $git.Source clone --filter=blob:none --no-checkout 'https://github.com/jacobo-mc/mc_b1.7.3_release.git' $mcRepo;if($LASTEXITCODE-ne 0){throw 'Failed to clone exact Beta repo.'}}
Push-Location $mcRepo
try{& $git.Source sparse-checkout init --cone|Out-Host;& $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft'|Out-Host;& $git.Source fetch --depth 1 origin $BetaCommit|Out-Host;if($LASTEXITCODE-ne 0){throw 'Failed to fetch Beta commit.'};& $git.Source checkout --detach $BetaCommit|Out-Host;if($LASTEXITCODE-ne 0){throw 'Failed to checkout Beta commit.'};& $git.Source sparse-checkout set '1.7.3-LTS/bin/minecraft'|Out-Host}finally{Pop-Location}
$betaBin=Join-Path $mcRepo '1.7.3-LTS\bin\minecraft'
if(-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)){throw 'Exact Beta class cache incomplete.'}
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue;New-Item -ItemType Directory -Force -Path $classes|Out-Null
Write-Host 'Compiling exact Beta needle oracle...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $oracleSource
if($LASTEXITCODE-ne 0){throw 'Needle Java oracle compilation failed.'}
foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d)|Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){$s=Join-Path (Join-Path $betaBin 'lang') $name;$d=Join-Path (Join-Path $classes 'lang') $name;if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}}
$s=Join-Path $betaBin 'achievement\map.txt';$d=Join-Path $classes 'achievement\map.txt';if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
$cp="$classes;$betaBin"

$outputRoot=Join-Path $ProjectRoot 'out\needle_spawn_p1';New-Item -ItemType Directory -Force -Path $outputRoot|Out-Null
$lastRun=Join-Path $outputRoot 'LAST_RUN.txt'
if($Resume){
    if(-not(Test-Path $lastRun -PathType Leaf)){throw 'No needle P1 LAST_RUN exists.'}
    $output=(Get-Content $lastRun -Raw).Trim();if(-not(Test-Path $output -PathType Container)){throw "Run missing: $output"}
    $ck=Join-Path $output 'checkpoint.txt';$kv=@{};foreach($line in Get-Content $ck){if($line-match '^(.*?)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
    $Count=[UInt64]$kv['COUNT'];$StartIndex=[UInt64]$kv['START_INDEX'];$RandomKey=[UInt64]$kv['RANDOM_KEY'];$SeedMode=$kv['SEED_MODE'];$MinDrop=[int]$kv['MIN_DROP'];$MinTopY=[int]$kv['MIN_TOP_Y']
}else{
    $stamp=Get-Date -Format 'yyyy-MM-dd_HH-mm-ss';$output=[IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"));New-Item -ItemType Directory -Force -Path $output|Out-Null;[IO.File]::WriteAllText($lastRun,$output,[Text.Encoding]::ASCII)
    if($null -eq $RandomKey){$bytes=New-Object byte[] 8;$rng=[Security.Cryptography.RandomNumberGenerator]::Create();try{$rng.GetBytes($bytes)}finally{$rng.Dispose()};$RandomKey=[BitConverter]::ToUInt64($bytes,0)}
}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 NEEDLE SPAWN P1'
Write-Host '=================================================================='
Write-Host 'Target: exact (0,0) sand spawn on a tiny elevated 1x1 top with huge drops on all four sides.'
Write-Host 'GPU: exact chunk(0,0) origin sand + positive-quadrant drop scout.'
Write-Host 'Java: exact full 17x17 client startup, then all 8 top neighbors + N/S/E/W drops.'
Write-Host "Count=$Count RandomKey=$RandomKey MinTopY=$MinTopY MinDrop=$MinDrop Top=$Top"
Write-Host "Output=$output"
Write-Host ''

$args=@('--output',$output,'--count',$Count.ToString([Globalization.CultureInfo]::InvariantCulture),'--start-index',$StartIndex.ToString([Globalization.CultureInfo]::InvariantCulture),'--random-key',([UInt64]$RandomKey).ToString([Globalization.CultureInfo]::InvariantCulture),'--seed-mode',$SeedMode,'--batch',$Batch,'--terrain-threads',$TerrainThreads,'--min-drop',$MinDrop,'--min-top-y',$MinTopY,'--top',$Top,'--progress-ms','1000','--checkpoint-ms','5000')
if($Resume){$args+='--resume-existing'}
& $exe @args
if($LASTEXITCODE-ne 0){throw 'Needle GPU scout failed.'}

$candidates=Join-Path $output 'top_candidates.csv'
if(-not(Test-Path $candidates -PathType Leaf)){throw 'Needle scout did not create top_candidates.csv'}
$verifyDir=Join-Path $output 'exact_verify';New-Item -ItemType Directory -Force -Path $verifyDir|Out-Null
$exact=Join-Path $verifyDir 'exact_needles.csv'
Write-Host ''
Write-Host 'Running exact Beta client-startup verification of retained candidates...'
& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173NeedleSpawnOracle --input $candidates --output $exact --min-drop $MinDrop --min-top-y $MinTopY
if($LASTEXITCODE-ne 0){throw 'Needle exact oracle failed.'}

Write-Host ''
Write-Host 'TOP EXACT 1x1 SAND NEEDLES:'
$hits=@(Import-Csv $exact|Where-Object{$_.hit-eq'1'})
if($hits.Count-eq 0){Write-Host 'NONE at current thresholds. Best scout candidates are still saved for inspection.'}else{$hits|Select-Object -First 25 seed,final_top_y,min_cardinal_drop,north_drop,south_drop,east_drop,west_drop,pillar_depth,min_eight_drop,r2_min_drop,r4_min_drop|Format-Table -AutoSize}
Write-Host "EXACT_CSV=$exact"
Write-Host "RESULT_DIR=$output"
