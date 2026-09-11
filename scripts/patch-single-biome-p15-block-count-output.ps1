param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\single_biome_radius.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Source file not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath)

if ($text.Contains('P15_BLOCK_COUNT_OUTPUT')) {
    Write-Host 'P15 block-count output is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('RAINFOREST_P14_TARGET')) {
    throw 'P15 expects the current Rainforest P14 source first.'
}
if (-not $text.Contains('rainforestBlocks=')) {
    throw 'P15 could not find the Rainforest P14 coverage label.'
}

$backupPath = $sourcePath + '.p14-before-p15-output.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P15_BLOCK_COUNT_OUTPUT
# The target window has a fixed size, so percentage coverage and raw block count
# contain the same information. For the current rainforest hunt the raw count is
# much easier to read: e.g. rainforestBlocks=137033 instead of 16.918%.
# This patch is display-only. Search/probe/exact ranking semantics are unchanged.

# Exact record / verify output: remove realCoverage percentage and denominator,
# leaving the exact number of rainforest positions inside the target square.
$recordPattern = '(?s)        std::cout << " realCoverage=" << std::fixed << std::setprecision\(3\)\r?\n                  << realCoveragePercent\(\*coverage\) << "%"\r?\n                  << " rainforestBlocks=" << coverage->sameCount\r?\n                  << ''/'' << coverage->totalCount;'
$recordMatches = [regex]::Matches($text, $recordPattern)
if ($recordMatches.Count -ne 1) {
    throw "Expected exactly one record coverage-output block, found $($recordMatches.Count)."
}
$recordReplacement = @'
        std::cout << " rainforestBlocks=" << coverage->sameCount;
'@
$text = [regex]::Replace($text, $recordPattern, $recordReplacement.TrimEnd(), 1)

# Periodic status output: show the best record's rainforest block count instead
# of its percentage coverage.
$statusPattern = '(?s)                std::cout << " realCoverage=" << std::fixed << std::setprecision\(3\)\r?\n                          << realCoveragePercent\(bestCoverage\) << "%"\r?\n                          << " bestSeed=" << bestExactSeed'
$statusMatches = [regex]::Matches($text, $statusPattern)
if ($statusMatches.Count -ne 1) {
    throw "Expected exactly one status coverage-output block, found $($statusMatches.Count)."
}
$statusReplacement = @'
                std::cout << " rainforestBlocks=" << bestCoverage.sameCount
                          << " bestSeed=" << bestExactSeed
'@
$text = [regex]::Replace($text, $statusPattern, $statusReplacement.TrimEnd(), 1)

# Add a durable marker near the P14 banner so rerunning the patch is idempotent.
$banner = 'P14 scout: RAINFOREST-only | hot-temp gate + lazy exact rain | fast permutation RNG | warp votes | tuned 4x16'
if (-not $text.Contains($banner)) {
    throw 'Could not find the P14 scout banner.'
}
$text = $text.Replace(
    $banner,
    'P15_BLOCK_COUNT_OUTPUT | P14 scout: RAINFOREST-only | hot-temp gate + lazy exact rain | fast permutation RNG | warp votes | tuned 4x16'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied P15 block-count output.' -ForegroundColor Green
Write-Host 'RECORD/VERIFY now print rainforestBlocks=N instead of realCoverage=P% and N/total.'
Write-Host 'Periodic status now prints rainforestBlocks=N instead of realCoverage=P%.'
Write-Host 'Search behavior is unchanged; this is output formatting only.'
