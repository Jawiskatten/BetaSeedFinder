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

if ($text.Contains('TU4_WATER_P10_RESTORE_PARTIAL_SORT')) {
    Write-Host 'TU4 Water P10 restore-partial-sort is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P9_LINEAR_TOP_POOLS')) {
    throw 'P10 requires TU4 Water P9 first.'
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P10 expects the P8 batched exact evaluator underneath P9.'
}

$backupPath = $sourcePath + '.p9-before-water-p10.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P9 benchmarked slower on the RX 7800 XT: roughly 2.55M seeds/s versus P8's
# ~2.70M seeds/s. With poolN only ~192, std::partial_sort's heap-based
# O(N log K) path is cheaper here than three nth_element partition passes plus
# three pool sorts. Restore P8's proven selection algorithm while keeping P8's
# batched exact evaluator and P9's harmless exactSeeds buffer reuse.

$markerPos = $text.IndexOf('// TU4_WATER_P9_LINEAR_TOP_POOLS')
if ($markerPos -lt 0) {
    throw 'Could not locate P9 marker.'
}
$marker = @'
// TU4_WATER_P10_RESTORE_PARTIAL_SORT
// P9 nth_element benchmarked slower on RX 7800 XT. Restore P8/P6 partial_sort
// top-pool selection; keep P8 batched exact evaluation and identical finalists.
'@
$text = $text.Insert($markerPos, $marker + "`n")

$selectStart = $text.IndexOf('            // We only need poolN candidates from each ordering.')
if ($selectStart -lt 0) {
    throw 'Could not locate P9 nth_element selection block start.'
}
$selectEndNeedle = '            int selectedCount = 0;'
$selectEnd = $text.IndexOf($selectEndNeedle, $selectStart)
if ($selectEnd -lt 0) {
    throw 'Could not locate P9 nth_element selection block end.'
}

$restoredSelection = @'
            // P10: restore the P8/P6 selection path. For the tiny retained pool
            // (192 at TopExact=24), partial_sort is faster on this RX 7800 XT
            // system than nth_element + a second sort pass.
            if (poolN < count) {
                std::partial_sort(indices.begin(), indices.begin() + poolN, indices.end(), betterPenalty);
                std::partial_sort(hardOrder.begin(), hardOrder.begin() + poolN, hardOrder.end(), betterHard);
                std::partial_sort(residualOrder.begin(), residualOrder.begin() + poolN, residualOrder.end(), betterResidual);
            } else {
                std::sort(indices.begin(), indices.end(), betterPenalty);
                std::sort(hardOrder.begin(), hardOrder.end(), betterHard);
                std::sort(residualOrder.begin(), residualOrder.end(), betterResidual);
            }

'@
$text = $text.Remove($selectStart, $selectEnd - $selectStart).Insert($selectStart, $restoredSelection)

$text = $text.Replace(
    'Scout P9: P8 batched exact + linear-time top-pool selection; P6 portfolio and exact metric unchanged.',
    'Scout P10: P8 batched exact + restored fast partial_sort pools; P6 portfolio and exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P10: restored P8 partial_sort finalist selection.' -ForegroundColor Green
Write-Host 'P9 nth_element path removed; P8 batched exact evaluator remains.'
Write-Host 'P6 16/4/4 finalist portfolio and P7 800x800 metric are unchanged.'
Write-Host 'P9 exactSeeds buffer reuse is retained (harmless tiny allocation optimization).'
Write-Host 'Expected target: return from ~2.55M toward the P8 ~2.70M seeds/s baseline.'