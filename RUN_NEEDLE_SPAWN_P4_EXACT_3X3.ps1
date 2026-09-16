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
    [ValidateRange(25,10000)][int]$Top = 5000
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=$PSScriptRoot
$OracleRef='9574fbc4ed942a7f6540a160117a41b4a5f2526a'

# IMPORTANT: the GPU worker is only a cheap positive-quadrant prescreen. It does
# NOT model the -X/-Z chunks around world origin. Never treat its NEW BEST lines
# as hits. The Java stage below loads the exact Beta client world (17x17 startup)
# and checks all eight cells of the real 3x3 ring around (0,0) at every shaft Y.
$exe=Join-Path $root 'build\needle-spawn-p1\NeedleSpawnP1_AMD.exe'
if(-not(Test-Path $exe -PathType Leaf)){throw "Needle GPU executable missing: $exe . Run RUN_NEEDLE_SPAWN_P1.ps1 once first."}

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

$build=Join-Path $root 'build\needle-spawn-p4-exact3x3'
$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes|Out-Null
$src=Join-Path $srcDir 'Beta173NeedleSpawnOracleV2.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$OracleRef/tools/Beta173NeedleSpawnOracleV2.java" -OutFile $src
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes|Out-Null
Write-Host 'Compiling authoritative exact 3x3 needle verifier...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $src
if($LASTEXITCODE-ne 0){throw 'Exact 3x3 verifier compile failed.'}
foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d)|Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){$s=Join-Path (Join-Path $betaBin 'lang') $name;$d=Join-Path (Join-Path $classes 'lang') $name;if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}}
$s=Join-Path $betaBin 'achievement\map.txt';$d=Join-Path $classes 'achievement\map.txt';if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
$cp="$classes;$betaBin"

$outputRoot=Join-Path $root 'out\needle_spawn_p4_exact3x3';New-Item -ItemType Directory -Force -Path $outputRoot|Out-Null
$stamp=Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$output=[IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"));New-Item -ItemType Directory -Force -Path $output|Out-Null
if($null -eq $RandomKey){$bytes=New-Object byte[] 8;$rng=[Security.Cryptography.RandomNumberGenerator]::Create();try{$rng.GetBytes($bytes)}finally{$rng.Dispose()};$RandomKey=[BitConverter]::ToUInt64($bytes,0)}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 NEEDLE SPAWN P4 - EXACT 3x3'
Write-Host '=================================================================='
Write-Host 'GPU stage = PRESCREEN ONLY. Its best seeds are NOT results.'
Write-Host 'Authoritative stage generates the real surrounding Beta chunks.'
Write-Host "Exact requirement: for >= $MinNeedleDepth levels, the center is solid and all 8 adjacent/diagonal cells in the 3x3 are AIR."
Write-Host "Exact N/S/E/W drop requirement: >= $MinDrop. Final top must be sand and player must stand on it."
Write-Host "Count=$Count RandomKey=$RandomKey MinTopY=$MinTopY MinDrop=$MinDrop MinNeedleDepth=$MinNeedleDepth Top=$Top"
Write-Host "Output=$output"
Write-Host ''

$args=@('--output',$output,'--count',$Count.ToString([Globalization.CultureInfo]::InvariantCulture),'--start-index',$StartIndex.ToString([Globalization.CultureInfo]::InvariantCulture),'--random-key',([UInt64]$RandomKey).ToString([Globalization.CultureInfo]::InvariantCulture),'--seed-mode',$SeedMode,'--batch',$Batch,'--terrain-threads',$TerrainThreads,'--min-drop',$MinDrop,'--min-top-y',$MinTopY,'--top',$Top,'--progress-ms','1000','--checkpoint-ms','5000')
& $exe @args
if($LASTEXITCODE-ne 0){throw 'Needle GPU prescreen failed.'}

$candidates=Join-Path $output 'top_candidates.csv'
if(-not(Test-Path $candidates -PathType Leaf)){throw 'GPU prescreen did not create top_candidates.csv'}
$verifyDir=Join-Path $output 'exact_verify';New-Item -ItemType Directory -Force -Path $verifyDir|Out-Null
$exact=Join-Path $verifyDir 'all_exact_3x3.csv'
Write-Host ''
Write-Host '============================================================='
Write-Host ' NOW DOING THE REAL CHECK: exact Beta chunks + full 3x3 ring'
Write-Host '============================================================='
& $java '-Xmx4G' '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173NeedleSpawnOracleV2 --input $candidates --output $exact --min-drop $MinDrop --min-top-y $MinTopY --min-needle-depth $MinNeedleDepth
if($LASTEXITCODE-ne 0){throw 'Exact 3x3 verification failed.'}

$all=@(Import-Csv $exact)
$hits=@($all|Where-Object{$_.hit-eq'1'})
$confirmed=Join-Path $output 'CONFIRMED_NEEDLES_ONLY.csv'
$hits|Export-Csv -NoTypeInformation -Encoding ASCII $confirmed

Write-Host ''
Write-Host '=================================================================='
Write-Host ' CONFIRMED NEEDLES ONLY - THESE are the seeds worth opening'
Write-Host '=================================================================='
if($hits.Count-eq 0){
    Write-Host 'NONE in this run.'
}else{
    $hits|Sort-Object {[int]$_.needle_depth} -Descending | Select-Object seed,final_top_y,needle_depth,min_cardinal_drop,north_drop,south_drop,east_drop,west_drop | Format-Table -AutoSize
}
Write-Host "CONFIRMED_CSV=$confirmed"
Write-Host "ALL_EXACT_CSV=$exact"
Write-Host "RESULT_DIR=$output"
