param()
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$root=$PSScriptRoot
$commit='e6bcb0ccf752fd2b677a50f1047354ca0b127a8b'
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

$build=Join-Path $root 'build\sand-wake-trigger-probe'
$srcDir=Join-Path $build 'src\net\minecraft\src'
$classes=Join-Path $build 'classes'
New-Item -ItemType Directory -Force -Path $srcDir,$classes | Out-Null
$src=Join-Path $srcDir 'Beta173SandWakeTriggerProbe.java'
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$commit/tools/Beta173SandWakeTriggerProbe.java" -OutFile $src
Remove-Item -Recurse -Force $classes -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $classes | Out-Null
Write-Host 'Compiling exact sand-wake trigger probe...'
& $javac -source 8 -target 8 -encoding UTF-8 -cp $betaBin -d $classes $src
if($LASTEXITCODE -ne 0){throw 'Trigger probe compile failed'}

foreach($d in @('lang','achievement')){New-Item -ItemType Directory -Force -Path (Join-Path $classes $d) | Out-Null}
foreach($name in @('en_US.lang','stats_US.lang')){
  $s=Join-Path (Join-Path $betaBin 'lang') $name; $d=Join-Path (Join-Path $classes 'lang') $name
  if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}
}
$s=Join-Path $betaBin 'achievement\map.txt'; $d=Join-Path $classes 'achievement\map.txt'
if(Test-Path $s -PathType Leaf){Copy-Item -Force $s $d}else{[IO.File]::WriteAllText($d,'')}

$outDir=Join-Path $run 'trigger_probe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out=Join-Path $outDir 'trigger_probe_all.csv'
$cp="$classes;$betaBin"
Write-Host "Probing every P2 candidate in $inputDir"
& $java -Xmx2g -cp $cp net.minecraft.src.Beta173SandWakeTriggerProbe --input-dir $inputDir --output $out
if($LASTEXITCODE -ne 0){throw 'Trigger probe failed'}

Write-Host ''
Write-Host 'Closest immediate liquid events:'
Import-Csv $out | Where-Object {$_.min_liquid_dist -ne 'NONE'} | Sort-Object {[int]$_.min_liquid_dist}, {[int]$_.predicted_drop} | Select-Object -First 25 seed,predicted_drop,pre_bottom_y,min_liquid_dist,nearest_liquid_x,nearest_liquid_y,nearest_liquid_z,nearest_liquid_id,direct_immediate_liquid_events,natural_wake | Format-Table -AutoSize
Write-Host ''
Write-Host 'Direct immediate neighbor contacts (the actual trigger condition):'
$direct=@(Import-Csv $out | Where-Object {[int]$_.direct_immediate_events -gt 0})
if($direct.Count -eq 0){Write-Host 'NONE'}else{$direct | Select-Object seed,predicted_drop,direct_immediate_events,direct_immediate_liquid_events,first_direct_x,first_direct_y,first_direct_z,first_direct_id,natural_wake | Format-Table -AutoSize}
Write-Host "TRIGGER_PROBE_CSV=$out"
