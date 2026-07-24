public class SearchSettings {
    public int chunkRadius = 7;
    public long seedsToCheck = 1_000_000;
    /**
     * The GPU backend is the throughput bottleneck. The 670M/31.9M research runs
     * showed that 10 Java exact workers keep the survivor queue empty, while 19
     * workers were about 2.9% slower from extra CPU/cache contention.
     */
    public int threads = Math.max(1, Math.min(10, Runtime.getRuntime().availableProcessors() - 1));

    /**
     * Use the exact HIP P20 pre-scout when the integrated native worker exists.
     * The engine automatically falls back to the trusted all-Java path if the
     * executable is missing, stale, or the current Stage0 geometry is incompatible.
     */
    public boolean gpuP20Enabled = true;

    /**
     * Use the exact HIP P20 -> Stage-1 -> Stage0/0.5 chain when available.
     * Survivors still rerun the trusted Java stages so Stage0.75 and the lazy
     * coarse path keep the same exact prepared terrain cache.
     */
    public boolean gpuStage0Enabled = true;

    /**
     * The optimized GPU chain gates the expensive 256-column scan behind an exact
     * 64-column P20 kernel. 32768 amortizes Java/native dispatch overhead and keeps
     * the RX 7800 XT fed; stop latency is still only a few seconds at full load.
     */
    public int gpuStage0BatchSize = 32_768;

    /**
     * Number of seeds sent to the persistent GPU worker per dispatch. 32768 is
     * large enough to keep the RX 7800 XT busy without making stop/progress
     * latency annoying.
     */
    public int gpuP20BatchSize = 32_768;

    public int minBlocks = 1000;
    public int minColumns = 100;
    public int minYForMatch = 60;

    public int minWidth = 10;
    public int minDepth = 10;
    public double minAvgThickness = 2.0;

    public int topResultsToKeep = 50;
    public boolean savePreviews = false;

    /**
     * When true, IslandSearchEngine first runs the coarse raw-density component
     * prefilter. This targets large floating islands and can skip smaller valid
     * islands. Turn this OFF for unbiased rarity/distribution runs.
     */
    public boolean hunterMode = false;

    /**
     * P23 Mega mode is an intentionally biased 30k+ hunting profile.
     *
     * General mode keeps the P22 production gates:
     *   Upper >= 5, High >= 5, P19 >= 4.02/extreme bypass, coarse >= 85.
     *
     * Mega mode keeps P20/coarse unchanged but applies the independently
     * validated early filters before expensive coarse generation:
     *   Upper >= 8
     *   High >= 6
     *   score < 5 && (Y96 largest cluster < 4 || FullY112 == 0) => reject
     *   extreme-topology bypass is always preserved.
     *
     * Research evidence before enabling this rule:
     *   257 observed 30k+ islands, 0 killed by the topology union rule.
     */
    public boolean megaMode = false;

    public static final int HUNT_PROFILE_GENERAL = 0;
    public static final int HUNT_PROFILE_MEGA = 1;
    public static final int HUNT_PROFILE_RECORD_60K = 2;
    public static final int HUNT_PROFILE_RECORD_80K = 3;

    /** Selected hunter profile. See the constants above. */
    public int huntProfile = HUNT_PROFILE_MEGA;

    /**
     * Aggressive empirically validated record hunt. It preserves every observed
     * 60k+ island in the bundled 120-record evidence set, but it is not a proof
     * that an unseen 60k+ island cannot be filtered.
     */
    public boolean recordHuntMode = false;

    /**
     * World-record coarse hunt. P38 keeps the proven Record-60 early gates, raises
     * only the high-reentry requirement to 20, and requires coarse >=700. This
     * preserves the compact/thick 113,331-block record that P37's old High>=22
     * profile would have rejected. It remains empirical rather than lossless.
     */
    public boolean extremeRecordHuntMode = false;

    public int record60P20MinPositiveColumns = 3;
    public int record60UpperMinPositiveColumns = 19;
    public int record60HighMinReentryColumns = 12;
    public double record60MinP19Score = 6.70;
    public int record60CoarseMinCells = 95;

    public int record80P20MinPositiveColumns = 3;
    public int record80UpperMinPositiveColumns = 19;
    public int record80HighMinReentryColumns = 20;
    public double record80MinP19Score = 6.70;
    public int record80CoarseMinCells = 700;

    public int megaUpperMinPositiveColumns = 8;
    public int megaHighMinReentryColumns = 6;
    public double megaTopologyMaxP19ScoreExclusive = 5.0;
    public int megaTopologyMinY96LargestCluster = 4;
    public boolean megaTopologyRejectWhenFullY112Zero = true;

    /**
     * Enables the heavy P22/P23 research backend: shadow variants, forced exact
     * audits, native samples, histograms, and backend batch telemetry. Keep this
     * ON while validating filters. Turn it OFF for the lean production hunt.
     */
    public boolean filterResearchEnabled = true;

    /**
     * Minimum coarse floating component cells needed before doing the expensive
     * full block/interpolation scan.
     *
     * Suggested values from your first batch:
     *   180 = safe 20k+ hunt
     *   240 = aggressive 25k+ hunt
     *   280 = monster 30k+ hunt, riskier until tested more
     */
    public int hunterCoarseMinCells = 85;

    /**
     * Stage 0 is a very cheap sparse full-radius density sniff test.
     * It samples vertical coarse columns across the whole search radius and
     * only runs the full coarse component scan if it sees at least a tiny
     * positive -> negative -> positive re-entry pattern.
     */
    public boolean hunterStage0Enabled = true;

    /**
     * Sparse sample stride in coarse lattice coordinates.
     * 4 is the default: radius 7 samples about 16x16 vertical columns instead
     * of the full 61x61 coarse grid. Larger = faster/riskier.
     */
    public int hunterStage0Step = 4;

    /**
     * Minimum sparse re-entry columns required before running the full coarse
     * component score. 1 is intentionally safe for first testing.
     */
    public int hunterStage0MinReentrySamples = 2;
    
    /**
     * Minimum upper coarse Y index for the normal Stage 0 re-entry pattern.
     * 9 is roughly Y72, 10 is roughly Y80, 11 is roughly Y88.
     */
    public int hunterStage0MinUpperYIndex = 9;

    /**
     * Stage 0.5 is a second sparse re-entry test that runs after the normal
     * Stage 0 check but before the full coarse component scan.
     *
     * Dataset result: stage0FullY88 >= 5 kept every 15k+/20k+/25k+/30k+
     * match in the latest run while rejecting most current Stage 0 survivors.
     * Use this for monster hunting, not unbiased rarity/distribution runs.
     */
    public boolean hunterStage0HighEnabled = true;

    /**
     * 11 is roughly Y88. Keep this at 11 for the tested Stage0 Y88 rule.
     */
    public int hunterStage0HighMinUpperYIndex = 11;

    /**
     * Tested sweet spot: Y88 >= 5. Do not casually raise to 6; the dataset had
     * a 25k+ island with exactly 5 Y88 samples.
     */
    public int hunterStage0HighMinReentrySamples = 5;

    /**
     * Stage0 audit samples a tiny fraction of seeds that pass normal Stage0
     * but fail Stage0.5. Those sampled rejects still get full coarse analyzed
     * and written to out/hunter_stage0_audit.csv, but they are still rejected.
     *
     * This is for proving whether Y88 >= 5 is safe without ruining speed.
     */
    public boolean hunterStage0AuditEnabled = true;

    /**
     * Deterministic audit sample mask. 255 means about 1/256 Y88-rejected
     * Stage0 survivors are audited. Use 511 for 1/512 or 1023 for 1/1024 if speed drops.
     */
    public long hunterStage0AuditSampleMask = 255L;

    /**
     * Lightweight sampled timing profiler. Common stages are timed only for
     * attempts selected by performanceProfileSampleMask; rare exact/border/hot
     * scans are timed every time.
     */
    public boolean performanceProfilerEnabled = true;

    /**
     * 1023 means about 1/1024 top-level attempts are timed for common stages.
     * Keep this as 2^n - 1 for the cheapest deterministic sampling check.
     */
    public long performanceProfileSampleMask = 1023L;

    /**
     * Headless benchmark support. When enabled, seed N is generated from N and
     * deterministicSeedSequenceSeed with SplitMix64, so the same attempt always
     * gets the same Minecraft seed regardless of thread scheduling.
     */
    public boolean deterministicSeedMode = false;
    public long deterministicSeedSequenceSeed = 123456789L;

    /**
     * Optional label written into each run manifest. The GUI can ignore this.
     */
    public String runLabel = "";

    /**
     * Feature-science logging is useful during research runs, but it repeats
     * rich coarse analysis and sparse Stage0 analysis for sampled rows. Turn
     * it OFF for long overnight hunting runs when only candidates/leaderboards
     * matter.
     */
    public boolean featureLoggingEnabled = true;

    /**
     * Interval between the large Stage0/coarse debug dumps. Production defaults
     * to one million regions; record modes raise this to five million in the GUI.
     * This prevents multi-hour runs from producing gigabyte event logs.
     */
    public long debugLogInterval = 1_000_000L;

}
