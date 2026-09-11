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
# Make multiline patch matching independent of Git/Windows line-ending policy.
$text = $text.Replace("`r`n", "`n")

if ($text.Contains('TU4_WATER_P2_FAST_SCOUT')) {
    Write-Host 'TU4 Water P2 fast scout is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4 Water Finder | exact world=864x864')) {
    throw 'This patch expects the P1 TU4 water finder source.'
}

$backupPath = $sourcePath + '.p1-before-water-p2.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

function Replace-Once([string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
    $count = ([regex]::Matches($script:text, [regex]::Escape($old))).Count
    if ($count -ne 1) {
        throw "Expected exactly one $label, found $count."
    }
    $script:text = $script:text.Replace($old, $new)
}

Replace-Once `
    'static constexpr int SCOUT_THREADS = 64;' `
    @'
// TU4_WATER_P2_FAST_SCOUT
// P2 uses a 32-point checkerboard macro scout and only the dominant final four
// noise5 octaves. Those four carry weights 4096+8192+16384+32768 = 93.75% of
// the octave weight sum, so they are a strong low-cost proxy for macro height.
// Exact record decisions still run the complete terrain generator path.
static constexpr int SCOUT_THREADS = 32;
static constexpr int SCOUT_GRID = 8;
static constexpr int SCOUT_FIRST_NOISE5_OCTAVE = 12;
'@.TrimEnd() `
    'SCOUT_THREADS constant'

$oldComment = @'
// For every seed the scout samples the macro-height field at an 8x8 lattice
// covering the whole TU4 world. The terrain's noise6/noise5-in-this-project
// field controls the large-scale vertical center (d7), so low d7 is a strong
// proxy for ocean-heavy worlds. Only the best scout seeds in each batch receive
// the exact 746,496-column water/land count below.
'@
$newComment = @'
// For every seed the scout samples 32 cell centers from an 8x8 checkerboard
// covering the whole TU4 world. It evaluates only noise5 octaves 12..15, whose
// aggregate legacy weights are 93.75% of the full 16-octave weight sum. This is
// intentionally a ranking proxy: only the best scout seeds receive the complete
// exact 746,496-column water/land count below, which remains authoritative.
'@
Replace-Once $oldComment.TrimEnd() $newComment.TrimEnd() 'P1 scout description'

$oldGrid = @'
    // 8x8 sample cell centers over the 216x216 coarse-cell TU4 footprint.
    const int row = lane >> 3;
    const int col = lane & 7;
    const int qx = -108 + ((2 * col + 1) * COARSE_CELLS) / 16;
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / 16;
'@
$newGrid = @'
    // 32 samples chosen as an alternating checkerboard of the original 8x8
    // cell-center lattice. Every row is represented and adjacent rows sample
    // opposite columns, avoiding the directional bias of a plain 4x8 grid.
    const int row = lane >> 2;
    const int col = ((lane & 3) << 1) + (row & 1);
    const int qx = -108 + ((2 * col + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
    const int qz = -108 + ((2 * row + 1) * COARSE_CELLS) / (2 * SCOUT_GRID);
'@
Replace-Once $oldGrid.TrimEnd() $newGrid.TrimEnd() '8x8 scout coordinate block'

$oldPrep = @'
        for (int i = 0; i < 58; ++i) consumePerlinRngOnly(rng);
    }
    __syncthreads();

    double noise5 = 0.0;
    double amplitude = 1.0;
    for (int octave = 0; octave < 16; ++octave) {
'@
$newPrep = @'
        for (int i = 0; i < 58; ++i) consumePerlinRngOnly(rng);
        // Skip the first twelve noise5 octaves without constructing their
        // permutations. Exact Java RNG consumption is preserved.
        for (int i = 0; i < SCOUT_FIRST_NOISE5_OCTAVE; ++i) {
            consumePerlinRngOnly(rng);
        }
    }
    __syncthreads();

    double noise5 = 0.0;
    // octave 12 starts at amplitude 2^-12. The four retained octaves dominate
    // the legacy weighted sum while cutting point evaluations by 4x.
    double amplitude = 1.0 / 4096.0;
    for (int octave = SCOUT_FIRST_NOISE5_OCTAVE; octave < 16; ++octave) {
'@
Replace-Once $oldPrep.TrimEnd() $newPrep.TrimEnd() 'noise5 scout octave loop'

# Tune defaults around the much cheaper scout. Four exact candidates from a
# 32768-seed batch is half the old exact-check frequency per searched seed.
$text = $text.Replace('int batch = 8192;', 'int batch = 32768;')
$text = $text.Replace('int topExact = 2;', 'int topExact = 4;')
$text = $text.Replace('(default 8192)', '(default 32768)')
$text = $text.Replace('(default 2)', '(default 4)')

$text = $text.Replace(
    'Scout: 8x8 exact macro-height (noise5) lattice; exact terrain for top candidates.',
    'Scout P2: 32-point checkerboard + dominant noise5 tail4; full exact terrain for top candidates.'
)

# Console/CSV labels now reflect the 32-sample scout.
$text = $text.Replace('scout_low64', 'scout_low32')
$text = $text.Replace('lowHeightSamples=" << globalScoutLow << "/64', 'lowHeightSamples=" << globalScoutLow << "/32')
$text = $text.Replace('scoutLow=" << hLow[idx] << "/64', 'scoutLow=" << hLow[idx] << "/32')
$text = $text.Replace('scoutBest=" << globalScoutLow << "/64', 'scoutBest=" << globalScoutLow << "/32')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P2 fast scout.' -ForegroundColor Green
Write-Host 'Scout samples: 64 -> 32 checkerboard points.'
Write-Host 'Scout terrain: full noise5 16 octaves -> dominant tail 4 octaves (12..15).'
Write-Host 'Default batch: 8192 -> 32768; exact candidates: 2 -> 4.'
Write-Host 'Exact 864x864 land/water measurement is unchanged and remains authoritative.'
