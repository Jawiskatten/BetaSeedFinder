param(
    [UInt64]$Count = 10000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,100)][int]$MinDrop = 10,
    [ValidateRange(63,127)][int]$MinTopY = 70,
    [ValidateRange(1,100)][int]$MinNeedleDepth = 10,
    [ValidateRange(25,10000)][int]$Top = 2000,
    [switch]$Resume
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$OracleRef='9574fbc4ed942a7f6540a160117a41b4a5f2526a'

# P2 intentionally reuses the already-proven fast P1 GPU terrain scout. The
# important correction is in the authoritative exact verifier: the center shaft
# must remain truly 1x1 for MinNeedleDepth consecutive blocks, not merely have an
# isolated cap at its top level.
$exe=Join-Path $root 'build\needle-spawn-p1\NeedleSpawnP1_AMD.exe'
if(-not(Test-Path $exe -PathType Leaf)){
    throw "P1 GPU executable missing: $exe . Run RUN_NEEDLE_SPAWN_P1.ps1 once first."
}

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
if(-not(Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)){throw "Exact Beta class cache missing: $betaBin . Run P1 once first."}

$build=Join-Path $root 'build\needle-spawn-p2'
$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes|Out-Null
$src=Join-Path $srcDir 'Beta173NeedleSpawnOracleV2.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$OracleRef/tools/Beta173NeedleSpawnOracleV2.java" -OutFile $src
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes|Out-Null
Write-Host 'Compiling exact freestanding needle V2 oracle...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $src
if($LASTEXITCODE-ne 0){throw 'Needle V2 Java compile failed.'}
foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d)|Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){$s=Join-Path (Join-Path $betaBin 'lang') $name;$d=Join-Path (Join-Path $classes 'lang') $name;if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}}
$s=Join-Path $betaBin 'achievement\map.txt';$d=Join-Path $classes 'achievement\map.txt';if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
$cp="$classes;$betaBin"

$outputRoot=Join-Path $root 'out\needle_spawn_p2';New-Item -ItemType Directory -Force -Path $outputRoot|Out-Null
$lastRun=Join-Path $outputRoot 'LAST_RUN.txt'
if($Resume){
    if(-not(Test-Path $lastRun -PathType Leaf)){throw 'No needle P2 LAST_RUN exists.'}
    $output=(Get-Content $lastRun -Raw).Trim();if(-not(Test-Path $output -PathType Container)){throw "Run missing: $output"}
    $cfg=Join-Path $output 'P2_CONFIG.txt';$kv=@{};foreach($line in Get-Content $cfg){if($line-match '^(.*?)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
    $Count=[UInt64]$kv['COUNT'];$StartIndex=[UInt64]$kv['START_INDEX'];$RandomKey=[UInt64]$kv['RANDOM_KEY'];$SeedMode=$kv['SEED_MODE'];$MinDrop=[int]$kv['MIN_DROP'];$MinTopY=[int]$kv['MIN_TOP_Y'];$MinNeedleDepth=[int]$kv['MIN_NEEDLE_DEPTH'];$Top=[int]$kv['TOP']
}else{
    $stamp=Get-Date -Format 'yyyy-MM-dd_HH-mm-ss';$output=[IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"));New-Item -ItemType Directory -Force -Path $output|Out-Null;[IO.File]::WriteAllText($lastRun,$output,[Text.Encoding]::ASCII)
    if($null -eq $RandomKey){$bytes=New-Object byte[] 8;$rng=[Security.Cryptography.RandomNumberGenerator]::Create();try{$rng.GetBytes($bytes)}finally{$rng.Dispose()};$RandomKey=[BitConverter]::ToUInt64($bytes,0)}
    @("COUNT=$Count","START_INDEX=$StartIndex","RANDOM_KEY=$RandomKey","SEED_MODE=$SeedMode","MIN_DROP=$MinDrop","MIN_TOP_Y=$MinTopY","MIN_NEEDLE_DEPTH=$MinNeedleDepth","TOP=$Top")|Set-Content -LiteralPath (Join-Path $output 'P2_CONFIG.txt') -Encoding ASCII
}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 NEEDLE SPAWN P2 - FREESTANDING SHAFT'
Write-Host '=================================================================='
Write-Host 'A hit is NOT merely a 1x1 cap anymore.'
Write-Host "Exact hard gate: all 8 neighboring blocks remain AIR for >= $MinNeedleDepth consecutive levels down the shaft."
Write-Host "Exact hard gate: N/S/E/W open-air drops are each >= $MinDrop blocks."
Write-Host "Exact hard gate: final populated top is sand and the player stands directly on it."
Write-Host "Count=$Count RandomKey=$RandomKey MinTopY=$MinTopY MinDrop=$MinDrop MinNeedleDepth=$MinNeedleDepth Top=$Top"
Write-Host "Output=$output"
Write-Host ''

$args=@('--output',$output,'--count',$Count.ToString([Globalization.CultureInfo]::InvariantCulture),'--start-index',$StartIndex.ToString([Globalization.CultureInfo]::InvariantCulture),'--random-key',([UInt64]$RandomKey).ToString([Globalization.CultureInfo]::InvariantCulture),'--seed-mode',$SeedMode,'--batch',$Batch,'--terrain-threads',$TerrainThreads,'--min-drop',$MinDrop,'--min-top-y',$MinTopY,'--top',$Top,'--progress-ms','1000','--checkpoint-ms','5000')
if($Resume){$args+='--resume-existing'}
& $exe @args
if($LASTEXITCODE-ne 0){throw 'Needle P2 GPU scout failed.'}

$candidates=Join-Path $output 'top_candidates.csv'
if(-not(Test-Path $candidates -PathType Leaf)){throw 'GPU scout did not create top_candidates.csv'}
$verifyDir=Join-Path $output 'exact_verify';New-Item -ItemType Directory -Force -Path $verifyDir|Out-Null
$exact=Join-Path $verifyDir 'exact_needles_v2.csv'
Write-Host ''
Write-Host 'Running authoritative exact Beta V2 verification...'
& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173NeedleSpawnOracleV2 --input $candidates --output $exact --min-drop $MinDrop --min-top-y $MinTopY --min-needle-depth $MinNeedleDepth
if($LASTEXITCODE-ne 0){throw 'Needle V2 exact oracle failed.'}

Write-Host ''
Write-Host 'TOP EXACT FREESTANDING 1x1 SAND NEEDLES:'
$hits=@(Import-Csv $exact|Where-Object{$_.hit-eq'1'})
if($hits.Count-eq 0){Write-Host 'NONE. Do not inspect scout-only seeds as successful needles; see rejection counts above.'}else{$hits|Select-Object -First 25 seed,final_top_y,min_cardinal_drop,needle_depth,north_drop,south_drop,east_drop,west_drop,min_eight_drop,r2_min_drop,r4_min_drop|Format-Table -AutoSize}
Write-Host "EXACT_V2_CSV=$exact"
Write-Host "RESULT_DIR=$output"
