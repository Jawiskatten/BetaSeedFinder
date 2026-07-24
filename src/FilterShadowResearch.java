import beta173.BetaTerrain173;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;

/**
 * Shadow-only research instrumentation for future filter tuning.
 *
 * IMPORTANT: nothing in this class changes a production search decision. It records
 * exact native stage telemetry, shadow rule damage, stratified samples, forced audit
 * outcomes, and low-frequency backend batch timing for offline analysis.
 */
public final class FilterShadowResearch {
    public static final long P21_SAMPLE_MASK = 2047L; // legacy Java feature sample
    public static final int FORCED_AUDIT_RATE_MULTIPLIER = 8; // vs overnight baseline

    private static final int[] P20_MAX_POSITIVES = {1, 2, 3, 4};
    private static final int[] SCOUT_MIN_POSITIVES = {6, 8, 10, 12};
    private static final int[] HIGH_MIN_REENTRY = {6, 7, 8, 10};
    private static final double[] P19_THRESHOLDS = {4.08D, 4.25D, 4.50D, 5.00D, 6.00D};
    private static final int[] COARSE_THRESHOLDS = {100, 120, 140, 154, 160, 180, 240, 280};

    private static final int P20_OFFSET = 0;
    private static final int SCOUT_OFFSET = P20_OFFSET + P20_MAX_POSITIVES.length;
    private static final int HIGH_OFFSET = SCOUT_OFFSET + SCOUT_MIN_POSITIVES.length;
    private static final int P19_OFFSET = HIGH_OFFSET + HIGH_MIN_REENTRY.length;
    private static final int COARSE_OFFSET = P19_OFFSET + P19_THRESHOLDS.length;

    private static final int UPPER_COMBO_OFFSET = COARSE_OFFSET + COARSE_THRESHOLDS.length;
    private static final int UPPER_COMBO_COUNT = 4;
    private static final int HIGH_COMBO_OFFSET = UPPER_COMBO_OFFSET + UPPER_COMBO_COUNT;
    private static final int HIGH_COMBO_COUNT = 4;
    private static final int P19_TOPO_OFFSET = HIGH_COMBO_OFFSET + HIGH_COMBO_COUNT;
    private static final int P19_TOPO_COUNT = 8;
    private static final int COARSE_COMBO_OFFSET = P19_TOPO_OFFSET + P19_TOPO_COUNT;
    private static final int COARSE_COMBO_COUNT = 5;
    private static final int VARIANT_COUNT = COARSE_COMBO_OFFSET + COARSE_COMBO_COUNT;

    public static final long NATIVE_BASE_SAMPLE_MASK = 16_383L; // ~1/16384 all seeds
    private static final long HISTOGRAM_WRITE_INTERVAL = 25_000_000L;

    private final boolean enabled;
    private final Path summaryPath;
    private final Path candidatePath;
    private final Path p21SamplePath;
    private final Path configPath;
    private final Path nativeSamplePath;
    private final Path auditPath;
    private final Path histogramPath;
    private final Path backendBatchPath;

    private final Object candidateLock = new Object();
    private final Object p21SampleLock = new Object();
    private final Object nativeSampleLock = new Object();
    private final Object auditLock = new Object();
    private final Object backendLock = new Object();
    private final StringBuilder nativeSampleBuffer = new StringBuilder(1 << 20);
    private final StringBuilder auditBuffer = new StringBuilder(1 << 20);

    private final AtomicLongArray eligible = new AtomicLongArray(VARIANT_COUNT);
    private final AtomicLongArray wouldReject = new AtomicLongArray(VARIANT_COUNT);
    private final DamageStats[] damage = new DamageStats[VARIANT_COUNT];
    private final AuditDamageStats[] auditDamage = new AuditDamageStats[VARIANT_COUNT];

    private final AtomicLongArray p20Histogram = new AtomicLongArray(65);
    private final AtomicLongArray upperHistogram = new AtomicLongArray(257);
    private final AtomicLongArray highHistogram = new AtomicLongArray(257);
    private final AtomicLongArray p19ScoreHistogram = new AtomicLongArray(65); // <0, 0..15.75 by .25, 16+
    private final AtomicLongArray coarseHistogram = new AtomicLongArray(321); // 0..319, 320+
    private final AtomicLongArray p20UpperJoint = new AtomicLongArray(65 * 257);
    private final AtomicLongArray upperHighJoint = new AtomicLongArray(257 * 257);
    private final AtomicLongArray p19ClusterJoint = new AtomicLongArray(65 * 33);

    private final AtomicLong nativeRowsWritten = new AtomicLong();
    private final AtomicLong auditQueued = new AtomicLong();
    private final AtomicLong auditCompleted = new AtomicLong();
    private final AtomicLong auditDroppedBusy = new AtomicLong();
    private final AtomicLongArray auditQueuedByStage = new AtomicLongArray(8);
    private final AtomicLongArray auditCompletedByStage = new AtomicLongArray(8);
    private final AtomicLong backendBatchCounter = new AtomicLong();
    private final AtomicLong lastHistogramWriteChecked = new AtomicLong(Long.MIN_VALUE / 4);

    private final ConcurrentHashMap<Long, PendingNative> pendingCandidates = new ConcurrentHashMap<>();

    public FilterShadowResearch(Path runOutputDir, boolean enabled) {
        this.enabled = enabled;
        this.summaryPath = runOutputDir.resolve("filter_shadow_summary.csv");
        this.candidatePath = runOutputDir.resolve("filter_shadow_candidates.csv");
        this.p21SamplePath = runOutputDir.resolve("p21_shadow_samples.csv");
        this.configPath = runOutputDir.resolve("filter_shadow_config.txt");
        this.nativeSamplePath = runOutputDir.resolve("filter_research_native_samples.csv");
        this.auditPath = runOutputDir.resolve("filter_research_forced_audits.csv");
        this.histogramPath = runOutputDir.resolve("filter_research_histograms.csv");
        this.backendBatchPath = runOutputDir.resolve("backend_pipeline_batches.csv");
        for (int i = 0; i < VARIANT_COUNT; i++) {
            damage[i] = new DamageStats();
            auditDamage[i] = new AuditDamageStats();
        }
    }

    public boolean isEnabled() { return enabled; }
    public Path getSummaryPath() { return summaryPath; }
    public Path getCandidatePath() { return candidatePath; }
    public Path getP21SamplePath() { return p21SamplePath; }
    public Path getConfigPath() { return configPath; }
    public Path getNativeSamplePath() { return nativeSamplePath; }
    public Path getAuditPath() { return auditPath; }
    public Path getHistogramPath() { return histogramPath; }
    public Path getBackendBatchPath() { return backendBatchPath; }

    public void initialize() throws Exception {
        if (!enabled) return;

        Files.writeString(candidatePath,
                "attempt,seed,p20UpperPositive,p20Y96Plus,p20Y104Plus,p20Y112Plus,p20PositiveAtY88,p20PositiveAtY96,p20HighestPositiveY,p20PositiveDensityCells,p20AvgPositiveDensity,p20MaxPositiveDensity,p20UpperLargestCluster,p20UpperClusterWidth,p20UpperClusterDepth,p20UpperOccupiedRows,p20UpperOccupiedCols,p20UpperQuadrants,p20UpperAdjacentEdges,p20Y96LargestCluster,p20Y96ClusterWidth,p20Y96ClusterDepth,p20Y96OccupiedRows,p20Y96OccupiedCols,p20Y96Quadrants,p20Y96AdjacentEdges,scoutPositiveColumns,highReentryRaw,p19Score,p19ExtremeBypass,coarse,matched,blocks,columns,width,depth,rawFootprintArea,fillPercent,avgThickness,minY,maxY,radiusUsed,shadowRejectMaskHex,shadowRejects,nativeRejectStage,nativeP20,nativeUpper,nativeHigh,nativeP19Score,nativeP19Extreme,nativeMegaTopologyRejected,nativeFullY88,nativeFullY96,nativeFullY104,nativeFullY112,nativeY88Largest,nativeY88Width,nativeY88Depth,nativeY88Border,nativeY96Largest,nativeY96Width,nativeY96Depth,nativeY96Border,nativeCoarse\n",
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        Files.writeString(p21SamplePath,
                "attempt,seed,p20UpperPositive,p20Y96Plus,p20Y104Plus,p20Y112Plus,p20PositiveAtY88,p20PositiveAtY96,p20HighestPositiveY,p20PositiveDensityCells,p20AvgPositiveDensity,p20MaxPositiveDensity,p20UpperLargestCluster,p20UpperClusterWidth,p20UpperClusterDepth,p20UpperOccupiedRows,p20UpperOccupiedCols,p20UpperQuadrants,p20UpperAdjacentEdges,p20Y96LargestCluster,p20Y96ClusterWidth,p20Y96ClusterDepth,p20Y96OccupiedRows,p20Y96OccupiedCols,p20Y96Quadrants,p20Y96AdjacentEdges\n",
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        Files.writeString(nativeSamplePath,
                nativeTelemetryHeader("sampleReason") + "\n",
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        Files.writeString(auditPath,
                nativeTelemetryHeader("auditReason")
                        + ",auditMs,matched,blocks,columns,width,depth,rawFootprintArea,fillPercent,avgThickness,minY,maxY,radiusUsed,shadowRejectMaskHex,shadowRejects\n",
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        Files.writeString(backendBatchPath,
                "epochMs,batchIndex,issuedTotal,completedChecked,completionLag,batchCount,gpuFilterMs,batchSeedsPerSec,queueSize,auditsInFlight,auditQueuedTotal,auditCompletedTotal,auditDroppedBusyTotal,nativeRowsWritten,p20Pass,upperPass,highPass,p19Pass,megaTopologyReject,coarsePass,avgP19Score,maxCoarse\n",
                StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        StringBuilder config = new StringBuilder();
        config.append("P23 filter/backend research build\n");
        config.append("General mode keeps the P22 production decisions. In P23 Mega mode, Upper>=8, High>=6, and the validated topology union are live; all remaining variants and forced audits are shadow-only.\n\n");
        config.append("Native stream telemetry: exact P20/full-upper/high counts, exact P19 score, extreme bypass, 12 exact topology features, exact coarse score.\n");
        config.append("Stratified native samples: base ~1/16384 plus denser near stage boundaries.\n");
        config.append("Forced exact audits: stratified production-reject samples at 8x the overnight baseline rate, capped by audit in-flight slots so research cannot swamp the live search.\n");
        config.append("Backend batches: one row every 32 GPU batches with timing, queue depth, and stage survivor counts.\n");
        config.append("Joint histograms: P20xUpper, UpperxHigh, P19-score x max-cluster, plus 1D stage distributions.\n\n");
        config.append("Existing shadow families: P20<=1,2,3,4; Scout min 6,8,10,12; High min 6,7,8,10; P19 thresholds 4.08,4.25,4.50,5.00,6.00; Coarse thresholds 100,120,140,154,160,180,240,280.\n");
        config.append("New Upper composites: (P20<=1 && Upper<8), (P20<=2 && Upper<8), (P20<=2 && Upper<10), (P20<=3 && Upper<8).\n");
        config.append("New High composites: (High<=5 && Upper<16), (High<=6 && Upper<16), (High<=6 && Upper<24), (High<=7 && Upper<20).\n");
        config.append("P19 topology composites always preserve the extreme-topology bypass.\n");
        config.append("Independent-validation additions: score<5 && Y96Largest<4; score<5 && FullY112==0; score<5 && (Y96Largest<4 || FullY112==0).\n");
        config.append("New Coarse composites: low coarse score combined with weak P19 score/topology.\n");
        config.append("Legacy P21 Java feature sample: ~1/").append(P21_SAMPLE_MASK + 1L).append(" of Java-replayed P20 survivors.\n");
        Files.writeString(configPath, config.toString(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);

        writeSummary(0L);
        writeHistograms();
    }

    /** Exact native stage telemetry for every input seed. Returns the full shadow reject mask. */
    public long recordNativeTelemetry(NativeTelemetry t) {
        if (!enabled || t == null) return 0L;

        p20Histogram.incrementAndGet(clamp(t.p20, 0, 64));
        upperHistogram.incrementAndGet(clamp(t.upper, 0, 256));
        highHistogram.incrementAndGet(clamp(t.high, 0, 256));
        p20UpperJoint.incrementAndGet(clamp(t.p20, 0, 64) * 257 + clamp(t.upper, 0, 256));
        upperHighJoint.incrementAndGet(clamp(t.upper, 0, 256) * 257 + clamp(t.high, 0, 256));
        if (Double.isFinite(t.p19Score)) {
            int scoreBin = p19ScoreBin(t.p19Score);
            p19ScoreHistogram.incrementAndGet(scoreBin);
            p19ClusterJoint.incrementAndGet(scoreBin * 33 + Math.min(32, t.maxCluster()));
        }
        if (t.reachesCoarse()) coarseHistogram.incrementAndGet(Math.min(320, Math.max(0, t.coarse)));

        long mask = 0L;
        if (t.p20 > 0) {
            for (int i = 0; i < P20_MAX_POSITIVES.length; i++) {
                mask = mark(mask, P20_OFFSET + i, t.p20 <= P20_MAX_POSITIVES[i]);
            }
            for (int i = 0; i < SCOUT_MIN_POSITIVES.length; i++) {
                mask = mark(mask, SCOUT_OFFSET + i, t.upper < SCOUT_MIN_POSITIVES[i]);
            }
            mask = mark(mask, UPPER_COMBO_OFFSET, t.p20 <= 1 && t.upper < 8);
            mask = mark(mask, UPPER_COMBO_OFFSET + 1, t.p20 <= 2 && t.upper < 8);
            mask = mark(mask, UPPER_COMBO_OFFSET + 2, t.p20 <= 2 && t.upper < 10);
            mask = mark(mask, UPPER_COMBO_OFFSET + 3, t.p20 <= 3 && t.upper < 8);
        }

        if (t.p20 > 0 && t.upper >= 5) {
            for (int i = 0; i < HIGH_MIN_REENTRY.length; i++) {
                mask = mark(mask, HIGH_OFFSET + i, t.high < HIGH_MIN_REENTRY[i]);
            }
            mask = mark(mask, HIGH_COMBO_OFFSET, t.high <= 5 && t.upper < 16);
            mask = mark(mask, HIGH_COMBO_OFFSET + 1, t.high <= 6 && t.upper < 16);
            mask = mark(mask, HIGH_COMBO_OFFSET + 2, t.high <= 6 && t.upper < 24);
            mask = mark(mask, HIGH_COMBO_OFFSET + 3, t.high <= 7 && t.upper < 20);
        }

        if (t.reachesP19()) {
            for (int i = 0; i < P19_THRESHOLDS.length; i++) {
                mask = mark(mask, P19_OFFSET + i, !t.p19Extreme && t.p19Score < P19_THRESHOLDS[i]);
            }
            int maxCluster = t.maxCluster();
            mask = mark(mask, P19_TOPO_OFFSET,
                    !t.p19Extreme && t.p19Score < 4.25D && maxCluster < 8);
            mask = mark(mask, P19_TOPO_OFFSET + 1,
                    !t.p19Extreme && t.p19Score < 4.50D && maxCluster < 10 && t.fullY104 < 8);
            mask = mark(mask, P19_TOPO_OFFSET + 2,
                    !t.p19Extreme && t.p19Score < 5.00D && maxCluster < 8 && t.fullY112 == 0);
            mask = mark(mask, P19_TOPO_OFFSET + 3,
                    !t.p19Extreme && t.p19Score < 6.00D && t.y96Largest < 4 && t.fullY104 < 4);
            mask = mark(mask, P19_TOPO_OFFSET + 4,
                    !t.p19Extreme && t.fullY96 < 4 && t.y88Largest < 6 && t.p19Score < 6.00D);

            // P23 rules validated on the 670M run plus the independent 31.9M run.
            // These remain shadow-only and deliberately preserve the extreme bypass.
            mask = mark(mask, P19_TOPO_OFFSET + 5,
                    !t.p19Extreme && t.p19Score < 5.00D && t.y96Largest < 4);
            mask = mark(mask, P19_TOPO_OFFSET + 6,
                    !t.p19Extreme && t.p19Score < 5.00D && t.fullY112 == 0);
            mask = mark(mask, P19_TOPO_OFFSET + 7,
                    !t.p19Extreme && t.p19Score < 5.00D
                            && (t.y96Largest < 4 || t.fullY112 == 0));
        }

        if (t.reachesCoarse()) {
            for (int i = 0; i < COARSE_THRESHOLDS.length; i++) {
                mask = mark(mask, COARSE_OFFSET + i, t.coarse < COARSE_THRESHOLDS[i]);
            }
            int maxCluster = t.maxCluster();
            mask = mark(mask, COARSE_COMBO_OFFSET,
                    t.coarse < 100 && t.p19Score < 5.00D && maxCluster < 8);
            mask = mark(mask, COARSE_COMBO_OFFSET + 1,
                    t.coarse < 120 && t.p19Score < 4.50D && maxCluster < 8);
            mask = mark(mask, COARSE_COMBO_OFFSET + 2,
                    t.coarse < 120 && t.fullY104 < 4 && t.y96Largest < 4);
            mask = mark(mask, COARSE_COMBO_OFFSET + 3,
                    t.coarse < 140 && t.p19Score < 4.25D && maxCluster < 6);
            mask = mark(mask, COARSE_COMBO_OFFSET + 4,
                    t.coarse < 160 && t.p19Score < 4.10D && maxCluster < 5);
        }

        String reason = nativeSampleReason(t);
        if (reason != null) appendNativeSample(t, reason);
        return mask;
    }

    public void rememberQueuedCandidate(long attempt, long mask, NativeTelemetry telemetry) {
        if (!enabled || attempt < 0L || telemetry == null) return;
        pendingCandidates.put(attempt, new PendingNative(mask, telemetry));
    }

    public boolean shouldQueueForcedAudit(NativeTelemetry t) {
        if (!enabled || t == null || t.rejectStage <= 0) return false;
        long hash = sampleHash(t.attempt, t.seed, 0xA17D17A5EEDL + t.rejectStage * 0x9E3779B97F4A7C15L);
        switch (t.rejectStage) {
            case 1: // current P20 zero-signal reject: 1/65536 (was 1/524288)
                return (hash & 65_535L) == 0L;
            case 2: // full-upper reject; 8x overnight rates
                if (t.upper == 7) return (hash & 2_047L) == 0L;
                if (t.upper == 6) return (hash & 8_191L) == 0L;
                if (t.upper == 5) return (hash & 16_383L) == 0L;
                if (t.upper == 4) return (hash & 4_095L) == 0L;
                if (t.upper == 3) return (hash & 16_383L) == 0L;
                return (hash & 65_535L) == 0L;
            case 3:
            case 4: // lower/high reentry reject; boundary is high=4
                if (t.high == 5) return (hash & 2_047L) == 0L;
                if (t.high == 4) return (hash & 2_047L) == 0L;
                if (t.high == 3) return (hash & 8_191L) == 0L;
                return (hash & 32_767L) == 0L;
            case 5: // P19 reject; oversample near 4.02
                if (t.p19Score >= 3.75D) return (hash & 1_023L) == 0L;
                if (t.p19Score >= 3.00D) return (hash & 4_095L) == 0L;
                return (hash & 16_383L) == 0L;
            case 6: // coarse reject; strongly oversample 75..84
                if (t.coarse >= 75) return (hash & 511L) == 0L;
                if (t.coarse >= 60) return (hash & 2_047L) == 0L;
                if (t.coarse >= 40) return (hash & 8_191L) == 0L;
                return (hash & 32_767L) == 0L;
            case 7: // live P23 Mega topology reject; keep independent exact safety audits
                if (t.p19Score >= 4.75D) return (hash & 2_047L) == 0L;
                return (hash & 8_191L) == 0L;
            default:
                return false;
        }
    }

    public void recordAuditQueued(int rejectStage) {
        auditQueued.incrementAndGet();
        if (rejectStage >= 0 && rejectStage < auditQueuedByStage.length()) auditQueuedByStage.incrementAndGet(rejectStage);
    }

    public void recordAuditDroppedBusy() {
        auditDroppedBusy.incrementAndGet();
    }

    public void recordForcedAuditOutcome(
            NativeTelemetry t,
            long shadowMask,
            double auditMs,
            boolean matched,
            int blocks,
            int columns,
            int width,
            int depth,
            double fillPercent,
            double avgThickness,
            int minY,
            int maxY,
            int radiusUsed
    ) {
        if (!enabled || t == null) return;
        auditCompleted.incrementAndGet();
        if (t.rejectStage >= 0 && t.rejectStage < auditCompletedByStage.length()) {
            auditCompletedByStage.incrementAndGet(t.rejectStage);
        }
        for (int i = 0; i < VARIANT_COUNT; i++) {
            if ((shadowMask & bit(i)) != 0L) auditDamage[i].record(matched, blocks);
        }

        int area = width > 0 && depth > 0 ? width * depth : 0;
        StringBuilder line = new StringBuilder(768);
        appendNativeTelemetry(line, t, auditReason(t));
        line.append(',').append(String.format(Locale.ROOT, "%.3f", auditMs))
                .append(',').append(matched)
                .append(',').append(blocks)
                .append(',').append(columns)
                .append(',').append(width)
                .append(',').append(depth)
                .append(',').append(area)
                .append(',').append(String.format(Locale.ROOT, "%.4f", fillPercent))
                .append(',').append(String.format(Locale.ROOT, "%.4f", avgThickness))
                .append(',').append(minY)
                .append(',').append(maxY)
                .append(',').append(radiusUsed)
                .append(',').append(Long.toUnsignedString(shadowMask, 16))
                .append(',').append(quoteCsv(formatRejectNames(shadowMask)))
                .append('\n');
        appendBuffered(auditPath, auditLock, auditBuffer, line.toString());
    }

    public void recordBackendBatch(
            long issuedTotal,
            long completedChecked,
            int batchCount,
            double gpuFilterMs,
            int queueSize,
            int auditsInFlight,
            int p20Pass,
            int upperPass,
            int highPass,
            int p19Pass,
            int megaTopologyReject,
            int coarsePass,
            double avgP19Score,
            int maxCoarse
    ) {
        if (!enabled) return;
        long batchIndex = backendBatchCounter.incrementAndGet();
        if (batchIndex != 1L && (batchIndex & 31L) != 0L) return;
        double rate = gpuFilterMs <= 0.0D ? 0.0D : batchCount * 1000.0D / gpuFilterMs;
        long completionLag = Math.max(0L, issuedTotal - completedChecked);
        String line = System.currentTimeMillis() + "," + batchIndex + "," + issuedTotal + "," + completedChecked + "," + completionLag + "," + batchCount + ","
                + String.format(Locale.ROOT, "%.3f", gpuFilterMs) + ","
                + String.format(Locale.ROOT, "%.1f", rate) + ","
                + queueSize + "," + auditsInFlight + ","
                + auditQueued.get() + "," + auditCompleted.get() + "," + auditDroppedBusy.get() + "," + nativeRowsWritten.get() + ","
                + p20Pass + "," + upperPass + "," + highPass + "," + p19Pass + "," + megaTopologyReject + "," + coarsePass + ","
                + String.format(Locale.ROOT, "%.6f", avgP19Score) + "," + maxCoarse + "\n";
        try {
            synchronized (backendLock) {
                Files.writeString(backendBatchPath, line, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            }
        } catch (Exception ignored) {
        }
    }

    // Legacy Java replay hooks. Native telemetry owns aggregate counts in this build;
    // these methods only preserve masks and legacy P21 sample collection.
    public void recordGpuP20SurvivorCount(int positiveColumns) {
        // no-op: exact native telemetry already counted every seed once
    }

    public long recordP20Survivor(long attempt, long seed, BetaTerrain173.ProgressiveStage0TierFeatures f) {
        if (!enabled || f == null) return 0L;
        if (attempt >= 0L && ((attempt & P21_SAMPLE_MASK) == 0L)) appendP21Sample(attempt, seed, f);
        long mask = 0L;
        for (int i = 0; i < P20_MAX_POSITIVES.length; i++) {
            if (f.upperPositiveColumns <= P20_MAX_POSITIVES[i]) mask |= bit(P20_OFFSET + i);
        }
        return mask;
    }

    public long recordScoutSurvivor(int positiveColumns, long mask) {
        if (!enabled) return mask;
        for (int i = 0; i < SCOUT_MIN_POSITIVES.length; i++) {
            if (positiveColumns < SCOUT_MIN_POSITIVES[i]) mask |= bit(SCOUT_OFFSET + i);
        }
        return mask;
    }

    public long recordHighGateSurvivor(int rawHighReentryCount, long mask) {
        if (!enabled) return mask;
        for (int i = 0; i < HIGH_MIN_REENTRY.length; i++) {
            if (rawHighReentryCount < HIGH_MIN_REENTRY[i]) mask |= bit(HIGH_OFFSET + i);
        }
        return mask;
    }

    public long recordP19Survivor(double score, boolean extremeBypass, long mask) {
        if (!enabled) return mask;
        for (int i = 0; i < P19_THRESHOLDS.length; i++) {
            if (!extremeBypass && score < P19_THRESHOLDS[i]) mask |= bit(P19_OFFSET + i);
        }
        return mask;
    }

    public long recordCurrentCoarseCandidate(int coarseScore, long mask) {
        if (!enabled) return mask;
        for (int i = 0; i < COARSE_THRESHOLDS.length; i++) {
            if (coarseScore < COARSE_THRESHOLDS[i]) mask |= bit(COARSE_OFFSET + i);
        }
        return mask;
    }

    public void recordCandidateOutcome(
            long attempt,
            long seed,
            BetaTerrain173.ProgressiveStage0TierFeatures f,
            int scoutPositiveColumns,
            int highReentryRaw,
            double p19Score,
            boolean p19ExtremeBypass,
            int coarseScore,
            long shadowMask,
            boolean matched,
            int blocks,
            int columns,
            int width,
            int depth,
            double fillPercent,
            double avgThickness,
            int minY,
            int maxY,
            int radiusUsed
    ) {
        if (!enabled) return;
        PendingNative pending = pendingCandidates.remove(attempt);
        if (pending != null) shadowMask |= pending.mask;

        for (int i = 0; i < VARIANT_COUNT; i++) {
            if ((shadowMask & bit(i)) != 0L) damage[i].record(coarseScore, matched, blocks);
        }

        int area = width > 0 && depth > 0 ? width * depth : 0;
        StringBuilder line = new StringBuilder(1400);
        line.append(attempt).append(',').append(seed).append(',');
        appendTierFeatures(line, f);
        line.append(',').append(scoutPositiveColumns)
                .append(',').append(highReentryRaw)
                .append(',').append(Double.isFinite(p19Score) ? String.format(Locale.ROOT, "%.9f", p19Score) : "")
                .append(',').append(p19ExtremeBypass)
                .append(',').append(coarseScore)
                .append(',').append(matched)
                .append(',').append(blocks)
                .append(',').append(columns)
                .append(',').append(width)
                .append(',').append(depth)
                .append(',').append(area)
                .append(',').append(String.format(Locale.ROOT, "%.4f", fillPercent))
                .append(',').append(String.format(Locale.ROOT, "%.4f", avgThickness))
                .append(',').append(minY)
                .append(',').append(maxY)
                .append(',').append(radiusUsed)
                .append(',').append(Long.toUnsignedString(shadowMask, 16))
                .append(',').append(quoteCsv(formatRejectNames(shadowMask)));
        appendPendingNativeColumns(line, pending == null ? null : pending.telemetry);
        line.append('\n');

        try {
            synchronized (candidateLock) {
                Files.writeString(candidatePath, line.toString(), StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            }
        } catch (Exception ignored) {
        }
    }

    public void writeSummary(long checked) {
        if (!enabled) return;
        try {
            StringBuilder sb = new StringBuilder(24_000);
            sb.append("checked,family,variant,stage,eligible,wouldReject,rejectPercent,candidatesKilled,matchesKilled,blocks20kKilled,blocks25kKilled,blocks30kKilled,blocks35kKilled,blocks40kKilled,maxBlocksKilled,maxCoarseKilled,auditSamplesHit,auditMatchesKilled,audit20kKilled,audit30kKilled,audit40kKilled,maxAuditBlocks\n");
            for (int i = 0; i < VARIANT_COUNT; i++) appendSummaryRow(sb, checked, i);
            Files.writeString(summaryPath, sb.toString(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        } catch (Exception ignored) {
        }
        long last = lastHistogramWriteChecked.get();
        if (checked == 0L || checked - last >= HISTOGRAM_WRITE_INTERVAL) {
            if (lastHistogramWriteChecked.compareAndSet(last, checked)) writeHistograms();
        }
    }

    public void writeHistograms() {
        if (!enabled) return;
        try {
            StringBuilder sb = new StringBuilder(8_000_000);
            sb.append("family,x,y,count\n");
            append1DHistogram(sb, "p20", p20Histogram);
            append1DHistogram(sb, "upper", upperHistogram);
            append1DHistogram(sb, "high", highHistogram);
            append1DHistogram(sb, "p19ScoreQuarter", p19ScoreHistogram);
            append1DHistogram(sb, "coarse", coarseHistogram);
            append2DHistogram(sb, "p20_x_upper", p20UpperJoint, 65, 257);
            append2DHistogram(sb, "upper_x_high", upperHighJoint, 257, 257);
            append2DHistogram(sb, "p19ScoreQuarter_x_maxCluster", p19ClusterJoint, 65, 33);
            Files.writeString(histogramPath, sb.toString(), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        } catch (Exception ignored) {
        }
    }

    public void flushResearchBuffers() {
        flushBuffer(nativeSamplePath, nativeSampleLock, nativeSampleBuffer);
        flushBuffer(auditPath, auditLock, auditBuffer);
        writeHistograms();
    }

    public List<String> compactStatusLines() {
        List<String> lines = new ArrayList<>();
        if (!enabled) return lines;
        lines.add(String.format(Locale.ROOT,
                "RESEARCH | nativeRows=%d | audits queued=%d done=%d droppedBusy=%d | pendingCandidates=%d",
                nativeRowsWritten.get(), auditQueued.get(), auditCompleted.get(), auditDroppedBusy.get(), pendingCandidates.size()));
        lines.add(String.format(Locale.ROOT,
                "SHADOW EARLY | P20<=1 %.2f%% | UpperCombo2 %.2f%% | HighCombo2 %.2f%% | P19Union %.2f%% | CoarseCombo2 %.2f%%",
                rejectPercent(P20_OFFSET), rejectPercent(UPPER_COMBO_OFFSET + 1), rejectPercent(HIGH_COMBO_OFFSET + 1),
                rejectPercent(P19_TOPO_OFFSET + 7), rejectPercent(COARSE_COMBO_OFFSET + 1)));
        String danger = formatDangerLine();
        if (danger != null) lines.add(danger);
        return lines;
    }

    private long mark(long mask, int index, boolean reject) {
        eligible.incrementAndGet(index);
        if (!reject) return mask;
        wouldReject.incrementAndGet(index);
        return mask | bit(index);
    }

    private String nativeSampleReason(NativeTelemetry t) {
        long hash = sampleHash(t.attempt, t.seed, 0x51A7E5A17L);
        if ((hash & NATIVE_BASE_SAMPLE_MASK) == 0L) return "base_1of16384";
        if (t.p20 > 0 && t.p20 <= 2 && (hash & 2_047L) == 0L) return "p20_low_boundary";
        if (t.p20 > 0 && t.upper >= 3 && t.upper <= 10 && (hash & 2_047L) == 0L) return "upper_boundary";
        if (t.upper >= 5 && t.high >= 2 && t.high <= 10 && (hash & 1_023L) == 0L) return "high_boundary";
        if (Double.isFinite(t.p19Score) && t.p19Score >= 2.5D && t.p19Score <= 6.5D && (hash & 511L) == 0L) {
            return "p19_boundary";
        }
        if (t.reachesCoarse() && t.coarse < 120 && (hash & 255L) == 0L) return "coarse_low_boundary";
        return null;
    }

    private void appendNativeSample(NativeTelemetry t, String reason) {
        StringBuilder line = new StringBuilder(512);
        appendNativeTelemetry(line, t, reason);
        line.append('\n');
        nativeRowsWritten.incrementAndGet();
        appendBuffered(nativeSamplePath, nativeSampleLock, nativeSampleBuffer, line.toString());
    }

    private static String nativeTelemetryHeader(String reasonColumn) {
        return "attempt,seed," + reasonColumn
                + ",rejectStage,p20,upper,high,p19Pass,p19Score,p19Extreme,megaTopologyRejected,fullY88,fullY96,fullY104,fullY112,y88Largest,y88Width,y88Depth,y88Border,y96Largest,y96Width,y96Depth,y96Border,coarse";
    }

    private static void appendNativeTelemetry(StringBuilder line, NativeTelemetry t, String reason) {
        line.append(t.attempt).append(',').append(t.seed).append(',').append(reason).append(',')
                .append(t.rejectStage).append(',').append(t.p20).append(',').append(t.upper).append(',').append(t.high).append(',')
                .append(t.p19Pass).append(',')
                .append(Double.isFinite(t.p19Score) ? String.format(Locale.ROOT, "%.9f", t.p19Score) : "").append(',')
                .append(t.p19Extreme).append(',').append(t.megaTopologyRejected).append(',')
                .append(t.fullY88).append(',').append(t.fullY96).append(',').append(t.fullY104).append(',').append(t.fullY112).append(',')
                .append(t.y88Largest).append(',').append(t.y88Width).append(',').append(t.y88Depth).append(',').append(t.y88Border).append(',')
                .append(t.y96Largest).append(',').append(t.y96Width).append(',').append(t.y96Depth).append(',').append(t.y96Border).append(',')
                .append(t.coarse);
    }

    private static String auditReason(NativeTelemetry t) {
        switch (t.rejectStage) {
            case 1: return "p20_zero";
            case 2: return "upper_reject_" + t.upper;
            case 3: return "low_reentry_reject_" + t.high;
            case 4: return "high_reentry_reject_" + t.high;
            case 5: return "p19_reject";
            case 6: return "coarse_reject_" + t.coarse;
            case 7: return "mega_topology_reject";
            default: return "unknown";
        }
    }

    private void appendPendingNativeColumns(StringBuilder line, NativeTelemetry t) {
        if (t == null) {
            for (int i = 0; i < 20; i++) line.append(',');
            return;
        }
        line.append(',').append(t.rejectStage)
                .append(',').append(t.p20)
                .append(',').append(t.upper)
                .append(',').append(t.high)
                .append(',').append(Double.isFinite(t.p19Score) ? String.format(Locale.ROOT, "%.9f", t.p19Score) : "")
                .append(',').append(t.p19Extreme)
                .append(',').append(t.megaTopologyRejected)
                .append(',').append(t.fullY88)
                .append(',').append(t.fullY96)
                .append(',').append(t.fullY104)
                .append(',').append(t.fullY112)
                .append(',').append(t.y88Largest)
                .append(',').append(t.y88Width)
                .append(',').append(t.y88Depth)
                .append(',').append(t.y88Border)
                .append(',').append(t.y96Largest)
                .append(',').append(t.y96Width)
                .append(',').append(t.y96Depth)
                .append(',').append(t.y96Border)
                .append(',').append(t.coarse);
    }

    private void appendSummaryRow(StringBuilder sb, long checked, int index) {
        DamageStats d = damage[index];
        AuditDamageStats a = auditDamage[index];
        long e = eligible.get(index);
        long r = wouldReject.get(index);
        sb.append(checked).append(',')
                .append(variantFamily(index)).append(',')
                .append(variantName(index)).append(',')
                .append(variantStage(index)).append(',')
                .append(e).append(',').append(r).append(',')
                .append(String.format(Locale.ROOT, "%.6f", percent(r, e))).append(',')
                .append(d.candidatesKilled.get()).append(',').append(d.matchesKilled.get()).append(',')
                .append(d.blocks20kKilled.get()).append(',').append(d.blocks25kKilled.get()).append(',')
                .append(d.blocks30kKilled.get()).append(',').append(d.blocks35kKilled.get()).append(',')
                .append(d.blocks40kKilled.get()).append(',').append(d.maxBlocksKilled.get()).append(',')
                .append(d.maxCoarseKilled.get()).append(',')
                .append(a.samplesHit.get()).append(',').append(a.matchesKilled.get()).append(',')
                .append(a.blocks20kKilled.get()).append(',').append(a.blocks30kKilled.get()).append(',')
                .append(a.blocks40kKilled.get()).append(',').append(a.maxBlocksKilled.get()).append('\n');
    }

    private static String variantFamily(int i) {
        if (i < SCOUT_OFFSET) return "P20";
        if (i < HIGH_OFFSET) return "Scout";
        if (i < P19_OFFSET) return "Stage0.5";
        if (i < COARSE_OFFSET) return "P19";
        if (i < UPPER_COMBO_OFFSET) return "Coarse";
        if (i < HIGH_COMBO_OFFSET) return "UpperComposite";
        if (i < P19_TOPO_OFFSET) return "HighComposite";
        if (i < COARSE_COMBO_OFFSET) return "P19Topology";
        return "CoarseComposite";
    }

    private static String variantStage(int i) {
        if (i < SCOUT_OFFSET) return "after64";
        if (i < HIGH_OFFSET) return "after256";
        if (i < P19_OFFSET) return "afterLowerCompletion";
        if (i < COARSE_OFFSET) return "beforeCoarse";
        if (i < UPPER_COMBO_OFFSET) return "beforeExact";
        if (i < HIGH_COMBO_OFFSET) return "after256";
        if (i < P19_TOPO_OFFSET) return "afterLowerCompletion";
        if (i < COARSE_COMBO_OFFSET) return "beforeCoarse";
        return "beforeExact";
    }

    private static String variantName(int i) {
        if (i >= P20_OFFSET && i < SCOUT_OFFSET) return "upperPositive<=" + P20_MAX_POSITIVES[i - P20_OFFSET];
        if (i >= SCOUT_OFFSET && i < HIGH_OFFSET) return "minPositive=" + SCOUT_MIN_POSITIVES[i - SCOUT_OFFSET];
        if (i >= HIGH_OFFSET && i < P19_OFFSET) return "minReentry=" + HIGH_MIN_REENTRY[i - HIGH_OFFSET];
        if (i >= P19_OFFSET && i < COARSE_OFFSET) return "threshold=" + formatThreshold(P19_THRESHOLDS[i - P19_OFFSET]);
        if (i >= COARSE_OFFSET && i < UPPER_COMBO_OFFSET) return "threshold=" + COARSE_THRESHOLDS[i - COARSE_OFFSET];
        if (i >= UPPER_COMBO_OFFSET && i < HIGH_COMBO_OFFSET) {
            return new String[]{"P20<=1_and_Upper<8", "P20<=2_and_Upper<8", "P20<=2_and_Upper<10", "P20<=3_and_Upper<8"}[i - UPPER_COMBO_OFFSET];
        }
        if (i >= HIGH_COMBO_OFFSET && i < P19_TOPO_OFFSET) {
            return new String[]{"High<=5_and_Upper<16", "High<=6_and_Upper<16", "High<=6_and_Upper<24", "High<=7_and_Upper<20"}[i - HIGH_COMBO_OFFSET];
        }
        if (i >= P19_TOPO_OFFSET && i < COARSE_COMBO_OFFSET) {
            return new String[]{
                    "score<4.25_cluster<8",
                    "score<4.50_cluster<10_Y104<8",
                    "score<5_cluster<8_Y112=0",
                    "score<6_Y96cluster<4_Y104<4",
                    "Y96<4_Y88cluster<6_score<6",
                    "score<5_Y96cluster<4",
                    "score<5_Y112=0",
                    "score<5_(Y96cluster<4_OR_Y112=0)"
            }[i - P19_TOPO_OFFSET];
        }
        return new String[]{"coarse<100_score<5_cluster<8", "coarse<120_score<4.5_cluster<8", "coarse<120_Y104<4_Y96cluster<4", "coarse<140_score<4.25_cluster<6", "coarse<160_score<4.1_cluster<5"}[i - COARSE_COMBO_OFFSET];
    }

    private String formatRejectNames(long mask) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < VARIANT_COUNT; i++) {
            if ((mask & bit(i)) == 0L) continue;
            if (sb.length() > 0) sb.append(';');
            sb.append(variantFamily(i)).append(':').append(variantName(i));
        }
        return sb.toString();
    }

    private String formatDangerLine() {
        StringBuilder sb = new StringBuilder("SHADOW DAMAGE");
        int shown = 0;
        for (int i = 0; i < VARIANT_COUNT; i++) {
            long kills30 = damage[i].blocks30kKilled.get();
            long audit30 = auditDamage[i].blocks30kKilled.get();
            if (kills30 <= 0 && audit30 <= 0) continue;
            if (shown++ == 0) sb.append(" | "); else sb.append(" ; ");
            sb.append(variantFamily(i)).append('/').append(variantName(i))
                    .append(" prod30k=").append(kills30)
                    .append(" audit30k=").append(audit30)
                    .append(" max=").append(Math.max(damage[i].maxBlocksKilled.get(), auditDamage[i].maxBlocksKilled.get()));
            if (shown >= 4) break;
        }
        return shown == 0 ? "SHADOW DAMAGE | no 30k+ would-be kills observed in production candidates or forced audits" : sb.toString();
    }

    private double rejectPercent(int index) {
        return percent(wouldReject.get(index), eligible.get(index));
    }

    private void appendP21Sample(long attempt, long seed, BetaTerrain173.ProgressiveStage0TierFeatures f) {
        StringBuilder line = new StringBuilder(512);
        line.append(attempt).append(',').append(seed).append(',');
        appendTierFeatures(line, f);
        line.append('\n');
        try {
            synchronized (p21SampleLock) {
                Files.writeString(p21SamplePath, line.toString(), StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            }
        } catch (Exception ignored) {
        }
    }

    private static void appendTierFeatures(StringBuilder sb, BetaTerrain173.ProgressiveStage0TierFeatures f) {
        if (f == null) {
            // Exactly 24 empty feature columns. The caller already emitted the
            // delimiter before the first field, so 23 internal commas are needed.
            for (int i = 0; i < 23; i++) sb.append(',');
            return;
        }
        sb.append(f.upperPositiveColumns)
                .append(',').append(f.y96PlusColumns)
                .append(',').append(f.y104PlusColumns)
                .append(',').append(f.y112PlusColumns)
                .append(',').append(f.positiveAtY88)
                .append(',').append(f.positiveAtY96)
                .append(',').append(f.highestPositiveYIndex)
                .append(',').append(f.positiveDensityCells)
                .append(',').append(String.format(Locale.ROOT, "%.9f", f.avgPositiveDensity))
                .append(',').append(String.format(Locale.ROOT, "%.9f", f.maxPositiveDensity))
                .append(',').append(f.upperLargestCluster)
                .append(',').append(f.upperClusterWidth)
                .append(',').append(f.upperClusterDepth)
                .append(',').append(f.upperOccupiedRows)
                .append(',').append(f.upperOccupiedCols)
                .append(',').append(f.upperQuadrants)
                .append(',').append(f.upperAdjacentEdges)
                .append(',').append(f.y96LargestCluster)
                .append(',').append(f.y96ClusterWidth)
                .append(',').append(f.y96ClusterDepth)
                .append(',').append(f.y96OccupiedRows)
                .append(',').append(f.y96OccupiedCols)
                .append(',').append(f.y96Quadrants)
                .append(',').append(f.y96AdjacentEdges);
    }

    private static int p19ScoreBin(double score) {
        if (!Double.isFinite(score) || score < 0.0D) return 0;
        int bin = 1 + (int) Math.floor(score * 4.0D);
        return Math.max(1, Math.min(64, bin));
    }

    private static void append1DHistogram(StringBuilder sb, String family, AtomicLongArray hist) {
        for (int x = 0; x < hist.length(); x++) {
            long count = hist.get(x);
            if (count != 0L) sb.append(family).append(',').append(x).append(",,").append(count).append('\n');
        }
    }

    private static void append2DHistogram(StringBuilder sb, String family, AtomicLongArray hist, int xSize, int ySize) {
        for (int x = 0; x < xSize; x++) {
            int base = x * ySize;
            for (int y = 0; y < ySize; y++) {
                long count = hist.get(base + y);
                if (count != 0L) sb.append(family).append(',').append(x).append(',').append(y).append(',').append(count).append('\n');
            }
        }
    }

    private static void appendBuffered(Path path, Object lock, StringBuilder buffer, String line) {
        synchronized (lock) {
            buffer.append(line);
            if (buffer.length() >= 262_144) flushBufferLocked(path, buffer);
        }
    }

    private static void flushBuffer(Path path, Object lock, StringBuilder buffer) {
        synchronized (lock) {
            flushBufferLocked(path, buffer);
        }
    }

    private static void flushBufferLocked(Path path, StringBuilder buffer) {
        if (buffer.length() == 0) return;
        try {
            Files.writeString(path, buffer.toString(), StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            buffer.setLength(0);
        } catch (Exception ignored) {
        }
    }

    private static long sampleHash(long attempt, long seed, long salt) {
        long z = seed ^ Long.rotateLeft(attempt * 0x9E3779B97F4A7C15L, 23) ^ salt;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    private static String formatThreshold(double value) { return String.format(Locale.ROOT, "%.2f", value); }
    private static long bit(int index) { return 1L << index; }
    private static int clamp(int value, int min, int max) { return Math.max(min, Math.min(max, value)); }
    private static double percent(long part, long total) { return total <= 0L ? 0.0D : part * 100.0D / total; }
    private static String quoteCsv(String value) { return '"' + value.replace("\"", "\"\"") + '"'; }

    private static void updateMax(AtomicInteger target, int value) {
        int old;
        do {
            old = target.get();
            if (value <= old) return;
        } while (!target.compareAndSet(old, value));
    }

    public static final class NativeTelemetry {
        public long attempt;
        public long seed;
        public int rejectStage;
        public int p20;
        public int upper;
        public int high;
        public boolean p19Pass;
        public double p19Score;
        public boolean p19Extreme;
        public boolean megaTopologyRejected;
        public int fullY88;
        public int fullY96;
        public int fullY104;
        public int fullY112;
        public int y88Largest;
        public int y88Width;
        public int y88Depth;
        public boolean y88Border;
        public int y96Largest;
        public int y96Width;
        public int y96Depth;
        public boolean y96Border;
        public int coarse;

        public NativeTelemetry() {
            p19Score = Double.NaN;
        }

        public NativeTelemetry(
                long attempt, long seed, int rejectStage, int p20, int upper, int high,
                boolean p19Pass, double p19Score, boolean p19Extreme, boolean megaTopologyRejected,
                int fullY88, int fullY96, int fullY104, int fullY112,
                int y88Largest, int y88Width, int y88Depth, boolean y88Border,
                int y96Largest, int y96Width, int y96Depth, boolean y96Border,
                int coarse
        ) {
            set(attempt, seed, rejectStage, p20, upper, high, p19Pass, p19Score, p19Extreme, megaTopologyRejected,
                    fullY88, fullY96, fullY104, fullY112,
                    y88Largest, y88Width, y88Depth, y88Border,
                    y96Largest, y96Width, y96Depth, y96Border, coarse);
        }

        public NativeTelemetry set(
                long attempt, long seed, int rejectStage, int p20, int upper, int high,
                boolean p19Pass, double p19Score, boolean p19Extreme, boolean megaTopologyRejected,
                int fullY88, int fullY96, int fullY104, int fullY112,
                int y88Largest, int y88Width, int y88Depth, boolean y88Border,
                int y96Largest, int y96Width, int y96Depth, boolean y96Border,
                int coarse
        ) {
            this.attempt = attempt;
            this.seed = seed;
            this.rejectStage = rejectStage;
            this.p20 = p20;
            this.upper = upper;
            this.high = high;
            this.p19Pass = p19Pass;
            this.p19Score = p19Score;
            this.p19Extreme = p19Extreme;
            this.megaTopologyRejected = megaTopologyRejected;
            this.fullY88 = fullY88;
            this.fullY96 = fullY96;
            this.fullY104 = fullY104;
            this.fullY112 = fullY112;
            this.y88Largest = y88Largest;
            this.y88Width = y88Width;
            this.y88Depth = y88Depth;
            this.y88Border = y88Border;
            this.y96Largest = y96Largest;
            this.y96Width = y96Width;
            this.y96Depth = y96Depth;
            this.y96Border = y96Border;
            this.coarse = coarse;
            return this;
        }

        public NativeTelemetry copy() {
            return new NativeTelemetry(attempt, seed, rejectStage, p20, upper, high, p19Pass, p19Score, p19Extreme, megaTopologyRejected,
                    fullY88, fullY96, fullY104, fullY112,
                    y88Largest, y88Width, y88Depth, y88Border,
                    y96Largest, y96Width, y96Depth, y96Border, coarse);
        }

        public boolean reachesP19() { return p20 > 0 && upper >= 5 && high >= 5 && Double.isFinite(p19Score); }
        public boolean reachesCoarse() { return p19Pass && !megaTopologyRejected; }
        public int maxCluster() { return Math.max(y88Largest, y96Largest); }
    }

    private static final class PendingNative {
        final long mask;
        final NativeTelemetry telemetry;
        PendingNative(long mask, NativeTelemetry telemetry) {
            this.mask = mask;
            this.telemetry = telemetry;
        }
    }

    private static final class DamageStats {
        final AtomicLong candidatesKilled = new AtomicLong();
        final AtomicLong matchesKilled = new AtomicLong();
        final AtomicLong blocks20kKilled = new AtomicLong();
        final AtomicLong blocks25kKilled = new AtomicLong();
        final AtomicLong blocks30kKilled = new AtomicLong();
        final AtomicLong blocks35kKilled = new AtomicLong();
        final AtomicLong blocks40kKilled = new AtomicLong();
        final AtomicInteger maxBlocksKilled = new AtomicInteger();
        final AtomicInteger maxCoarseKilled = new AtomicInteger();

        void record(int coarseScore, boolean matched, int blocks) {
            candidatesKilled.incrementAndGet();
            updateMax(maxCoarseKilled, coarseScore);
            if (!matched) return;
            matchesKilled.incrementAndGet();
            updateMax(maxBlocksKilled, blocks);
            if (blocks >= 20_000) blocks20kKilled.incrementAndGet();
            if (blocks >= 25_000) blocks25kKilled.incrementAndGet();
            if (blocks >= 30_000) blocks30kKilled.incrementAndGet();
            if (blocks >= 35_000) blocks35kKilled.incrementAndGet();
            if (blocks >= 40_000) blocks40kKilled.incrementAndGet();
        }
    }

    private static final class AuditDamageStats {
        final AtomicLong samplesHit = new AtomicLong();
        final AtomicLong matchesKilled = new AtomicLong();
        final AtomicLong blocks20kKilled = new AtomicLong();
        final AtomicLong blocks30kKilled = new AtomicLong();
        final AtomicLong blocks40kKilled = new AtomicLong();
        final AtomicInteger maxBlocksKilled = new AtomicInteger();

        void record(boolean matched, int blocks) {
            samplesHit.incrementAndGet();
            if (!matched) return;
            matchesKilled.incrementAndGet();
            updateMax(maxBlocksKilled, blocks);
            if (blocks >= 20_000) blocks20kKilled.incrementAndGet();
            if (blocks >= 30_000) blocks30kKilled.incrementAndGet();
            if (blocks >= 40_000) blocks40kKilled.incrementAndGet();
        }
    }
}
