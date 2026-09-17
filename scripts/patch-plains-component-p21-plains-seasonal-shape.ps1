param(
    [string]$ProjectRoot = ""
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$sourcePath = Join-Path $ProjectRoot 'native\src\plains_component_finder.cpp'
if (-not (Test-Path $sourcePath -PathType Leaf)) {
    throw "Plains component source not found: $sourcePath"
}

$text = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")

if ($text.Contains('P21_PLAINS_SEASONAL_SHAPE')) {
    Write-Host 'P21 Plains + Seasonal Forest shape objective is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('// P20_DRY_PLAINS_TERRAIN_MASK')) {
    throw 'P21 expects P20 dry-terrain masking to be applied first.'
}
if (-not $text.Contains('P18_LARGEST_PLAINS_400_SQUARE')) {
    throw 'P21 expects the generated P18 Plains source underneath P20.'
}

$backupPath = $sourcePath + '.p20-before-p21-plains-seasonal-shape.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# Durable marker. Keep the P18 marker too because older compatibility patches
# recognize it, while P21 defines the active objective.
$text = $text.Replace(
    '// P18_LARGEST_PLAINS_400_SQUARE',
    "// P21_PLAINS_SEASONAL_SHAPE`n// P18_LARGEST_PLAINS_400_SQUARE"
)

# ---------------------------------------------------------------------------
# 1) Scout: PLAINS and SEASONAL_FOREST are one allowed connected region.
#    Also stop rewarding a sprawling bounding box. The old scout used
#    count*128+bboxArea, which actually preferred more spread-out shapes when
#    sample count tied. P21 uses a compactness-aware 8x8 score instead.
# ---------------------------------------------------------------------------
$oldScoutPredicate = '    return classifyQuantizedBiome(temperature, rain) == static_cast<unsigned char>(PLAINS);'
if (-not $text.Contains($oldScoutPredicate)) {
    throw 'Could not find the P18 Plains scout predicate.'
}
$newScoutPredicate = @'
    const unsigned char biome = classifyQuantizedBiome(temperature, rain);
    return biome == static_cast<unsigned char>(PLAINS) ||
           biome == static_cast<unsigned char>(SEASONAL_FOREST);
'@
$text = $text.Replace($oldScoutPredicate, $newScoutPredicate.TrimEnd())

$helperPattern = '(?s)__device__ __forceinline__ int p18LargestSampleComponent\(.*?\n\}\n\n__global__ void searchKernel'
$helperMatches = [regex]::Matches($text, $helperPattern)
if ($helperMatches.Count -ne 1) {
    throw "Expected exactly one p18LargestSampleComponent helper, found $($helperMatches.Count)."
}
$newHelper = @'
__device__ __forceinline__ int p18LargestSampleComponent(
        unsigned long long mask,
        int& bestShapeWeight
) {
    constexpr unsigned long long COL0 = 0x0101010101010101ULL;
    constexpr unsigned long long COL7 = 0x8080808080808080ULL;
    unsigned long long remaining = mask;
    int bestCount = 0;
    int bestScore = -1;
    bestShapeWeight = 0;

    while (remaining != 0ULL) {
        const unsigned long long seedBit = remaining & (~remaining + 1ULL);
        unsigned long long component = 0ULL;
        unsigned long long frontier = seedBit;
        while (frontier != 0ULL) {
            component |= frontier;
            unsigned long long expanded = 0ULL;
            expanded |= (frontier & ~COL0) >> 1;
            expanded |= (frontier & ~COL7) << 1;
            expanded |= frontier >> 8;
            expanded |= frontier << 8;
            frontier = expanded & mask & ~component;
        }
        remaining &= ~component;

        const int count = p18PopCount64(component);
        int minCol = 8, maxCol = -1, minRow = 8, maxRow = -1;
        unsigned long long bits = component;
        while (bits != 0ULL) {
            const int bit = __ffsll(static_cast<long long>(bits)) - 1;
            const int row = bit >> 3;
            const int col = bit & 7;
            if (col < minCol) minCol = col;
            if (col > maxCol) maxCol = col;
            if (row < minRow) minRow = row;
            if (row > maxRow) maxRow = row;
            bits &= bits - 1ULL;
        }

        const int width = maxCol - minCol + 1;
        const int height = maxRow - minRow + 1;
        const int bboxArea = width * height;
        const int fillPermille = bboxArea > 0 ? (count * 1000) / bboxArea : 0;
        const int longSide = width > height ? width : height;
        const int shortSide = width < height ? width : height;
        const int aspectPermille = longSide > 0 ? (shortSide * 1000) / longSide : 0;

        // 70% area, up to 20% bbox-fill bonus, up to 10% aspect bonus.
        // The resulting weight is 700..1000. Multiplying by sample count makes
        // shape meaningful without letting a tiny perfect blob dominate a much
        // larger connected region.
        const int shapeWeight = 700 + fillPermille / 5 + aspectPermille / 10;
        const int score = count * shapeWeight; // max 64,000: fits ushort

        if (score > bestScore ||
            (score == bestScore && count > bestCount)) {
            bestScore = score;
            bestCount = count;
            bestShapeWeight = shapeWeight;
        }
    }
    return bestCount;
}

__global__ void searchKernel
'@
$text = [regex]::Replace($text, $helperPattern, $newHelper.TrimEnd(), 1)

$oldScoutScore = @'
        int bboxArea = 0;
        const int connectedSamples = p18LargestSampleComponent(plainsMask, bboxArea);
        const int coarseScore = connectedSamples * 128 + bboxArea;
'@
if (-not $text.Contains($oldScoutScore.TrimEnd())) {
    throw 'Could not find the P18 coarse-score block.'
}
$newScoutScore = @'
        int shapeWeight = 0;
        const int connectedSamples = p18LargestSampleComponent(plainsMask, shapeWeight);
        const int coarseScore = connectedSamples * shapeWeight;
'@
$text = $text.Replace($oldScoutScore.TrimEnd(), $newScoutScore.TrimEnd())

# ---------------------------------------------------------------------------
# 2) Exact biome bitmap: either PLAINS or SEASONAL_FOREST is allowed. P20 then
#    removes every ocean/sea column before CPU flood fill, so water still breaks
#    connectivity even when the climate biome on both shores is allowed.
# ---------------------------------------------------------------------------
$exactPredicatePattern = '(?s)        map\[idx\] = biomeAt\(s, blockX, blockZ\) == static_cast<unsigned char>\(PLAINS\)\s*\? 1 : 0;'
$exactPredicateMatches = [regex]::Matches($text, $exactPredicatePattern)
if ($exactPredicateMatches.Count -ne 1) {
    throw "Expected exactly one P18 exact Plains predicate, found $($exactPredicateMatches.Count)."
}
$newExactPredicate = @'
        const unsigned char biome = biomeAt(s, blockX, blockZ);
        map[idx] = (biome == static_cast<unsigned char>(PLAINS) ||
                    biome == static_cast<unsigned char>(SEASONAL_FOREST)) ? 1 : 0;
'@
$text = [regex]::Replace($text, $exactPredicatePattern, $newExactPredicate.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3) Exact component ranking: still strictly 4-neighbour connected and dry, but
#    rank a component by area * shapeWeight instead of raw area alone.
#
#    shapeWeight = 70% base + 20% bbox fill + 10% aspect ratio.
#    A huge irregular region can still win, but a holey/snaky/spread-out region
#    pays a real penalty. safeRadius remains the ACTUAL connected block area;
#    firstMismatchD2 is repurposed as the P21 integer shape score.
#    measurementHalfSize stores bbox-fill permille and expansionCount stores
#    aspect permille for display, avoiding a struct-layout change.
# ---------------------------------------------------------------------------
$oldExactSelect = @'
        if (area > result.safeRadius) {
            result.safeRadius = area;
            result.componentWidthX = maxLX - minLX + 1;
            result.componentHeightZ = maxLZ - minLZ + 1;
            result.minX = centerX - target + minLX;
            result.maxX = centerX - target + maxLX;
            result.minZ = centerZ - target + minLZ;
            result.maxZ = centerZ - target + maxLZ;
            result.touchesBoundary =
                    (minLX == 0 || maxLX == side - 1 ||
                     minLZ == 0 || maxLZ == side - 1) ? 1 : 0;
        }
'@
if (-not $text.Contains($oldExactSelect.TrimEnd())) {
    throw 'Could not find the P18/P20 exact component-selection block.'
}
$newExactSelect = @'
        const int widthX = maxLX - minLX + 1;
        const int heightZ = maxLZ - minLZ + 1;
        const int bboxArea = widthX * heightZ;
        const int fillPermille = bboxArea > 0 ? (area * 1000) / bboxArea : 0;
        const int longSide = widthX > heightZ ? widthX : heightZ;
        const int shortSide = widthX < heightZ ? widthX : heightZ;
        const int aspectPermille = longSide > 0 ? (shortSide * 1000) / longSide : 0;
        const int shapeWeight = 700 + fillPermille / 5 + aspectPermille / 10;
        const int shapeScore = static_cast<int>(
                (static_cast<long long>(area) * shapeWeight) / 1000LL);

        if (shapeScore > result.firstMismatchD2 ||
            (shapeScore == result.firstMismatchD2 && area > result.safeRadius)) {
            result.safeRadius = area;              // actual connected block area
            result.firstMismatchD2 = shapeScore;  // P21 record score
            result.componentWidthX = widthX;
            result.componentHeightZ = heightZ;
            result.minX = centerX - target + minLX;
            result.maxX = centerX - target + maxLX;
            result.minZ = centerZ - target + minLZ;
            result.maxZ = centerZ - target + maxLZ;
            result.touchesBoundary =
                    (minLX == 0 || maxLX == side - 1 ||
                     minLZ == 0 || maxLZ == side - 1) ? 1 : 0;
            result.measurementHalfSize = fillPermille;
            result.expansionCount = aspectPermille;
        }
'@
$text = $text.Replace($oldExactSelect.TrimEnd(), $newExactSelect.TrimEnd())

# Host-level records must compare the P21 shape score, while jackpots/verify still
# use safeRadius (actual connected area), so only the record gate/assignment move.
$recordGate = 'if (result.safeRadius > bestExact) {'
$recordGateMatches = [regex]::Matches($text, [regex]::Escape($recordGate))
if ($recordGateMatches.Count -ne 1) {
    throw "Expected exactly one raw-area record gate, found $($recordGateMatches.Count)."
}
$text = $text.Replace($recordGate, 'if (result.firstMismatchD2 > bestExact) {')

$bestAssign = '                bestExact = result.safeRadius;'
$bestAssignMatches = [regex]::Matches($text, [regex]::Escape($bestAssign))
if ($bestAssignMatches.Count -ne 1) {
    throw "Expected exactly one bestExact raw-area assignment, found $($bestAssignMatches.Count)."
}
$text = $text.Replace($bestAssign, '                bestExact = result.firstMismatchD2;')

# ---------------------------------------------------------------------------
# 4) Output/log semantics. Old raw-area runs are a different metric, so use a
#    new CSV. Keep connectedArea as the actual number of dry allowed blocks and
#    print the score + compactness diagnostics separately.
# ---------------------------------------------------------------------------
$text = $text.Replace('dry_plains_component_hits.csv', 'plains_seasonal_shape_hits.csv')
$text = $text.Replace(' dryPlainsArea=', ' connectedArea=')
$text = $text.Replace(' bestDryPlainsArea=', ' bestShapeScore=')
$text = $text.Replace(
    'P20_DRY_PLAINS_TERRAIN_MASK | 8x8 Plains biome scout | exact dry PLAINS land | 4-neighbour 800x800 area',
    'P21_PLAINS_SEASONAL_SHAPE | PLAINS+SEASONAL_FOREST | dry 4-neighbour land | compactness-aware 800x800 score'
)

# P18 occasionally retained P17's old true-size diagnostics. Replace them with
# P21's actual finite-square shape diagnostics if present.
$legacyGeomPattern = '(?s)\s*<< " trueSize=" << \(result\.touchesBoundary \? "NO" : "YES"\)\s*<< " measuredWindow=" << \(2 \* result\.measurementHalfSize\)\s*<< ''x'' << \(2 \* result\.measurementHalfSize\)\s*<< " expansions=" << result\.expansionCount;'
if ([regex]::IsMatch($text, $legacyGeomPattern)) {
    $geomReplacement = @'
              << " touchesSquareBoundary=" << (result.touchesBoundary ? "YES" : "NO")
              << " shapeScore=" << result.firstMismatchD2
              << " bboxFill=" << std::fixed << std::setprecision(1)
              << (static_cast<double>(result.measurementHalfSize) / 10.0) << "%"
              << " aspect=" << (static_cast<double>(result.expansionCount) / 10.0) << "%";
'@
    $text = [regex]::Replace($text, $legacyGeomPattern, "`n" + $geomReplacement.TrimEnd(), 1)
}

# If firstDifferent output survived earlier patch history, it is now invalid
# because firstMismatchD2 carries shapeScore. Remove that suffix completely.
$mismatchPattern = '(?s)\s*if \(result\.firstMismatchD2 < 0\) \{.*?\n\s*\}\n\s*std::cout << " center="'
if ([regex]::IsMatch($text, $mismatchPattern)) {
    $text = [regex]::Replace($text, $mismatchPattern, "`n    std::cout << \" center=\"", 1)
}

# If the legacy geometry block had already been normalized by P18, add shape
# fields after touchesSquareBoundary instead.
$touchOnly = '              << " touchesSquareBoundary=" << (result.touchesBoundary ? "YES" : "NO");'
if ($text.Contains($touchOnly)) {
    $touchShape = @'
              << " touchesSquareBoundary=" << (result.touchesBoundary ? "YES" : "NO")
              << " shapeScore=" << result.firstMismatchD2
              << " bboxFill=" << std::fixed << std::setprecision(1)
              << (static_cast<double>(result.measurementHalfSize) / 10.0) << "%"
              << " aspect=" << (static_cast<double>(result.expansionCount) / 10.0) << "%";
'@
    $text = $text.Replace($touchOnly, $touchShape.TrimEnd())
}

# Normalize the two periodic status fragments created by the old P16/P17 stack.
$text = $text.Replace('                      << " bestShapeScore=" << bestExact;',
                     '                      << " bestShapeScore=" << bestExact;')
$statusOld = @'
                std::cout << " bestShapeScore=" << bestExact
                          << " length=" << std::max(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " width=" << std::min(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " bestSeed=" << bestExactSeed
'@
if ($text.Contains($statusOld.TrimEnd())) {
    $statusNew = @'
                std::cout << " connectedArea=" << bestComponent.safeRadius
                          << " shapeScore=" << bestExact
                          << " bboxFill=" << std::fixed << std::setprecision(1)
                          << (static_cast<double>(bestComponent.measurementHalfSize) / 10.0) << "%"
                          << " aspect=" << (static_cast<double>(bestComponent.expansionCount) / 10.0) << "%"
                          << " length=" << std::max(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " width=" << std::min(bestComponent.componentWidthX, bestComponent.componentHeightZ)
                          << " bestSeed=" << bestExactSeed
'@
    $text = $text.Replace($statusOld.TrimEnd(), $statusNew.TrimEnd())
}

# The coarse-record biome label is inherited from the old Rainforest tool. Make
# it generic so it no longer lies about what the scout accepts.
$text = $text.Replace('                          << " biome=" << biomeName(hBiome[static_cast<std::size_t>(i)])',
                     '                          << " allowed=PLAINS+SEASONAL_FOREST"')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath)
if (-not $verify.Contains('P21_PLAINS_SEASONAL_SHAPE')) {
    throw 'P21 marker missing after write.'
}
if (-not $verify.Contains('SEASONAL_FOREST')) {
    throw 'P21 allowed-biome predicate is missing SEASONAL_FOREST.'
}
if (-not $verify.Contains('const int shapeScore')) {
    throw 'P21 exact shape scoring was not inserted.'
}
if (-not $verify.Contains('if (result.firstMismatchD2 > bestExact) {')) {
    throw 'P21 host record ranking is not using shapeScore.'
}

Write-Host 'Applied P21 PLAINS + SEASONAL_FOREST connected-shape objective.' -ForegroundColor Green
Write-Host 'Allowed land: PLAINS or SEASONAL_FOREST, but ocean/sea columns still break connectivity.'
Write-Host 'Exact rank: connectedArea * shapeWeight, with shapeWeight = 70% base + 20% bbox fill + 10% aspect.'
Write-Host 'Scout uses the same two biomes and compactness-aware 8x8 component scoring.'
Write-Host 'New records use plains_seasonal_shape_hits.csv; older Plains-only metrics stay separate.'
