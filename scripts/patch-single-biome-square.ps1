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
if ($text.Contains('SQUARE_TARGET_864_V2')) {
    Write-Host 'Exact 864x864 square target patch is already applied.'
    exit 0
}

$circleProbe = @'
void makeProbePoints(
        const std::vector<int>& radii,
        std::vector<int>& dx,
        std::vector<int>& dz
) {
    dx.resize(radii.size() * SEARCH_THREADS);
    dz.resize(radii.size() * SEARCH_THREADS);

    for (std::size_t ring = 0; ring < radii.size(); ++ring) {
        const int r = radii[ring];
        const double phase = (ring & 1U) ? PI / static_cast<double>(SEARCH_THREADS) : 0.0;
        for (int lane = 0; lane < SEARCH_THREADS; ++lane) {
            const double angle = 2.0 * PI * static_cast<double>(lane) /
                                 static_cast<double>(SEARCH_THREADS) + phase;
            int x = static_cast<int>(std::llround(std::cos(angle) * static_cast<double>(r)));
            int z = static_cast<int>(std::llround(std::sin(angle) * static_cast<double>(r)));

            // Rounding a circle can place a lattice point a fraction outside r.
            // Pull it inward so a valid target disk can never be falsely rejected.
            while (static_cast<long long>(x) * x + static_cast<long long>(z) * z >
                   static_cast<long long>(r) * r) {
                if (std::abs(x) >= std::abs(z) && x != 0) x += x > 0 ? -1 : 1;
                else if (z != 0) z += z > 0 ? -1 : 1;
                else break;
            }
            dx[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = x;
            dz[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = z;
        }
    }
}
'@

$v1Probe = @'
void makeProbePoints(
        const std::vector<int>& radii,
        std::vector<int>& dx,
        std::vector<int>& dz
) {
    // SQUARE_TARGET_PATCH_V1
    // 64 evenly-spaced samples around each Chebyshev-radius square perimeter.
    // Lanes 0/16/32/48 land exactly on the four corners.
    dx.resize(radii.size() * SEARCH_THREADS);
    dz.resize(radii.size() * SEARCH_THREADS);

    for (std::size_t ring = 0; ring < radii.size(); ++ring) {
        const int r = radii[ring];
        const int perimeter = 8 * r;
        for (int lane = 0; lane < SEARCH_THREADS; ++lane) {
            const int pos = (lane * perimeter) / SEARCH_THREADS;
            int x = 0;
            int z = 0;
            if (pos < 2 * r) {
                x = -r + pos;
                z = -r;
            } else if (pos < 4 * r) {
                x = r;
                z = -r + (pos - 2 * r);
            } else if (pos < 6 * r) {
                x = r - (pos - 4 * r);
                z = r;
            } else {
                x = -r;
                z = r - (pos - 6 * r);
            }
            dx[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = x;
            dz[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = z;
        }
    }
}
'@

$newProbe = @'
void makeProbePoints(
        const std::vector<int>& radii,
        std::vector<int>& dx,
        std::vector<int>& dz
) {
    // SQUARE_TARGET_864_V2
    // target=432 means an exact 864x864 window: offsets [-432, +431].
    // Each scout ring r samples the perimeter of [-r, r-1]^2.
    dx.resize(radii.size() * SEARCH_THREADS);
    dz.resize(radii.size() * SEARCH_THREADS);

    for (std::size_t ring = 0; ring < radii.size(); ++ring) {
        const int r = radii[ring];
        for (int lane = 0; lane < SEARCH_THREADS; ++lane) {
            const int side = lane / 16;
            const int t = lane % 16;
            const int along = (t * (2 * r - 1)) / 15;
            int x = 0;
            int z = 0;
            if (side == 0) {
                x = -r + along; z = -r;
            } else if (side == 1) {
                x = r - 1; z = -r + along;
            } else if (side == 2) {
                x = r - 1 - along; z = r - 1;
            } else {
                x = -r; z = r - 1 - along;
            }
            dx[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = x;
            dz[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = z;
        }
    }
}
'@

if ($text.Contains($circleProbe)) { $text = $text.Replace($circleProbe, $newProbe) }
elseif ($text.Contains($v1Probe)) { $text = $text.Replace($v1Probe, $newProbe) }
else { throw 'Could not find probe generator to convert to exact 864x864 square.' }

$circleExact = @'
std::vector<ExactPoint> makeExactPoints(int target) {
    const long long r2 = static_cast<long long>(target) * target;
    std::vector<ExactPoint> points;
    const double estimate = PI * static_cast<double>(target) * static_cast<double>(target);
    points.reserve(static_cast<std::size_t>(estimate + target * 8.0 + 64.0));

    for (int dz = -target; dz <= target; ++dz) {
        for (int dx = -target; dx <= target; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const long long d2 = static_cast<long long>(dx) * dx + static_cast<long long>(dz) * dz;
            if (d2 <= r2) points.push_back({dx, dz, static_cast<int>(d2)});
        }
    }
    std::sort(points.begin(), points.end(), [](const ExactPoint& a, const ExactPoint& b) {
        return a.d2 < b.d2;
    });
    return points;
}
'@

$v1Exact = @'
std::vector<ExactPoint> makeExactPoints(int target) {
    // Square target: every integer X/Z offset in [-target, +target].
    // ExactPoint::d2 stores Chebyshev-radius squared so the existing exact
    // verifier finds the largest fully-uniform centered square.
    std::vector<ExactPoint> points;
    const std::size_t side = static_cast<std::size_t>(target * 2 + 1);
    points.reserve(side * side - 1U);

    for (int dz = -target; dz <= target; ++dz) {
        for (int dx = -target; dx <= target; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int squareRadius = std::max(std::abs(dx), std::abs(dz));
            const int metric2 = squareRadius * squareRadius;
            points.push_back({dx, dz, metric2});
        }
    }
    std::sort(points.begin(), points.end(), [](const ExactPoint& a, const ExactPoint& b) {
        return a.d2 < b.d2;
    });
    return points;
}
'@

$newExact = @'
std::vector<ExactPoint> makeExactPoints(int target) {
    // Exact 2*target by 2*target square. For target=432 this is 864x864,
    // with offsets [-432, +431] on both axes = 746496 total positions.
    // metric2 stores the expansion layer r for the square [-r, r-1]^2.
    std::vector<ExactPoint> points;
    const std::size_t side = static_cast<std::size_t>(target * 2);
    points.reserve(side * side - 1U);

    for (int dz = -target; dz < target; ++dz) {
        for (int dx = -target; dx < target; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const int rx = dx >= 0 ? dx + 1 : -dx;
            const int rz = dz >= 0 ? dz + 1 : -dz;
            const int squareLayer = std::max(rx, rz);
            const int metric2 = squareLayer * squareLayer;
            points.push_back({dx, dz, metric2});
        }
    }
    std::sort(points.begin(), points.end(), [](const ExactPoint& a, const ExactPoint& b) {
        return a.d2 < b.d2;
    });
    return points;
}
'@

if ($text.Contains($circleExact)) { $text = $text.Replace($circleExact, $newExact) }
elseif ($text.Contains($v1Exact)) { $text = $text.Replace($v1Exact, $newExact) }
else { throw 'Could not find exact-point generator to convert to 864x864 square.' }

$text = $text.Replace('Preparing exact disk points...', 'Preparing exact 864-square points...')
$text = $text.Replace('Preparing exact square points...', 'Preparing exact 864-square points...')
$text = $text.Replace('-block disk is one biome.', '-halfwidth square (864x864 at target 432) is one biome.')
$text = $text.Replace('-block square is one biome.', '-halfwidth square (864x864 at target 432) is one biome.')
$text = $text.Replace('inside its measured integer disk', 'inside its measured square [-r,r-1]^2')
$text = $text.Replace('inside its measured centered square', 'inside its measured square [-r,r-1]^2')
$text = $text.Replace('uniform disk necessarily passes', 'uniform target square necessarily passes')
$text = $text.Replace('uniform square necessarily passes', 'uniform target square necessarily passes')
$text = $text.Replace('firstDifferentDistance=', 'firstDifferentSquareLayer=')
$text = $text.Replace('firstDifferentSquareRadius=', 'firstDifferentSquareLayer=')
$text = $text.Replace('firstMismatchDistance', 'firstMismatchSquareLayer')
$text = $text.Replace('firstMismatchSquareRadius', 'firstMismatchSquareLayer')

[System.IO.File]::WriteAllText($sourcePath, $text, [System.Text.UTF8Encoding]::new($false))

Write-Host 'Applied EXACT 864x864 square target semantics.'
Write-Host 'target=432 => X offsets -432..+431 and Z offsets -432..+431.'
Write-Host 'Total target positions: 864 x 864 = 746496.'
Write-Host 'safeRadius now means the largest clean square [-r, r-1] on both axes.'
