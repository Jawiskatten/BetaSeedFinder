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

if ($text.Contains('TU4_WATER_P13B_SCREEN_RECALL_AUDIT')) {
    Write-Host 'TU4 Water P13b screen recall audit is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P12_DIRECT_SCREEN')) {
    throw 'P13b requires the P12c direct exact-density screen first.'
}
if (-not $text.Contains('TU4_WATER_P8_BATCHED_EXACT')) {
    throw 'P13b expects the P8 batched exact evaluator underneath P12c.'
}

$backupPath = $sourcePath + '.p12c-before-water-p13b.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

$markerPos = $text.IndexOf('// TU4_WATER_P12_DIRECT_SCREEN')
if ($markerPos -lt 0) { throw 'Could not locate P12c marker.' }
$marker = @'
// TU4_WATER_P13B_SCREEN_RECALL_AUDIT
// Optional diagnostic: full-exact all P6 finalists and measure P12c chosen-8 recall.
// Normal search remains the P12c 8/24 route when audit mode is not requested.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# -------------------------------------------------------------------------
# CLI option: --audit-screen-batches N
# -------------------------------------------------------------------------
$optionsNeedle = '    bool verifyOnly = false;'
$pos = $text.IndexOf($optionsNeedle)
if ($pos -lt 0) { throw 'Could not locate Options verifyOnly field.' }
$text = $text.Insert($pos + $optionsNeedle.Length, "`n    int auditScreenBatches = 0;")

$helpNeedle = '        << "  --verify-seed SEED     Exact-check one seed and exit\n"'
$pos = $text.IndexOf($helpNeedle)
if ($pos -lt 0) { throw 'Could not locate help verify-seed line.' }
$helpInsert = $helpNeedle + "`n" + '        << "  --audit-screen-batches N  Audit P12c chosen-8 recall for N scout batches\n"'
$text = $text.Remove($pos, $helpNeedle.Length).Insert($pos, $helpInsert)

$parseNeedle = '        else if (a == "--verify-seed") { o.verifySeed = parseI64(need("--verify-seed"), "verify-seed"); o.verifyOnly = true; }'
$pos = $text.IndexOf($parseNeedle)
if ($pos -lt 0) { throw 'Could not locate verify-seed option parser.' }
$parseInsert = @'
        else if (a == "--audit-screen-batches") o.auditScreenBatches = parseInt(need("--audit-screen-batches"), "audit-screen-batches");
        else if (a == "--verify-seed") { o.verifySeed = parseI64(need("--verify-seed"), "verify-seed"); o.verifyOnly = true; }
'@.TrimEnd()
$text = $text.Remove($pos, $parseNeedle.Length).Insert($pos, $parseInsert)

$validateNeedle = '    if (o.topExact > o.batch) o.topExact = o.batch;'
$pos = $text.IndexOf($validateNeedle)
if ($pos -lt 0) { throw 'Could not locate Options validation.' }
$validateInsert = $validateNeedle + "`n" + '    if (o.auditScreenBatches < 0) throw std::runtime_error("--audit-screen-batches must be >= 0");'
$text = $text.Remove($pos, $validateNeedle.Length).Insert($pos, $validateInsert)

# -------------------------------------------------------------------------
# Audit stats/helpers.
# -------------------------------------------------------------------------
$runStart = $text.IndexOf('std::vector<ExactWaterResult> runExactBatch(')
if ($runStart -lt 0) { throw 'Could not locate P12c runExactBatch.' }

$auditHelpers = @'
struct P13ScreenAuditStats {
    std::uint64_t batches = 0;
    std::uint64_t chosenHits = 0;
    std::uint64_t screenTop1Hits = 0;
    std::uint64_t screenTop3Hits = 0;
    std::uint64_t screenTop6Hits = 0;
    std::uint64_t screenTop8Hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t missRegretSum = 0;
    int maxMissRegret = 0;
    std::uint64_t trueBestLE120k = 0;
    std::uint64_t hitLE120k = 0;
    std::uint64_t trueBestLE110k = 0;
    std::uint64_t hitLE110k = 0;
    std::uint64_t trueBestLE100k = 0;
    std::uint64_t hitLE100k = 0;
    std::uint64_t screenRankHistogram[64] = {};
};

static bool gP13AuditScreen = false;
static P13ScreenAuditStats gP13Audit{};

void p13PrintAuditSummary(const char* tag) {
    const auto& a = gP13Audit;
    if (a.batches == 0) return;
    auto pct = [&](std::uint64_t v, std::uint64_t d) {
        return d == 0 ? 0.0 : 100.0 * static_cast<double>(v) / static_cast<double>(d);
    };
    const double avgMissRegret = a.misses == 0
            ? 0.0
            : static_cast<double>(a.missRegretSum) / static_cast<double>(a.misses);
    std::cout << tag
              << " batches=" << a.batches
              << " chosen8Recall=" << std::fixed << std::setprecision(3) << pct(a.chosenHits, a.batches) << "%"
              << " screenTop1=" << pct(a.screenTop1Hits, a.batches) << "%"
              << " screenTop3=" << pct(a.screenTop3Hits, a.batches) << "%"
              << " screenTop6=" << pct(a.screenTop6Hits, a.batches) << "%"
              << " screenTop8=" << pct(a.screenTop8Hits, a.batches) << "%"
              << " misses=" << a.misses
              << " avgMissRegret=" << std::setprecision(1) << avgMissRegret
              << " maxMissRegret=" << a.maxMissRegret
              << " <=120k=" << a.hitLE120k << '/' << a.trueBestLE120k
              << " <=110k=" << a.hitLE110k << '/' << a.trueBestLE110k
              << " <=100k=" << a.hitLE100k << '/' << a.trueBestLE100k
              << "\n";
}

'@
$text = $text.Insert($runStart, $auditHelpers)

# -------------------------------------------------------------------------
# Ground truth: after P12c chooses 8, but before it overwrites the all-24 state
# buffers, exact all finalists and compare the true best with the chosen set.
# -------------------------------------------------------------------------
$auditInsertNeedle = '    // Compact chosen candidates into front slots and run the unchanged full exact.'
$pos = $text.IndexOf($auditInsertNeedle)
if ($pos -lt 0) { throw 'Could not locate P12c chosen-candidate compaction point.' }

$auditBranch = @'
    if (gP13AuditScreen && n > 1) {
        const int auditTotalPoints = n * COARSE_POINT_COUNT;
        const int auditPointBlocks = (auditTotalPoints + EXACT_THREADS - 1) / EXACT_THREADS;
        hipLaunchKernelGGL(
                exactSeaDensityKernel,
                dim3(auditPointBlocks), dim3(EXACT_THREADS), 0, 0,
                w.dTerrain, w.dTemp, w.dRain, w.dBlend, w.dDensity,
                n, COARSE_POINTS, false);
        HIP_CHECK(hipGetLastError());

        const std::size_t auditNN = static_cast<std::size_t>(n);
        HIP_CHECK(hipMemset(w.dLand, 0, auditNN * sizeof(int)));
        const int auditTotalCells = n * COARSE_CELL_COUNT;
        const int auditCellBlocks = (auditTotalCells + EXACT_THREADS - 1) / EXACT_THREADS;
        hipLaunchKernelGGL(
                countLandKernel,
                dim3(auditCellBlocks), dim3(EXACT_THREADS), 0, 0,
                w.dDensity, w.dLand, n);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(w.hLand.data(), w.dLand,
                            auditNN * sizeof(int), hipMemcpyDeviceToHost));

        int trueBestSource = 0;
        int trueBestLand = w.hLand[0];
        for (int i = 1; i < n; ++i) {
            const int land = w.hLand[static_cast<std::size_t>(i)];
            if (land < trueBestLand) {
                trueBestLand = land;
                trueBestSource = i;
            }
        }

        int bestChosenLand = SEARCH_COLUMNS + 1;
        bool chosenHit = false;
        for (int source : chosen) {
            const int land = w.hLand[static_cast<std::size_t>(source)];
            if (land < bestChosenLand) bestChosenLand = land;
            if (source == trueBestSource) chosenHit = true;
        }

        int screenRank = n;
        for (int rank = 0; rank < n; ++rank) {
            if (screen[static_cast<std::size_t>(rank)].source == trueBestSource) {
                screenRank = rank;
                break;
            }
        }

        auto& a = gP13Audit;
        ++a.batches;
        if (chosenHit) ++a.chosenHits;
        if (screenRank == 0) ++a.screenTop1Hits;
        if (screenRank < 3) ++a.screenTop3Hits;
        if (screenRank < 6) ++a.screenTop6Hits;
        if (screenRank < 8) ++a.screenTop8Hits;
        if (screenRank >= 0 && screenRank < 64) ++a.screenRankHistogram[screenRank];

        if (!chosenHit) {
            ++a.misses;
            const int regret = std::max(0, bestChosenLand - trueBestLand);
            a.missRegretSum += static_cast<std::uint64_t>(regret);
            if (regret > a.maxMissRegret) a.maxMissRegret = regret;
        }

        if (trueBestLand <= 120000) {
            ++a.trueBestLE120k;
            if (chosenHit) ++a.hitLE120k;
        }
        if (trueBestLand <= 110000) {
            ++a.trueBestLE110k;
            if (chosenHit) ++a.hitLE110k;
        }
        if (trueBestLand <= 100000) {
            ++a.trueBestLE100k;
            if (chosenHit) ++a.hitLE100k;
        }

        if ((a.batches % 100) == 0) p13PrintAuditSummary("[AUDIT]");

        std::vector<ExactWaterResult> auditResults(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            auditResults[static_cast<std::size_t>(i)].landColumns = SEARCH_COLUMNS + 1;
            auditResults[static_cast<std::size_t>(i)].waterColumns = 0;
        }
        for (int source : chosen) {
            const int land = w.hLand[static_cast<std::size_t>(source)];
            auditResults[static_cast<std::size_t>(source)].landColumns = land;
            auditResults[static_cast<std::size_t>(source)].waterColumns = TOTAL_COLUMNS - land;
        }
        return auditResults;
    }

'@
$text = $text.Insert($pos, $auditBranch)

# -------------------------------------------------------------------------
# Main audit setup/final report.
# -------------------------------------------------------------------------
$sequenceEndNeedle = '                    ^ 0x5455345741544552ULL;'
$pos = $text.IndexOf($sequenceEndNeedle)
if ($pos -lt 0) { throw 'Could not locate sequence initialization in main.' }
$mainAuditSetup = @'

        if (o.auditScreenBatches > 0) {
            gP13AuditScreen = true;
            const std::uint64_t auditAttempts = static_cast<std::uint64_t>(o.batch)
                    * static_cast<std::uint64_t>(o.auditScreenBatches);
            if (o.maxAttempts == 0 || o.maxAttempts > auditAttempts) {
                o.maxAttempts = auditAttempts;
            }
            std::cout << "[AUDIT MODE] batches=" << o.auditScreenBatches
                      << " attempts=" << o.maxAttempts
                      << " | full-exacting all P6 finalists for recall ground truth\n";
        }
'@
$text = $text.Insert($pos + $sequenceEndNeedle.Length, $mainAuditSetup)

$freeNeedle = '        (void)hipFree(dLow);'
$pos = $text.IndexOf($freeNeedle)
if ($pos -lt 0) { throw 'Could not locate final GPU cleanup in main.' }
$finalAudit = @'
        if (gP13AuditScreen) {
            p13PrintAuditSummary("[AUDIT FINAL]");
            std::cout << "[AUDIT HIST] true-best screen rank (0=best):";
            const int histN = std::min(o.topExact, 64);
            for (int rank = 0; rank < histN; ++rank) {
                std::cout << ' ' << rank << ':' << gP13Audit.screenRankHistogram[rank];
            }
            std::cout << "\n";
        }

'@
$text = $text.Insert($pos, $finalAudit)

$text = $text.Replace(
    'Scout P12c: P10/P8 + 16x16 full-density screen; 8/24 full exact; final metric unchanged.',
    'Scout P13b: P12c 8/24 screen + optional all-24 recall audit; final metric unchanged.'
)

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P13b screen-recall audit.' -ForegroundColor Green
Write-Host 'Normal search remains P12c when -AuditScreenBatches is omitted.'
Write-Host 'Audit mode full-exacts all 24 finalists and measures chosen-8 recall.'
Write-Host 'Reports screen top-1/3/6/8 recall, miss regret, tail recall, and rank histogram.'
