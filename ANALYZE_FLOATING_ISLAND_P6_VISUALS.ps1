param(
    [string]$RunDir = '',
    [ValidateRange(4,8)][int]$ChunkRadius = 4,
    [ValidateRange(1,10000)][int]$Top = 500,
    [ValidateRange(1,1024)][int]$Batch = 256
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ProjectRoot = $PSScriptRoot
. (Join-Path $ProjectRoot 'scripts\cursed-spawn-origin-p1-common.ps1')

if ([string]::IsNullOrWhiteSpace($RunDir)) {
    $last = Join-Path $ProjectRoot 'out\floating_island_spawn_p6\LAST_RUN.txt'
    if (-not (Test-Path $last -PathType Leaf)) { throw 'No P6 LAST_RUN.txt found; pass -RunDir explicitly.' }
    $RunDir = (Get-Content -LiteralPath $last -Raw).Trim()
}
$RunDir = [IO.Path]::GetFullPath($RunDir)
$input = Join-Path $RunDir 'verified_all.csv'
if (-not (Test-Path $input -PathType Leaf)) { throw "verified_all.csv not found: $input" }

$SourceRef = 'da58a8103b07088c0e5a2a0150b2def22e9fa9e5'
$RepoRaw = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef"
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
if ((Get-CoarseGpuApi $nativeSourceDir) -ne 'modern') { throw 'P6 visual analyzer requires the modern coarse GPU API.' }

$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p6-visual'
$srcDir = Join-Path $build 'source'
$genDir = Join-Path $build 'generated-chunk'
New-Item -ItemType Directory -Force -Path $build,$srcDir,$genDir | Out-Null

Write-Host 'Preparing P6 visual-insanity analyzer...'
$cpp = Join-Path $srcDir 'FloatingIslandSpawnVisualRankP6.cpp'
Invoke-WebRequest -UseBasicParsing "$RepoRaw/native/floating_island_spawn/FloatingIslandSpawnVisualRankP6.cpp" -OutFile $cpp

# The downloaded source lives in build/source, so make the embedded include resolve
# through -I native/highest_pillar_spawn instead of using its repository-relative path.
$text = [IO.File]::ReadAllText($cpp)
$text = $text.Replace('#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"', '#include "HighestPillarSpawnGpuFinder.cpp"')
# Empty sampled surroundings should not receive a perfect-clear score merely because
# a gigantic component bbox consumed the whole analysis window.
$text = $text.Replace('sameTotal > 0 ? 100.0 * (1.0 - static_cast<double>(sameCols) / sameTotal) : 100.0;', 'sameTotal > 0 ? 100.0 * (1.0 - static_cast<double>(sameCols) / sameTotal) : 0.0;')
$text = $text.Replace('nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(highCols) / nearTotal) : 100.0;', 'nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(highCols) / nearTotal) : 0.0;')
$text = $text.Replace('nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(baseCols) / nearTotal) : 100.0;', 'nearTotal > 0 ? 100.0 * (1.0 - static_cast<double>(baseCols) / nearTotal) : 0.0;')
$text = $text.Replace('r.nearestSameHeight = nearestSame;', 'r.nearestSameHeight = sameTotal > 0 ? nearestSame : 0.0;')
$text = $text.Replace('r.nearestHigh = nearestHigh;', 'r.nearestHigh = nearTotal > 0 ? nearestHigh : 0.0;')
[IO.File]::WriteAllText($cpp, $text, [Text.UTF8Encoding]::new($false))

# Generate the exact per-chunk 5x17x5 terrain headers already validated against
# the real Beta 1.7.3 save. Vanilla func_4061_a climate indices are 1,4,7,10,13.
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
[IO.File]::WriteAllText($corePath, $core, [Text.UTF8Encoding]::new($false))

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
if (-not $gpu.Contains($oldCoordinates)) { throw 'Could not patch chunk-local vanilla climate coordinates.' }
$gpu = $gpu.Replace($oldCoordinates, $newCoordinates)
[IO.File]::WriteAllText($gpuPath, $gpu, [Text.UTF8Encoding]::new($false))
$config = [IO.File]::ReadAllText($configPath).Replace('static constexpr int CHUNK_RADIUS = 4;', 'static constexpr int CHUNK_RADIUS = 0;')
[IO.File]::WriteAllText($configPath, $config, [Text.UTF8Encoding]::new($false))

$exe = Join-Path $build 'FloatingIslandSpawnVisualRankP6_AMD.exe'
$highestDir = Join-Path $ProjectRoot 'native\highest_pillar_spawn'
Write-Host "Compiling P6 visual analyzer for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$genDir" "-I$nativeSourceDir" "-I$highestDir" $cpp -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'P6 visual analyzer compilation failed.' }

$out = Join-Path $RunDir 'visual_analysis'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host ''
Write-Host '============================================================'
Write-Host ' P6 VISUAL INSANITY ANALYSIS'
Write-Host '============================================================'
Write-Host 'Scores every verified hit using surrounding terrain, not just island size.'
Write-Host 'Measures nearby same-height terrain, elevated clutter, isolation distance,'
Write-Host 'lower 1x1 pedestal depth, altitude, and unusual thin geometry.'
Write-Host 'Also writes biggest/longest/most-visual 1xN or Nx1 components.'
Write-Host "Input=$input"
Write-Host "Output=$out"
Write-Host ''

& $exe --input $input --output $out --chunk-radius $ChunkRadius --batch $Batch --terrain-threads 64 --top $Top
if ($LASTEXITCODE -ne 0) { throw 'P6 visual analysis failed.' }

function Add-Preview([System.Collections.Generic.List[string]]$lines, [string]$title, [string]$file, [int]$count = 20) {
    $lines.Add('')
    $lines.Add($title)
    if (-not (Test-Path $file -PathType Leaf)) { $lines.Add('  none'); return }
    $rows = @(Import-Csv -LiteralPath $file | Select-Object -First $count)
    if ($rows.Count -eq 0) { $lines.Add('  none'); return }
    foreach ($r in $rows) {
        $lines.Add(("  #{0} seed={1} score={2} blocks={3} footprint={4} span={5}x{6} Y={7}..{8} feetY={9} nearSame={10} nearHigh={11} lower1x1={12}" -f `
            $r.rank,$r.seed,$r.visual_score,$r.component_blocks,$r.footprint_columns,$r.span_x,$r.span_z,$r.min_y,$r.max_y,$r.player_feet_y,$r.nearest_same_height,$r.nearest_high,$r.lower_1x1_depth))
    }
}

$summary = New-Object 'System.Collections.Generic.List[string]'
$summary.Add('P6 VISUAL INSANITY SUMMARY')
$summary.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$summary.Add("Run: $RunDir")
$summary.Add('')
$summary.Add('Visual score favors isolation and clean surroundings. Absolute height is only a smaller bonus.')
$summary.Add('nearSame = nearest non-component terrain at roughly spawn height (feetY-2).')
$summary.Add('nearHigh = nearest non-component terrain at feetY-8. Distances are capped around 65 blocks.')
$summary.Add('lower1x1 = consecutive clean 1x1 levels on the lower spawn pedestal.')
Add-Preview $summary 'TOP VISUAL INSANITY' (Join-Path $out 'top_visual_insanity.csv')
Add-Preview $summary 'TOP CLEAN 1x1' (Join-Path $out 'top_clean_1x1.csv')
Add-Preview $summary 'TOP LOWER 1x1 PEDESTAL' (Join-Path $out 'top_lower_1x1_pedestal.csv')
Add-Preview $summary 'BIGGEST 1xN / Nx1' (Join-Path $out 'top_1xN_biggest.csv')
Add-Preview $summary 'LONGEST 1xN / Nx1' (Join-Path $out 'top_1xN_longest.csv')
Add-Preview $summary 'MOST VISUAL 1xN / Nx1' (Join-Path $out 'top_1xN_visual.csv')
Add-Preview $summary 'TOP ISOLATION' (Join-Path $out 'top_visual_isolation.csv')
$summaryPath = Join-Path $out 'VISUAL_SUMMARY.txt'
[IO.File]::WriteAllLines($summaryPath, $summary, [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host 'DONE.'
Write-Host "Summary: $summaryPath"
Write-Host "All ranked hits: $(Join-Path $out 'visual_rank_all.csv')"
Write-Host ''
Get-Content -LiteralPath $summaryPath | Select-Object -First 120
