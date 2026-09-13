param(
    [string]$RunDir = '',
    [ValidateRange(3,8)][int]$ChunkRadius = 6,
    [ValidateRange(1,10000)][int]$Top = 500,
    [ValidateRange(1,512)][int]$Batch = 96
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

$SourceRef = 'c5e447c2e0953ef3b1f36af095914dae86179525'
$RepoRaw = "https://raw.githubusercontent.com/Jawiskatten/BetaSeedFinder/$SourceRef"
$hipcc = Get-Hipcc
$arches = @(Get-HipGpuArchitectures $hipcc)
$archKey = ($arches -join ',')
$archArgs = @($arches | ForEach-Object { "--offload-arch=$_" })
$nativeSourceDir = Get-BetaGpuNativeSourceDir $ProjectRoot
if ((Get-CoarseGpuApi $nativeSourceDir) -ne 'modern') { throw 'P6 SkyBlock analyzer requires the modern coarse GPU API.' }

$build = Join-Path $ProjectRoot 'build\floating-island-spawn-p6-skyblock'
$srcDir = Join-Path $build 'source'
$genDir = Join-Path $build 'generated-chunk'
New-Item -ItemType Directory -Force -Path $build,$srcDir,$genDir | Out-Null

Write-Host 'Preparing P6 SkyBlock candidate analyzer...'
$visual = Join-Path $srcDir 'FloatingIslandSpawnVisualRankP6.cpp'
$sky = Join-Path $srcDir 'FloatingIslandSpawnSkyblockPrefilterP6.cpp'
Invoke-WebRequest -UseBasicParsing "$RepoRaw/native/floating_island_spawn/FloatingIslandSpawnVisualRankP6.cpp" -OutFile $visual
Invoke-WebRequest -UseBasicParsing "$RepoRaw/native/floating_island_spawn/FloatingIslandSpawnSkyblockPrefilterP6.cpp" -OutFile $sky

# Both sources are downloaded beside each other. Redirect the visual analyzer's embedded
# HighestPillar include through our include path, and add cstring for the SkyBlock Java RNG helper.
$visualText = [IO.File]::ReadAllText($visual)
$visualText = $visualText.Replace('#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"', '#include "HighestPillarSpawnGpuFinder.cpp"')
[IO.File]::WriteAllText($visual, $visualText, [Text.UTF8Encoding]::new($false))
$skyText = [IO.File]::ReadAllText($sky)
if (-not $skyText.Contains('#include <cstring>')) {
    $skyText = $skyText.Replace('#include <deque>', "#include <cstring>`r`n#include <deque>")
}
# Remove one deliberately unused geometry-cover probe from the source downloaded at the pinned commit.
$skyText = $skyText.Replace('            const int cover=populationCoverCount(x,y); // typo-resistant overload not available; corrected below' + "`r`n" + '            (void)cover;' + "`r`n", '')
$skyText = $skyText.Replace('            const int cover=populationCoverCount(x,y); // typo-resistant overload not available; corrected below' + "`n" + '            (void)cover;' + "`n", '')
# Save all rows rather than limiting the file named all_skyblock_prefilter_ranked.csv to 10k.
$skyText = $skyText.Replace('static_cast<int>(std::min<std::size_t>(10000,out.size()))', 'static_cast<int>(out.size())')
[IO.File]::WriteAllText($sky, $skyText, [Text.UTF8Encoding]::new($false))

# Generate the same exact chunk-local terrain headers that passed the P6 vanilla regression.
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

$exe = Join-Path $build 'FloatingIslandSpawnSkyblockPrefilterP6_AMD.exe'
$highestDir = Join-Path $ProjectRoot 'native\highest_pillar_spawn'
Write-Host "Compiling P6 SkyBlock analyzer for $archKey..."
& $hipcc -O3 -std=c++17 -x hip @archArgs '-DSKYBLOCK_COARSE_API_MODERN=1' "-I$srcDir" "-I$genDir" "-I$nativeSourceDir" "-I$highestDir" $sky -o $exe | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'P6 SkyBlock analyzer compilation failed.' }

$out = Join-Path $RunDir 'skyblock_analysis'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Write-Host ''
Write-Host '============================================================'
Write-Host ' P6 SKYBLOCK RESOURCE CANDIDATE ANALYSIS'
Write-Host '============================================================'
Write-Host 'Pass 1: exact P6 base-terrain component geometry + Beta biome at origin.'
Write-Host 'Pass 2: tree-host clearance, 0,0 tree structural candidates, spring slots,'
Write-Host '        waterfall drops, and early water/lava lake population attempts.'
Write-Host ''
Write-Host 'IMPORTANT: tree and spring files are CANDIDATE rankings, not final proof of'
Write-Host 'population. Beta population RNG is affected by earlier successful dungeons,'
Write-Host 'trees, caves and feature mutations. The lake-attempt coordinates themselves'
Write-Host 'are replayed from the early population RNG; lake success is terrain-only.'
Write-Host 'We use this pass to crush 23k hits into a small list, then verify the winners'
Write-Host 'in actual Beta saves.'
Write-Host ''
Write-Host "Input=$input"
Write-Host "Output=$out"
Write-Host "ChunkRadius=$ChunkRadius Batch=$Batch"
Write-Host ''

& $exe --input $input --output $out --chunk-radius $ChunkRadius --batch $Batch --terrain-threads 64 --top $Top
if ($LASTEXITCODE -ne 0) { throw 'P6 SkyBlock candidate analysis failed.' }

function Add-Preview([System.Collections.Generic.List[string]]$lines, [string]$title, [string]$file, [int]$count = 20) {
    $lines.Add('')
    $lines.Add($title)
    if (-not (Test-Path $file -PathType Leaf)) { $lines.Add('  none'); return }
    $rows = @(Import-Csv -LiteralPath $file | Select-Object -First $count)
    if ($rows.Count -eq 0) { $lines.Add('  none'); return }
    foreach ($r in $rows) {
        $lines.Add(("  #{0} seed={1} biome={2} blocks={3} foot={4} span={5}x{6} feetY={7} treeHosts={8} treeOpp={9} tree0={10} springs={11} falls={12} maxFall={13} waterExp={14} lavaExp={15} waterLake={16} lavaLake={17}" -f `
            $r.rank,$r.seed,$r.biome,$r.component_blocks,$r.footprint,$r.span_x,$r.span_z,$r.feet_y,$r.tree_host_columns,$r.tree_opportunity,$r.origin_tree_structural,$r.spring_slots,$r.waterfall_slots,$r.max_fall,$r.expected_water_spring_hits,$r.expected_lava_spring_hits,$r.water_lake_likely_on_island,$r.lava_lake_likely_on_island))
    }
}

$summary = New-Object 'System.Collections.Generic.List[string]'
$summary.Add('P6 SKYBLOCK RESOURCE CANDIDATE SUMMARY')
$summary.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$summary.Add("Run: $RunDir")
$summary.Add('')
$summary.Add('treeHosts = upper-component columns structurally able to host a minimum oak tree on the base terrain.')
$summary.Add('treeOpp = heuristic natural-tree opportunity using Beta biome tree density and population coverage.')
$summary.Add('tree0 = a tree at x=0,z=0 is structurally possible on the actual spawn component.')
$summary.Add('springs/falls = WorldGenLiquids-compatible stone geometry proxy; maxFall is exposed drop length.')
$summary.Add('waterExp/lavaExp = expected RNG hits on compatible slots from Beta 50-water / 20-lava spring attempts.')
$summary.Add('waterLake/lavaLake = early population lake attempt that likely generates into the component on base terrain.')
$summary.Add('')
$summary.Add('These are shortlists. Final resource presence must be checked against actual Beta population/caves.')
Add-Preview $summary 'TOP TREE / SKYBLOCK ISLAND CANDIDATES' (Join-Path $out 'top_tree_candidates.csv')
Add-Preview $summary 'TOP TREE AT 0,0 CANDIDATES' (Join-Path $out 'top_tree_at_origin_candidates.csv')
Add-Preview $summary 'TOP TREE + WATERFALL CANDIDATES' (Join-Path $out 'top_tree_waterfall_candidates.csv')
Add-Preview $summary 'TOP TREE + LAVAFALL CANDIDATES' (Join-Path $out 'top_tree_lavafall_candidates.csv')
Add-Preview $summary 'TOP TREE + BOTH FLUID CANDIDATES' (Join-Path $out 'top_tree_both_fluids_candidates.csv')
Add-Preview $summary 'TOP TREE + WATER/LAVA LAKE CANDIDATES' (Join-Path $out 'top_tree_lake_candidates.csv')
$summaryPath = Join-Path $out 'SKYBLOCK_SUMMARY.txt'
[IO.File]::WriteAllLines($summaryPath, $summary, [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host 'DONE.'
Write-Host "Summary: $summaryPath"
Write-Host "All ranked candidates: $(Join-Path $out 'all_skyblock_prefilter_ranked.csv')"
Write-Host ''
Get-Content -LiteralPath $summaryPath | Select-Object -First 170
