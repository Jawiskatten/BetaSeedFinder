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

if ($text.Contains('TU4_WATER_P14_PURE_SCREEN_TOP8')) {
    Write-Host 'TU4 Water P14 pure screen top-8 routing is already applied.' -ForegroundColor Green
    exit 0
}
$hasP13b = $text.Contains('TU4_WATER_P13B_SCREEN_RECALL_AUDIT')
$hasP13 = $text.Contains('TU4_WATER_P13_SCREEN_RECALL_AUDIT')
if (-not ($hasP13b -or $hasP13)) {
    throw 'P14 requires P13b screen-recall audit first.'
}
if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    throw 'P14 expects the P12c exact-density screen underneath P13b.'
}

$backupPath = $sourcePath + '.p13b-before-water-p14.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P13b audit over 2000 batches showed:
#   chosen 6 screen + 2 safety recall = 99.2% (1984/2000)
#   pure screen top-8 recall         = 99.8% (1996/2000)
#   screen-rank misses: rank 8 x3, rank 12 x1
# Keep the same eight full exacts, but route all eight by screen rank.

$parentMarker = if ($hasP13b) { '// TU4_WATER_P13B_SCREEN_RECALL_AUDIT' } else { '// TU4_WATER_P13_SCREEN_RECALL_AUDIT' }
$markerPos = $text.IndexOf($parentMarker)
if ($markerPos -lt 0) { throw 'Could not locate P13/P13b marker.' }
$marker = @'
// TU4_WATER_P14_PURE_SCREEN_TOP8
// P13b proved pure screen top-8 beats 6 screen + 2 safety (99.8% vs 99.2% recall).
// Keep eight full exacts, but route all eight strictly by 16x16 exact-density rank.
'@
$text = $text.Insert($markerPos, $marker + "`n")

$oldBlock = @'
    // At TopExact=24 this sends 8 candidates to the full 201x201 exact grid.
    // Six slots follow the strong full-density screen; two are P6 safety slots.
    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 2) / 3));
    std::vector<int> chosen;
    chosen.reserve(static_cast<std::size_t>(fullN));
    auto addUnique = [&](int source) {
        if (source < 0 || source >= n || static_cast<int>(chosen.size()) >= fullN) return;
        if (std::find(chosen.begin(), chosen.end(), source) == chosen.end()) chosen.push_back(source);
    };

    const int screenTarget = std::max(0, fullN - 2);
    for (int i = 0; i < n && static_cast<int>(chosen.size()) < screenTarget; ++i) {
        addUnique(screen[static_cast<std::size_t>(i)].source);
    }
    addUnique(0);
    const int diverseStart = std::min(n - 1, (n * 2 + 2) / 3);
    addUnique(diverseStart);
    for (int i = 0; i < n && static_cast<int>(chosen.size()) < fullN; ++i) {
        addUnique(screen[static_cast<std::size_t>(i)].source);
    }
'@.TrimEnd()

$newBlock = @'
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

$count = ([regex]::Matches($text, [regex]::Escape($oldBlock))).Count
if ($count -ne 1) {
    throw "Expected exactly one P12c 6+2 routing block, found $count."
}
$text = $text.Replace($oldBlock, $newBlock)

$text = $text.Replace(
    'Scout P13b: P12c 8/24 screen + optional all-24 recall audit; final metric unchanged.',
    'Scout P14: pure 16x16 screen top-8 + optional all-24 recall audit; exact metric unchanged.'
)
$text = $text.Replace(
    'Scout P13: P12c screen + optional all-24 recall audit; normal 8/24 search unchanged.',
    'Scout P14: pure 16x16 screen top-8 + optional all-24 recall audit; exact metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P14 pure screen top-8 routing.' -ForegroundColor Green
Write-Host 'P13b audit: old chosen8 recall 99.2%; pure screen top-8 recall 99.8% over 2000 batches.'
Write-Host 'Still exactly 8/24 full exacts, so normal throughput should remain near P12c (~4.15M seeds/s).'
Write-Host 'P13b audit mode remains available for larger follow-up audits.'
Write-Host 'VERIFY the 96617-land record before resuming the long search.'