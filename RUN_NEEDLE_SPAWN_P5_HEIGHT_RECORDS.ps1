param(
    [UInt64]$Count = 10000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(63,127)][int]$MinTopY = 70,
    [ValidateRange(25,10000)][int]$Top = 5000
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$branch='floating-island-spawn-p6-vanilla-spawn'
$baseSourceRef='fba49d79d2701c6df4c066af5258e7af0fc8f89f'
$oracleRef='9574fbc4ed942a7f6540a160117a41b4a5f2526a'

# This version deliberately has NO fixed 10-block needle threshold.
# The GPU keeps the deepest positive-quadrant shafts first. The exact Beta
# verifier then generates the real -X/-Z chunks and measures the full 3x3 shaft
# depth. Every deeper exact shaft becomes a new height record.

$helper=Join-Path $root 'scripts\cursed-spawn-origin-p1-common.ps1'
if(-not(Test-Path $helper -PathType Leaf)){throw "Missing helper: $helper . Run the earlier needle P1 runner once first."}
. $helper

$hipcc=Get-Hipcc
$arches=@(Get-HipGpuArchitectures $hipcc)
$archArgs=@($arches|ForEach-Object{"--offload-arch=$_"})
$archKey=($arches -join ',')

$p1build=Join-Path $root 'build\needle-spawn-p1'
$baseCpp=Join-Path $p1build 'source\native\needle_spawn\NeedleSpawnGpuFinderP1.cpp'
$nativeSourceDir=Join-Path $p1build 'source\native'
$genDir=Join-Path $p1build 'generated-chunk0'
if(-not(Test-Path $baseCpp -PathType Leaf)){throw "Missing P1 source: $baseCpp . Run RUN_NEEDLE_SPAWN_P1.ps1 once first."}
if(-not(Test-Path (Join-Path $genDir 'coarse_exact_core.hpp') -PathType Leaf)){throw "Missing P1 generated headers: $genDir . Run RUN_NEEDLE_SPAWN_P1.ps1 once first."}

$build=Join-Path $root 'build\needle-spawn-p5-height-records'
New-Item -ItemType Directory -Force -Path $build|Out-Null
$patchedCpp=Join-Path (Split-Path $baseCpp -Parent) 'NeedleSpawnGpuFinderP5HeightRecords.cpp'
$cpp=[IO.File]::ReadAllText($baseCpp)

# Make actual positive-side shaft depth the dominant GPU retention metric.
$oldScore=@'
    // Lexicographic intent: the minimum immediate drop dominates, then altitude,
    // then how many consecutive levels remain a 1x1 positive-quadrant shaft.
    r.score = static_cast<std::int64_t>(r.minPositiveDrop) * 1000000000000LL
            + static_cast<std::int64_t>(topY) * 1000000000LL
            + static_cast<std::int64_t>(std::min(999,r.quadrantDepth)) * 1000000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r2MinDrop))) * 1000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r4MinDrop)));
'@
$newScore=@'
    // P5 HEIGHT RECORDS: depth dominates. There is no fixed 10-block target.
    // Keeping Top N by positive-side depth gives the exact cross-chunk verifier
    // the best chances to discover progressively taller true 3x3 needles.
    r.score = static_cast<std::int64_t>(std::min(999,r.quadrantDepth)) * 1000000000000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.minPositiveDrop))) * 1000000000LL
            + static_cast<std::int64_t>(topY) * 1000000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r2MinDrop))) * 1000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r4MinDrop)));
'@
if(-not $cpp.Contains($oldScore)){throw 'Could not locate P1 score block to patch.'}
$cpp=$cpp.Replace($oldScore,$newScore)
$cpp=$cpp.Replace('NeedleSpawn P1 | exact origin sand gate + fast chunk(0,0) positive-quadrant needle scout','NeedleSpawn P5 HEIGHT RECORDS | positive-side prescreen ranked by shaft depth')
$cpp=$cpp.Replace('Hard gates: topY >= ','Prescreen gates: topY >= ')
$cpp=$cpp.Replace('NEW BEST NEEDLE SCOUT seed=','NEW BEST POSITIVE-DEPTH SCOUT seed=')
$cpp=$cpp.Replace('FINAL NEEDLE SCOUT BEST seed=','FINAL POSITIVE-DEPTH SCOUT BEST seed=')
[IO.File]::WriteAllText($patchedCpp,$cpp,[Text.UTF8Encoding]::new($false))

$exe=Join-Path $build 'NeedleSpawnP5HeightRecords_AMD.exe'
Write-Host "Compiling P5 height-record GPU prescreen for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" $patchedCpp -o $exe | Out-Host
if($LASTEXITCODE-ne 0){throw 'P5 height-record GPU compile failed.'}

function Find-Javac {
    $cmd=Get-Command javac.exe -ErrorAction SilentlyContinue;if($cmd){return $cmd.Source}
    $cmd=Get-Command javac -ErrorAction SilentlyContinue;if($cmd){return $cmd.Source}
    if($env:JAVA_HOME){$p=Join-Path $env:JAVA_HOME 'bin\javac.exe';if(Test-Path $p -PathType Leaf){return $p}}
    foreach($pattern in @('C:\Program Files\Java\jdk*\bin\javac.exe','C:\Program Files\Eclipse Adoptium\jdk*\bin\javac.exe','C:\Program Files\Microsoft\jdk*\bin\javac.exe',"$env:USERPROFILE\.jdks\*\bin\javac.exe")){foreach($f in Get-ChildItem $pattern -File -ErrorAction SilentlyContinue){return $f.FullName}}
    return $null
}
$javac=Find-Javac;if(-not $javac){throw 'JDK/javac required.'}
$java=Join-Path (Split-Path $javac -Parent) 'java.exe'
$betaBin=Join-Path $root 'build\beta173-exact-client-oracle\mc_b1.7.3_release\1.7.3-LTS\bin\minecraft'
if(-not(Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)){throw "Exact Beta class cache missing: $betaBin"}

$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes|Out-Null
$oracleSrc=Join-Path $srcDir 'Beta173NeedleSpawnOracleV2.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$oracleRef/tools/Beta173NeedleSpawnOracleV2.java" -OutFile $oracleSrc
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes|Out-Null
Write-Host 'Compiling exact Beta 3x3 height-measurement oracle...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $oracleSrc
if($LASTEXITCODE-ne 0){throw 'P5 exact oracle compile failed.'}
foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d)|Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){$s=Join-Path (Join-Path $betaBin 'lang') $name;$d=Join-Path (Join-Path $classes 'lang') $name;if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}}
$s=Join-Path $betaBin 'achievement\map.txt';$d=Join-Path $classes 'achievement\map.txt';if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
$cp="$classes;$betaBin"

$outputRoot=Join-Path $root 'out\needle_spawn_p5_height_records';New-Item -ItemType Directory -Force -Path $outputRoot|Out-Null
$stamp=Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$output=[IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"));New-Item -ItemType Directory -Force -Path $output|Out-Null
if($null -eq $RandomKey){$bytes=New-Object byte[] 8;$rng=[Security.Cryptography.RandomNumberGenerator]::Create();try{$rng.GetBytes($bytes)}finally{$rng.Dispose()};$RandomKey=[BitConverter]::ToUInt64($bytes,0)}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 NEEDLE SPAWN P5 - INCREASING HEIGHT RECORD'
Write-Host '=================================================================='
Write-Host 'NO fixed 10-block needle threshold.'
Write-Host 'GPU keeps the deepest +X/+Z shafts. Exact Beta generates all surrounding chunks.'
Write-Host 'Exact depth = consecutive Y levels where the center is solid and ALL 8 cells of the surrounding 3x3 are AIR.'
Write-Host 'Every larger exact depth becomes the new needle-height record.'
Write-Host "Count=$Count RandomKey=$RandomKey MinTopY=$MinTopY Top=$Top"
Write-Host "Output=$output"
Write-Host ''

# min-drop=1 removes the old arbitrary 10-block gate. Depth is the ranking metric.
$args=@('--output',$output,'--count',$Count.ToString([Globalization.CultureInfo]::InvariantCulture),'--start-index',$StartIndex.ToString([Globalization.CultureInfo]::InvariantCulture),'--random-key',([UInt64]$RandomKey).ToString([Globalization.CultureInfo]::InvariantCulture),'--seed-mode',$SeedMode,'--batch',$Batch,'--terrain-threads',$TerrainThreads,'--min-drop','1','--min-top-y',$MinTopY,'--top',$Top,'--progress-ms','1000','--checkpoint-ms','5000')
& $exe @args
if($LASTEXITCODE-ne 0){throw 'P5 GPU prescreen failed.'}

$candidates=Join-Path $output 'top_candidates.csv'
if(-not(Test-Path $candidates -PathType Leaf)){throw 'P5 GPU prescreen did not create top_candidates.csv'}
$verifyDir=Join-Path $output 'exact_verify';New-Item -ItemType Directory -Force -Path $verifyDir|Out-Null
$exact=Join-Path $verifyDir 'all_exact_depths.csv'
Write-Host ''
Write-Host 'Running exact cross-chunk 3x3 depth measurement...'
& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173NeedleSpawnOracleV2 --input $candidates --output $exact --min-drop 1 --min-top-y $MinTopY --min-needle-depth 1
if($LASTEXITCODE-ne 0){throw 'P5 exact depth verification failed.'}

$hits=@(Import-Csv $exact | Where-Object {$_.hit -eq '1'})
$ranked=@($hits | Sort-Object @{Expression={[int]$_.needle_depth};Descending=$true}, @{Expression={[int]$_.final_top_y};Descending=$true})
$rankedPath=Join-Path $output 'EXACT_NEEDLES_BY_HEIGHT.csv'
$ranked | Export-Csv -NoTypeInformation -Encoding ASCII $rankedPath

# Reconstruct monotonically increasing exact records by seed sequence.
$bySeq=@($hits | Sort-Object @{Expression={[UInt64]$_.sequence_index};Descending=$false})
$records=New-Object System.Collections.Generic.List[object]
$best=0
foreach($r in $bySeq){
    $d=[int]$r.needle_depth
    if($d -gt $best){
        $best=$d
        $records.Add([pscustomobject]@{
            threshold=$d
            seed=$r.seed
            sequence_index=$r.sequence_index
            final_top_y=$r.final_top_y
            needle_depth=$r.needle_depth
            north_drop=$r.north_drop
            south_drop=$r.south_drop
            east_drop=$r.east_drop
            west_drop=$r.west_drop
        })
        Write-Host "NEW EXACT NEEDLE HEIGHT RECORD threshold=$d seed=$($r.seed) topY=$($r.final_top_y)"
    }
}
$recordsPath=Join-Path $output 'HEIGHT_RECORDS.csv'
$records | Export-Csv -NoTypeInformation -Encoding ASCII $recordsPath

Write-Host ''
Write-Host '=================================================================='
Write-Host ' TALLEST EXACT 3x3 NEEDLES'
Write-Host '=================================================================='
if($ranked.Count-eq 0){
    Write-Host 'NONE in this run.'
}else{
    $ranked | Select-Object -First 30 seed,final_top_y,needle_depth,north_drop,south_drop,east_drop,west_drop | Format-Table -AutoSize
    Write-Host "BEST_EXACT_DEPTH=$($ranked[0].needle_depth) BEST_SEED=$($ranked[0].seed)"
}
Write-Host "HEIGHT_RECORDS=$recordsPath"
Write-Host "RANKED_EXACT=$rankedPath"
Write-Host "ALL_EXACT=$exact"
Write-Host "RESULT_DIR=$output"
