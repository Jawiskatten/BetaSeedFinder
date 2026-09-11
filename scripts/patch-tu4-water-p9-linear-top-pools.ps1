param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\tu4_water_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "TU4 water source not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P9_LINEAR_TOP_POOLS')) {
    Write-Host 'TU4 Water P9 linear top-pool selection is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P9 requires TU4 Water P8 batched exact evaluator first.'
}

$backupPath = $sourcePath + '.p8-before-water-p9.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P8 removed most exact-evaluator launch/sync overhead. At ~2.7M seeds/s the
# remaining CPU-side finalist selection matters more: P6/P8 partial_sort all
# 131072 candidates three separate times to retain only a tiny top pool (192 at
# TopExact=24). P9 preserves the exact same three ranking comparators and pool
# size, but uses nth_element (linear average time) then sorts only the retained
# pool. Search semantics and exact metric are unchanged.

$markerPos = $text.IndexOf('// TU4_WATER_P8_BATCHED_EXACT')
if ($markerPos -lt 0) {
    throw 'Could not locate P8 marker.'
}
$marker = @'
// TU4_WATER_P9_LINEAR_TOP_POOLS
// Use nth_element + tiny-pool sort instead of three full partial_sort passes.
// The same P6 ranking portfolio and exact finalists are retained.
'@
$text = $text.Insert($markerPos, $marker + "`n")

$selectStart = $text.IndexOf('            if (poolN < count) {')
if ($selectStart -lt 0) {
    throw 'Could not locate P6/P8 top-pool selection block.'
}
$selectEndNeedle = '            int selectedCount = 0;'
$selectEnd = $text.IndexOf($selectEndNeedle, $selectStart)
if ($selectEnd -lt 0) {
    throw 'Could not locate end of P6/P8 top-pool selection block.'
}

$newSelection = @'
            // We only need poolN candidates from each ordering. partial_sort
            // over all count entries costs O(count log poolN); nth_element
            // partitions in average O(count), then we sort only the tiny pool.
            auto selectTopPool = [&](std::vector<int>& order, const auto& better) {
                if (poolN < count) {
                    auto middle = order.begin() + poolN;
                    std::nth_element(order.begin(), middle, order.end(), better);
                    std::sort(order.begin(), middle, better);
                } else {
                    std::sort(order.begin(), order.end(), better);
                }
            };
            selectTopPool(indices, betterPenalty);
            selectTopPool(hardOrder, betterHard);
            selectTopPool(residualOrder, betterResidual);

'@
$text = $text.Remove($selectStart, $selectEnd - $selectStart).Insert($selectStart, $newSelection)

# Reuse the tiny exact seed vector across batches instead of allocating it on
# every iteration. This is a small win, but essentially free once P8 is batched.
$declNeedle = '        std::vector<int> selectedIndices(static_cast<std::size_t>(o.topExact));'
$declPos = $text.IndexOf($declNeedle)
if ($declPos -lt 0) {
    throw 'Could not locate selectedIndices declaration.'
}
$declInsert = $declNeedle + "`n        std::vector<std::int64_t> exactSeeds(static_cast<std::size_t>(o.topExact));"
$text = $text.Remove($declPos, $declNeedle.Length).Insert($declPos, $declInsert)

$localSeeds = '            std::vector<std::int64_t> exactSeeds(static_cast<std::size_t>(exactN));'
$localSeedsPos = $text.IndexOf($localSeeds)
if ($localSeedsPos -lt 0) {
    throw 'Could not locate P8 per-batch exactSeeds allocation.'
}
$text = $text.Remove($localSeedsPos, $localSeeds.Length).Insert(
    $localSeedsPos,
    '            exactSeeds.resize(static_cast<std::size_t>(exactN));'
)

$text = $text.Replace(
    'Scout P8: P7 800x800 + P6 diverse finalists + batched exact GPU evaluation; exact metric unchanged.',
    'Scout P9: P8 batched exact + linear-time top-pool selection; P6 portfolio and exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P9 linear-time finalist pool selection.' -ForegroundColor Green
Write-Host 'Three partial_sort passes -> nth_element + sort only the retained tiny pools.'
Write-Host 'P6 16/4/4 finalist portfolio, P7 800x800 metric, and P8 batched exact evaluator are unchanged.'
Write-Host 'Exact seed scratch allocation is also reused across batches.'
Write-Host 'Benchmark against the P8 baseline of ~2.71M seeds/s with Batch=131072 TopExact=24.'