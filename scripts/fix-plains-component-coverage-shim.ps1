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

# P18 is generated from the locally-patched P17 Rainforest source.  The P16/P17
# host path still contains legacy CoverageResult bookkeeping/calls, but on some
# local patch histories the old runCoverage helper itself is no longer present.
# Coverage is NOT part of the Plains objective: ranking comes entirely from the
# exact connected-component result returned by runExact.  Supply a zero-cost
# compatibility shim so those stale bookkeeping calls compile without launching
# another full biome-coverage pass or affecting the Plains score.

if ($text.Contains('// P18_PLAINS_COVERAGE_COMPAT_SHIM')) {
    Write-Host 'P18 Plains coverage compatibility shim is already applied.' -ForegroundColor Green
    exit 0
}

if ($text.Contains('CoverageResult runCoverage(')) {
    Write-Host 'A real runCoverage helper is already present; no Plains shim needed.' -ForegroundColor Green
    exit 0
}

$callCount = ([regex]::Matches($text, '\brunCoverage\s*\(')).Count
if ($callCount -eq 0) {
    Write-Host 'No runCoverage calls remain; no Plains shim needed.' -ForegroundColor Green
    exit 0
}

if (-not $text.Contains('struct CoverageResult')) {
    throw 'Generated Plains source calls runCoverage but CoverageResult is missing.'
}

$mainPos = $text.IndexOf('int main(')
if ($mainPos -lt 0) {
    throw 'Could not locate int main(...) in generated Plains source.'
}

$shim = @'
// P18_PLAINS_COVERAGE_COMPAT_SHIM
// Legacy P16/P17 bookkeeping still calls runCoverage, but connected Plains area
// is measured exclusively by runExact. Keep these calls compile-compatible and
// deliberately zero-cost; no ranking/output path uses this dummy coverage value.
CoverageResult runCoverage(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* dPoints,
        int pointCount,
        CoverageResult* dResult
) {
    (void)seed;
    (void)centerX;
    (void)centerZ;
    (void)dPoints;
    (void)pointCount;
    (void)dResult;
    return CoverageResult{};
}

'@

$text = $text.Insert($mainPos, $shim)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath)
if (-not $verify.Contains('// P18_PLAINS_COVERAGE_COMPAT_SHIM')) {
    throw 'Coverage compatibility shim write verification failed.'
}
if (-not $verify.Contains('CoverageResult runCoverage(')) {
    throw 'Coverage compatibility function is still missing after patch.'
}

Write-Host "Fixed generated Plains source: supplied zero-cost runCoverage compatibility shim for $callCount legacy call(s)." -ForegroundColor Green
Write-Host 'This does not change the Plains metric: exact ranking remains largest connected PLAINS area in the 800x800 square.'
