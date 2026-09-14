param(
    [UInt64]$Count = 100000000,
    [UInt64]$StartIndex = 0,
    [Nullable[UInt64]]$RandomKey = $null,
    [ValidateSet('unique48','splitmix64')][string]$SeedMode = 'unique48',
    [ValidateRange(100000,100000000)][UInt64]$ScoutChunk = 1000000,
    [ValidateRange(256,32768)][int]$Batch = 32768,
    [ValidateSet(64,128,256)][int]$TerrainThreads = 64,
    [ValidateRange(1,60)][int]$MinPotentialDrop = 5,
    [ValidateRange(1,60)][int]$MinDrop = 5,
    [switch]$Resume
)

$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$root=$PSScriptRoot
# This P4 runner intentionally reuses the corrected native P2 scout already
# compiled on this machine, but stops running the expensive full client oracle
# on every deep-cave candidate. Instead it does:
#   native deep-cave scout -> exact spring-pocket geometry -> full client startup
# and only the tiny spring-pocket subset reaches the full startup oracle.
$SourceRef='bd0d74c79e8e9e58c06c78e35ee0e1f888fb8b91'
$scoutExe=Join-Path $root 'build\sand-wake-freefall-p1\SandWakeFreefallScoutP1_AMD.exe'
if(-not (Test-Path $scoutExe -PathType Leaf)){
    throw "Corrected P2 scout executable not found: $scoutExe . Run RUN_SAND_WAKE_FREEFALL_P2.ps1 once first."
}

$javac=(Get-Command javac.exe -ErrorAction SilentlyContinue)
if(-not $javac){$javac=(Get-Command javac -ErrorAction SilentlyContinue)}
if(-not $javac){throw 'javac not found'}
$javac=$javac.Source
$java=Join-Path (Split-Path $javac -Parent) 'java.exe'
$betaBin=Join-Path $root 'build\beta173-exact-client-oracle\mc_b1.7.3_release\1.7.3-LTS\bin\minecraft'
if(-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)){throw "Beta class cache missing: $betaBin"}

$build=Join-Path $root 'build\sand-wake-p4-pocket-first'
$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes | Out-Null
$pocketSrc=Join-Path $srcDir 'Beta173SandWakeSpringPocketProbe.java'
$oracleSrc=Join-Path $srcDir 'Beta173SandWakeFreefallOracleV2.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef/tools/Beta173SandWakeSpringPocketProbe.java" -OutFile $pocketSrc
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef/tools/Beta173SandWakeFreefallOracleV2.java" -OutFile $oracleSrc
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
Write-Host 'Compiling exact spring-pocket filter + V2 full-startup oracle...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $pocketSrc $oracleSrc
if($LASTEXITCODE -ne 0){throw 'P4 Java compile failed'}

foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d) | Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){
    $s=Join-Path (Join-Path $betaBin 'lang') $name; $d=Join-Path (Join-Path $classes 'lang') $name
    if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
}
$s=Join-Path $betaBin 'achievement\map.txt'; $d=Join-Path $classes 'achievement\map.txt'
if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
$cp="$classes;$betaBin"

$outputRoot=Join-Path $root 'out\sand_wake_freefall_p4_pocket_first'
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$lastRun=Join-Path $outputRoot 'LAST_RUN.txt'
if($Resume){
    if(-not (Test-Path $lastRun -PathType Leaf)){throw 'No P4 LAST_RUN.txt exists.'}
    $output=(Get-Content $lastRun -Raw).Trim()
    if(-not (Test-Path $output -PathType Container)){throw "P4 run directory missing: $output"}
}else{
    $stamp=Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
    $output=[IO.Path]::GetFullPath((Join-Path $outputRoot "run_$stamp"))
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    [IO.File]::WriteAllText($lastRun,$output,[Text.Encoding]::ASCII)
}

$scoutDir=Join-Path $output 'dry_cave_geometry'
$pocketDir=Join-Path $output 'spring_pockets'
$verifyDir=Join-Path $output 'exact_client_verify'
$tmpRoot=Join-Path $output '_pocket_input_tmp'
New-Item -ItemType Directory -Force -Path $scoutDir,$pocketDir,$verifyDir,$tmpRoot | Out-Null
$geometryBest=Join-Path $output 'BEST_GEOMETRY_POTENTIAL.txt'
$naturalBest=Join-Path $output 'BEST_AUTHORITATIVE_SAND_WAKE.txt'
$checkpoint=Join-Path $output 'checkpoint.txt'

if($Resume){
    $kv=@{}
    foreach($line in Get-Content $checkpoint){if($line -match '^(.*?)=(.*)$'){$kv[$matches[1]]=$matches[2]}}
    $StartIndex=[UInt64]$kv['START_INDEX']; $Count=[UInt64]$kv['COUNT']; $RandomKey=[UInt64]$kv['RANDOM_KEY']; $SeedMode=$kv['SEED_MODE']; $completed=[UInt64]$kv['COMPLETED']
    if($kv.ContainsKey('MIN_POTENTIAL_DROP')){$MinPotentialDrop=[int]$kv['MIN_POTENTIAL_DROP']}
    if($kv.ContainsKey('MIN_DROP')){$MinDrop=[int]$kv['MIN_DROP']}
}else{
    [UInt64]$completed=0
    if($null -eq $RandomKey){
        $bytes=New-Object byte[] 8
        $rng=[Security.Cryptography.RandomNumberGenerator]::Create()
        try{$rng.GetBytes($bytes)}finally{$rng.Dispose()}
        $RandomKey=[BitConverter]::ToUInt64($bytes,0)
    }
}

Write-Host ''
Write-Host '=================================================================='
Write-Host ' BETA 1.7.3 SAND-WAKE FREEFALL P4 - POCKET FIRST'
Write-Host '=================================================================='
Write-Host 'Stage 1: corrected native dormant-sand + dry-cave scout.'
Write-Host 'Stage 2: exact Beta one-step WorldGenLiquids spring-pocket geometry.'
Write-Host 'Stage 3: exact 17x17 client startup ONLY for spring-pocket seeds.'
Write-Host "Count=$Count StartIndex=$StartIndex Completed=$completed RandomKey=$RandomKey"
Write-Host "MinPotentialDrop=$MinPotentialDrop MinDrop=$MinDrop Output=$output"
Write-Host ''

$invariant=[Globalization.CultureInfo]::InvariantCulture
[UInt64]$totalDeep=0
[UInt64]$totalPocket=0
while($completed -lt $Count){
    [UInt64]$n=[Math]::Min([double]$ScoutChunk,[double]($Count-$completed))
    [UInt64]$index=$StartIndex+$completed
    Write-Host "--- P4 SCOUT start=$index count=$n ---"
    & $scoutExe --output $scoutDir --best-state $geometryBest --count $n.ToString($invariant) --start-index $index.ToString($invariant) `
        --random-key ([UInt64]$RandomKey).ToString($invariant) --seed-mode $SeedMode --batch $Batch `
        --terrain-threads $TerrainThreads --min-potential-drop $MinPotentialDrop --progress-ms 1000
    if($LASTEXITCODE -ne 0){throw "P4 native scout failed at start=$index"}

    $candidateFile=Join-Path $scoutDir ("candidates_{0}.csv" -f $index)
    $deepRows=@(Import-Csv $candidateFile)
    $deepCount=$deepRows.Count
    $totalDeep += [UInt64]$deepCount
    Write-Host "Deep-collapse geometry candidates this chunk: $deepCount"

    if($deepCount -gt 0){
        $tmpDir=Join-Path $tmpRoot ("chunk_{0}" -f $index)
        Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
        Copy-Item -Force $candidateFile (Join-Path $tmpDir (Split-Path $candidateFile -Leaf))
        $pocketOut=Join-Path $pocketDir ("spring_pockets_{0}.csv" -f $index)
        & $java -Xmx2g -cp $cp net.minecraft.src.Beta173SandWakeSpringPocketProbe --input-dir $tmpDir --output $pocketOut
        if($LASTEXITCODE -ne 0){throw "P4 spring-pocket probe failed at start=$index"}

        $pockets=@(Import-Csv $pocketOut | Where-Object {[int]$_.pocket_count -gt 0})
        $pocketCount=$pockets.Count
        $totalPocket += [UInt64]$pocketCount
        Write-Host "EXACT ONE-STEP SPRING POCKETS this chunk: $pocketCount | run total=$totalPocket"

        if($pocketCount -gt 0){
            foreach($r in $pockets){
                Write-Host ("POCKET seed={0} predictedDrop={1} source=({2},{3},{4}) target=(0,{5},0)" -f $r.seed,$r.predicted_drop,$r.best_source_x,$r.best_source_y,$r.best_source_z,$r.target_y)
            }
            $oracleInput=Join-Path $pocketDir ("oracle_candidates_{0}.csv" -f $index)
            $pockets | ForEach-Object {
                [pscustomobject]@{seed=$_.seed;sequence_index=$_.sequence_index;potential_drop=$_.predicted_drop}
            } | Export-Csv -NoTypeInformation -Encoding ASCII $oracleInput

            Write-Host 'Running authoritative full client startup on pocket seeds only...'
            & $java -Xmx4g '-Djava.awt.headless=true' -cp $cp net.minecraft.src.Beta173SandWakeFreefallOracleV2 `
                --input $oracleInput --output $verifyDir --best-state $naturalBest --min-drop $MinDrop --progress-every 1
            if($LASTEXITCODE -ne 0){throw "P4 full-startup oracle failed at start=$index"}
        }
    }

    $completed += $n
    @(
        'VERSION=SandWakeFreefallP4PocketFirst'
        "START_INDEX=$StartIndex"
        "COUNT=$Count"
        "COMPLETED=$completed"
        "NEXT_INDEX=$($StartIndex+$completed)"
        "RANDOM_KEY=$RandomKey"
        "SEED_MODE=$SeedMode"
        "MIN_POTENTIAL_DROP=$MinPotentialDrop"
        "MIN_DROP=$MinDrop"
    ) | Set-Content -LiteralPath $checkpoint -Encoding ASCII
    Write-Host "P4 CHECKPOINT $completed/$Count | deep=$totalDeep pockets=$totalPocket"
}

Write-Host ''
Write-Host 'P4 SEARCH COMPLETE.'
Write-Host "Deep geometry rows seen this process: $totalDeep"
Write-Host "Exact one-step pocket rows seen this process: $totalPocket"
$summary=Join-Path $verifyDir 'SUMMARY.txt'
if(Test-Path $summary){Get-Content $summary}else{Write-Host 'No pocket reached the full startup oracle in this run.'}
$top=Join-Path $verifyDir 'top_natural_sand_wake_freefalls_v2.csv'
if(Test-Path $top){
    Write-Host ''
    Write-Host 'TOP AUTHORITATIVE NATURAL SAND-WAKE FREEFALLS:'
    Import-Csv $top | Select-Object -First 25 | Format-Table -AutoSize
}
Write-Host "RESULT_DIR=$output"
