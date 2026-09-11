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

$text = [System.IO.File]::ReadAllText($sourcePath)
$text = $text.Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P7_800_INTERIOR')) {
    Write-Host 'TU4 Water P7 800x800 interior is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P6_DIVERSE_FINALISTS')) {
    throw 'P7 requires TU4 Water P6 diverse finalists first.'
}

$backupPath = $sourcePath + '.p6-before-water-p7.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# TU4 forces the outer part of the 864x864 finite world to ocean. For the
# maximum-water objective, columns outside the centered 800x800 square therefore
# cannot contribute land. P7 scans only the variable interior [-400,+399] while
# still reporting water against the full 864x864 = 746,496-column TU4 world.
#
# This removes 106,496 guaranteed-water columns from every exact finalist scan:
#   864^2 - 800^2 = 106,496.
# Scout samples are also moved inward to cover only the variable 800x800 area.

$constStart = $text.IndexOf('static constexpr int WORLD_SIDE = 864;')
if ($constStart -lt 0) {
    throw 'Could not locate TU4 world-size constants.'
}
$constEndMarker = 'static constexpr int COARSE_CELL_COUNT = COARSE_CELLS * COARSE_CELLS;'
$constEnd = $text.IndexOf($constEndMarker, $constStart)
if ($constEnd -lt 0) {
    throw 'Could not locate end of TU4 geometry constants.'
}
$constEnd += $constEndMarker.Length

$newConstants = @'
// TU4_WATER_P7_800_INTERIOR
// Full TU4 is 864x864, but the outer ring beyond the 400-block variable region
// is forced to water by the finite-world generator. Exact terrain work therefore
// only needs the centered 800x800 interior. Land count is unchanged; full-world
// water is TOTAL_COLUMNS - interiorLand because every omitted column is water.
static constexpr int FULL_WORLD_SIDE = 864;
static constexpr int SEARCH_SIDE = 800;
static constexpr int SEARCH_HALF = 400;
static constexpr int TOTAL_COLUMNS = FULL_WORLD_SIDE * FULL_WORLD_SIDE; // 746,496
static constexpr int SEARCH_COLUMNS = SEARCH_SIDE * SEARCH_SIDE;       // 640,000
static constexpr int FORCED_WATER_COLUMNS = TOTAL_COLUMNS - SEARCH_COLUMNS; // 106,496
static constexpr int COARSE_CELLS = SEARCH_SIDE / 4;                   // 200
static constexpr int COARSE_POINTS = COARSE_CELLS + 1;                 // 201
static constexpr int COARSE_POINT_COUNT = COARSE_POINTS * COARSE_POINTS;
static constexpr int COARSE_CELL_COUNT = COARSE_CELLS * COARSE_CELLS;
static constexpr int SEARCH_COARSE_MIN = -(SEARCH_HALF / 4);            // -100
'@.TrimEnd()
$text = $text.Remove($constStart, $constEnd - $constStart).Insert($constStart, $newConstants)

# P5/P6 full64 scout has three -108 origins (qxA, qxB, qz), and the exact
# density lattice has two more (X/Z). Move all of them to SEARCH_COARSE_MIN.
$originCount = ([regex]::Matches($text, [regex]::Escape('-108 +'))).Count
if ($originCount -lt 5) {
    throw "Expected at least five -108 coarse origins after P6, found $originCount."
}
$text = $text.Replace('-108 +', 'SEARCH_COARSE_MIN +')

# Make result initialization reflect the scanned interior. runExact still reports
# full-world water as TOTAL_COLUMNS-land, which automatically includes the fixed
# 106,496-column outer ocean ring.
$text = $text.Replace('    int landColumns = TOTAL_COLUMNS;', '    int landColumns = SEARCH_COLUMNS;')

# Update user-facing descriptions. These replacements are deliberately optional
# so harmless wording changes in earlier patches do not block P7 application.
$text = $text.Replace(
    '// 54x54 chunks = 864x864 blocks, centered as [-432,+431] on X/Z.',
    '// 54x54 chunks = 864x864 full world; variable terrain search is centered 800x800.'
)
$text = $text.Replace(
    '// footprint / maximum WATER footprint in the finite 864x864 world.',
    '// footprint / maximum WATER footprint. P7 scans the variable 800x800 interior; the outer TU4 ring is forced water.'
)
$text = $text.Replace(
    'TU4WaterFinder - Beta 1.7.3 / TU4 864x864 maximum-water search',
    'TU4WaterFinder - TU4 864x864 maximum-water search (exact 800x800 variable interior)'
)
$text = $text.Replace(
    'Metric: minimize land columns at sea surface y=63 across all 746,496 columns.',
    'Metric: minimize land in the centered 800x800 variable interior; the 106,496 omitted outer TU4 columns are forced water.'
)
$text = $text.Replace(
    'TU4 Water Finder | exact world=864x864 (54x54 chunks) | columns=',
    'TU4 Water Finder | exact variable interior=800x800 | full TU4=864x864 | columns='
)
$text = $text.Replace(
    'Objective: MIN land at y=63 / MAX water footprint.',
    'Objective: MIN interior land at y=63 / MAX full-TU4 water footprint (outer 106496 columns forced water).'
)
$text = $text.Replace(
    '[JACKPOT] Entire 864x864 TU4 terrain footprint is water at sea surface.',
    '[JACKPOT] Zero land in the 800x800 variable interior; with the forced outer ocean, the full TU4 footprint is water.'
)

# P6 startup line: keep the finalist portfolio, but make the reduced exact area
# visible so benchmarks cannot be confused with the old 864x864 raw scan.
$text = $text.Replace(
    'Scout P6: P5 full64 + 16 penalty / 4 hard-count / 4 residual finalists; exact metric unchanged.',
    'Scout P7: P6 diverse finalists + 800x800 variable-interior exact scan; outer TU4 ring counted as forced water.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P7 800x800 variable-interior scan.' -ForegroundColor Green
Write-Host 'Exact area: 864x864 -> centered 800x800 (-400..399 on X/Z).'
Write-Host 'Omitted outer columns: 106496, counted as guaranteed TU4 water.'
Write-Host 'Exact coarse lattice: 217x217 -> 201x201 points; cells: 216x216 -> 200x200.'
Write-Host 'Scout 8x8 coverage is also recentered onto the 800x800 variable region.'
Write-Host 'Reported waterPercent remains for the full 746496-column TU4 world.'
