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

if ($text.Contains('TUNDRA_P4_GPU_COMPACTION')) {
    Write-Host 'Tundra P4 GPU compaction is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TUNDRA_P3_GROUPED_SEARCH')) {
    throw 'P4 requires the P3 grouped Tundra scout. Apply/tune P3 first.'
}
if (-not $text.Contains('SQUARE_TARGET_864_V2')) {
    throw 'P4 requires the exact 864x864 square target patch.'
}

$backupPath = $sourcePath + '.p3-before-p4.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# ---------------------------------------------------------------------------
# 1. Add a tiny device summary + compaction kernel.
#    For topExact <= 1 the CPU only needs:
#      - the best probe/index in the batch
#      - indices that fully passed target and MUST be exact checked
#    It no longer needs the full probeRadius[] / biome[] arrays.
# ---------------------------------------------------------------------------
$exactMarker = '__global__ void exactKernel('
if ([regex]::Matches($text, [regex]::Escape($exactMarker)).Count -ne 1) {
    throw 'Could not find unique exactKernel insertion point.'
}

$compactKernel = @'
// TUNDRA_P4_GPU_COMPACTION
// Small per-batch result produced entirely on the GPU. bestKey packs
// (probeRadius, inverseIndex), so atomicMax chooses the largest radius and,
// on ties, the smallest seed index just like the old CPU ordering.
struct SearchBatchSummary {
    unsigned int targetCount;
    unsigned long long bestKey;
};

__global__ void compactSearchResultsKernel(
        const unsigned short* probeRadius,
        int count,
        int target,
        SearchBatchSummary* summary,
        int* targetIndices
) {
    const int index = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (index >= count) return;

    const unsigned int probe = static_cast<unsigned int>(probeRadius[index]);
    const unsigned int inverseIndex = 0xFFFFFFFFu - static_cast<unsigned int>(index);
    const unsigned long long key =
            (static_cast<unsigned long long>(probe) << 32) |
            static_cast<unsigned long long>(inverseIndex);
    atomicMax(&summary->bestKey, key);

    if (probe == static_cast<unsigned int>(target)) {
        const unsigned int slot = atomicAdd(&summary->targetCount, 1u);
        targetIndices[slot] = index;
    }
}

'@
$text = $text.Replace($exactMarker, $compactKernel + $exactMarker)

# ---------------------------------------------------------------------------
# 2. Device allocations for summary + compacted mandatory target survivors.
# ---------------------------------------------------------------------------
$allocPattern = 'unsigned char\* dBiome = deviceAlloc<unsigned char>\(static_cast<std::size_t>\(o\.batch\)\);'
if ([regex]::Matches($text, $allocPattern).Count -ne 1) {
    throw 'Could not find dBiome allocation to add P4 buffers.'
}
$allocReplacement = @'
unsigned char* dBiome = deviceAlloc<unsigned char>(static_cast<std::size_t>(o.batch));
    SearchBatchSummary* dSearchSummary = deviceAlloc<SearchBatchSummary>(1);
    int* dTargetIndices = deviceAlloc<int>(static_cast<std::size_t>(o.batch));
'@
$text = [regex]::Replace($text, $allocPattern, $allocReplacement.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 3. Host buffers: keep legacy full arrays only for --top-exact > 1 fallback.
# ---------------------------------------------------------------------------
$hostPattern = '(?s)    std::vector<unsigned short> hProbeRadius\(static_cast<std::size_t>\(o\.batch\)\);\r?\n    std::vector<unsigned char> hBiome\(static_cast<std::size_t>\(o\.batch\)\);'
if ([regex]::Matches($text, $hostPattern).Count -ne 1) {
    throw 'Could not find host probe buffers.'
}
$hostReplacement = @'
    std::vector<unsigned short> hProbeRadius;
    std::vector<unsigned char> hBiome;
    if (o.topExact > 1) {
        // Compatibility fallback for unusual multi-candidate runs.
        hProbeRadius.resize(static_cast<std::size_t>(o.batch));
        hBiome.resize(static_cast<std::size_t>(o.batch));
    }
    SearchBatchSummary hSearchSummary{};
    std::vector<int> hTargetIndices;
'@
$text = [regex]::Replace($text, $hostPattern, $hostReplacement.TrimEnd(), 1)

# ---------------------------------------------------------------------------
# 4. Replace search -> D2H copy -> CPU sort/scan with a compact GPU handoff.
#    topExact <= 1 is the production fast path. --top-exact > 1 retains the
#    old behavior to avoid changing CLI semantics for debugging experiments.
# ---------------------------------------------------------------------------
$batchPattern = '(?s)        hipLaunchKernelGGL\(\r?\n                searchKernel,.*?        for \(int index : exactCandidates\) \{'
$batchMatches = [regex]::Matches($text, $batchPattern)
if ($batchMatches.Count -ne 1) {
    throw "Expected exactly one search/CPU-selection block, found $($batchMatches.Count)."
}

$batchReplacement = @'
        // Clear the tiny P4 summary before the search. Commands in the default
        // HIP stream are ordered, so the compaction kernel sees completed scout results.
        HIP_CHECK(hipMemset(dSearchSummary, 0, sizeof(SearchBatchSummary)));

        hipLaunchKernelGGL(
                searchKernel,
                dim3((count + SEARCH_SEEDS_PER_BLOCK - 1) / SEARCH_SEEDS_PER_BLOCK), dim3(SEARCH_THREADS), 0, 0,
                o.sequence, attemptBase, count, o.centerX, o.centerZ,
                dProbeDx, dProbeDz, dRingRadii, static_cast<int>(ringRadii.size()),
                bestExact >= 0 ? std::min(o.target, bestExact + 1) : 0,
                dProbeRadius, dBiome);
        HIP_CHECK(hipGetLastError());

        std::vector<int> exactCandidates;

        if (o.topExact <= 1) {
            // Fast production path: summarize/compact on GPU. This replaces two
            // multi-megabyte D2H copies, a full CPU scan, and partial_sort.
            constexpr int COMPACT_THREADS = 256;
            hipLaunchKernelGGL(
                    compactSearchResultsKernel,
                    dim3((count + COMPACT_THREADS - 1) / COMPACT_THREADS),
                    dim3(COMPACT_THREADS), 0, 0,
                    dProbeRadius, count, o.target, dSearchSummary, dTargetIndices);
            HIP_CHECK(hipGetLastError());

            // This blocking copy also waits for search + compaction in the default stream.
            HIP_CHECK(hipMemcpy(
                    &hSearchSummary, dSearchSummary,
                    sizeof(SearchBatchSummary), hipMemcpyDeviceToHost));

            const unsigned int rawTargetCount = hSearchSummary.targetCount;
            const int targetCount = static_cast<int>(std::min<unsigned int>(
                    rawTargetCount, static_cast<unsigned int>(count)));
            if (targetCount > 0) {
                hTargetIndices.resize(static_cast<std::size_t>(targetCount));
                HIP_CHECK(hipMemcpy(
                        hTargetIndices.data(), dTargetIndices,
                        static_cast<std::size_t>(targetCount) * sizeof(int),
                        hipMemcpyDeviceToHost));
            } else {
                hTargetIndices.clear();
            }

            const int batchBestProbe = static_cast<int>(hSearchSummary.bestKey >> 32);
            const unsigned int inverseIndex =
                    static_cast<unsigned int>(hSearchSummary.bestKey & 0xFFFFFFFFULL);
            const unsigned int decodedIndex = 0xFFFFFFFFu - inverseIndex;
            const int batchBestIndex = decodedIndex < static_cast<unsigned int>(count)
                    ? static_cast<int>(decodedIndex) : -1;

            if (batchBestIndex >= 0 && batchBestProbe > bestProbe) {
                bestProbe = batchBestProbe;
                const std::uint64_t attempt =
                        attemptBase + static_cast<std::uint64_t>(batchBestIndex);
                const std::int64_t seed = static_cast<std::int64_t>(
                        p20::splitMixDeterministicSeed(o.sequence, attempt));
                std::cout << "[PROBE RECORD] seed=" << seed
                          << " biome=" << (bestProbe > 0 ? "TUNDRA" : "UNKNOWN")
                          << " passedProbeRadius=" << bestProbe << "\n";
            }

            // With the dynamic record gate, a seed whose probe score is not above
            // bestExact cannot possibly improve the exact record: it failed an exact
            // in-target sample at/before bestExact+1. Thus we can skip the old
            // unconditional one-exact-check-per-batch work as well.
            if (o.topExact == 1 && batchBestIndex >= 0 && batchBestProbe > bestExact) {
                exactCandidates.push_back(batchBestIndex);
            }

            // Every full target survivor is mandatory. This is the jackpot recall
            // guarantee; no true all-Tundra 864x864 square can be dropped here.
            for (int index : hTargetIndices) {
                if (index != batchBestIndex || exactCandidates.empty() || exactCandidates.front() != index) {
                    exactCandidates.push_back(index);
                }
            }
        } else {
            // Legacy compatibility path for --top-exact > 1.
            HIP_CHECK(hipDeviceSynchronize());
            HIP_CHECK(hipMemcpy(hProbeRadius.data(), dProbeRadius,
                                static_cast<std::size_t>(count) * sizeof(unsigned short), hipMemcpyDeviceToHost));
            HIP_CHECK(hipMemcpy(hBiome.data(), dBiome,
                                static_cast<std::size_t>(count) * sizeof(unsigned char), hipMemcpyDeviceToHost));

            std::vector<int> order(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) order[static_cast<std::size_t>(i)] = i;
            const int wantedTop = std::min(o.topExact, count);
            if (wantedTop > 0) {
                std::partial_sort(order.begin(), order.begin() + wantedTop, order.end(),
                        [&](int a, int b) {
                            if (hProbeRadius[static_cast<std::size_t>(a)] != hProbeRadius[static_cast<std::size_t>(b)])
                                return hProbeRadius[static_cast<std::size_t>(a)] > hProbeRadius[static_cast<std::size_t>(b)];
                            return a < b;
                        });
            }

            exactCandidates.reserve(static_cast<std::size_t>(wantedTop) + 8U);
            std::unordered_set<int> seen;
            for (int i = 0; i < wantedTop; ++i) {
                const int index = order[static_cast<std::size_t>(i)];
                exactCandidates.push_back(index);
                seen.insert(index);
            }
            for (int i = 0; i < count; ++i) {
                if (static_cast<int>(hProbeRadius[static_cast<std::size_t>(i)]) == o.target &&
                    seen.insert(i).second) {
                    exactCandidates.push_back(i);
                }
            }

            for (int i = 0; i < count; ++i) {
                const int probe = static_cast<int>(hProbeRadius[static_cast<std::size_t>(i)]);
                if (probe > bestProbe) {
                    bestProbe = probe;
                    const std::uint64_t attempt = attemptBase + static_cast<std::uint64_t>(i);
                    const std::int64_t seed = static_cast<std::int64_t>(
                            p20::splitMixDeterministicSeed(o.sequence, attempt));
                    std::cout << "[PROBE RECORD] seed=" << seed
                              << " biome=" << biomeName(hBiome[static_cast<std::size_t>(i)])
                              << " passedProbeRadius=" << bestProbe << "\n";
                }
            }
        }

        checked += static_cast<std::uint64_t>(count);

        for (int index : exactCandidates) {
'@
$text = [regex]::Replace($text, $batchPattern, $batchReplacement, 1)

# ---------------------------------------------------------------------------
# 5. Banner + cleanup.
# ---------------------------------------------------------------------------
$bannerPattern = 'P3 scout: TUNDRA-only \| [^"\r\n]+'
if ([regex]::Matches($text, $bannerPattern).Count -ge 1) {
    $text = [regex]::Replace(
        $text,
        $bannerPattern,
        'P4 scout: TUNDRA-only | GPU survivor compaction | tuned grouped search | dense outer-band',
        1
    )
}

$freeMarker = '    hipFree(dBiome);'
if ([regex]::Matches($text, [regex]::Escape($freeMarker)).Count -ne 1) {
    throw 'Could not find search-mode dBiome cleanup.'
}
$freeReplacement = @'
    hipFree(dTargetIndices);
    hipFree(dSearchSummary);
    hipFree(dBiome);
'@
$text = $text.Replace($freeMarker, $freeReplacement.TrimEnd())

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TUNDRA P4 GPU survivor compaction.' -ForegroundColor Green
Write-Host 'Normal topExact=0/1 runs no longer copy/scan the full probe and biome arrays on the CPU.'
Write-Host 'GPU returns one best probe plus every mandatory full-target survivor.'
Write-Host 'The old full CPU selection path remains available automatically when --top-exact > 1.'
Write-Host 'True 864x864 all-Tundra jackpot recall is unchanged.'
Write-Host 'Your tuned SEARCH_SEEDS_PER_BLOCK and runner batch settings are preserved.'
