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

if ($text.Contains('targetAreaCoverage=')) {
    Write-Host 'Coverage output is already patched.'
    exit 0
}

$oldPrint = @'
void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius;
'@

$newPrint = @'
void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    const double radiusRatio = static_cast<double>(result.safeRadius) / static_cast<double>(o.target);
    const double targetAreaCoverage = 100.0 * radiusRatio * radiusRatio;
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius
              << " targetAreaCoverage=" << std::fixed << std::setprecision(2)
              << targetAreaCoverage << "%";
'@

if (-not $text.Contains($oldPrint)) {
    throw 'Could not find printExactResult block to patch. Source may have changed.'
}
$text = $text.Replace($oldPrint, $newPrint)

$oldStatus = @'
            if (bestExact >= 0) {
                std::cout << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
'@

$newStatus = @'
            if (bestExact >= 0) {
                const double radiusRatio = static_cast<double>(bestExact) / static_cast<double>(o.target);
                const double exactCoverage = 100.0 * radiusRatio * radiusRatio;
                std::cout << " exactCoverage=" << std::fixed << std::setprecision(2)
                          << exactCoverage << "%"
                          << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
'@

if (-not $text.Contains($oldStatus)) {
    throw 'Could not find status block to patch. Source may have changed.'
}
$text = $text.Replace($oldStatus, $newStatus)

[System.IO.File]::WriteAllText($sourcePath, $text, [System.Text.UTF8Encoding]::new($false))

Write-Host 'Patched SingleBiomeRadiusFinder coverage output.'
Write-Host 'Coverage means guaranteed same-biome disk area / requested target disk area.'
Write-Host 'Example: safeRadius=406 of target=432 => 88.33%.'
