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

if ($text.Contains('TU4_WATER_P17_WIDE_TARGETED_FUNNEL')) {
    Write-Host 'TU4 Water P17 wide targeted funnel is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P15_PURE_SCREEN_TOP5')) {
    throw 'P17 requires the structurally-applied P15 top-5 source first.'
}
if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    throw 'P17 expects the P12c exact-density screen underneath P15.'
}

$backupPath = $sourcePath + '.p15-before-water-p17.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P17 changes the funnel rather than shaving another few percent from the final
# exact stage.  P15 only lets 24 cheap-scout finalists reach the strong 16x16
# full-density screen.  For rare ~87%+ water worlds that first bottleneck is now
# the likely limiting factor.  P17 decouples screen-pool size from full-exact
# count: --top-exact now means "how many P6 finalists get the 16x16 exact-density
# screen", while only the best FIVE screen-ranked candidates receive the full
# 201x201 exact metric.  This lets us try 48/64/96/128-wide targeted funnels
# without exploding the expensive final stage.

$markerPos = $text.IndexOf('// TU4_WATER_P15_PURE_SCREEN_TOP5')
if ($markerPos -lt 0) { throw 'Could not locate P15 marker.' }
$marker = @'
// TU4_WATER_P17_WIDE_TARGETED_FUNNEL
// --top-exact is repurposed as the wide 16x16 exact-density SCREEN POOL.
// Only the five strongest screen-ranked candidates receive full 201x201 exact.
// This widens the funnel for extremely rare ocean worlds without multiplying
// the authoritative final-exact count.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# Accept P15 or P16 as the local predecessor. P16 changed the same fullN formula
# to top-4; P17 intentionally returns to five full exacts, but over a much wider
# screen pool.
$formulas = @(
    '    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 4) / 5));',
    '    const int fullN = (n <= 1) ? n : std::min(n, std::max(1, (n + 5) / 6));'
)
$found = 0
foreach ($formula in $formulas) {
    if ($text.Contains($formula)) {
        $text = $text.Replace($formula,
            '    const int fullN = (n <= 1) ? n : std::min(n, 5);')
        $found++
    }
}
if ($found -ne 1) {
    throw "Expected exactly one P15/P16 fullN formula, found $found."
}

# Update the routing comments near fullN so future patches do not assume a
# one-fifth relationship between screen pool and full-exact count.
$text = $text.Replace(
    '// P13b ground truth: screen ranks 0..4 held the true best-of-24 in`n    // 1959/2000 batches = 97.95%. Exact the strongest ~one fifth of the`n    // pre-finalists; at TopExact=24 this is five full exact evaluations.',
    '// P17: exact only the five strongest 16x16 screen ranks, independent of`n    // screen-pool width. --top-exact controls how many cheap-scout finalists`n    // receive the strong exact-density screen (96 recommended first test).'
)

$bannerTargets = @(
    'Scout P15: pure 16x16 screen top-5 + optional all-24 recall audit; exact metric unchanged.',
    'Scout P16: pure 16x16 screen top-4 + optional all-24 recall audit; exact metric unchanged.'
)
foreach ($banner in $bannerTargets) {
    $text = $text.Replace($banner,
        'Scout P17: wide targeted 16x16 density funnel; screenPool=N -> top5 full exact; exact metric unchanged.')
}

# Make the startup parameter name reflect its new role. The CLI flag remains
# --top-exact for backward compatibility with the existing runner.
$text = $text.Replace('<< " topExact=" << o.topExact', '<< " screenPool=" << o.topExact')

# Help wording can vary slightly between patch generations, so use broad but
# harmless replacements.
$text = $text.Replace('Exact-check N best scout seeds per batch',
                     'P17 16x16 screen-pool size; best 5 get full exact')

# Audit under P17 measures whether the fixed chosen five contains the true best
# among the whole widened pool.
$text = $text.Replace(' chosen4Recall=', ' chosen5Recall=')
$text = $text.Replace(' chosen8Recall=', ' chosen5Recall=')

# Fix the screened-out sentinel if the user came from P15 rather than P16.
$oldRecordIf = '                if (r.landColumns < bestLand) {'
$newRecordIf = '                if (r.landColumns <= SEARCH_COLUMNS && r.landColumns < bestLand) {'
if ($text.Contains($oldRecordIf)) {
    $count = ([regex]::Matches($text, [regex]::Escape($oldRecordIf))).Count
    if ($count -ne 1) { throw "Expected one normal record-update condition, found $count." }
    $text = $text.Replace($oldRecordIf, $newRecordIf)
}
elseif (-not $text.Contains($newRecordIf)) {
    throw 'Could not verify the screened-out sentinel record guard.'
}

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath)
if (-not $verify.Contains('TU4_WATER_P17_WIDE_TARGETED_FUNNEL')) {
    throw 'P17 wrote the source but the marker is missing.'
}
if (-not $verify.Contains('std::min(n, 5)')) {
    throw 'P17 wrote the source but the fixed top-5 full-exact routing is missing.'
}

Write-Host 'Applied TU4 Water P17 wide targeted funnel.' -ForegroundColor Green
Write-Host '--top-exact now controls the 16x16 exact-density SCREEN POOL.'
Write-Host 'The final 201x201 exact stage is fixed at the best 5 screen-ranked candidates.'
Write-Host 'Recommended first normal benchmark: -TopExact 96 (24 -> 96 wider funnel).'
Write-Host 'Recommended recall audit: -TopExact 96 -AuditScreenBatches 500.'
Write-Host 'Authoritative 800x800 exact metric is unchanged. VERIFY 96617 land first.'
