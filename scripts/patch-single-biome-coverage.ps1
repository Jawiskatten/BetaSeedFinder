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

if ($text.Contains('realCoveragePercent')) {
    Write-Host 'True full-disk coverage output is already patched.'
    exit 0
}

# If the older radius-squared coverage patch was already applied locally,
# normalize those two output blocks back to the original form first.
$oldRadiusPrint = @'
void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    const double radiusRatio = static_cast<double>(result.safeRadius) / static_cast<double>(o.target);
    const double targetAreaCoverage = 100.0 * radiusRatio * radiusRatio;
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius
              << " targetAreaCoverage=" << std::fixed << std::setprecision(2)
              << targetAreaCoverage << "%";
'@
$plainPrint = @'
void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius;
'@
if ($text.Contains($oldRadiusPrint)) {
    $text = $text.Replace($oldRadiusPrint, $plainPrint)
}

$oldRadiusStatus = @'
            if (bestExact >= 0) {
                const double radiusRatio = static_cast<double>(bestExact) / static_cast<double>(o.target);
                const double exactCoverage = 100.0 * radiusRatio * radiusRatio;
                std::cout << " exactCoverage=" << std::fixed << std::setprecision(2)
                          << exactCoverage << "%"
                          << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
'@
$plainStatus = @'
            if (bestExact >= 0) {
                std::cout << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
'@
if ($text.Contains($oldRadiusStatus)) {
    $text = $text.Replace($oldRadiusStatus, $plainStatus)
}

$oldStruct = @'
struct ExactResult {
    int safeRadius;
    int firstMismatchD2;
    int baseBiome;
};
'@
$newStruct = @'
struct ExactResult {
    int safeRadius;
    int firstMismatchD2;
    int baseBiome;
};

struct CoverageResult {
    int sameCount;
    int totalCount;
};
'@
if (-not $text.Contains($oldStruct)) {
    throw 'Could not find ExactResult struct to patch.'
}
$text = $text.Replace($oldStruct, $newStruct)

$parseMarker = 'std::uint64_t parseU64(const std::string& value, const char* name) {'
if (-not $text.Contains($parseMarker)) {
    throw 'Could not find parseU64 insertion point.'
}
$coverageKernel = @'
__global__ void coverageKernel(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* points,
        int pointCount,
        CoverageResult* result
) {
    const int lane = static_cast<int>(threadIdx.x);
    __shared__ ClimateState s;
    __shared__ int sameCount;
    initClimate(s, seed);

    if (lane == 0) {
        s.baseBiome = static_cast<int>(biomeAt(s, centerX, centerZ));
        sameCount = 1; // center block itself
    }
    __syncthreads();

    int localSame = 0;
    for (int idx = lane; idx < pointCount; idx += EXACT_THREADS) {
        const ExactPoint p = points[idx];
        const unsigned char b = biomeAt(s, centerX + p.dx, centerZ + p.dz);
        if (static_cast<int>(b) == s.baseBiome) ++localSame;
    }
    atomicAdd(&sameCount, localSame);
    __syncthreads();

    if (lane == 0) {
        result->sameCount = sameCount;
        result->totalCount = pointCount + 1;
    }
}

'@
$text = $text.Replace($parseMarker, $coverageKernel + $parseMarker)

$ensureMarker = 'void ensureLogHeader(const std::string& path) {'
if (-not $text.Contains($ensureMarker)) {
    throw 'Could not find ensureLogHeader insertion point.'
}
$coverageHost = @'
CoverageResult runCoverage(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* dPoints,
        int pointCount,
        CoverageResult* dResult
) {
    hipLaunchKernelGGL(
            coverageKernel,
            dim3(1), dim3(EXACT_THREADS), 0, 0,
            seed, centerX, centerZ, dPoints, pointCount, dResult);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    CoverageResult result{};
    HIP_CHECK(hipMemcpy(&result, dResult, sizeof(result), hipMemcpyDeviceToHost));
    return result;
}

'@
$text = $text.Replace($ensureMarker, $coverageHost + $ensureMarker)

$oldPrintFunction = @'
void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius;
    if (result.firstMismatchD2 < 0) {
        std::cout << " firstDifferent=NONE_WITHIN_" << o.target;
    } else {
        std::cout << " firstDifferentDistance=" << std::fixed << std::setprecision(3)
                  << std::sqrt(static_cast<double>(result.firstMismatchD2));
    }
    std::cout << " center=(" << o.centerX << ',' << o.centerZ << ")\n";
}
'@
$newPrintFunction = @'
double realCoveragePercent(const CoverageResult& coverage) {
    if (coverage.totalCount <= 0) return 0.0;
    return 100.0 * static_cast<double>(coverage.sameCount) /
           static_cast<double>(coverage.totalCount);
}

void printExactResult(
        const Options& o,
        std::int64_t seed,
        const ExactResult& result,
        const CoverageResult* coverage = nullptr
) {
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius;
    if (coverage != nullptr) {
        std::cout << " realCoverage=" << std::fixed << std::setprecision(3)
                  << realCoveragePercent(*coverage) << "%"
                  << " sameBiomeBlocks=" << coverage->sameCount
                  << '/' << coverage->totalCount;
    }
    if (result.firstMismatchD2 < 0) {
        std::cout << " firstDifferent=NONE_WITHIN_" << o.target;
    } else {
        std::cout << " firstDifferentDistance=" << std::fixed << std::setprecision(3)
                  << std::sqrt(static_cast<double>(result.firstMismatchD2));
    }
    std::cout << " center=(" << o.centerX << ',' << o.centerZ << ")\n";
}
'@
if (-not $text.Contains($oldPrintFunction)) {
    throw 'Could not find printExactResult function to patch.'
}
$text = $text.Replace($oldPrintFunction, $newPrintFunction)

$oldAlloc = @'
    ExactPoint* dExactPoints = deviceAlloc<ExactPoint>(exactPoints.size());
    ExactResult* dExactResult = deviceAlloc<ExactResult>(1);
'@
$newAlloc = @'
    ExactPoint* dExactPoints = deviceAlloc<ExactPoint>(exactPoints.size());
    ExactResult* dExactResult = deviceAlloc<ExactResult>(1);
    CoverageResult* dCoverageResult = deviceAlloc<CoverageResult>(1);
'@
if (-not $text.Contains($oldAlloc)) {
    throw 'Could not find exact-result allocation block.'
}
$text = $text.Replace($oldAlloc, $newAlloc)

$oldVerify = @'
    if (o.verifyOnly) {
        const ExactResult result = runExact(
                o.verifySeed, o.centerX, o.centerZ, o.target,
                dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);
        std::cout << "[VERIFY] ";
        printExactResult(o, o.verifySeed, result);
        hipFree(dExactResult);
        hipFree(dExactPoints);
        return result.safeRadius >= o.target ? 0 : 2;
    }
'@
$newVerify = @'
    if (o.verifyOnly) {
        const ExactResult result = runExact(
                o.verifySeed, o.centerX, o.centerZ, o.target,
                dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);
        const CoverageResult coverage = runCoverage(
                o.verifySeed, o.centerX, o.centerZ,
                dExactPoints, static_cast<int>(exactPoints.size()), dCoverageResult);
        std::cout << "[VERIFY] ";
        printExactResult(o, o.verifySeed, result, &coverage);
        hipFree(dCoverageResult);
        hipFree(dExactResult);
        hipFree(dExactPoints);
        return result.safeRadius >= o.target ? 0 : 2;
    }
'@
if (-not $text.Contains($oldVerify)) {
    throw 'Could not find verify-only block to patch.'
}
$text = $text.Replace($oldVerify, $newVerify)

$oldBestVars = @'
    int bestExact = -1;
    std::int64_t bestExactSeed = 0;
    unsigned char bestExactBiome = 255;
    bool stop = false;
'@
$newBestVars = @'
    int bestExact = -1;
    std::int64_t bestExactSeed = 0;
    unsigned char bestExactBiome = 255;
    CoverageResult bestCoverage{};
    bool stop = false;
'@
if (-not $text.Contains($oldBestVars)) {
    throw 'Could not find best-record variables to patch.'
}
$text = $text.Replace($oldBestVars, $newBestVars)

$oldCandidateBlock = @'
            const ExactResult result = runExact(
                    seed, o.centerX, o.centerZ, o.target,
                    dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);

            if (result.safeRadius > bestExact) {
                bestExact = result.safeRadius;
                bestExactSeed = seed;
                bestExactBiome = static_cast<unsigned char>(result.baseBiome);
                std::cout << "[RECORD] ";
                printExactResult(o, seed, result);
                appendHit(o, checked, attempt, seed, result);
            }

            if (result.safeRadius >= o.target) {
                std::cout << "\n[JACKPOT] Full " << o.target
                          << "-block disk is one biome.\n[JACKPOT] ";
                printExactResult(o, seed, result);
                appendHit(o, checked, attempt, seed, result);
                if (!o.continueAfterHit) {
                    stop = true;
                    break;
                }
            }
'@
$newCandidateBlock = @'
            const ExactResult result = runExact(
                    seed, o.centerX, o.centerZ, o.target,
                    dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);

            CoverageResult coverage{};
            bool haveCoverage = false;
            if (result.safeRadius > bestExact) {
                coverage = runCoverage(
                        seed, o.centerX, o.centerZ,
                        dExactPoints, static_cast<int>(exactPoints.size()), dCoverageResult);
                haveCoverage = true;
                bestExact = result.safeRadius;
                bestExactSeed = seed;
                bestExactBiome = static_cast<unsigned char>(result.baseBiome);
                bestCoverage = coverage;
                std::cout << "[RECORD] ";
                printExactResult(o, seed, result, &coverage);
                appendHit(o, checked, attempt, seed, result);
            }

            if (result.safeRadius >= o.target) {
                if (!haveCoverage) {
                    coverage = runCoverage(
                            seed, o.centerX, o.centerZ,
                            dExactPoints, static_cast<int>(exactPoints.size()), dCoverageResult);
                    haveCoverage = true;
                }
                std::cout << "\n[JACKPOT] Full " << o.target
                          << "-block disk is one biome.\n[JACKPOT] ";
                printExactResult(o, seed, result, &coverage);
                appendHit(o, checked, attempt, seed, result);
                if (!o.continueAfterHit) {
                    stop = true;
                    break;
                }
            }
'@
if (-not $text.Contains($oldCandidateBlock)) {
    throw 'Could not find exact-candidate record block to patch.'
}
$text = $text.Replace($oldCandidateBlock, $newCandidateBlock)

$newStatus = @'
            if (bestExact >= 0) {
                std::cout << " realCoverage=" << std::fixed << std::setprecision(3)
                          << realCoveragePercent(bestCoverage) << "%"
                          << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
'@
if (-not $text.Contains($plainStatus)) {
    throw 'Could not find status record block to patch.'
}
$text = $text.Replace($plainStatus, $newStatus)

$oldFree = @'
    hipFree(dProbeDx);
    hipFree(dExactResult);
    hipFree(dExactPoints);
'@
$newFree = @'
    hipFree(dProbeDx);
    hipFree(dCoverageResult);
    hipFree(dExactResult);
    hipFree(dExactPoints);
'@
if (-not $text.Contains($oldFree)) {
    throw 'Could not find cleanup block to patch.'
}
$text = $text.Replace($oldFree, $newFree)

[System.IO.File]::WriteAllText($sourcePath, $text, [System.Text.UTF8Encoding]::new($false))

Write-Host 'Patched SingleBiomeRadiusFinder with TRUE full-disk biome coverage.'
Write-Host 'realCoverage = matching-biome block positions / all block positions inside the target disk.'
Write-Host 'The full coverage scan only runs for a new exact record, verify command, or jackpot.'
