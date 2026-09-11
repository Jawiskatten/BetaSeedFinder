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

if ($text.Contains('TU4_WATER_P16_PURE_SCREEN_TOP4')) {
    Write-Host 'TU4 Water P16 pure screen top-4 is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    throw 'P16 requires the structurally-applied P15 top-5 source first.'
}
if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    throw 'P16 expects the P12c direct-screen chain underneath P15.'
}

$backupPath = $sourcePath + '.p15-before-water-p16.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P13b ground-truth histogram over 2000 batches:
# ranks 0..3 held the true best-of-24 1914/2000 = 95.70% of the time.
# P15 top-5 held 1959/2000 = 97.95%. This benchmark removes one more full
# 201x201 exact evaluation per batch. P16 is worth keeping only if the raw-rate
# gain is enough to offset the 2.25 percentage-point recall loss.

$markerPos = $text.IndexOf('// TU4_WATER_P15_PURE_SCREEN_TOP5')
if ($markerPos -lt 0) { throw 'Could not locate P15 marker.' }
$marker = @'
// TU4_WATER_P16_PURE_SCREEN_TOP4
// Benchmark route: exact only screen ranks 0..3 at TopExact=24.
// P13b measured 1914/2000 = 95.70% best-of-24 recall for pure screen top-4.
// Final exact metric is unchanged; only finalist routing changes.
'@
$text = $text.Insert($markerPos, $marker + "`n")

$oldFormula = '    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 4) / 5));'
$newFormula = '    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 5) / 6));'
$count = ([regex]::Matches($text, [regex]::Escape($oldFormula))).Count
if ($count -ne 1) {
    throw "Expected exactly one P15 top-5 fullN formula, found $count."
}
$text = $text.Replace($oldFormula, $newFormula)

$text = $text.Replace(
    'Scout P15: pure 16x16 screen top-5 + optional all-24 recall audit; exact metric unchanged.',
    'Scout P16: pure 16x16 screen top-4 + optional all-24 recall audit; exact metric unchanged.'
)
$text = $text.Replace(' chosen5Recall=', ' chosen4Recall=')

# P12c/P15 uses SEARCH_COLUMNS+1 as the sentinel for screened-out finalists.
# bestLand starts at TOTAL_COLUMNS+1, so without this guard a sentinel can be
# printed/logged as a fake first [RECORD] (640001 land, 0 water). Ignore any
# result outside the physically valid 0..SEARCH_COLUMNS exact-land range.
$oldRecordIf = '                if (r.landColumns < bestLand) {'
$newRecordIf = '                if (r.landColumns <= SEARCH_COLUMNS && r.landColumns < bestLand) {'
$recordCount = ([regex]::Matches($text, [regex]::Escape($oldRecordIf))).Count
if ($recordCount -ne 1) {
    throw "Expected exactly one normal record-update condition, found $recordCount."
}
$text = $text.Replace($oldRecordIf, $newRecordIf)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P16 pure screen top-4 benchmark.' -ForegroundColor Green
Write-Host 'TopExact=24 now sends 4 candidates to full exact (95.70% measured best-of-24 recall).'
Write-Host 'Also fixed screened-out sentinel results so 640001-land placeholders cannot print as fake records.'
Write-Host 'Final exact 800x800 metric is unchanged. VERIFY 96617 land before benchmarking.'
Write-Host 'Decision threshold: P16 needs roughly >4.64M seeds/s to beat P15 on measured recall-adjusted throughput.'
