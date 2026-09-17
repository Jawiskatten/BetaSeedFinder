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

# P18_PLAINS_COVERAGE_COMPAT_SHIM_V2
#
# The generated Plains source inherits a few legacy runCoverage(...) calls from
# the P16/P17 host path.  Coverage is not used by the P18 objective; exact Plains
# ranking comes entirely from runExact().  Some local patch histories no longer
# contain the old runCoverage helper, so we provide a zero-cost compatibility
# function.
#
# V1 inserted that helper immediately before int main(...).  In this source,
# int main is OUTSIDE namespace singlebiome, while CoverageResult/ExactPoint and
# the legacy calls are inside it.  That made the helper both too late for the
# calls and outside the namespace.  V2 removes any V1 shim and inserts the helper
# immediately after struct CoverageResult, which is inside the namespace and
# before every call site.

# Remove the broken V1/V2 shim first so this repair is safe on already-patched
# generated files.  A genuine historical runCoverage helper has no P18 marker
# and is therefore left untouched.
$shimPattern = '(?s)\n?// P18_PLAINS_COVERAGE_COMPAT_SHIM(?:_V2)?\n.*?return CoverageResult\{\};\n\}\n\n?'
$shimMatches = [regex]::Matches($text, $shimPattern)
if ($shimMatches.Count -gt 1) {
    throw "Expected at most one P18 coverage shim, found $($shimMatches.Count)."
}
if ($shimMatches.Count -eq 1) {
    $text = [regex]::Replace($text, $shimPattern, "`n", 1)
}

# If the full historical helper is actually present, no compatibility shim is
# required.  Still write the file in case we just removed the broken V1 copy.
if ($text.Contains('CoverageResult runCoverage(')) {
    [System.IO.File]::WriteAllText(
        $sourcePath,
        $text,
        [System.Text.UTF8Encoding]::new($false)
    )
    Write-Host 'A real runCoverage helper is already present; removed any obsolete P18 shim.' -ForegroundColor Green
    exit 0
}

$callCount = ([regex]::Matches($text, '\brunCoverage\s*\(')).Count
if ($callCount -eq 0) {
    [System.IO.File]::WriteAllText(
        $sourcePath,
        $text,
        [System.Text.UTF8Encoding]::new($false)
    )
    Write-Host 'No runCoverage calls remain; removed any obsolete P18 shim.' -ForegroundColor Green
    exit 0
}

$coverageStructPattern = '(?s)struct CoverageResult\s*\{.*?\};\n'
$coverageStructMatches = [regex]::Matches($text, $coverageStructPattern)
if ($coverageStructMatches.Count -ne 1) {
    throw "Expected exactly one CoverageResult struct, found $($coverageStructMatches.Count)."
}

$shim = @'

// P18_PLAINS_COVERAGE_COMPAT_SHIM_V2
// Legacy bookkeeping only. P18 connected-Plains ranking is measured by runExact.
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

$structMatch = $coverageStructMatches[0]
$insertPos = $structMatch.Index + $structMatch.Length
$text = $text.Insert($insertPos, $shim)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

$verify = [System.IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
if (-not $verify.Contains('// P18_PLAINS_COVERAGE_COMPAT_SHIM_V2')) {
    throw 'Coverage compatibility V2 marker is missing after write.'
}
$definitionPos = $verify.IndexOf('CoverageResult runCoverage(')
$firstUsePos = $verify.IndexOf('runCoverage(')
if ($definitionPos -lt 0 -or $firstUsePos -lt 0) {
    throw 'runCoverage compatibility definition is missing after write.'
}
if ($definitionPos -gt $firstUsePos) {
    throw 'runCoverage compatibility definition was inserted after a call site.'
}
$namespaceClosePos = $verify.LastIndexOf('} // namespace singlebiome')
if ($namespaceClosePos -lt 0) {
    # Older generated sources may use an unlabelled namespace close; the compile
    # will still verify scope.  Do not guess its position here.
    $namespaceClosePos = $verify.LastIndexOf("`n}`n`nint main(")
}
if ($namespaceClosePos -ge 0 -and $definitionPos -gt $namespaceClosePos) {
    throw 'runCoverage compatibility definition is still outside namespace singlebiome.'
}

Write-Host "Fixed generated Plains source: relocated zero-cost runCoverage shim before $callCount legacy call(s), inside namespace singlebiome." -ForegroundColor Green
Write-Host 'This does not change the Plains metric: exact ranking remains largest connected PLAINS area in the 800x800 square.'
