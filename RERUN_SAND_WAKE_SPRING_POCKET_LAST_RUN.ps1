param()
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$root=$PSScriptRoot
$commit='fb209a5d05d76cc160809bb75e39a947690d641a'
$last=Join-Path $root 'out\sand_wake_freefall_p2\LAST_RUN.txt'
if(-not (Test-Path $last -PathType Leaf)){throw 'No P2 LAST_RUN.txt found.'}
$run=(Get-Content $last -Raw).Trim()
if(-not (Test-Path $run -PathType Container)){throw "Run directory missing: $run"}
$inputDir=Join-Path $run 'dry_cave_geometry'
if(-not (Test-Path $inputDir -PathType Container)){throw "Candidate directory missing: $inputDir"}

$javac=(Get-Command javac.exe -ErrorAction SilentlyContinue)
if(-not $javac){$javac=(Get-Command javac -ErrorAction SilentlyContinue)}
if(-not $javac){throw 'javac not found'}
$javac=$javac.Source
$java=Join-Path (Split-Path $javac -Parent) 'java.exe'
$betaBin=Join-Path $root 'build\beta173-exact-client-oracle\mc_b1.7.3_release\1.7.3-LTS\bin\minecraft'
if(-not (Test-Path (Join-Path $betaBin 'net\minecraft\src\World.class') -PathType Leaf)){throw "Beta class cache missing: $betaBin"}

$build=Join-Path $root 'build\sand-wake-spring-pocket-probe'
$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes | Out-Null
$src=Join-Path $srcDir 'Beta173SandWakeSpringPocketProbe.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$commit/tools/Beta173SandWakeSpringPocketProbe.java" -OutFile $src
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
Write-Host 'Compiling exact sand-wake spring-pocket probe...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $src
if($LASTEXITCODE -ne 0){throw 'Spring-pocket probe compile failed'}

foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d) | Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){
  $s=Join-Path (Join-Path $betaBin 'lang') $name; $d=Join-Path (Join-Path $classes 'lang') $name
  if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
}
$s=Join-Path $betaBin 'achievement\map.txt'; $d=Join-Path $classes 'achievement\map.txt'
if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}

$outDir=Join-Path $run 'spring_pocket_probe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out=Join-Path $outDir 'spring_pocket_all.csv'
$cp="$classes;$betaBin"
Write-Host "Testing one-step spring pockets for every P2 candidate in $inputDir"
& $java -Xmx2g -cp $cp net.minecraft.src.Beta173SandWakeSpringPocketProbe --input-dir $inputDir --output $out
if($LASTEXITCODE -ne 0){throw 'Spring-pocket probe failed'}

Write-Host ''
Write-Host 'Exact one-step spring pockets:'
$rows=@(Import-Csv $out | Where-Object {[int]$_.pocket_count -gt 0} | Sort-Object {[int]$_.predicted_drop} -Descending)
if($rows.Count -eq 0){
  Write-Host 'NONE'
}else{
  $rows | Select-Object seed,predicted_drop,bottom_sand_y,target_y,pocket_count,best_source_x,best_source_y,best_source_z,best_source_center_id | Format-Table -AutoSize
}
Write-Host "SPRING_POCKET_CSV=$out"
