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

if ($text.Contains('TU4_WATER_P6_DIVERSE_FINALISTS')) {
    Write-Host 'TU4 Water P6 diverse finalists is already applied.' -ForegroundColor Green
    exit 0
}
if (-not $text.Contains('TU4_WATER_P5_FULL64_SOFT_WATER')) {
    throw 'P6 requires TU4 Water P5 full-64 soft-water scout first.'
}

$backupPath = $sourcePath + '.p5-before-water-p6.bak'
if (-not (Test-Path $backupPath -PathType Leaf)) {
    Copy-Item -LiteralPath $sourcePath -Destination $backupPath
}

# P5 found a new record with scoutLow=59/64 and penalty=6.119 even though the
# global scout leader was 64/64 with penalty=2.749. That is strong evidence that
# one scalar scout ordering is useful but not sufficient. P6 keeps P5 unchanged
# on the GPU and changes only finalist selection on the CPU:
#   - 2/3 of exact slots: lowest P5 soft land penalty (preserves P5 strength)
#   - 1/6: highest hard submerged count
#   - 1/6: lowest excess-elevation residual
# The residual subtracts the unavoidable half-penalty associated with samples
# already on the land side of the hard 7.875 threshold. It therefore favors
# seeds whose few bad samples are only barely bad instead of towering peaks.
#
# Default TopExact becomes 24. That preserves 16 primary P5-style finalists per
# 131072-seed batch and adds 8 diverse finalists, rather than stealing slots from
# the scout that already produced the 138,418-land record.

$markerPos = $text.IndexOf('// TU4_WATER_P5_FULL64_SOFT_WATER')
if ($markerPos -lt 0) {
    throw 'Could not locate P5 marker.'
}
$marker = @'
// TU4_WATER_P6_DIVERSE_FINALISTS
// Finalists are selected as a portfolio of soft-penalty, hard-submerged-count,
// and excess-elevation-residual rankings. Exact land count remains authoritative.
'@
$text = $text.Insert($markerPos, $marker + "`n")

# Add persistent scratch vectors next to the existing indices vector. Keeping
# them allocated across batches avoids repeated heap churn in the hot loop.
$declOld = '        std::vector<int> indices(static_cast<std::size_t>(o.batch));'
$declPos = $text.IndexOf($declOld)
if ($declPos -lt 0) {
    throw 'Could not locate indices vector declaration.'
}
$declNew = @'
        std::vector<int> indices(static_cast<std::size_t>(o.batch));
        std::vector<int> hardOrder(static_cast<std::size_t>(o.batch));
        std::vector<int> residualOrder(static_cast<std::size_t>(o.batch));
        std::vector<int> selectedIndices(static_cast<std::size_t>(o.topExact));
'@.TrimEnd()
$text = $text.Remove($declPos, $declOld.Length).Insert($declPos, $declNew)

# Replace the old single partial_sort with three small top-pool rankings. This
# region is located structurally so local formatting from P3/P4/P5 cannot break
# the patcher.
$selectStart = $text.IndexOf('            indices.resize(static_cast<std::size_t>(count));')
if ($selectStart -lt 0) {
    throw 'Could not locate finalist selection start.'
}
$batchBestOld = '            const int batchBest = indices[0];'
$batchBestPos = $text.IndexOf($batchBestOld, $selectStart)
if ($batchBestPos -lt 0) {
    throw 'Could not locate finalist selection end.'
}
$selectEnd = $batchBestPos + $batchBestOld.Length

$newSelection = @'
            indices.resize(static_cast<std::size_t>(count));
            hardOrder.resize(static_cast<std::size_t>(count));
            residualOrder.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                indices[static_cast<std::size_t>(i)] = i;
                hardOrder[static_cast<std::size_t>(i)] = i;
                residualOrder[static_cast<std::size_t>(i)] = i;
            }

            const int exactN = std::min(o.topExact, count);
            const int primaryTarget = std::min(exactN, (exactN * 2 + 2) / 3);
            const int diverseSlots = exactN - primaryTarget;
            const int hardTarget = diverseSlots / 2;
            const int residualTarget = diverseSlots - hardTarget;
            const int poolN = std::min(count, std::max(exactN, exactN * 8));

            auto betterPenalty = [&](int a, int b) {
                if (hSum[a] != hSum[b]) return hSum[a] < hSum[b];
                return hLow[a] > hLow[b];
            };
            auto betterHard = [&](int a, int b) {
                if (hLow[a] != hLow[b]) return hLow[a] > hLow[b];
                return hSum[a] < hSum[b];
            };
            auto residualScore = [&](int i) {
                const double hardLandSamples = static_cast<double>(64 - hLow[i]);
                return hSum[i] - 0.5 * hardLandSamples;
            };
            auto betterResidual = [&](int a, int b) {
                const double sa = residualScore(a);
                const double sb = residualScore(b);
                if (sa != sb) return sa < sb;
                if (hLow[a] != hLow[b]) return hLow[a] > hLow[b];
                return hSum[a] < hSum[b];
            };

            if (poolN < count) {
                std::partial_sort(indices.begin(), indices.begin() + poolN, indices.end(), betterPenalty);
                std::partial_sort(hardOrder.begin(), hardOrder.begin() + poolN, hardOrder.end(), betterHard);
                std::partial_sort(residualOrder.begin(), residualOrder.begin() + poolN, residualOrder.end(), betterResidual);
            } else {
                std::sort(indices.begin(), indices.end(), betterPenalty);
                std::sort(hardOrder.begin(), hardOrder.end(), betterHard);
                std::sort(residualOrder.begin(), residualOrder.end(), betterResidual);
            }

            int selectedCount = 0;
            auto alreadySelected = [&](int idx) {
                for (int j = 0; j < selectedCount; ++j) {
                    if (selectedIndices[static_cast<std::size_t>(j)] == idx) return true;
                }
                return false;
            };
            auto addUnique = [&](int idx) {
                if (selectedCount >= exactN || alreadySelected(idx)) return false;
                selectedIndices[static_cast<std::size_t>(selectedCount++)] = idx;
                return true;
            };

            for (int i = 0; i < poolN && selectedCount < primaryTarget; ++i) {
                addUnique(indices[static_cast<std::size_t>(i)]);
            }
            const int hardStop = primaryTarget + hardTarget;
            for (int i = 0; i < poolN && selectedCount < hardStop; ++i) {
                addUnique(hardOrder[static_cast<std::size_t>(i)]);
            }
            const int residualStop = primaryTarget + hardTarget + residualTarget;
            for (int i = 0; i < poolN && selectedCount < residualStop; ++i) {
                addUnique(residualOrder[static_cast<std::size_t>(i)]);
            }

            // Fill any overlap-created holes with the best remaining candidates
            // across all three rankings, then deterministic batch order as a
            // final safety fallback. exactN candidates are always produced.
            for (int i = 0; i < poolN && selectedCount < exactN; ++i) {
                addUnique(indices[static_cast<std::size_t>(i)]);
                if (selectedCount < exactN) addUnique(hardOrder[static_cast<std::size_t>(i)]);
                if (selectedCount < exactN) addUnique(residualOrder[static_cast<std::size_t>(i)]);
            }
            for (int i = 0; i < count && selectedCount < exactN; ++i) addUnique(i);

            // The scout-record display should still represent the best pure P5
            // penalty score, not whichever portfolio candidate is first later.
            const int batchBest = indices[0];
'@.TrimEnd()
$text = $text.Remove($selectStart, $selectEnd - $selectStart).Insert($selectStart, $newSelection)

# Exact-check the portfolio rather than the old single-ranked indices list.
$exactIdxOld = '                const int idx = indices[static_cast<std::size_t>(rank)];'
$exactIdxPos = $text.IndexOf($exactIdxOld, $selectStart + $newSelection.Length)
if ($exactIdxPos -lt 0) {
    throw 'Could not locate exact finalist index read.'
}
$exactIdxNew = '                const int idx = selectedIndices[static_cast<std::size_t>(rank)];'
$text = $text.Remove($exactIdxPos, $exactIdxOld.Length).Insert($exactIdxPos, $exactIdxNew)

# Update startup text and defaults. With TopExact=24, P6 retains sixteen pure
# P5 candidates plus four hard-count and four residual candidates per batch.
$text = $text.Replace(
    'Scout P5: full 64-point tail4 via 32 lanes + soft sea-level land penalty; exact finalists unchanged.',
    'Scout P6: P5 full64 + 16 penalty / 4 hard-count / 4 residual finalists; exact metric unchanged.'
)
$text = $text.Replace('int topExact = 16;', 'int topExact = 24;')
$text = $text.Replace('(default 16)', '(default 24)')

[System.IO.File]::WriteAllText(
    $sourcePath,
    $text,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied TU4 Water P6 diverse finalist portfolio.' -ForegroundColor Green
Write-Host 'GPU scout is unchanged from P5; only CPU finalist selection changed.'
Write-Host 'Default TopExact: 24 = 16 soft-penalty + 4 hard-count + 4 residual slots.'
Write-Host 'The current P5 search path is preserved instead of sacrificing its 16 finalist slots.'
Write-Host 'Exact 864x864 land measurement remains authoritative.'
