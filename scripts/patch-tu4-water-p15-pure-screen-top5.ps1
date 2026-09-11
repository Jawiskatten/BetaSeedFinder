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

if ($text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    Write-Host 'TU4 Water P15 pure screen top-5 routing is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P14_PURE_SCREEN_TOP8')) {
    throw 'P15 requires P14 pure screen top-8 first.'
}
if (-not $text.Contains('TU4_WATER_P13_SCREEN_RECALL_AUDIT')) {
    throw 'P15 expects P13b audit machinery underneath P14.'
}

$backupPath = $sourcePath + '.p14-before-water-p15.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P13b 2000-batch ground-truth histogram for the true best-of-24 screen rank:
#   rank 0..4 = 1271+422+152+69+45 = 1959 / 2000 = 97.95% recall
#   rank 0..7 = 1996 / 2000 = 99.80% recall
# P14 proves 8 full exacts run at ~4.14M seeds/s on the RX 7800 XT.  Reducing
# the expensive 201x201 full exact stage from 8 to 5 should increase raw rate.
# Even after multiplying by the measured 97.95% best-of-24 retention, the
# expected effective search throughput is higher than P14.  Final records remain
# fully exact; this only changes which screened finalists receive full exact.

$markerPos = $text.IndexOf('// TU4_WATER_P14_PURE_SCREEN_TOP8')
if ($markerPos -lt 0) { throw 'Could not locate P14 marker.' }
$marker = @'
// TU4_WATER_P15_PURE_SCREEN_TOP5
// P13b audit measured pure screen top-5 at 97.95% best-of-24 recall.
// Route five full exacts instead of eight to trade 1.85pp recall for more raw throughput.
// Every reported record still uses the unchanged authoritative 800x800 exact metric.
'@
$text = $text.Insert($markerPos, $marker + "`n")

$oldBlock = @'
    // P14: P13b measured pure screen top-8 at 99.8% best-of-24 recall versus
    // 99.2% for the old 6-screen + 2-safety portfolio. Keep the exact same
    // full-exact count (8 at TopExact=24) and take the first fullN screen ranks.
    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 2) / 3));
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(fullN));
    for (int rank = 0; rank < n && static_cast<int>(chosen.size()) < fullN; ++rank) {
        chosen.push_back(screen[static_cast<std::size_t>(rank)].source);
    }
'@.TrimEnd()

$newBlock = @'
    // P15: exact the strongest ~one fifth of P6 finalists by the 16x16 screen.
    // With TopExact=24 this is 5 full exacts. P13b measured screen top-5 recall
    // at 1959/2000 = 97.95% for the true best-of-24 candidate.
    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 4) / 5));
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(fullN));
    for (int rank = 0; rank < n && static_cast<int>(chosen.size()) < fullN; ++rank) {
        chosen.push_back(screen[static_cast<std::size_t>(rank)].source);
    }
'@.TrimEnd()

$count = ([regex]::Matches($text, [regex]::Escape($oldBlock))).Count
if ($count -ne 1) {
    throw "Expected exactly one P14 top-8 routing block, found $count."
}
$text = $text.Replace($oldBlock, $newBlock)

$text = $text.Replace(
    'Scout P14: pure 16x16 screen top-8 + optional all-24 recall audit; exact metric unchanged.',
    'Scout P15: pure 16x16 screen top-5 + optional all-24 recall audit; exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P15 pure screen top-5 routing.' -ForegroundColor Green
Write-Host 'P13b ground truth: top-5 retained the true best-of-24 in 1959/2000 batches (97.95%).'
Write-Host 'Full exact count at TopExact=24: 8 -> 5. Screen and authoritative final metric are unchanged.'
Write-Host 'Expected: higher raw throughput than P14 ~4.14M/s; benchmark before committing to a long run.'
Write-Host 'VERIFY the 96617-land record before benchmarking.'
