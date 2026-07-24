import beta173.BetaTerrain173;

import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.StandardCopyOption;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.PriorityQueue;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicLongArray;
import java.util.concurrent.atomic.AtomicReference;
import javax.imageio.ImageIO;

public class IslandSearchEngine {
    private volatile boolean stopRequested = false;
    private ExecutorService executor;
    private volatile GpuStage0Scout gpuStage0Scout;
    private volatile GpuP20Scout gpuP20Scout;
    private volatile Thread supervisorThread;
    private volatile boolean running;
    private final AtomicLong nextProgressEmitNs = new AtomicLong();

    private final Object topLock = new Object();
    private final Object candidateCsvLock = new Object();
    private final Object featureCsvLock = new Object();
    private final Object stage0AuditCsvLock = new Object();
    private final Object recordRejectAuditCsvLock = new Object();
    private final Object eventLogLock = new Object();

    private static final int LEADERBOARD_WRITE_RETRIES = 6;
    private static final long LEADERBOARD_WRITE_RETRY_DELAY_MS = 50L;
    private final AtomicLong nonFatalLeaderboardWriteFailures = new AtomicLong();
    private volatile boolean leaderboardSnapshotsDirty = false;

    private static final String BUILD_ID = "Hunter-GPU-P38-ReliableGUI-RejectEvidence-P19Lean12-SafeCoarse700-P36Parallel64-2026-07-12";
    private static final DateTimeFormatter RUN_TIME_FORMAT = DateTimeFormatter.ofPattern("yyyy-MM-dd_HH-mm-ss");

    private Path runOutputDir;
    private Path candidateCsvPath;
    private Path featureCsvPath;
    private Path stage0AuditCsvPath;
    private Path recordRejectAuditCsvPath;
    private Path topResultsPath;
    private Path legacyTopResultsPath;
    private Path topFilledFootprintPath;
    private Path topRawFootprintPath;
    private Path topSideboardsCsvPath;
    private Path manifestPath;
    private Path performancePath;
    private Path eventLogPath;

    private PerfStats perfStats;
    private FilterShadowResearch shadowResearch;

    private static final int ISLAND_FLOOR_Y = 68;

    // Hunter v2 coarse raw-density prefilter.
    // These values match Beta 1.7.3 terrain's 5x17x5 coarse density lattice per chunk.
    private static final int COARSE_Y_LEVELS = 17;
    private static final int COARSE_STEP_Y = 8;
    private static final int MIN_INTERESTING_COARSE_Y_INDEX = 8; // ~Y64
    private static final int MIN_FLOATING_GAP_STEPS = 1; // 1 coarse step = about 8 blocks
    private static final int P17_LAZY_COARSE_MAX_LOWER_COLUMNS = 512;
    private static final int P17_LAZY_COARSE_MAX_EXPANSION_ROUNDS = 32;
    private static final int P17_LAZY_COARSE_MAX_COMPONENT_QUEUE = 12000;

    // Stage 0 sparse full-radius sniff test.
    // It only samples a few vertical coarse columns and rejects seeds with no
    // sampled positive -> negative -> positive re-entry pattern.
    private static final int DEFAULT_STAGE0_STEP = 4;
    private static final int DEFAULT_STAGE0_MIN_REENTRY_SAMPLES = 1;
    private static final int DEFAULT_STAGE0_MIN_UPPER_Y_INDEX = 9; // ~Y72

    // Stage 0.5 sparse high re-entry test. Dataset result: Y88 >= 5 kept all
    // 15k+/20k+/25k+/30k+ hits in the latest run and rejected most Stage0 passers.
    private static final int DEFAULT_STAGE0_UPPER_MIN_POSITIVE_COLUMNS = 5;
    private static final int DEFAULT_STAGE0_HIGH_MIN_REENTRY_SAMPLES = 5;
    private static final int DEFAULT_STAGE0_HIGH_MIN_UPPER_Y_INDEX = 11; // ~Y88

    // P20 reject audit is intentionally much sparser than the existing Stage0
    // audit because the pre-scout rejects roughly half of all seeds.
    private static final long P20_PROGRESSIVE_SCOUT_AUDIT_MASK = 32767L; // ~1/32768

    // If an exact island touches the search-area side, it might simply be clipped.
    // Expand through sparse checkpoints until the component is fully contained or the
    // hard safety cap is reached. This is independent from the coarse-driven hot scan.
    private static final int[] BORDER_VERIFY_RADII = {10, 13, 16, 20, 24};
    private static final int BORDER_VERIFY_MAX_RADIUS = BORDER_VERIFY_RADII[BORDER_VERIFY_RADII.length - 1];

    // Hot-seed expansion: high coarse scores mean the whole nearby area may be cursed.
    // Keep the normal search radius fast, but exact-scan rare high-score seeds wider.
    private static final boolean HOT_SEED_SCAN_ENABLED = true;
    private static final int HOT_SEED_COARSE_THRESHOLD = 120;
    private static final int HOT_SEED_SCAN_RADIUS = 16;

    // Exceptional preview policy. Preview generation intentionally happens only
    // after a rare result qualifies, because the enhanced renderer reruns one
    // exact scan to reconstruct the actual connected island footprint.
    private static final int PREVIEW_MIN_BLOCKS = 35_000;
    private static final int PREVIEW_MIN_FOOTPRINT_AREA = 9_000;
    private static final int PREVIEW_MIN_COLUMNS = 3_500;
    private static final int PREVIEW_OUTPUT_SIZE = 960;
    private static final int PREVIEW_CONTEXT_MARGIN = 16;
    private static final int PREVIEW_MIN_CROP_SIZE = 96;

    // Feature logger: collect cheap/sampled signals that might predict high coarse scores.
    // It logs all coarse>=50 rows, plus a small deterministic sample of low-coarse rows.
    private static final int FEATURE_LOG_MIN_COARSE_ALWAYS = 30;
    private static final long FEATURE_LOG_LOW_COARSE_SAMPLE_MASK = 255L; // 1/256 low-score sample

    private static final int[][] COARSE_DIRS_3D = {
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1}
    };

    private PriorityQueue<ResultRecord> topResults;
    private PriorityQueue<ResultRecord> topFilledFootprintResults;
    private PriorityQueue<ResultRecord> topRawFootprintResults;
    private Set<String> seenMatchKeys;
    private volatile boolean p28CoverageActive;

    public synchronized void start(SearchSettings settings, SearchListener listener) {
        if (running) {
            throw new IllegalStateException("Search is already running");
        }
        stopRequested = false;
        running = true;
        nextProgressEmitNs.set(0L);

        Thread supervisor = new Thread(() -> {
            try {
                runSearch(settings, listener);
            } catch (Throwable e) {
                listener.onError(e);
            } finally {
                running = false;
            }
        }, "island-search-supervisor");

        supervisor.setDaemon(false);
        supervisorThread = supervisor;
        supervisor.start();
    }

    public boolean isRunning() {
        return running;
    }

    public boolean awaitStopped(long timeoutMillis) throws InterruptedException {
        Thread thread = supervisorThread;
        if (thread == null) return true;
        thread.join(Math.max(0L, timeoutMillis));
        return !thread.isAlive();
    }

    public void stop() {
        stopRequested = true;

        GpuStage0Scout stage0Scout = gpuStage0Scout;
        if (stage0Scout != null) {
            stage0Scout.close();
        }

        GpuP20Scout scout = gpuP20Scout;
        if (scout != null) {
            scout.close();
        }

        if (executor != null) {
            executor.shutdownNow();
        }
    }

    /**
     * Generates the new exact-component preview for an island loaded from run history.
     * This is intentionally an on-demand, presentation-only path: it performs an exact
     * scan at the radius recorded in hunter_candidates.csv, reconstructs the matching
     * component, and saves the PNG inside the original run folder.
     */
    public Path generatePreviewForIsland(IslandRecord island) throws Exception {
        if (island == null) {
            throw new IllegalArgumentException("No island selected");
        }
        if (island.run == null || island.run.folder == null) {
            throw new IllegalArgumentException("Island has no source run folder");
        }

        List<int[]> centers = historicalPreviewCenters(island);
        List<Integer> radii = historicalPreviewRadii(island);
        StringBuilder attempts = new StringBuilder();
        MatchResult closest = null;

        for (int radius : radii) {
            SearchSettings settings = historicalPreviewSettings(island);
            settings.chunkRadius = radius;

            for (int[] center : centers) {
                int searchCenterChunkX = center[0];
                int searchCenterChunkZ = center[1];
                MatchResult match = findFloatingIslandInSeed(
                        island.seed,
                        settings,
                        new Workspace(),
                        new DebugStats(),
                        false,
                        false,
                        -1L,
                        false,
                        searchCenterChunkX,
                        searchCenterChunkZ
                );

                if (attempts.length() > 0) attempts.append("; ");
                attempts.append("chunk(")
                        .append(searchCenterChunkX).append(',').append(searchCenterChunkZ)
                        .append(") r").append(radius);

                if (match == null) {
                    attempts.append(" -> none");
                    continue;
                }

                attempts.append(" -> ").append(match.component.blocks)
                        .append(" blocks / ").append(match.component.columns).append(" columns");
                if (closest == null || historicalMatchDistance(match.component, island)
                        < historicalMatchDistance(closest.component, island)) {
                    closest = match;
                }

                if (!matchesHistoricalIsland(match.component, island)) {
                    continue;
                }

                Path output = island.previewPath();
                if (output == null) {
                    throw new IllegalStateException("No preview output path available");
                }
                Files.createDirectories(output.getParent());
                savePreview(
                        island.seed, match.component, output.toString(), settings,
                        searchCenterChunkX, searchCenterChunkZ
                );
                return output;
            }
        }

        String found = closest == null
                ? "no exact floating component"
                : closest.component.blocks + " blocks / " + closest.component.columns
                        + " columns at X/Z " + closest.component.centerX + "," + closest.component.centerZ;
        throw new IllegalStateException(
                "Could not reconstruct the historical island. Expected "
                        + island.blocks + " blocks / " + island.columns + " columns at X/Z "
                        + island.centerX + "," + island.centerZ + "; closest result: " + found
                        + ". Tried: " + attempts
        );
    }

    /**
     * Reconstructs the selected historical component and returns a compact exact
     * column model for the interactive 3D preview. The expensive terrain scan is
     * performed off the Swing event thread by the caller.
     */
    public Island3DData generate3DPreviewForIsland(IslandRecord island) throws Exception {
        if (island == null) throw new IllegalArgumentException("No island selected");

        List<int[]> centers = historicalPreviewCenters(island);
        List<Integer> radii = historicalPreviewRadii(island);
        MatchResult closest = null;

        for (int radius : radii) {
            SearchSettings settings = historicalPreviewSettings(island);
            settings.chunkRadius = radius;

            for (int[] center : centers) {
                MatchResult match = findFloatingIslandInSeed(
                        island.seed,
                        settings,
                        new Workspace(),
                        new DebugStats(),
                        false,
                        false,
                        -1L,
                        false,
                        center[0],
                        center[1]
                );
                if (match == null) continue;
                if (closest == null || historicalMatchDistance(match.component, island)
                        < historicalMatchDistance(closest.component, island)) {
                    closest = match;
                }
                if (!matchesHistoricalIsland(match.component, island)) continue;
                return buildIsland3DData(
                        island.seed, match.component, settings, center[0], center[1]
                );
            }
        }

        String found = closest == null
                ? "no exact floating component"
                : closest.component.blocks + " blocks at X/Z "
                        + closest.component.centerX + "," + closest.component.centerZ;
        throw new IllegalStateException("Could not reconstruct 3D preview; closest result: " + found);
    }

    /** Reconstructs a result from the active GUI search for its live 3D preview. */
    public Island3DData generate3DPreviewForSearchResult(SearchResult result, int preferredRadius) throws Exception {
        if (result == null) throw new IllegalArgumentException("No search result selected");

        List<int[]> centers = new ArrayList<>();
        int[] inferred = inferProductionSearchCenter(result.centerX, result.centerZ);
        addHistoricalPreviewCenter(centers, inferred[0], inferred[1]);
        addHistoricalPreviewCenter(centers, Math.floorDiv(result.centerX, 16), Math.floorDiv(result.centerZ, 16));
        addHistoricalPreviewCenter(centers, 0, 0);

        List<Integer> radii = new ArrayList<>();
        addHistoricalPreviewRadius(radii, preferredRadius);
        addHistoricalPreviewRadius(radii, 7);
        addHistoricalPreviewRadius(radii, 16);

        for (int radius : radii) {
            SearchSettings settings = new SearchSettings();
            settings.chunkRadius = radius;
            settings.minBlocks = 1;
            settings.minColumns = 1;
            settings.minYForMatch = 0;
            settings.minWidth = 1;
            settings.minDepth = 1;
            settings.minAvgThickness = 0.0;
            settings.hunterMode = false;
            settings.savePreviews = false;
            settings.featureLoggingEnabled = false;
            settings.performanceProfilerEnabled = false;

            for (int[] center : centers) {
                MatchResult match = findFloatingIslandInSeed(
                        result.seed,
                        settings,
                        new Workspace(),
                        new DebugStats(),
                        false,
                        false,
                        -1L,
                        false,
                        center[0],
                        center[1]
                );
                if (match == null || !matchesSearchResult(match.component, result)) continue;
                return buildIsland3DData(
                        result.seed, match.component, settings, center[0], center[1]
                );
            }
        }
        throw new IllegalStateException("Could not reconstruct the selected result for 3D preview");
    }

    private boolean matchesSearchResult(Component c, SearchResult result) {
        return c.blocks == result.blocks
                && c.columns == result.columns
                && c.centerX == result.centerX
                && c.centerZ == result.centerZ
                && c.maxWorldX - c.minWorldX + 1 == result.width
                && c.maxWorldZ - c.minWorldZ + 1 == result.depth
                && c.minY == result.minY
                && c.maxY == result.maxY;
    }

    private Island3DData buildIsland3DData(
            long seed,
            Component target,
            SearchSettings settings,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
        PreviewData preview = buildPreviewData(
                seed, target, settings, searchCenterChunkX, searchCenterChunkZ
        );
        if (preview == null) {
            throw new IllegalStateException("Exact island surface could not be reconstructed");
        }

        int searchMinWorldX = (searchCenterChunkX - settings.chunkRadius) * 16;
        int searchMinWorldZ = (searchCenterChunkZ - settings.chunkRadius) * 16;
        int width = target.maxWorldX - target.minWorldX + 1;
        int depth = target.maxWorldZ - target.minWorldZ + 1;
        int[] minY = new int[width * depth];
        int[] maxY = new int[width * depth];
        Arrays.fill(minY, Integer.MAX_VALUE);
        Arrays.fill(maxY, Integer.MIN_VALUE);

        int sourceStartX = target.minWorldX - searchMinWorldX;
        int sourceStartZ = target.minWorldZ - searchMinWorldZ;
        for (int x = 0; x < width; x++) {
            for (int z = 0; z < depth; z++) {
                int sourceIndex = (sourceStartX + x) * preview.sizeZ + (sourceStartZ + z);
                int targetIndex = x * depth + z;
                minY[targetIndex] = preview.minY[sourceIndex];
                maxY[targetIndex] = preview.maxY[sourceIndex];
            }
        }

        return new Island3DData(
                seed,
                width,
                depth,
                target.minWorldX,
                target.minWorldZ,
                target.minY,
                target.maxY,
                target.blocks,
                target.columns,
                minY,
                maxY
        );
    }

    private List<int[]> historicalPreviewCenters(IslandRecord island) {
        List<int[]> centers = new ArrayList<>();

        // The CSV coordinates are authoritative and are valid regardless of the
        // build name. Enumerating P27/P28/... IDs caused P37/P38 previews to scan
        // spawn instead of the saved off-center region.
        if (island.searchCenterChunkX != Integer.MIN_VALUE
                && island.searchCenterChunkZ != Integer.MIN_VALUE) {
            addHistoricalPreviewCenter(centers, island.searchCenterChunkX, island.searchCenterChunkZ);
        }

        int[] inferred = inferProductionSearchCenter(island.centerX, island.centerZ);
        addHistoricalPreviewCenter(centers, inferred[0], inferred[1]);

        // Useful for imported or very old CSVs that lack search-center columns.
        addHistoricalPreviewCenter(
                centers,
                Math.floorDiv(island.centerX, 16),
                Math.floorDiv(island.centerZ, 16)
        );
        addHistoricalPreviewCenter(centers, 0, 0);
        return centers;
    }

    private void addHistoricalPreviewCenter(List<int[]> centers, int chunkX, int chunkZ) {
        for (int[] existing : centers) {
            if (existing[0] == chunkX && existing[1] == chunkZ) return;
        }
        centers.add(new int[]{chunkX, chunkZ});
    }

    private List<Integer> historicalPreviewRadii(IslandRecord island) {
        List<Integer> radii = new ArrayList<>();
        addHistoricalPreviewRadius(radii, island.radius);
        addHistoricalPreviewRadius(radii, manifestInt(island.run, "chunkRadius", 7));
        addHistoricalPreviewRadius(radii, 7);
        addHistoricalPreviewRadius(radii, 16);
        return radii;
    }

    private void addHistoricalPreviewRadius(List<Integer> radii, int radius) {
        if (radius <= 0 || radius > BORDER_VERIFY_MAX_RADIUS) return;
        if (!radii.contains(radius)) radii.add(radius);
    }

    private long historicalMatchDistance(Component c, IslandRecord island) {
        if (c == null) return Long.MAX_VALUE;
        long distance = 0L;
        distance += Math.abs((long) c.blocks - island.blocks) * 1_000_000L;
        distance += Math.abs((long) c.columns - island.columns) * 10_000L;
        distance += Math.abs((long) c.centerX - island.centerX) * 100L;
        distance += Math.abs((long) c.centerZ - island.centerZ) * 100L;
        distance += Math.abs((long) c.minY - island.minY);
        distance += Math.abs((long) c.maxY - island.maxY);
        return distance;
    }

    private SearchSettings historicalPreviewSettings(IslandRecord island) {
        SearchSettings settings = new SearchSettings();
        settings.chunkRadius = island.radius > 0
                ? island.radius
                : manifestInt(island.run, "chunkRadius", 7);
        settings.minBlocks = manifestInt(island.run, "minBlocks", 1);
        settings.minColumns = manifestInt(island.run, "minColumns", 1);
        settings.minYForMatch = manifestInt(island.run, "minYForMatch", 0);
        settings.minWidth = manifestInt(island.run, "minWidth", 1);
        settings.minDepth = manifestInt(island.run, "minDepth", 1);
        settings.minAvgThickness = manifestDouble(island.run, "minAvgThickness", 0.0);

        // Exact-only reconstruction. No hunter filters, logging, profiling or recursive previews.
        settings.hunterMode = false;
        settings.savePreviews = false;
        settings.featureLoggingEnabled = false;
        settings.performanceProfilerEnabled = false;
        return settings;
    }

    private int[] inferProductionSearchCenter(int componentCenterX, int componentCenterZ) {
        int bestChunkX = 0;
        int bestChunkZ = 0;
        long bestDistance = Long.MAX_VALUE;
        for (int i = 0; i < GpuStage0Scout.CENTER_COUNT; i++) {
            int chunkX = GpuStage0Scout.productionCenterChunkX(i);
            int chunkZ = GpuStage0Scout.productionCenterChunkZ(i);
            long dx = componentCenterX - (long) chunkX * 16L;
            long dz = componentCenterZ - (long) chunkZ * 16L;
            long distance = dx * dx + dz * dz;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestChunkX = chunkX;
                bestChunkZ = chunkZ;
            }
        }
        return new int[]{bestChunkX, bestChunkZ};
    }

    private int manifestInt(RunRecord run, String key, int fallback) {
        if (run == null || run.manifest == null) return fallback;
        try {
            return Integer.parseInt(run.manifest.getOrDefault(key, Integer.toString(fallback)).trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private double manifestDouble(RunRecord run, String key, double fallback) {
        if (run == null || run.manifest == null) return fallback;
        try {
            return Double.parseDouble(run.manifest.getOrDefault(key, Double.toString(fallback)).trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private boolean matchesHistoricalIsland(Component c, IslandRecord island) {
        if (c == null) return false;
        int width = c.maxWorldX - c.minWorldX + 1;
        int depth = c.maxWorldZ - c.minWorldZ + 1;
        return c.blocks == island.blocks
                && c.columns == island.columns
                && width == island.width
                && depth == island.depth
                && c.minY == island.minY
                && c.maxY == island.maxY
                && c.centerX == island.centerX
                && c.centerZ == island.centerZ;
    }

    /**
     * Headless one-seed diagnostic used by FilterRegression.
     * It does not write run CSVs and does not require start(...).
     */
    public SeedDiagnostic inspectSeed(long seed, SearchSettings settings) {
        BetaTerrain173 terrain = new BetaTerrain173(seed);
        Workspace workspace = new Workspace();

        BetaTerrain173.SparseReentryStats sparse = terrain.analyzeSparseCoarseReentryAroundZero(
                settings.chunkRadius,
                getStage0Step(settings)
        );

        CoarseFeatureSummary coarse = getCoarseFeatures(terrain, settings.chunkRadius, workspace);

        boolean currentStage0Pass = !isStage0Enabled(settings)
                || sparse.stage0FullY72 >= getStage0MinReentrySamples(settings);
        boolean currentStage0HighPass = !isStage0HighEnabled(settings)
                || sparse.stage0FullY88 >= getEffectiveHighMinSamples(settings);
        boolean currentCoarsePass = !settings.hunterMode
                || coarse.bestCells >= getHunterCoarseThreshold(settings);
        boolean currentPipelinePass = currentStage0Pass && currentStage0HighPass && currentCoarsePass;

        boolean experimentalY96Cluster3Pass = currentStage0Pass
                && currentStage0HighPass
                && sparse.stage0Y96LargestCluster >= 3
                && currentCoarsePass;

        SearchSettings exactSettings = copySettingsWithRadius(settings, settings.chunkRadius);
        exactSettings.hunterMode = false;
        exactSettings.savePreviews = false;
        exactSettings.performanceProfilerEnabled = false;

        MatchResult exact = findFloatingIslandInSeed(
                seed,
                exactSettings,
                new Workspace(),
                new DebugStats(),
                true,
                false,
                -1L
        );

        Component c = exact == null ? null : exact.component;
        return new SeedDiagnostic(
                seed,
                sparse.stage0FullY72,
                sparse.stage0FullY88,
                sparse.stage0FullY96,
                sparse.stage0Y88LargestCluster,
                sparse.stage0Y96LargestCluster,
                coarse.bestCells,
                currentPipelinePass,
                experimentalY96Cluster3Pass,
                c == null ? 0 : c.blocks,
                c == null ? 0 : c.columns,
                c == null ? 0 : c.maxWorldX - c.minWorldX + 1,
                c == null ? 0 : c.maxWorldZ - c.minWorldZ + 1,
                c == null ? 0 : c.minY,
                c == null ? 0 : c.maxY,
                exact == null ? 0 : exact.chunkRadiusUsed,
                c != null && c.touchesSideBorder
        );
    }

    /**
     * Headless exact-only diagnostic from an explicit starting radius. Useful for
     * continuing clipped hot-scan results without recomputing Stage0/coarse research.
     */
    public SeedDiagnostic inspectExactSeed(long seed, SearchSettings settings, int startRadius) {
        SearchSettings exactSettings = copySettingsWithRadius(settings, startRadius);
        exactSettings.hunterMode = false;
        exactSettings.savePreviews = false;
        exactSettings.performanceProfilerEnabled = false;

        MatchResult exact = findFloatingIslandInSeed(
                seed,
                exactSettings,
                new Workspace(),
                new DebugStats(),
                true,
                false,
                -1L,
                false
        );

        Component c = exact == null ? null : exact.component;
        return new SeedDiagnostic(
                seed,
                0, 0, 0, 0, 0, -1,
                true,
                true,
                c == null ? 0 : c.blocks,
                c == null ? 0 : c.columns,
                c == null ? 0 : c.maxWorldX - c.minWorldX + 1,
                c == null ? 0 : c.maxWorldZ - c.minWorldZ + 1,
                c == null ? 0 : c.minY,
                c == null ? 0 : c.maxY,
                exact == null ? 0 : exact.chunkRadiusUsed,
                c != null && c.touchesSideBorder
        );
    }

    /** Headless P17 full-grid vs lazy-relevant coarse diagnostic. */
    public P17CoarseDiagnostic inspectP17Coarse(long seed, int chunkRadius) {
        BetaTerrain173 fullTerrain = new BetaTerrain173(seed);
        BetaTerrain173 lazyTerrain = new BetaTerrain173(seed);
        Workspace fullWorkspace = new Workspace();
        Workspace lazyWorkspace = new Workspace();

        long fullStart = System.nanoTime();
        CoarseGrid full = loadCoarseGridAroundZero(fullTerrain, chunkRadius, fullWorkspace);
        int fullScore = findBestCoarseFloatingCellsFast(full, fullWorkspace);
        long fullNs = System.nanoTime() - fullStart;

        long lazyStart = System.nanoTime();
        CoarseGrid lazy = loadRelevantCoarseGridAroundZero(lazyTerrain, chunkRadius, lazyWorkspace);
        int lazyScore = findBestCoarseFloatingCellsLazy(lazyTerrain, lazy, lazyWorkspace);
        long lazyNs = System.nanoTime() - lazyStart;

        int rawBitMismatches = 0;
        int generatedPoints = 0;
        for (int i = 0; i < lazy.values.length; i++) {
            if (!lazy.filled[i]) continue;
            generatedPoints++;
            if (Double.doubleToRawLongBits(full.values[i]) != Double.doubleToRawLongBits(lazy.values[i])) {
                rawBitMismatches++;
            }
        }

        return new P17CoarseDiagnostic(
                seed,
                fullScore,
                lazyScore,
                rawBitMismatches,
                generatedPoints,
                lazy.lazyLowerColumnsGenerated,
                lazy.lazyExpansionRounds,
                lazy.lazyFallbackToFull,
                fullNs,
                lazyNs
        );
    }

    /**
     * Headless equivalence diagnostic for Patch 3. It generates one coarse grid,
     * scores it with the fast path, then scores the same grid with the rich path.
     */
    public CoarsePathDiagnostic inspectCoarsePaths(long seed, int chunkRadius) {
        BetaTerrain173 terrain = new BetaTerrain173(seed);
        Workspace workspace = new Workspace();
        CoarseGrid grid = loadCoarseGridAroundZero(terrain, chunkRadius, workspace);

        long fastStart = System.nanoTime();
        int fastScore = findBestCoarseFloatingCellsFast(grid, workspace);
        long fastNs = System.nanoTime() - fastStart;

        long richStart = System.nanoTime();
        int richScore = findCoarseFeatureSummary(grid, workspace).bestCells;
        long richNs = System.nanoTime() - richStart;

        return new CoarsePathDiagnostic(seed, fastScore, richScore, fastNs, richNs);
    }

    private void runSearch(SearchSettings settings, SearchListener listener) throws Exception {
        initializeRunOutput(settings);
        initializeCandidateCsv();
        initializeFeatureCsv();
        initializeStage0AuditCsv();
        initializeRecordRejectAuditCsv();

        shadowResearch = new FilterShadowResearch(
                runOutputDir,
                settings.hunterMode && settings.filterResearchEnabled
        );
        shadowResearch.initialize();

        perfStats = new PerfStats(settings.performanceProfilerEnabled, getPerformanceProfileMask(settings));
        SearchListener runListener = new RunLoggingListener(listener);

        topResults = new PriorityQueue<>(this::compareLargestBetter);
        topFilledFootprintResults = new PriorityQueue<>(this::compareFilledFootprintBetter);
        topRawFootprintResults = new PriorityQueue<>(this::compareRawFootprintBetter);
        seenMatchKeys = ConcurrentHashMap.newKeySet();
        p28CoverageActive = settings.gpuStage0Enabled && isGpuStage0Compatible(settings);

        AtomicLong checked = new AtomicLong(0);
        AtomicInteger matches = new AtomicInteger(0);
        AtomicInteger topUpdates = new AtomicInteger(0);
        AtomicReference<Throwable> fatalError = new AtomicReference<>(null);
        DebugStats debugStats = new DebugStats();
        long debugLogInterval = getDebugLogInterval(settings);
        AtomicLong nextDebugLogAt = new AtomicLong(debugLogInterval);

        long startTime = System.currentTimeMillis();
        writeManifest(settings, "RUNNING", 0L, 0, 0, startTime, 0L);

        executor = Executors.newFixedThreadPool(settings.threads);

        runListener.onLog("Search started.");
        runListener.onLog("Build: " + BUILD_ID);
        runListener.onLog("Run folder: " + runOutputDir.toString());
        runListener.onLog("Stage0 fused scan "
                + (settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings) ? "ON" : "OFF")
                + " | one sparse traversal for Y72 + Y88");
        runListener.onLog("Stage0 sparse noise batch ON | 16x16 sampled columns in one strided cached-axis pass");
        runListener.onLog("Active-Y blend noise ON | compiled column masks visit only Y points the terrain blend can use");
        runListener.onLog("Reusable seed state ON | each worker reseeds terrain/climate generators in place instead of rebuilding them");
        runListener.onLog("Stage0.25 progressive pre-scout ON | 64 distributed exact Y88+ columns | zero-signal reject | audit ~1/32768");
        runListener.onLog("Upper-Y Stage0 scout ON | exact Y88+ necessary-condition cascade; P20 survivors complete the remaining 192 columns");
        runListener.onLog("Candidate-column Stage0 completion ON | lower 11 Y levels only for columns with Y88+ positive density");
        runListener.onLog("Stage0.75 monster gate ON | cached Y88/Y96 topology | threshold="
                + String.format(Locale.ROOT, "%.2f", getEffectiveP19MinScore(settings)));
        runListener.onLog("Lazy relevant coarse ON | generate Y64+ globally, then lower Y only along components reachable from upper terrain");
        runListener.onLog("Fast Perlin gradient dispatch ON | exact 16-case switch replaces branch-heavy gradient selection");
        runListener.onLog("Fast simplex climate ON | direct gradient dispatch + cached Z coordinates + precomputed permutation mod 12");
        runListener.onLog("Coarse fast path ON | score-only live analysis, rich features only for logged rows");
        runListener.onLog("Coarse climate stride ON | exact 4-block climate samples, no dense downsample grid");
        runListener.onLog("Coarse noise axis cache ON | cached X/Y/Z Perlin state + first-octave direct write");
        runListener.onLog("Exceptional previews " + (settings.savePreviews ? "ON" : "OFF")
                + " | blocks>=" + PREVIEW_MIN_BLOCKS
                + " OR footprintArea>=" + PREVIEW_MIN_FOOTPRINT_AREA
                + " OR columns>=" + PREVIEW_MIN_COLUMNS
                + " | exact island mask + thickness shading");
        runListener.onLog("Performance profiler " + (settings.performanceProfilerEnabled ? "ON" : "OFF")
                + " | common-stage sample ~1/" + (getPerformanceProfileMask(settings) + 1));
        if (settings.deterministicSeedMode) {
            runListener.onLog("Deterministic seed mode ON | sequenceSeed=" + settings.deterministicSeedSequenceSeed);
        }

        if (settings.hunterMode) {
            runListener.onLog("Hunter profile: " + getHuntProfileName(settings));
            if (settings.megaMode) {
                runListener.onLog("P38 Mega gates LIVE | P20>=1 | Upper>=" + getEffectiveUpperMinSamples(settings)
                        + " | High>=" + getEffectiveHighMinSamples(settings)
                        + " | base P19 + weak-topology reject before coarse | extreme bypass preserved");
            } else if (settings.recordHuntMode) {
                runListener.onLog("P38 RECORD 60k+ LIVE | P20>=" + getEffectiveP20MinSamples(settings)
                        + " | Upper>=" + getEffectiveUpperMinSamples(settings)
                        + " | High>=" + getEffectiveHighMinSamples(settings)
                        + " | P19>=" + String.format(Locale.ROOT, "%.2f", getEffectiveP19MinScore(settings))
                        + "/extreme | coarse>=" + getHunterCoarseThreshold(settings));
                runListener.onLog("Record evidence: 32 unique observed 60k+ islands across 8 runs, zero rejected; empirical risk remains.");
            } else if (settings.extremeRecordHuntMode) {
                runListener.onLog("P38 WORLD RECORD COARSE 700 LIVE | P20>=" + getEffectiveP20MinSamples(settings)
                        + " | Upper>=" + getEffectiveUpperMinSamples(settings)
                        + " | High>=" + getEffectiveHighMinSamples(settings)
                        + " | P19>=" + String.format(Locale.ROOT, "%.2f", getEffectiveP19MinScore(settings))
                        + "/extreme | coarse>=" + getHunterCoarseThreshold(settings));
                runListener.onLog("World-record gate rebuilt after the 113,331 compact-thick record: Record60 early gates + High>=20 + coarse>=700. Empirical risk remains.");
            }
            runListener.onLog("Hunter v2 coarse debug ON | threshold=" + getHunterCoarseThreshold(settings));
            runListener.onLog("Stage 0 sparse filter " + (isStage0Enabled(settings) ? "ON" : "OFF")
                    + " | step=" + getStage0Step(settings)
                    + " | minReentry=" + getStage0MinReentrySamples(settings)
                    + " | minUpperYIndex=" + getStage0MinUpperYIndex(settings));
            runListener.onLog("Stage 0.5 high sparse filter " + (isStage0HighEnabled(settings) ? "ON" : "OFF")
                    + " | step=" + getStage0Step(settings)
                    + " | production minReentry=" + getEffectiveHighMinSamples(settings)
                    + " | minUpperYIndex=" + getStage0HighMinUpperYIndex(settings));
            runListener.onLog("Debug line meaning: checked | stage0.25 pass% | stage0 pass% | stage0.5 pass% | stage0.75 pass% | coarse pass% | fullScans | matches/fullScan | avg/max coarse score");
            runListener.onLog("Distribution bins: 0 | 1-9 | 10-29 | 30-49 | 50-79 | 80-99 | 100-139 | 140-179 | 180-239 | 240-279 | 280+");
            runListener.onLog("Stage0 bins: 0 | 1 | 2-3 | 4-7 | 8-15 | 16+");
            runListener.onLog("Hot seed scan " + (HOT_SEED_SCAN_ENABLED ? "ON" : "OFF")
                    + " | coarse>=" + HOT_SEED_COARSE_THRESHOLD
                    + " -> exact radius " + HOT_SEED_SCAN_RADIUS);
            runListener.onLog("Candidate CSV: " + candidateCsvPath);
            runListener.onLog("Feature CSV: " + featureCsvPath + " | " + (settings.featureLoggingEnabled ? "ON" : "OFF")
                    + " | all coarse>=" + FEATURE_LOG_MIN_COARSE_ALWAYS
                    + " + ~1/" + (FEATURE_LOG_LOW_COARSE_SAMPLE_MASK + 1L) + " lower-score sample");
            runListener.onLog("Stage0 audit CSV: " + stage0AuditCsvPath + " | " + (isStage0AuditEnabled(settings) ? "ON" : "OFF")
                    + " | ~1/" + (getStage0AuditMask(settings) + 1L) + " rejected-seed sample");
            runListener.onLog("Record reject evidence CSV: " + recordRejectAuditCsvPath
                    + " | " + ((settings.recordHuntMode || settings.extremeRecordHuntMode) ? "sparse exact audits ON" : "OFF"));
            runListener.onLog("Overnight filter/backend research " + (shadowResearch != null && shadowResearch.isEnabled() ? "ON" : "OFF")
                    + (settings.megaMode
                        ? " | P37 Mega filters are LIVE; remaining variants stay shadow-only"
                        : settings.recordHuntMode
                            ? " | P37 Record60 filters are LIVE; sparse exact reject audits remain enabled"
                            : settings.extremeRecordHuntMode
                                ? " | P38 safe-coarse700 record filters are LIVE; sparse exact reject audits remain enabled"
                                : " | production decisions unchanged")
                    + (shadowResearch != null && shadowResearch.isEnabled()
                        ? " | summary=" + shadowResearch.getSummaryPath()
                            + " | candidates=" + shadowResearch.getCandidatePath()
                            + " | nativeSamples=" + shadowResearch.getNativeSamplePath()
                            + " | forcedAudits=" + shadowResearch.getAuditPath()
                            + " | histograms=" + shadowResearch.getHistogramPath()
                            + " | backendBatches=" + shadowResearch.getBackendBatchPath()
                            + " | P21 samples=" + shadowResearch.getP21SamplePath()
                        : ""));
            runListener.onLog("Top largest: " + topResultsPath);
            runListener.onLog("Top filled footprint: " + topFilledFootprintPath + " | ranked by occupied X/Z columns");
            runListener.onLog("Top raw footprint: " + topRawFootprintPath + " | ranked by bounding-box area");
            runListener.onLog("Combined sideboards CSV: " + topSideboardsCsvPath);
            runListener.onLog("Non-fatal leaderboard writes ON | locked result files are retried, then skipped without stopping the search");
        } else {
            runListener.onLog("Hunter mode OFF | coarse prefilter disabled");
        }

        boolean gpuStage0Active = tryStartGpuStage0(settings, runListener);
        if (gpuStage0Active) {
            runGpuStage0Pipeline(
                    settings,
                    runListener,
                    checked,
                    matches,
                    topUpdates,
                    fatalError,
                    debugStats,
                    nextDebugLogAt,
                    debugLogInterval,
                    startTime
            );
        } else {
            boolean gpuP20Active = tryStartGpuP20(settings, runListener);
            if (gpuP20Active) {
                runGpuP20Pipeline(
                        settings,
                        runListener,
                        checked,
                        matches,
                        topUpdates,
                        fatalError,
                        debugStats,
                        nextDebugLogAt,
                        debugLogInterval,
                        startTime
                );
            } else {
                runCpuWorkers(
                        settings,
                        runListener,
                        checked,
                        matches,
                        topUpdates,
                        fatalError,
                        debugStats,
                        nextDebugLogAt,
                        debugLogInterval,
                        startTime
                );
            }
        }

        executor.shutdown();
        executor.awaitTermination(7, TimeUnit.DAYS);

        GpuStage0Scout completedStage0Scout = gpuStage0Scout;
        gpuStage0Scout = null;
        if (completedStage0Scout != null) {
            completedStage0Scout.close();
        }

        GpuP20Scout completedScout = gpuP20Scout;
        gpuP20Scout = null;
        if (completedScout != null) {
            completedScout.close();
        }

        Throwable error = fatalError.get();

        if (error != null) {
            if (shadowResearch != null) {
                shadowResearch.writeSummary(Math.min(checked.get(), settings.seedsToCheck));
                shadowResearch.flushResearchBuffers();
            }
            writeManifest(settings, "ERROR", Math.min(checked.get(), settings.seedsToCheck), matches.get(), topUpdates.get(), startTime, System.currentTimeMillis());
            throw new RuntimeException("Search thread failed", error);
        }

        long finalChecked = Math.min(checked.get(), settings.seedsToCheck);
        long endTime = System.currentTimeMillis();

        sendProgress(runListener, finalChecked, matches.get(), topUpdates.get(), startTime);
        logDebugStats(runListener, finalChecked, matches.get(), startTime, debugStats, settings);
        if (shadowResearch != null) {
            shadowResearch.writeSummary(finalChecked);
            shadowResearch.flushResearchBuffers();
        }
        retryDirtyLeaderboardSnapshots(settings);
        writePerformanceReport(runListener, finalChecked, endTime - startTime);

        if (stopRequested) {
            runListener.onLog("Search stopped.");
        } else {
            runListener.onLog("Search finished.");
        }

        writeManifest(settings, stopRequested ? "STOPPED" : "FINISHED", finalChecked, matches.get(), topUpdates.get(), startTime, endTime);
        publishLatestRunCopies();

        runListener.onFinished();
    }

    private boolean tryStartGpuStage0(SearchSettings settings, SearchListener listener) {
        if (!settings.gpuStage0Enabled) {
            p28CoverageActive = false;
            listener.onLog("GPU Stage0 chain OFF | disabled in SearchSettings");
            return false;
        }
        if (!isGpuStage0Compatible(settings)) {
            p28CoverageActive = false;
            listener.onLog(
                    "GPU Stage0 chain unavailable for this configuration | requires hunter radius=7, step=4, Y72>=2, Y88>=5"
            );
            return false;
        }
        if (!GpuStage0Scout.executableExists()) {
            p28CoverageActive = false;
            listener.onLog("GPU Stage0 chain unavailable | executable not found: " + GpuStage0Scout.executablePath());
            return false;
        }

        int batchSize = Math.max(1, settings.gpuStage0BatchSize);
        try {
            gpuStage0Scout = new GpuStage0Scout(batchSize, getGpuProfileArgument(settings), settings.filterResearchEnabled, listener::onLog);
            p28CoverageActive = true;
            listener.onLog(
                    "GPU Stage0+P19+coarse chain ON | profile=" + getHuntProfileName(settings)
                            + " | telemetry=" + (settings.filterResearchEnabled ? "34B research" : "16B lean + exact P19 score")
                            + " | exact P20 -> Stage1 -> Stage0/0.5 -> exact cached Stage0.75 gate"
                             + (settings.megaMode ? " -> Mega topology reject"
                                    : (settings.recordHuntMode || settings.extremeRecordHuntMode)
                                        ? " -> aggressive record gate"
                                        : "")
                            + " -> exact radius-7 coarse score | batch=" + batchSize
                            + " | Java reruns only GPU coarse survivors/audit samples"
            );
            return true;
        } catch (Exception e) {
            gpuStage0Scout = null;
            p28CoverageActive = false;
            listener.onLog("GPU Stage0 startup failed; trying P20-only fallback | " + e.getMessage());
            listener.onLog("WARNING: P38 8-center coverage is OFF in fallback mode; the configured budget is then processed as seed worlds.");
            if (settings.megaMode || settings.recordHuntMode || settings.extremeRecordHuntMode) {
                listener.onLog("WARNING: Hunter-profile decisions remain correct in CPU fallback, but 8-center GPU coverage and record-mode speed are OFF.");
            }
            return false;
        }
    }

    private boolean isGpuStage0Compatible(SearchSettings settings) {
        return settings.hunterMode
                && isStage0Enabled(settings)
                && isStage0HighEnabled(settings)
                && settings.chunkRadius == 7
                && getStage0Step(settings) == 4
                && getStage0MinUpperYIndex(settings) == 9
                && getStage0MinReentrySamples(settings) == 2
                && getStage0HighMinUpperYIndex(settings) == 11
                && getStage0HighMinReentrySamples(settings) == 5;
    }

    private boolean tryStartGpuP20(SearchSettings settings, SearchListener listener) {
        if (!settings.gpuP20Enabled) {
            listener.onLog("GPU P20 prefilter OFF | disabled in SearchSettings");
            return false;
        }
        if (!isGpuP20Compatible(settings)) {
            listener.onLog(
                    "GPU P20 prefilter unavailable for this configuration | requires hunter mode, radius=7, Stage0 step=4, Y88 upper gate"
            );
            return false;
        }
        if (!GpuP20Scout.executableExists()) {
            listener.onLog("GPU P20 prefilter unavailable | executable not found: " + GpuP20Scout.executablePath());
            return false;
        }

        int batchSize = Math.max(1, settings.gpuP20BatchSize);
        try {
            gpuP20Scout = new GpuP20Scout(batchSize, listener::onLog);
            listener.onLog(
                    "GPU P20 prefilter ON | exact 64-column Y88+ scout | batch=" + batchSize
                            + " | CPU pipeline reruns P20 only for GPU survivors"
            );
            return true;
        } catch (Exception e) {
            gpuP20Scout = null;
            listener.onLog("GPU P20 startup failed; falling back to all-Java path | " + e.getMessage());
            return false;
        }
    }

    private boolean isGpuP20Compatible(SearchSettings settings) {
        return settings.hunterMode
                && isStage0Enabled(settings)
                && isStage0HighEnabled(settings)
                && settings.chunkRadius == 7
                && getStage0Step(settings) == 4
                && getStage0HighMinUpperYIndex(settings) == 11;
    }

    private void runCpuWorkers(
            SearchSettings settings,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            AtomicReference<Throwable> fatalError,
            DebugStats debugStats,
            AtomicLong nextDebugLogAt,
            long debugLogInterval,
            long startTime
    ) {
        for (int t = 0; t < settings.threads; t++) {
            executor.execute(() -> {
                Workspace workspace = new Workspace();
                while (!stopRequested) {
                    if (fatalError.get() != null) {
                        break;
                    }

                    long attempt = checked.incrementAndGet();
                    if (attempt > settings.seedsToCheck) {
                        break;
                    }

                    try {
                        long seed = settings.deterministicSeedMode
                                ? deterministicSeedForAttempt(settings.deterministicSeedSequenceSeed, attempt)
                                : ThreadLocalRandom.current().nextLong();

                        maybeReportProgressAndDebug(
                                attempt,
                                settings,
                                runListener,
                                checked,
                                matches,
                                topUpdates,
                                debugStats,
                                nextDebugLogAt,
                                debugLogInterval,
                                startTime
                        );

                        processSeedAttempt(
                                attempt,
                                seed,
                                settings,
                                workspace,
                                debugStats,
                                runListener,
                                checked,
                                matches,
                                topUpdates,
                                startTime
                        );
                    } catch (Throwable e) {
                        fatalError.compareAndSet(null, e);
                        break;
                    }
                }
            });
        }
    }

    private static final class GpuSeedWork {
        final long attempt;
        final long seed;
        final int centerChunkX;
        final int centerChunkZ;
        final int p20Count;
        final int upperCount;
        final int highCount;
        final int stage0Score;
        final int stage0HighScore;
        final int coarseScore;
        final boolean p19Pass;
        final double p19Score;
        final boolean p19Extreme;
        final int rejectStage;
        final boolean leanAudit;
        final boolean researchAudit;
        final FilterShadowResearch.NativeTelemetry researchTelemetry;
        final long researchShadowMask;

        GpuSeedWork(long attempt, long seed) {
            this(attempt, seed, 0, 0, -1, -1, -1, -1, false, Double.NaN, false,
                    0, false, false, null, 0L);
        }

        GpuSeedWork(
                long attempt,
                long seed,
                int centerChunkX,
                int centerChunkZ,
                int p20Count,
                int upperCount,
                int highCount,
                int coarseScore,
                boolean p19Pass,
                double p19Score,
                boolean p19Extreme
        ) {
            this(attempt, seed, centerChunkX, centerChunkZ, p20Count, upperCount, highCount,
                    coarseScore, p19Pass, p19Score, p19Extreme, 0, false, false, null, 0L);
        }

        GpuSeedWork(
                long attempt,
                long seed,
                int centerChunkX,
                int centerChunkZ,
                int p20Count,
                int upperCount,
                int highCount,
                int coarseScore,
                boolean p19Pass,
                double p19Score,
                boolean p19Extreme,
                int rejectStage,
                boolean leanAudit,
                boolean researchAudit,
                FilterShadowResearch.NativeTelemetry researchTelemetry,
                long researchShadowMask
        ) {
            this.attempt = attempt;
            this.seed = seed;
            this.centerChunkX = centerChunkX;
            this.centerChunkZ = centerChunkZ;
            this.p20Count = p20Count;
            this.upperCount = upperCount;
            this.highCount = highCount;
            this.stage0Score = highCount;
            this.stage0HighScore = highCount;
            this.coarseScore = coarseScore;
            this.p19Pass = p19Pass;
            this.p19Score = p19Score;
            this.p19Extreme = p19Extreme;
            this.rejectStage = rejectStage;
            this.leanAudit = leanAudit;
            this.researchAudit = researchAudit;
            this.researchTelemetry = researchTelemetry;
            this.researchShadowMask = researchShadowMask;
        }
    }

    private void runGpuStage0Pipeline(
            SearchSettings settings,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            AtomicReference<Throwable> fatalError,
            DebugStats debugStats,
            AtomicLong nextDebugLogAt,
            long debugLogInterval,
            long startTime
    ) {
        GpuStage0Scout scout = gpuStage0Scout;
        if (scout == null) {
            throw new IllegalStateException("GPU Stage0 pipeline started without a scout process");
        }

        final int batchSize = scout.capacity();
        final int workerCount = Math.max(1, settings.threads);
        final int queueCapacity = Math.max(batchSize, workerCount * 1024);
        final BlockingQueue<GpuSeedWork> survivorQueue = new ArrayBlockingQueue<>(queueCapacity);
        final GpuSeedWork poison = new GpuSeedWork(-1L, 0L);
        final AtomicLong issuedRegions = new AtomicLong(0L);
        final AtomicLong issuedWorlds = new AtomicLong(0L);
        final AtomicLong gpuP20Rejected = new AtomicLong(0L);
        final AtomicLong gpuStage1Rejected = new AtomicLong(0L);
        final AtomicLong gpuStage0Rejected = new AtomicLong(0L);
        final AtomicLong gpuStage05Rejected = new AtomicLong(0L);
        final AtomicLong gpuStage075Rejected = new AtomicLong(0L);
        final AtomicLong gpuMegaTopologyRejected = new AtomicLong(0L);
        final AtomicLong gpuCoarseRejected = new AtomicLong(0L);
        final AtomicLong gpuPassed = new AtomicLong(0L);
        final AtomicLong gpuAuditQueued = new AtomicLong(0L);
        final AtomicInteger researchAuditsInFlight = new AtomicInteger(0);
        final int maxResearchAuditsInFlight = Math.max(2, workerCount * 2);
        long[] seeds = new long[batchSize];
        long gpuFilterNs = 0L;
        long gpuBatches = 0L;
        FilterShadowResearch.NativeTelemetry reusableResearchTelemetry = new FilterShadowResearch.NativeTelemetry();

        runListener.onLog(
                "GPU Stage0+P19+coarse pipeline | persistent native worker | CPU survivor queue=" + queueCapacity
                        + " | workers=" + workerCount
                        + " | profile=" + getHuntProfileName(settings)
                        + " | native P19" + (settings.megaMode ? "+Mega topology" : "")
                        + " rejects before exact GPU coarse; 8 exact regions/world; Java exact-scans only coarse passers"
        );
        runListener.onLog(
                "P38 coverage mode | GUI target counts radius-7 REGIONS | 8 regions/world | "
                        + "spawn plus seven non-overlapping centers at 15-chunk spacing"
        );
        runListener.onLog(
                "P38 exact cache reuse + P36 parallel64 scorer + direct-write noise2/3 + shared-Y compact upper/lower | 66 terrain + 10 climate states/world | cached exact coarse initialization"
        );

        for (int t = 0; t < workerCount; t++) {
            executor.execute(() -> {
                Workspace workspace = new Workspace();
                DebugStats researchAuditStats = new DebugStats();
                SearchSettings researchExactSettings = copySettingsForResearchExactAudit(settings);
                try {
                    while (!stopRequested && fatalError.get() == null) {
                        GpuSeedWork work = survivorQueue.poll(100, TimeUnit.MILLISECONDS);
                        if (work == null) continue;
                        if (work == poison) break;

                        try {
                            if (work.researchAudit) {
                                processResearchAudit(work, researchExactSettings, workspace, researchAuditStats);
                            } else if (work.leanAudit) {
                                processLeanRejectAudit(work, researchExactSettings, workspace, researchAuditStats);
                            } else {
                                processGpuRegionAttempt(
                                        work,
                                        settings,
                                        workspace,
                                        debugStats,
                                        runListener,
                                        checked,
                                        matches,
                                        topUpdates,
                                        startTime
                                );
                            }
                        } catch (Throwable e) {
                            fatalError.compareAndSet(null, e);
                        } finally {
                            if (work.researchAudit) researchAuditsInFlight.decrementAndGet();
                            completeGpuAttempt(
                                    settings,
                                    runListener,
                                    checked,
                                    matches,
                                    topUpdates,
                                    debugStats,
                                    nextDebugLogAt,
                                    debugLogInterval,
                                    startTime
                            );
                        }
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    if (!stopRequested) fatalError.compareAndSet(null, e);
                }
            });
        }

        try {
            while (!stopRequested && fatalError.get() == null) {
                long alreadyIssued = issuedRegions.get();
                if (alreadyIssued >= settings.seedsToCheck) break;

                long remainingRegions = settings.seedsToCheck - alreadyIssued;
                int regionsThisBatch = (int) Math.min(
                        (long) batchSize * scout.centerCount(), remainingRegions
                );
                int count = (regionsThisBatch + scout.centerCount() - 1) / scout.centerCount();
                long worldBase = issuedWorlds.get();
                for (int i = 0; i < count; i++) {
                    long worldAttempt = worldBase + i + 1L;
                    seeds[i] = settings.deterministicSeedMode
                            ? deterministicSeedForAttempt(settings.deterministicSeedSequenceSeed, worldAttempt)
                            : ThreadLocalRandom.current().nextLong();
                }
                issuedWorlds.addAndGet(count);
                issuedRegions.addAndGet(regionsThisBatch);

                long gpuStartNs = System.nanoTime();
                GpuStage0Scout.BatchResult result = scout.filter(seeds, count);
                long batchFilterNs = System.nanoTime() - gpuStartNs;
                gpuFilterNs += batchFilterNs;
                gpuBatches++;

                int batchP20Pass = 0;
                int batchUpperPass = 0;
                int batchHighPass = 0;
                int batchP19Pass = 0;
                int batchMegaTopologyReject = 0;
                int batchCoarsePass = 0;
                int batchP19ScoreCount = 0;
                double batchP19ScoreTotal = 0.0D;
                int batchMaxCoarse = 0;

                for (int regionOffset = 0; regionOffset < regionsThisBatch; regionOffset++) {
                    if (stopRequested || fatalError.get() != null) break;

                    int worldIndex = regionOffset / scout.centerCount();
                    int centerIndex = regionOffset % scout.centerCount();
                    int resultIndex = scout.resultIndex(worldIndex, centerIndex);
                    long attempt = alreadyIssued + regionOffset + 1L;
                    long seed = seeds[worldIndex];
                    int centerChunkX = scout.centerChunkX(centerIndex);
                    int centerChunkZ = scout.centerChunkZ(centerIndex);
                    int p20Count = result.p20Counts[resultIndex] & 0xFF;
                    int fullUpperCount = result.fullUpperCounts[resultIndex] & 0xFF;
                    int highReentryCount = result.highReentryCounts[resultIndex] & 0xFF;
                    boolean gpuP19Pass = result.p19Pass[resultIndex] != 0;
                    boolean gpuMegaTopologyReject = result.megaTopologyRejected[resultIndex] != 0;
                    int gpuCoarseScore = result.coarseScores[resultIndex];
                    double nativeP19Score = result.p19Scores[resultIndex];
                    boolean nativeP19Extreme = result.p19ExtremeBypass[resultIndex] != 0;

                    int rejectStage;
                    if (p20Count < getEffectiveP20MinSamples(settings)) rejectStage = 1;
                    else if (fullUpperCount < getEffectiveUpperMinSamples(settings)) rejectStage = 2;
                    else if (highReentryCount < getStage0MinReentrySamples(settings)) rejectStage = 3;
                    else if (highReentryCount < getEffectiveHighMinSamples(settings)) rejectStage = 4;
                    else if (!gpuP19Pass) rejectStage = 5;
                    else if (gpuMegaTopologyReject) rejectStage = 7;
                    else if (gpuCoarseScore < getHunterCoarseThreshold(settings)) rejectStage = 6;
                    else rejectStage = 0;

                    if (p20Count >= getEffectiveP20MinSamples(settings)) batchP20Pass++;
                    if (p20Count >= getEffectiveP20MinSamples(settings) && fullUpperCount >= getEffectiveUpperMinSamples(settings)) batchUpperPass++;
                    if (p20Count >= getEffectiveP20MinSamples(settings) && fullUpperCount >= getEffectiveUpperMinSamples(settings)
                            && highReentryCount >= getEffectiveHighMinSamples(settings)) batchHighPass++;
                    if (Double.isFinite(nativeP19Score)) {
                        batchP19ScoreTotal += nativeP19Score;
                        batchP19ScoreCount++;
                    }
                    if (gpuP19Pass) batchP19Pass++;
                    if (gpuMegaTopologyReject) batchMegaTopologyReject++;
                    if (gpuP19Pass) batchMaxCoarse = Math.max(batchMaxCoarse, gpuCoarseScore);
                    if (rejectStage == 0) batchCoarsePass++;

                    reusableResearchTelemetry.set(
                            attempt, seed, rejectStage, p20Count, fullUpperCount, highReentryCount,
                            gpuP19Pass, nativeP19Score, nativeP19Extreme, gpuMegaTopologyReject,
                            result.p19FullY88[resultIndex], result.p19FullY96[resultIndex], result.p19FullY104[resultIndex], result.p19FullY112[resultIndex],
                            result.p19Y88LargestCluster[resultIndex], result.p19Y88Width[resultIndex], result.p19Y88Depth[resultIndex], result.p19Y88TouchesBorder[resultIndex] != 0,
                            result.p19Y96LargestCluster[resultIndex], result.p19Y96Width[resultIndex], result.p19Y96Depth[resultIndex], result.p19Y96TouchesBorder[resultIndex] != 0,
                            gpuCoarseScore
                    );
                    long researchShadowMask = shadowResearch != null
                            ? shadowResearch.recordNativeTelemetry(reusableResearchTelemetry)
                            : 0L;

                    boolean researchAudit = shadowResearch != null
                            && shadowResearch.shouldQueueForcedAudit(reusableResearchTelemetry);

                    boolean auditReject;
                    if (rejectStage == 1) {
                        auditReject = isStage0AuditEnabled(settings)
                                && ((attempt & getP20AuditMask(settings)) == 0L);
                    } else if (rejectStage == 7) {
                        // P23 Mega topology rejects use the dedicated exact forced-audit path above.
                        auditReject = false;
                    } else if (rejectStage != 0) {
                        auditReject = shouldAuditStage0Reject(seed, settings);
                    } else {
                        auditReject = false;
                    }

                    if (rejectStage == 0) {
                        gpuPassed.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(highReentryCount);
                        debugStats.stage0Accepted.incrementAndGet();
                        debugStats.recordStage0HighScore(highReentryCount);
                        debugStats.stage0HighAccepted.incrementAndGet();
                        debugStats.stage075Evaluated.incrementAndGet();
                        debugStats.stage075Accepted.incrementAndGet();
                        debugStats.recordCoarseScore(gpuCoarseScore, seed);
                        debugStats.coarseAccepted.incrementAndGet();
                        debugStats.recordAcceptedScore(gpuCoarseScore);
                        if (shadowResearch != null) {
                            shadowResearch.rememberQueuedCandidate(attempt, researchShadowMask, reusableResearchTelemetry.copy());
                        }
                        GpuSeedWork survivor = new GpuSeedWork(
                                attempt, seed, centerChunkX, centerChunkZ,
                                p20Count, fullUpperCount, highReentryCount, gpuCoarseScore,
                                gpuP19Pass, nativeP19Score, nativeP19Extreme
                        );
                        if (!enqueueGpuWork(survivorQueue, survivor, fatalError)) break;
                        continue;
                    }

                    if (researchAudit) {
                        int current = researchAuditsInFlight.incrementAndGet();
                        if (current <= maxResearchAuditsInFlight) {
                            if (shadowResearch != null) shadowResearch.recordAuditQueued(rejectStage);
                            GpuSeedWork auditWork = new GpuSeedWork(
                                    attempt, seed, centerChunkX, centerChunkZ,
                                    p20Count, fullUpperCount, highReentryCount, gpuCoarseScore,
                                    gpuP19Pass, nativeP19Score, nativeP19Extreme,
                                    rejectStage, false, true, reusableResearchTelemetry.copy(), researchShadowMask
                            );
                            if (!enqueueGpuWork(survivorQueue, auditWork, fatalError)) {
                                researchAuditsInFlight.decrementAndGet();
                                break;
                            }
                            continue;
                        }
                        researchAuditsInFlight.decrementAndGet();
                        if (shadowResearch != null) shadowResearch.recordAuditDroppedBusy();
                    }

                    if (auditReject) {
                        gpuAuditQueued.incrementAndGet();
                        GpuSeedWork auditWork = new GpuSeedWork(
                                attempt, seed, centerChunkX, centerChunkZ,
                                p20Count, fullUpperCount, highReentryCount, gpuCoarseScore,
                                gpuP19Pass, nativeP19Score, nativeP19Extreme,
                                rejectStage, true, false, null, 0L
                        );
                        if (!enqueueGpuWork(survivorQueue, auditWork, fatalError)) break;
                        continue;
                    }

                    if (rejectStage == 1) {
                        gpuP20Rejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Rejected.incrementAndGet();
                    } else if (rejectStage == 2) {
                        gpuStage1Rejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutRejected.incrementAndGet();
                    } else if (rejectStage == 3) {
                        gpuStage0Rejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(Math.min(highReentryCount, getStage0MinReentrySamples(settings)));
                        debugStats.stage0Rejected.incrementAndGet();
                    } else if (rejectStage == 4) {
                        gpuStage05Rejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(getStage0MinReentrySamples(settings));
                        debugStats.stage0Accepted.incrementAndGet();
                        debugStats.recordStage0HighScore(Math.min(highReentryCount, getEffectiveHighMinSamples(settings)));
                        debugStats.stage0HighRejected.incrementAndGet();
                    } else if (rejectStage == 5) {
                        gpuStage075Rejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(getStage0MinReentrySamples(settings));
                        debugStats.stage0Accepted.incrementAndGet();
                        debugStats.recordStage0HighScore(getEffectiveHighMinSamples(settings));
                        debugStats.stage0HighAccepted.incrementAndGet();
                        debugStats.stage075Evaluated.incrementAndGet();
                        debugStats.stage075Rejected.incrementAndGet();
                    } else if (rejectStage == 7) {
                        gpuMegaTopologyRejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(getStage0MinReentrySamples(settings));
                        debugStats.stage0Accepted.incrementAndGet();
                        debugStats.recordStage0HighScore(getEffectiveHighMinSamples(settings));
                        debugStats.stage0HighAccepted.incrementAndGet();
                        debugStats.stage075Evaluated.incrementAndGet();
                        debugStats.stage075Accepted.incrementAndGet();
                    } else {
                        gpuCoarseRejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Accepted.incrementAndGet();
                        debugStats.stage0ScoutEvaluated.incrementAndGet();
                        debugStats.stage0ScoutAccepted.incrementAndGet();
                        debugStats.recordStage0Score(getStage0MinReentrySamples(settings));
                        debugStats.stage0Accepted.incrementAndGet();
                        debugStats.recordStage0HighScore(getEffectiveHighMinSamples(settings));
                        debugStats.stage0HighAccepted.incrementAndGet();
                        debugStats.stage075Evaluated.incrementAndGet();
                        debugStats.stage075Accepted.incrementAndGet();
                        debugStats.recordCoarseScore(gpuCoarseScore, seed);
                        debugStats.coarseRejected.incrementAndGet();
                    }

                    completeGpuAttempt(
                            settings,
                            runListener,
                            checked,
                            matches,
                            topUpdates,
                            debugStats,
                            nextDebugLogAt,
                            debugLogInterval,
                            startTime
                    );
                }

                if (shadowResearch != null && shadowResearch.isEnabled()) {
                    shadowResearch.recordBackendBatch(
                            Math.min(issuedRegions.get(), settings.seedsToCheck),
                            Math.min(checked.get(), settings.seedsToCheck),
                            regionsThisBatch,
                            batchFilterNs / 1_000_000.0D,
                            survivorQueue.size(),
                            researchAuditsInFlight.get(),
                            batchP20Pass,
                            batchUpperPass,
                            batchHighPass,
                            batchP19Pass,
                            batchMegaTopologyReject,
                            batchCoarsePass,
                            batchP19ScoreCount == 0 ? 0.0D : batchP19ScoreTotal / batchP19ScoreCount,
                            batchMaxCoarse
                    );
                }
            }
        } catch (Throwable e) {
            if (!stopRequested) fatalError.compareAndSet(null, e);
        } finally {
            if (!stopRequested && fatalError.get() == null) {
                for (int i = 0; i < workerCount; i++) {
                    if (!enqueueGpuWork(survivorQueue, poison, fatalError)) break;
                }
            }

            double gpuSeconds = gpuFilterNs / 1_000_000_000.0;
            double gpuThroughput = gpuSeconds > 0.0 ? issuedRegions.get() / gpuSeconds : 0.0;
            long immediateRejects = gpuP20Rejected.get() + gpuStage1Rejected.get()
                    + gpuStage0Rejected.get() + gpuStage05Rejected.get()
                    + gpuStage075Rejected.get() + gpuMegaTopologyRejected.get() + gpuCoarseRejected.get();
            double survivorPercent = issuedRegions.get() > 0L ? gpuPassed.get() * 100.0 / issuedRegions.get() : 0.0;
            runListener.onLog(
                    "GPU Stage0+coarse dispatch done | regions=" + issuedRegions.get()
                            + " | worlds=" + issuedWorlds.get()
                            + " | batches=" + gpuBatches
                            + " | filterThroughput=" + String.format(Locale.ROOT, "%.1f", gpuThroughput) + " regions/s"
                            + " | P20Rejects=" + gpuP20Rejected.get()
                            + " | Stage1Rejects=" + gpuStage1Rejected.get()
                            + " | Stage0Rejects=" + gpuStage0Rejected.get()
                            + " | Stage0.5Rejects=" + gpuStage05Rejected.get()
                            + " | Stage0.75Rejects=" + gpuStage075Rejected.get()
                            + " | MegaTopologyRejects=" + gpuMegaTopologyRejected.get()
                            + " | CoarseRejects=" + gpuCoarseRejected.get()
                            + " | immediateRejects=" + immediateRejects
                            + " | auditQueued=" + gpuAuditQueued.get()
                            + " | GPU survivors=" + gpuPassed.get()
                            + " (" + String.format(Locale.ROOT, "%.2f%%", survivorPercent) + ")"
            );
        }
    }

    private void runGpuP20Pipeline(
            SearchSettings settings,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            AtomicReference<Throwable> fatalError,
            DebugStats debugStats,
            AtomicLong nextDebugLogAt,
            long debugLogInterval,
            long startTime
    ) {
        GpuP20Scout scout = gpuP20Scout;
        if (scout == null) {
            throw new IllegalStateException("GPU P20 pipeline started without a scout process");
        }

        final int batchSize = scout.capacity();
        final int workerCount = Math.max(1, settings.threads);
        final int queueCapacity = Math.max(batchSize, workerCount * 1024);
        final BlockingQueue<GpuSeedWork> survivorQueue = new ArrayBlockingQueue<>(queueCapacity);
        final GpuSeedWork poison = new GpuSeedWork(-1L, 0L);
        final AtomicLong issued = new AtomicLong(0L);
        final AtomicLong gpuRejected = new AtomicLong(0L);
        final AtomicLong gpuPassed = new AtomicLong(0L);
        long[] seeds = new long[batchSize];
        long[] attempts = new long[batchSize];
        long gpuFilterNs = 0L;
        long gpuBatches = 0L;

        runListener.onLog(
                "GPU P20 pipeline | persistent native worker | CPU survivor queue=" + queueCapacity
                        + " | workers=" + workerCount
                        + " | checked advances when rejects/survivors finish"
        );

        for (int t = 0; t < workerCount; t++) {
            executor.execute(() -> {
                Workspace workspace = new Workspace();
                try {
                    while (!stopRequested && fatalError.get() == null) {
                        GpuSeedWork work = survivorQueue.poll(100, TimeUnit.MILLISECONDS);
                        if (work == null) {
                            continue;
                        }
                        if (work == poison) {
                            break;
                        }

                        try {
                            processSeedAttempt(
                                    work.attempt,
                                    work.seed,
                                    settings,
                                    workspace,
                                    debugStats,
                                    runListener,
                                    checked,
                                    matches,
                                    topUpdates,
                                    startTime
                            );
                        } catch (Throwable e) {
                            fatalError.compareAndSet(null, e);
                        } finally {
                            completeGpuAttempt(
                                    settings,
                                    runListener,
                                    checked,
                                    matches,
                                    topUpdates,
                                    debugStats,
                                    nextDebugLogAt,
                                    debugLogInterval,
                                    startTime
                            );
                        }
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    if (!stopRequested) {
                        fatalError.compareAndSet(null, e);
                    }
                }
            });
        }

        try {
            while (!stopRequested && fatalError.get() == null) {
                long alreadyIssued = issued.get();
                if (alreadyIssued >= settings.seedsToCheck) {
                    break;
                }

                int count = (int) Math.min(batchSize, settings.seedsToCheck - alreadyIssued);
                for (int i = 0; i < count; i++) {
                    long attempt = alreadyIssued + i + 1L;
                    attempts[i] = attempt;
                    seeds[i] = settings.deterministicSeedMode
                            ? deterministicSeedForAttempt(settings.deterministicSeedSequenceSeed, attempt)
                            : ThreadLocalRandom.current().nextLong();
                }
                issued.addAndGet(count);

                long gpuStartNs = System.nanoTime();
                byte[] positiveCounts = scout.filter(seeds, count);
                gpuFilterNs += System.nanoTime() - gpuStartNs;
                gpuBatches++;

                for (int i = 0; i < count; i++) {
                    if (stopRequested || fatalError.get() != null) {
                        break;
                    }

                    long attempt = attempts[i];
                    long seed = seeds[i];
                    boolean gpuPass = (positiveCounts[i] & 0xFF) > 0;
                    boolean auditReject = !gpuPass
                            && isStage0AuditEnabled(settings)
                            && ((attempt & P20_PROGRESSIVE_SCOUT_AUDIT_MASK) == 0L);

                    if (!gpuPass && !auditReject) {
                        gpuRejected.incrementAndGet();
                        debugStats.stage025Evaluated.incrementAndGet();
                        debugStats.stage025Rejected.incrementAndGet();
                        completeGpuAttempt(
                                settings,
                                runListener,
                                checked,
                                matches,
                                topUpdates,
                                debugStats,
                                nextDebugLogAt,
                                debugLogInterval,
                                startTime
                        );
                        continue;
                    }

                    if (gpuPass) {
                        gpuPassed.incrementAndGet();
                    }

                    if (!enqueueGpuWork(survivorQueue, new GpuSeedWork(attempt, seed), fatalError)) {
                        break;
                    }
                }
            }
        } catch (Throwable e) {
            if (!stopRequested) {
                fatalError.compareAndSet(null, e);
            }
        } finally {
            if (!stopRequested && fatalError.get() == null) {
                for (int i = 0; i < workerCount; i++) {
                    if (!enqueueGpuWork(survivorQueue, poison, fatalError)) {
                        break;
                    }
                }
            }

            double gpuSeconds = gpuFilterNs / 1_000_000_000.0;
            double gpuThroughput = gpuSeconds > 0.0 ? issued.get() / gpuSeconds : 0.0;
            double passPercent = issued.get() > 0L ? gpuPassed.get() * 100.0 / issued.get() : 0.0;
            runListener.onLog(
                    "GPU P20 dispatch done | issued=" + issued.get()
                            + " | batches=" + gpuBatches
                            + " | filterThroughput=" + String.format(Locale.ROOT, "%.1f", gpuThroughput) + " seeds/s"
                            + " | immediateRejects=" + gpuRejected.get()
                            + " | GPU survivors=" + gpuPassed.get()
                            + " (" + String.format(Locale.ROOT, "%.2f%%", passPercent) + ")"
            );
        }
    }

    private boolean enqueueGpuWork(
            BlockingQueue<GpuSeedWork> queue,
            GpuSeedWork work,
            AtomicReference<Throwable> fatalError
    ) {
        try {
            while (!stopRequested && fatalError.get() == null) {
                if (queue.offer(work, 100, TimeUnit.MILLISECONDS)) {
                    return true;
                }
            }
            return false;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            if (!stopRequested) {
                fatalError.compareAndSet(null, e);
            }
            return false;
        }
    }

    private void completeGpuAttempt(
            SearchSettings settings,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            DebugStats debugStats,
            AtomicLong nextDebugLogAt,
            long debugLogInterval,
            long startTime
    ) {
        long completed = checked.incrementAndGet();
        maybeReportProgressAndDebug(
                completed,
                settings,
                runListener,
                checked,
                matches,
                topUpdates,
                debugStats,
                nextDebugLogAt,
                debugLogInterval,
                startTime
        );
    }

    private void maybeReportProgressAndDebug(
            long progressValue,
            SearchSettings settings,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            DebugStats debugStats,
            AtomicLong nextDebugLogAt,
            long debugLogInterval,
            long startTime
    ) {
        long currentChecked = Math.min(checked.get(), settings.seedsToCheck);
        long nowNs = System.nanoTime();
        long nextNs = nextProgressEmitNs.get();
        if (currentChecked == settings.seedsToCheck
                || (nowNs >= nextNs && nextProgressEmitNs.compareAndSet(nextNs, nowNs + 250_000_000L))) {
            sendProgress(runListener, currentChecked, matches.get(), topUpdates.get(), startTime);
        }

        long debugAt = nextDebugLogAt.get();
        if (currentChecked >= debugAt && nextDebugLogAt.compareAndSet(debugAt, debugAt + debugLogInterval)) {
            logDebugStats(runListener, currentChecked, matches.get(), startTime, debugStats, settings);
            retryDirtyLeaderboardSnapshots(settings);
        }
    }

    private void processSeedAttempt(
            long attempt,
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            long startTime
    ) throws Exception {
        MatchResult match = findFloatingIslandInSeed(seed, settings, workspace, debugStats, attempt);
        handleResolvedMatch(attempt, seed, settings, match, runListener, checked, matches, topUpdates, startTime);
    }

    private void processGpuRegionAttempt(
            GpuSeedWork work,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            long startTime
    ) throws Exception {
        SearchSettings exactSettings = copySettingsWithRadius(settings, settings.chunkRadius);
        exactSettings.hunterMode = false;
        exactSettings.savePreviews = false;
        exactSettings.featureLoggingEnabled = false;
        exactSettings.performanceProfilerEnabled = settings.performanceProfilerEnabled;

        MatchResult normalMatch = findFloatingIslandInSeed(
                work.seed,
                exactSettings,
                workspace,
                debugStats,
                true,
                false,
                work.attempt,
                false,
                work.centerChunkX,
                work.centerChunkZ
        );
        if (normalMatch != null) {
            normalMatch = new MatchResult(
                    work.seed,
                    normalMatch.component,
                    normalMatch.chunkRadiusUsed,
                    work.stage0Score,
                    work.stage0HighScore,
                    work.coarseScore,
                    work.centerChunkX,
                    work.centerChunkZ
            );
        }

        boolean hotScanRan = false;
        MatchResult hotResult = null;
        MatchResult match = normalMatch;
        if (shouldRunHotSeedScan(settings, work.coarseScore, true)) {
            hotScanRan = true;
            hotResult = runHotSeedScan(
                    work.seed,
                    settings,
                    work.stage0Score,
                    work.stage0HighScore,
                    work.coarseScore,
                    debugStats,
                    work.centerChunkX,
                    work.centerChunkZ
            );
            if (hotResult != null
                    && hotResult.component.touchesSideBorder
                    && hotResult.chunkRadiusUsed < BORDER_VERIFY_MAX_RADIUS) {
                hotResult = verifySideBorderMatch(
                        work.seed,
                        hotResult,
                        settings,
                        debugStats,
                        work.centerChunkX,
                        work.centerChunkZ
                );
            }
            if (hotResult != null
                    && (match == null || compareComponents(hotResult.component, match.component) > 0)) {
                match = hotResult;
            }
        }

        boolean uniqueMatch = handleResolvedMatch(
                work.attempt, work.seed, settings, match,
                runListener, checked, matches, topUpdates, startTime
        );
        boolean duplicateMerged = match != null && !uniqueMatch;

        if (settings.hunterMode) {
            appendCandidateCsv(
                    work.attempt, work.seed, work.stage0Score, work.stage0HighScore, work.coarseScore,
                    work.p20Count, work.upperCount, work.highCount, work.p19Pass, work.p19Score, work.p19Extreme,
                    work.centerChunkX, work.centerChunkZ,
                    normalMatch, match, hotScanRan, hotResult, duplicateMerged
            );
        }
        recordShadowCandidateOutcome(
                work.attempt, work.seed, null, -1, work.stage0HighScore,
                work.p19Score, work.p19Extreme, work.coarseScore, 0L, match
        );
    }

    private boolean handleResolvedMatch(
            long attempt,
            long seed,
            SearchSettings settings,
            MatchResult match,
            SearchListener runListener,
            AtomicLong checked,
            AtomicInteger matches,
            AtomicInteger topUpdates,
            long startTime
    ) throws Exception {
        if (match == null) return false;
        if (!registerUniqueMatch(seed, match.component)) {
            runListener.onLog(
                    "DUPLICATE REGION HIT MERGED | seed " + seed
                            + " | center=(" + match.component.centerX + "," + match.component.centerZ + ")"
                            + " | scanChunk=(" + match.searchCenterChunkX + "," + match.searchCenterChunkZ + ")"
            );
            return false;
        }

        int matchNumber = matches.incrementAndGet();
        runListener.onHit(attempt, new SearchResult(
                seed,
                match.component.blocks,
                match.component.columns,
                match.component.maxWorldX - match.component.minWorldX + 1,
                match.component.maxWorldZ - match.component.minWorldZ + 1,
                match.component.minY,
                match.component.maxY,
                match.component.centerX,
                match.component.centerZ,
                null
        ));

        TopUpdate topUpdate = recordTopResult(seed, match.component, settings, match.chunkRadiusUsed);
        List<SideboardUpdate> sideboardUpdates = recordSideboardResults(seed, match.component, settings, match.chunkRadiusUsed);

        if (settings.savePreviews && shouldSaveExceptionalPreview(match.component)) {
            SearchSettings previewSettings = copySettingsWithRadius(settings, match.chunkRadiusUsed);
            String previewPath = previewPathFor(seed, match.component).toString();
            trySavePreview(
                    seed, match.component, previewPath, previewSettings, runListener,
                    match.searchCenterChunkX, match.searchCenterChunkZ
            );
        }

        for (SideboardUpdate sideboardUpdate : sideboardUpdates) {
            runListener.onLog(
                    "NEW " + sideboardUpdate.boardLabel
                            + " | rank #" + sideboardUpdate.rank
                            + " | seed " + seed
                            + " | blocks=" + sideboardUpdate.record.blocks
                            + " | footprint=" + sideboardUpdate.record.width + "x" + sideboardUpdate.record.depth
                            + " (" + sideboardUpdate.record.footprintArea + ")"
                            + " | fill=" + String.format(Locale.US, "%.2f%%", sideboardUpdate.record.fillPercent)
                            + " | filledColumns=" + sideboardUpdate.record.columns
            );
        }

        if (topUpdate != null) {
            int topUpdateNumber = topUpdates.incrementAndGet();
            runListener.onLog(
                    "NEW TOP RESULT #" + topUpdateNumber
                            + " | rank #" + topUpdate.rank
                            + " | " + topUpdate.record.blocks + " blocks"
                            + " | seed " + seed
                            + " | scanChunk=(" + match.searchCenterChunkX + "," + match.searchCenterChunkZ + ")"
                            + " | r=" + topUpdate.record.chunkRadiusUsed
                            + (topUpdate.record.touchesSideBorder ? " | touchesSide" : "")
                            + (match.stage0Score >= 0 ? " | stage0=" + match.stage0Score : "")
                            + (match.stage0HighScore >= 0 ? " | stage0Y88=" + match.stage0HighScore : "")
                            + (match.coarseScore >= 0 ? " | coarse=" + match.coarseScore : "")
            );
            runListener.onTopResult(topUpdate.record.toSearchResult(), topUpdate.rank);
        }

        if (matchNumber % 10 == 0) {
            sendProgress(
                    runListener,
                    Math.min(checked.get(), settings.seedsToCheck),
                    matches.get(),
                    topUpdates.get(),
                    startTime
            );
        }
        return true;
    }

    private boolean registerUniqueMatch(long seed, Component c) {
        if (seenMatchKeys == null || c == null) return true;
        String key = seed
                + ":" + c.minWorldX + ":" + c.maxWorldX
                + ":" + c.minWorldZ + ":" + c.maxWorldZ
                + ":" + c.minY + ":" + c.maxY
                + ":" + c.blocks + ":" + c.columns;
        return seenMatchKeys.add(key);
    }

    private void processResearchAudit(
            GpuSeedWork work,
            SearchSettings researchExactSettings,
            Workspace workspace,
            DebugStats researchAuditStats
    ) throws Exception {
        if (shadowResearch == null || !shadowResearch.isEnabled() || work.researchTelemetry == null) return;

        long auditStartNs = System.nanoTime();
        MatchResult result = findFloatingIslandInSeed(
                work.seed,
                researchExactSettings,
                workspace,
                researchAuditStats,
                false,
                false,
                -1L,
                false,
                work.centerChunkX,
                work.centerChunkZ
        );

        double auditMs = (System.nanoTime() - auditStartNs) / 1_000_000.0D;
        boolean matched = result != null;
        Component c = matched ? result.component : null;
        int blocks = matched ? c.blocks : 0;
        int columns = matched ? c.columns : 0;
        int width = matched ? c.maxWorldX - c.minWorldX + 1 : 0;
        int depth = matched ? c.maxWorldZ - c.minWorldZ + 1 : 0;
        double fillPercent = matched && width > 0 && depth > 0
                ? columns * 100.0D / (width * (double) depth)
                : 0.0D;
        double avgThickness = matched ? blocks / (double) Math.max(1, columns) : 0.0D;
        int minY = matched ? c.minY : -1;
        int maxY = matched ? c.maxY : -1;
        int radiusUsed = matched ? result.chunkRadiusUsed : researchExactSettings.chunkRadius;

        shadowResearch.recordForcedAuditOutcome(
                work.researchTelemetry,
                work.researchShadowMask,
                auditMs,
                matched,
                blocks,
                columns,
                width,
                depth,
                fillPercent,
                avgThickness,
                minY,
                maxY,
                radiusUsed
        );
    }

    private void processLeanRejectAudit(
            GpuSeedWork work,
            SearchSettings exactSettings,
            Workspace workspace,
            DebugStats auditStats
    ) {
        long auditStartNs = System.nanoTime();
        try {
            MatchResult result = findFloatingIslandInSeed(
                    work.seed,
                    exactSettings,
                    workspace,
                    auditStats,
                    false,
                    false,
                    -1L,
                    false,
                    work.centerChunkX,
                    work.centerChunkZ
            );
            double auditMs = (System.nanoTime() - auditStartNs) / 1_000_000.0D;
            Component c = result == null ? null : result.component;
            int width = c == null ? 0 : c.maxWorldX - c.minWorldX + 1;
            int depth = c == null ? 0 : c.maxWorldZ - c.minWorldZ + 1;
            String line = String.format(
                    Locale.US,
                    "%d,%d,%d,%d,%d,%s,%d,%d,%d,%s,%.6f,%s,%d,%.3f,%s,%d,%d,%d,%d,%d,%d,%d,%s%n",
                    work.attempt,
                    work.seed,
                    work.centerChunkX,
                    work.centerChunkZ,
                    work.rejectStage,
                    rejectStageName(work.rejectStage),
                    work.p20Count,
                    work.upperCount,
                    work.highCount,
                    work.p19Pass,
                    work.p19Score,
                    work.p19Extreme,
                    work.coarseScore,
                    auditMs,
                    result != null,
                    c == null ? 0 : c.blocks,
                    c == null ? 0 : c.columns,
                    width,
                    depth,
                    c == null ? -1 : c.minY,
                    c == null ? -1 : c.maxY,
                    result == null ? exactSettings.chunkRadius : result.chunkRadiusUsed,
                    c != null && c.touchesSideBorder
            );
            synchronized (recordRejectAuditCsvLock) {
                Files.writeString(
                        recordRejectAuditCsvPath,
                        line,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND
                );
            }
        } catch (Exception ignored) {
            // Sparse record-profile audits are evidence only and must never stop a search.
        }
    }

    private static String rejectStageName(int rejectStage) {
        return switch (rejectStage) {
            case 1 -> "p20";
            case 2 -> "upper";
            case 3 -> "reentry";
            case 4 -> "high";
            case 5 -> "p19";
            case 6 -> "coarse";
            case 7 -> "mega_topology";
            default -> "unknown";
        };
    }

    private void sendProgress(
            SearchListener listener,
            long checked,
            int matches,
            int topUpdates,
            long startTime
    ) {
        long elapsed = Math.max(1, System.currentTimeMillis() - startTime);
        double seedsPerSecond = checked / (elapsed / 1000.0);

        listener.onProgress(checked, matches, topUpdates, seedsPerSecond);
    }

    private void logDebugStats(
            SearchListener listener,
            long checked,
            int matches,
            long startTime,
            DebugStats stats,
            SearchSettings settings
    ) {
        long elapsed = Math.max(1, System.currentTimeMillis() - startTime);
        double seedsPerSecond = checked / (elapsed / 1000.0);

        long stage025Evaluated = stats.stage025Evaluated.get();
        long stage025Accepted = stats.stage025Accepted.get();
        long stage025Rejected = stats.stage025Rejected.get();
        long stage0Evaluated = stats.stage0Evaluated.get();
        long stage0Accepted = stats.stage0Accepted.get();
        long stage0Rejected = stats.stage0Rejected.get();
        long stage0HighEvaluated = stats.stage0HighEvaluated.get();
        long stage0HighAccepted = stats.stage0HighAccepted.get();
        long stage0HighRejected = stats.stage0HighRejected.get();
        long stage075Evaluated = stats.stage075Evaluated.get();
        long stage075Accepted = stats.stage075Accepted.get();
        long stage075Rejected = stats.stage075Rejected.get();
        long coarseEvaluated = stats.coarseEvaluated.get();
        long coarseAccepted = stats.coarseAccepted.get();
        long coarseRejected = stats.coarseRejected.get();
        long fullScans = stats.fullScans.get();

        double stage025PassPercent = stage025Evaluated == 0 ? 0.0 : stage025Accepted * 100.0 / stage025Evaluated;
        double stage0PassPercent = stage0Evaluated == 0 ? 0.0 : stage0Accepted * 100.0 / stage0Evaluated;
        double avgStage0Score = stage0Evaluated == 0 ? 0.0 : stats.stage0ScoreTotal.get() / (double) stage0Evaluated;
        double stage0HighPassPercent = stage0HighEvaluated == 0 ? 0.0 : stage0HighAccepted * 100.0 / stage0HighEvaluated;
        double avgStage0HighScore = stage0HighEvaluated == 0 ? 0.0 : stats.stage0HighScoreTotal.get() / (double) stage0HighEvaluated;
        double stage075PassPercent = stage075Evaluated == 0 ? 0.0 : stage075Accepted * 100.0 / stage075Evaluated;
        double passPercent = coarseEvaluated == 0 ? 0.0 : coarseAccepted * 100.0 / coarseEvaluated;
        double fullScanPercent = checked == 0 ? 0.0 : fullScans * 100.0 / checked;
        double matchesPerFullScan = fullScans == 0 ? 0.0 : matches * 100.0 / fullScans;
        double avgCoarseScore = coarseEvaluated == 0 ? 0.0 : stats.coarseScoreTotal.get() / (double) coarseEvaluated;

        int maxCoarseScore = stats.maxCoarseScore.get();
        int minAcceptedScore = stats.minAcceptedScore.get();
        if (minAcceptedScore == Integer.MAX_VALUE) {
            minAcceptedScore = 0;
        }

        if (stage025Evaluated > 0) {
            listener.onLog(
                    String.format(
                            Locale.ROOT,
                            "STAGE0.25 DEBUG | checked=%d | evaluated=%d | accepted=%d (%.3f%%) | rejected=%d | rule=64-column zero Y88+ signal",
                            checked, stage025Evaluated, stage025Accepted, stage025PassPercent, stage025Rejected
                    )
            );
        }

        long scoutEvaluated = stats.stage0ScoutEvaluated.get();
        if (scoutEvaluated > 0) {
            long scoutAccepted = stats.stage0ScoutAccepted.get();
            long scoutRejected = stats.stage0ScoutRejected.get();
            double scoutRejectPercent = scoutRejected * 100.0 / scoutEvaluated;
            listener.onLog(
                    String.format(
                            "STAGE-1 SCOUT | checked=%d | evaluated=%d | passed=%d | rejected=%d (%.3f%%)",
                            checked, scoutEvaluated, scoutAccepted, scoutRejected, scoutRejectPercent
                    )
            );
        }

        if (stage0Evaluated > 0) {
            listener.onLog(
                    String.format(
                            "STAGE0 DEBUG | checked=%d | evaluated=%d | accepted=%d (%.3f%%) | rejected=%d | avgReentry=%.2f | maxReentry=%d",
                            checked,
                            stage0Evaluated,
                            stage0Accepted,
                            stage0PassPercent,
                            stage0Rejected,
                            avgStage0Score,
                            stats.maxStage0Score.get()
                    )
            );
            listener.onLog("STAGE0 DIST | " + stats.formatStage0Distribution());
        }

        if (stage0HighEvaluated > 0) {
            listener.onLog(
                    String.format(
                            "STAGE0.5 DEBUG | checked=%d | evaluated=%d | accepted=%d (%.3f%%) | rejected=%d | avgY88=%.2f | maxY88=%d",
                            checked,
                            stage0HighEvaluated,
                            stage0HighAccepted,
                            stage0HighPassPercent,
                            stage0HighRejected,
                            avgStage0HighScore,
                            stats.maxStage0HighScore.get()
                    )
            );
            listener.onLog("STAGE0.5 DIST | " + stats.formatStage0HighDistribution());
        }

        if (stage075Evaluated > 0) {
            listener.onLog(
                    String.format(
                            Locale.ROOT,
                            "STAGE0.75 DEBUG | checked=%d | evaluated=%d | accepted=%d (%.3f%%) | rejected=%d | threshold=%.2f",
                            checked,
                            stage075Evaluated,
                            stage075Accepted,
                            stage075PassPercent,
                            stage075Rejected,
                            getEffectiveP19MinScore(settings)
                    )
            );
        }

        long stage025AuditSamples = stats.stage025AuditSamples.get();
        if (stage025AuditSamples > 0) {
            listener.onLog(
                    String.format(
                            "STAGE0.25 AUDIT | sampledRejects=%d | maxRejectedCoarse=%d | rejectedCoarse>=85=%d | >=120=%d | >=180=%d | >=240=%d | >=280=%d",
                            stage025AuditSamples,
                            stats.maxStage025AuditRejectedCoarse.get(),
                            stats.stage025AuditCoarse85Plus.get(),
                            stats.stage025AuditCoarse120Plus.get(),
                            stats.stage025AuditCoarse180Plus.get(),
                            stats.stage025AuditCoarse240Plus.get(),
                            stats.stage025AuditCoarse280Plus.get()
                    )
            );
        }

        long auditSamples = stats.stage0AuditSamples.get();
        if (auditSamples > 0) {
            listener.onLog(
                    String.format(
                            "STAGE0 AUDIT | sampledRejects=%d | maxRejectedCoarse=%d | rejectedCoarse>=85=%d | >=120=%d | >=180=%d | >=240=%d | >=280=%d",
                            auditSamples,
                            stats.maxStage0AuditRejectedCoarse.get(),
                            stats.stage0AuditCoarse85Plus.get(),
                            stats.stage0AuditCoarse120Plus.get(),
                            stats.stage0AuditCoarse180Plus.get(),
                            stats.stage0AuditCoarse240Plus.get(),
                            stats.stage0AuditCoarse280Plus.get()
                    )
            );
        }

        long stage075AuditSamples = stats.stage075AuditSamples.get();
        if (stage075AuditSamples > 0) {
            listener.onLog(
                    String.format(
                            "STAGE0.75 AUDIT | sampledRejects=%d | maxRejectedCoarse=%d | rejectedCoarse>=85=%d | >=120=%d | >=180=%d | >=240=%d | >=280=%d",
                            stage075AuditSamples,
                            stats.maxStage075AuditRejectedCoarse.get(),
                            stats.stage075AuditCoarse85Plus.get(),
                            stats.stage075AuditCoarse120Plus.get(),
                            stats.stage075AuditCoarse180Plus.get(),
                            stats.stage075AuditCoarse240Plus.get(),
                            stats.stage075AuditCoarse280Plus.get()
                    )
            );
        }

        listener.onLog(
                String.format(
                        "COARSE DEBUG | checked=%d | %.1f regions/s | evaluated=%d | accepted=%d (%.3f%%) | rejected=%d | fullScans=%d (%.3f%%) | matches=%d | matches/fullScan=%.3f%% | avgScore=%.1f | maxScore=%d | acceptedScoreRange=%d-%d",
                        checked,
                        seedsPerSecond,
                        coarseEvaluated,
                        coarseAccepted,
                        passPercent,
                        coarseRejected,
                        fullScans,
                        fullScanPercent,
                        matches,
                        matchesPerFullScan,
                        avgCoarseScore,
                        maxCoarseScore,
                        minAcceptedScore,
                        stats.maxAcceptedScore.get()
                )
        );

        if (coarseEvaluated > 0) {
            listener.onLog("COARSE DIST | " + stats.formatDistribution());
            listener.onLog("COARSE TOP  | " + stats.formatTopScores());
        }

        if (shadowResearch != null && shadowResearch.isEnabled()) {
            shadowResearch.writeSummary(checked);
            for (String line : shadowResearch.compactStatusLines()) {
                listener.onLog(line);
            }
        }
    }

    private String getHuntProfileName(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return "WORLD RECORD 80k+";
        if (settings.recordHuntMode) return "RECORD 60k+";
        if (settings.megaMode) return "MEGA 30k+";
        return "GENERAL";
    }

    private String getGpuProfileArgument(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return "record80";
        if (settings.recordHuntMode) return "record60";
        return settings.megaMode ? "mega" : "general";
    }

    private int getEffectiveP20MinSamples(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return Math.max(1, settings.record80P20MinPositiveColumns);
        if (settings.recordHuntMode) return Math.max(1, settings.record60P20MinPositiveColumns);
        return 1;
    }

    private double getEffectiveP19MinScore(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return settings.record80MinP19Score;
        if (settings.recordHuntMode) return settings.record60MinP19Score;
        return P19MonsterGate.THRESHOLD;
    }

    private int getHunterCoarseThreshold(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return Math.max(1, settings.record80CoarseMinCells);
        if (settings.recordHuntMode) return Math.max(1, settings.record60CoarseMinCells);
        int threshold = settings.hunterCoarseMinCells;
        return threshold > 0 ? threshold : 180;
    }

    private boolean isStage0Enabled(SearchSettings settings) {
        return settings.hunterStage0Enabled;
    }

    private int getStage0Step(SearchSettings settings) {
        return settings.hunterStage0Step > 0 ? settings.hunterStage0Step : DEFAULT_STAGE0_STEP;
    }

    private int getStage0MinReentrySamples(SearchSettings settings) {
        return settings.hunterStage0MinReentrySamples > 0
                ? settings.hunterStage0MinReentrySamples
                : DEFAULT_STAGE0_MIN_REENTRY_SAMPLES;
    }

    private int getStage0MinUpperYIndex(SearchSettings settings) {
        int y = settings.hunterStage0MinUpperYIndex > 0
                ? settings.hunterStage0MinUpperYIndex
                : DEFAULT_STAGE0_MIN_UPPER_Y_INDEX;
        return Math.max(0, Math.min(16, y));
    }

    private boolean isStage0HighEnabled(SearchSettings settings) {
        return settings.hunterStage0HighEnabled;
    }

    private int getStage0HighMinReentrySamples(SearchSettings settings) {
        return settings.hunterStage0HighMinReentrySamples > 0
                ? settings.hunterStage0HighMinReentrySamples
                : DEFAULT_STAGE0_HIGH_MIN_REENTRY_SAMPLES;
    }

    private int getEffectiveUpperMinSamples(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return Math.max(1, settings.record80UpperMinPositiveColumns);
        if (settings.recordHuntMode) return Math.max(1, settings.record60UpperMinPositiveColumns);
        if (!settings.megaMode) return DEFAULT_STAGE0_UPPER_MIN_POSITIVE_COLUMNS;
        return MegaFilter.upperMin(settings);
    }

    private int getEffectiveHighMinSamples(SearchSettings settings) {
        if (settings.extremeRecordHuntMode) return Math.max(1, settings.record80HighMinReentryColumns);
        if (settings.recordHuntMode) return Math.max(1, settings.record60HighMinReentryColumns);
        if (!settings.megaMode) return getStage0HighMinReentrySamples(settings);
        return MegaFilter.highMin(settings);
    }

    private boolean isMegaTopologyReject(
            SearchSettings settings,
            double p19Score,
            boolean extremeBypass,
            BetaTerrain173.PreparedStage0MonsterFeatures features
    ) {
        if (features == null) return false;
        return MegaFilter.rejects(
                settings,
                p19Score,
                extremeBypass,
                features.stage0FullY112,
                features.stage0Y96LargestCluster
        );
    }

    private int getStage0HighMinUpperYIndex(SearchSettings settings) {
        int y = settings.hunterStage0HighMinUpperYIndex > 0
                ? settings.hunterStage0HighMinUpperYIndex
                : DEFAULT_STAGE0_HIGH_MIN_UPPER_Y_INDEX;
        return Math.max(0, Math.min(16, y));
    }

    private boolean isStage0AuditEnabled(SearchSettings settings) {
        return settings.hunterStage0AuditEnabled;
    }

    private long getStage0AuditMask(SearchSettings settings) {
        return settings.hunterStage0AuditSampleMask > 0L ? settings.hunterStage0AuditSampleMask : 511L;
    }

    private long getP20AuditMask(SearchSettings settings) {
        if (settings.recordHuntMode || settings.extremeRecordHuntMode) {
            return getStage0AuditMask(settings);
        }
        return P20_PROGRESSIVE_SCOUT_AUDIT_MASK;
    }

    private boolean shouldAuditStage0Reject(long seed, SearchSettings settings) {
        return isStage0AuditEnabled(settings) && ((seed & getStage0AuditMask(settings)) == 0L);
    }

    private long getPerformanceProfileMask(SearchSettings settings) {
        long mask = settings.performanceProfileSampleMask;
        return mask >= 0L ? mask : 1023L;
    }

    private long getDebugLogInterval(SearchSettings settings) {
        long configured = settings.debugLogInterval > 0L ? settings.debugLogInterval : 1_000_000L;
        if (settings.recordHuntMode || settings.extremeRecordHuntMode) {
            return Math.max(configured, 5_000_000L);
        }
        return configured;
    }

    private boolean shouldProfileAttempt(long attempt, SearchSettings settings) {
        return perfStats != null
                && perfStats.enabled
                && attempt >= 0L
                && ((attempt & getPerformanceProfileMask(settings)) == 0L);
    }

    private static long deterministicSeedForAttempt(long sequenceSeed, long attempt) {
        long z = sequenceSeed + attempt * 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    private MatchResult findFloatingIslandInSeed(long seed, SearchSettings settings, Workspace workspace, DebugStats debugStats) {
        return findFloatingIslandInSeed(seed, settings, workspace, debugStats, true, -1L);
    }

    private MatchResult findFloatingIslandInSeed(
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            long attempt
    ) {
        return findFloatingIslandInSeed(seed, settings, workspace, debugStats, true, attempt);
    }

    private MatchResult findFloatingIslandInSeed(
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            boolean allowBorderVerify,
            long attempt
    ) {
        return findFloatingIslandInSeed(seed, settings, workspace, debugStats, allowBorderVerify, true, attempt);
    }

    private MatchResult findFloatingIslandInSeed(
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            boolean allowBorderVerify,
            boolean allowHotSeedScan,
            long attempt
    ) {
        return findFloatingIslandInSeed(
                seed, settings, workspace, debugStats, allowBorderVerify, allowHotSeedScan, attempt, true
        );
    }

    private MatchResult findFloatingIslandInSeed(
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            boolean allowBorderVerify,
            boolean allowHotSeedScan,
            long attempt,
            boolean recordShadowOutcome
    ) {
        return findFloatingIslandInSeed(
                seed, settings, workspace, debugStats, allowBorderVerify, allowHotSeedScan,
                attempt, recordShadowOutcome, 0, 0
        );
    }

    private MatchResult findFloatingIslandInSeed(
            long seed,
            SearchSettings settings,
            Workspace workspace,
            DebugStats debugStats,
            boolean allowBorderVerify,
            boolean allowHotSeedScan,
            long attempt,
            boolean recordShadowOutcome,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
    BetaTerrain173 terrain = workspace.terrainForSeed(seed);

    int stage0Score = -1;
    int stage0HighScore = -1;
    int coarseScore = -1;
    int stage0ScoutPositiveColumns = -1;
    int stage0LowerCandidateColumns = -1;
    int shadowHighReentryRaw = -1;
    double shadowP19Score = Double.NaN;
    boolean shadowP19ExtremeBypass = false;
    long shadowRejectMask = 0L;
    BetaTerrain173.ProgressiveStage0TierFeatures shadowTier64 = null;

    if (settings.hunterMode) {
        boolean profileCommon = shouldProfileAttempt(attempt, settings);
        long stage0StartNs = profileCommon ? System.nanoTime() : 0L;

        if (isStage0Enabled(settings) && isStage0HighEnabled(settings)) {
            BetaTerrain173.ProgressiveStage0TierFeatures progressive64 =
                    terrain.prepareStage0UpperPositiveScoutTier64AroundZero(
                            settings.chunkRadius,
                            getStage0Step(settings),
                            getStage0HighMinUpperYIndex(settings)
                    );
            debugStats.stage025Evaluated.incrementAndGet();
            if (progressive64.upperPositiveColumns < getEffectiveP20MinSamples(settings)) {
                debugStats.stage025Rejected.incrementAndGet();
                if (profileCommon) {
                    perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                }
                maybeAppendStage0AuditCsv(
                        attempt, seed, settings, terrain, workspace, debugStats,
                        "stage025_reject_sample", -1, -1, -1, -1
                );
                return null;
            }
            debugStats.stage025Accepted.incrementAndGet();
            shadowTier64 = progressive64;
            if (shadowResearch != null) {
                shadowRejectMask |= shadowResearch.recordP20Survivor(attempt, seed, progressive64);
            }

            debugStats.stage0ScoutEvaluated.incrementAndGet();
            stage0ScoutPositiveColumns = terrain.completeStage0UpperPositiveScoutAfterProgressiveTiers(
                    settings.chunkRadius,
                    getStage0Step(settings),
                    getStage0HighMinUpperYIndex(settings)
            );
            if (stage0ScoutPositiveColumns < getEffectiveUpperMinSamples(settings)) {
                debugStats.stage0ScoutRejected.incrementAndGet();
                if (profileCommon) {
                    perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                }
                maybeAppendStage0AuditCsv(
                        attempt, seed, settings, terrain, workspace, debugStats,
                        "scout_reject_sample", -1, -1, stage0ScoutPositiveColumns, -1
                );
                return null;
            }
            debugStats.stage0ScoutAccepted.incrementAndGet();
            if (shadowResearch != null) {
                shadowRejectMask = shadowResearch.recordScoutSurvivor(stage0ScoutPositiveColumns, shadowRejectMask);
            }

            BetaTerrain173.SparseGateCounts gateCounts = terrain.completeHighSparseReentryAfterUpperScoutCandidateColumns(
                    settings.chunkRadius,
                    getStage0Step(settings),
                    getStage0MinUpperYIndex(settings),
                    getStage0MinReentrySamples(settings),
                    getStage0HighMinUpperYIndex(settings),
                    getEffectiveHighMinSamples(settings)
            );
            stage0LowerCandidateColumns = terrain.getLastStage0LowerCandidateColumns();

            // The high Y88+ gate mathematically subsumes the low Y72+ gate.
            // P16 computes the exact high-gate decision and derives a decision-
            // equivalent low score, so impossible columns never need lower-Y noise.
            stage0Score = Math.min(gateCounts.lowCount, getStage0MinReentrySamples(settings));
            stage0HighScore = Math.min(gateCounts.highCount, getEffectiveHighMinSamples(settings));

            debugStats.recordStage0Score(stage0Score);
            if (stage0Score < getStage0MinReentrySamples(settings)) {
                debugStats.stage0Rejected.incrementAndGet();
                if (profileCommon) {
                    perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                }
                return null;
            }
            debugStats.stage0Accepted.incrementAndGet();

            debugStats.recordStage0HighScore(stage0HighScore);
            if (stage0HighScore < getEffectiveHighMinSamples(settings)) {
                debugStats.stage0HighRejected.incrementAndGet();
                if (profileCommon) {
                    perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                }
                maybeAppendStage0AuditCsv(
                        attempt, seed, settings, terrain, workspace, debugStats,
                        "high_gate_reject_sample", stage0Score, stage0HighScore,
                        stage0ScoutPositiveColumns, stage0LowerCandidateColumns
                );
                return null;
            }
            debugStats.stage0HighAccepted.incrementAndGet();
            if (shadowResearch != null && shadowResearch.isEnabled()) {
                shadowHighReentryRaw = terrain.countPreparedHighSparseReentryColumns(
                        getStage0HighMinUpperYIndex(settings)
                );
                shadowRejectMask = shadowResearch.recordHighGateSurvivor(shadowHighReentryRaw, shadowRejectMask);
            } else {
                shadowHighReentryRaw = gateCounts.highCount;
            }

            // P19 Stage0.75: use only exact values already present in the P15/P16
            // cache. No terrain noise is generated here. The model predicts coarse
            // monster potential and rejects the weakest half before P17 pays for
            // the global upper coarse pass.
            BetaTerrain173.PreparedStage0MonsterFeatures monsterFeatures =
                    terrain.analyzePreparedStage0MonsterFeatures(
                            settings.chunkRadius,
                            getStage0Step(settings)
                    );
            shadowP19Score = P19MonsterGate.score(stage0ScoutPositiveColumns, monsterFeatures);
            shadowP19ExtremeBypass = P19MonsterGate.hasExtremeTopologySignal(monsterFeatures);
            boolean monsterGatePass = shadowP19ExtremeBypass || shadowP19Score >= getEffectiveP19MinScore(settings);
            debugStats.stage075Evaluated.incrementAndGet();
            if (!monsterGatePass) {
                debugStats.stage075Rejected.incrementAndGet();
                if (profileCommon) {
                    perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                }
                maybeAppendStage0AuditCsv(
                        attempt, seed, settings, terrain, workspace, debugStats,
                        "stage075_reject_sample", stage0Score, stage0HighScore,
                        stage0ScoutPositiveColumns, stage0LowerCandidateColumns
                );
                return null;
            }
            debugStats.stage075Accepted.incrementAndGet();
            if (shadowResearch != null) {
                shadowRejectMask = shadowResearch.recordP19Survivor(
                        shadowP19Score,
                        shadowP19ExtremeBypass,
                        shadowRejectMask
                );
            }

            if (isMegaTopologyReject(settings, shadowP19Score, shadowP19ExtremeBypass, monsterFeatures)) {
                maybeAppendStage0AuditCsv(
                        attempt, seed, settings, terrain, workspace, debugStats,
                        "mega_topology_reject_sample", stage0Score, stage0HighScore,
                        stage0ScoutPositiveColumns, stage0LowerCandidateColumns
                );
                return null;
            }
        } else {
            if (isStage0Enabled(settings)) {
                stage0Score = getStage0SparseReentrySamples(terrain, settings, workspace);
                debugStats.recordStage0Score(stage0Score);

                if (stage0Score < getStage0MinReentrySamples(settings)) {
                    debugStats.stage0Rejected.incrementAndGet();
                    if (profileCommon) {
                        perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                    }
                    return null;
                }

                debugStats.stage0Accepted.incrementAndGet();
            }

            if (isStage0HighEnabled(settings)) {
                stage0HighScore = getStage0HighSparseReentrySamples(terrain, settings, workspace);
                debugStats.recordStage0HighScore(stage0HighScore);

                if (stage0HighScore < getEffectiveHighMinSamples(settings)) {
                    debugStats.stage0HighRejected.incrementAndGet();
                    if (profileCommon) {
                        perfStats.recordStage0(System.nanoTime() - stage0StartNs);
                    }
                    maybeAppendStage0AuditCsv(
                            attempt, seed, settings, terrain, workspace, debugStats,
                            "high_gate_reject_sample", stage0Score, stage0HighScore, -1, -1
                    );
                    return null;
                }

                debugStats.stage0HighAccepted.incrementAndGet();
            }
        }

        if (profileCommon) {
            perfStats.recordStage0(System.nanoTime() - stage0StartNs);
        }

        long coarseGridStartNs = profileCommon ? System.nanoTime() : 0L;
        CoarseGrid coarseGrid = loadRelevantCoarseGridAroundZero(terrain, settings.chunkRadius, workspace);
        if (profileCommon) {
            perfStats.recordCoarseGrid(System.nanoTime() - coarseGridStartNs);
        }

        long coarseScoreStartNs = profileCommon ? System.nanoTime() : 0L;
        coarseScore = findBestCoarseFloatingCellsLazy(terrain, coarseGrid, workspace);
        if (profileCommon) {
            perfStats.recordCoarseScore(System.nanoTime() - coarseScoreStartNs);
        }

        debugStats.recordCoarseScore(coarseScore, seed);

        maybeAppendFeatureCsv(
                attempt,
                seed,
                settings,
                terrain,
                workspace,
                coarseGrid,
                stage0Score,
                stage0HighScore,
                coarseScore,
                stage0ScoutPositiveColumns,
                stage0LowerCandidateColumns
        );

        int threshold = getHunterCoarseThreshold(settings);
        if (coarseScore < threshold) {
            debugStats.coarseRejected.incrementAndGet();
            return null;
        }

        debugStats.coarseAccepted.incrementAndGet();
        debugStats.recordAcceptedScore(coarseScore);
        if (shadowResearch != null) {
            shadowRejectMask = shadowResearch.recordCurrentCoarseCandidate(coarseScore, shadowRejectMask);
        }
    }

    debugStats.fullScans.incrementAndGet();
    long exactStartNs = (perfStats != null && perfStats.enabled && attempt >= 0L) ? System.nanoTime() : 0L;

    int sizeX = (settings.chunkRadius * 2 + 1) * 16;
    int sizeZ = (settings.chunkRadius * 2 + 1) * 16;
    int sizeY = BetaTerrain173.WORLD_HEIGHT;

    int minWorldX = (searchCenterChunkX - settings.chunkRadius) * 16;
    int minWorldZ = (searchCenterChunkZ - settings.chunkRadius) * 16;

    int volume = sizeX * sizeY * sizeZ;
    int columnVolume = sizeX * sizeZ;

    workspace.prepare(volume, columnVolume);

    byte[] stone = workspace.stone;
    byte[] visited = workspace.visited;
    int[] queue = workspace.queue;
    int[] columnSeen = workspace.columnSeen;
    int[] candidateStarts = workspace.candidateStarts;

    int candidateCount = 0;

    int floodMinY = ISLAND_FLOOR_Y;

    int scanStartY = Math.max(settings.minYForMatch, ISLAND_FLOOR_Y);
    scanStartY = Math.max(0, Math.min(sizeY - 1, scanStartY));

    int copyStartY = Math.max(0, ISLAND_FLOOR_Y - 1);

    int columnMark = 1;

    for (int chunkX = searchCenterChunkX - settings.chunkRadius;
         chunkX <= searchCenterChunkX + settings.chunkRadius; chunkX++) {
        for (int chunkZ = searchCenterChunkZ - settings.chunkRadius;
             chunkZ <= searchCenterChunkZ + settings.chunkRadius; chunkZ++) {
            int[] blocks = terrain.generateChunkBaseTerrain(chunkX, chunkZ);

            int baseX = (chunkX - (searchCenterChunkX - settings.chunkRadius)) * 16;
            int baseZ = (chunkZ - (searchCenterChunkZ - settings.chunkRadius)) * 16;


            for (int x = 0; x < 16; x++) {
                int worldArrayX = baseX + x;

                for (int z = 0; z < 16; z++) {
                    int worldArrayZ = baseZ + z;

                    boolean allowCandidateColumn = isSampleCandidateColumn(x, z);

                    for (int y = copyStartY; y < sizeY; y++) {
                        int block = blocks[BetaTerrain173.index(x, y, z)];

                        if (block == BetaTerrain173.STONE) {
                            int flatIndex = index3(worldArrayX, y, worldArrayZ, sizeY, sizeZ);
                            stone[flatIndex] = 1;

                            if (allowCandidateColumn && y >= ISLAND_FLOOR_Y) {
                                int belowBlock = blocks[BetaTerrain173.index(x, y - 1, z)];

                                if (belowBlock != BetaTerrain173.STONE) {
                                    candidateStarts[candidateCount++] = flatIndex;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component bestMatch = null;

// IMPORTANT:
// floodMinY and scanStartY should already be declared BEFORE the chunk-copy loop,
// because the candidate creation uses scanStartY.
// So do not redeclare them here if you already moved them earlier.

int yzArea = sizeY * sizeZ;

for (int i = 0; i < candidateCount; i++) {
    int index = candidateStarts[i];

    if (stone[index] == 0 || visited[index] != 0) {
        continue;
    }

    int x = index / yzArea;
    int remainder = index - x * yzArea;
    int y = remainder / sizeZ;
    int z = remainder - y * sizeZ;

    Component c = floodFillFlat(
            stone,
            visited,
            queue,
            columnSeen,
            columnMark++,
            x,
            y,
            z,
            sizeX,
            sizeY,
            sizeZ,
            minWorldX,
            minWorldZ,
            floodMinY
    );

    if (!isMatch(c, settings)) {
        continue;
    }

    if (bestMatch == null || compareComponents(c, bestMatch) > 0) {
        bestMatch = c;
    }
}

    if (exactStartNs != 0L) {
        perfStats.recordNormalExact(System.nanoTime() - exactStartNs);
    }

    if (bestMatch == null) {
        boolean hotScanRan = false;
        MatchResult hotResult = null;

        if (shouldRunHotSeedScan(settings, coarseScore, allowHotSeedScan)) {
            hotScanRan = true;
            hotResult = runHotSeedScan(
                    seed, settings, stage0Score, stage0HighScore, coarseScore, debugStats,
                    searchCenterChunkX, searchCenterChunkZ
            );

            // A hot scan can itself be clipped at radius 16. Border correctness must
            // not depend on coarse score, so continue expanding any verified side touch.
            if (allowBorderVerify
                    && hotResult != null
                    && hotResult.component.touchesSideBorder
                    && hotResult.chunkRadiusUsed < BORDER_VERIFY_MAX_RADIUS) {
                hotResult = verifySideBorderMatch(
                        seed, hotResult, settings, debugStats, searchCenterChunkX, searchCenterChunkZ
                );
            }
        }

        if (settings.hunterMode) {
            appendCandidateCsv(
                    attempt, seed, stage0Score, stage0HighScore, coarseScore,
                    -1, -1, stage0HighScore, Double.isFinite(shadowP19Score), shadowP19Score, shadowP19ExtremeBypass,
                    searchCenterChunkX, searchCenterChunkZ,
                    null, hotResult, hotScanRan, hotResult, false
            );
        }
        if (recordShadowOutcome) {
            recordShadowCandidateOutcome(
                    attempt, seed, shadowTier64, stage0ScoutPositiveColumns, shadowHighReentryRaw,
                    shadowP19Score, shadowP19ExtremeBypass, coarseScore, shadowRejectMask, hotResult
            );
        }
        return hotResult;
    }

    MatchResult rawResult = new MatchResult(
            seed, bestMatch, settings.chunkRadius, stage0Score, stage0HighScore, coarseScore,
            searchCenterChunkX, searchCenterChunkZ
    );
    MatchResult verifiedResult = rawResult;

    if (allowBorderVerify && bestMatch.touchesSideBorder && settings.chunkRadius < BORDER_VERIFY_MAX_RADIUS) {
        verifiedResult = verifySideBorderMatch(
                seed, rawResult, settings, debugStats, searchCenterChunkX, searchCenterChunkZ
        );
    }

    boolean hotScanRan = false;
    MatchResult hotResult = null;

    if (shouldRunHotSeedScan(settings, coarseScore, allowHotSeedScan)) {
        hotScanRan = true;
        hotResult = runHotSeedScan(
                seed, settings, stage0Score, stage0HighScore, coarseScore, debugStats,
                searchCenterChunkX, searchCenterChunkZ
        );

        // Finish border verification before comparing the hot result against the
        // normal result. A clipped radius-16 component can shrink or disappear when
        // the border opens, and must never displace a valid contained normal island.
        if (allowBorderVerify
                && hotResult != null
                && hotResult.component.touchesSideBorder
                && hotResult.chunkRadiusUsed < BORDER_VERIFY_MAX_RADIUS) {
            hotResult = verifySideBorderMatch(
                    seed, hotResult, settings, debugStats, searchCenterChunkX, searchCenterChunkZ
            );
        }

        if (hotResult != null && (verifiedResult == null || compareComponents(hotResult.component, verifiedResult.component) > 0)) {
            verifiedResult = hotResult;
        }
    }

    if (settings.hunterMode) {
        appendCandidateCsv(
                attempt, seed, stage0Score, stage0HighScore, coarseScore,
                -1, -1, stage0HighScore, Double.isFinite(shadowP19Score), shadowP19Score, shadowP19ExtremeBypass,
                searchCenterChunkX, searchCenterChunkZ,
                rawResult, verifiedResult, hotScanRan, hotResult, false
        );
    }

    if (recordShadowOutcome) {
        recordShadowCandidateOutcome(
                attempt, seed, shadowTier64, stage0ScoutPositiveColumns, shadowHighReentryRaw,
                shadowP19Score, shadowP19ExtremeBypass, coarseScore, shadowRejectMask, verifiedResult
        );
    }
    return verifiedResult;
}

    private void recordShadowCandidateOutcome(
            long attempt,
            long seed,
            BetaTerrain173.ProgressiveStage0TierFeatures tier64,
            int scoutPositiveColumns,
            int highReentryRaw,
            double p19Score,
            boolean p19ExtremeBypass,
            int coarseScore,
            long shadowRejectMask,
            MatchResult result
    ) {
        if (shadowResearch == null || !shadowResearch.isEnabled()) return;

        boolean matched = result != null;
        Component c = matched ? result.component : null;
        int blocks = matched ? c.blocks : 0;
        int columns = matched ? c.columns : 0;
        int width = matched ? c.maxWorldX - c.minWorldX + 1 : 0;
        int depth = matched ? c.maxWorldZ - c.minWorldZ + 1 : 0;
        double fillPercent = matched && width > 0 && depth > 0
                ? columns * 100.0D / (width * (double) depth)
                : 0.0D;
        double avgThickness = matched && columns > 0 ? blocks / (double) columns : 0.0D;
        int minY = matched ? c.minY : -1;
        int maxY = matched ? c.maxY : -1;
        int radiusUsed = matched ? result.chunkRadiusUsed : 0;

        shadowResearch.recordCandidateOutcome(
                attempt,
                seed,
                tier64,
                scoutPositiveColumns,
                highReentryRaw,
                p19Score,
                p19ExtremeBypass,
                coarseScore,
                shadowRejectMask,
                matched,
                blocks,
                columns,
                width,
                depth,
                fillPercent,
                avgThickness,
                minY,
                maxY,
                radiusUsed
        );
    }

    private boolean shouldRunHotSeedScan(SearchSettings settings, int coarseScore, boolean allowHotSeedScan) {
        return allowHotSeedScan
                && HOT_SEED_SCAN_ENABLED
                && settings.chunkRadius < HOT_SEED_SCAN_RADIUS
                && coarseScore >= HOT_SEED_COARSE_THRESHOLD;
    }

    private MatchResult runHotSeedScan(
            long seed,
            SearchSettings settings,
            int stage0Score,
            int stage0HighScore,
            int coarseScore,
            DebugStats debugStats,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
        long hotStartNs = perfStats != null && perfStats.enabled ? System.nanoTime() : 0L;
        try {
            SearchSettings hotSettings = copySettingsWithRadius(settings, HOT_SEED_SCAN_RADIUS);

            // Exact-only wide scan. The seed already passed Stage0/coarse at the normal radius.
            hotSettings.hunterMode = false;
            hotSettings.savePreviews = false;

            // Use a throwaway workspace so one rare radius-16 scan does not permanently
            // bloat the normal worker thread's reused arrays.
            Workspace hotWorkspace = new Workspace();

            MatchResult hot = findFloatingIslandInSeed(
                    seed,
                    hotSettings,
                    hotWorkspace,
                    debugStats,
                    false,
                    false,
                    -1L,
                    false,
                    searchCenterChunkX,
                    searchCenterChunkZ
            );

            if (hot == null) {
                return null;
            }

            return new MatchResult(
                    seed, hot.component, hot.chunkRadiusUsed, stage0Score, stage0HighScore, coarseScore,
                    searchCenterChunkX, searchCenterChunkZ
            );
        } finally {
            if (hotStartNs != 0L) {
                perfStats.recordHotScan(System.nanoTime() - hotStartNs);
            }
        }
    }

    private MatchResult verifySideBorderMatch(
            long seed,
            MatchResult original,
            SearchSettings settings,
            DebugStats debugStats,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
        long borderStartNs = perfStats != null && perfStats.enabled ? System.nanoTime() : 0L;
        try {
            MatchResult best = original;

            // Large-radius exact arrays are intentionally isolated from the normal worker
            // workspace. A rare radius-24 verification must not permanently bloat every
            // worker's reused buffers.
            Workspace borderWorkspace = new Workspace();

            for (int radius : BORDER_VERIFY_RADII) {
                if (radius <= best.chunkRadiusUsed) {
                    continue;
                }

                SearchSettings expandedSettings = copySettingsWithRadius(settings, radius);

                // Verification is exact-only. The seed already reached an exact component;
                // re-running hunter filters here is both wasted work and logically wrong.
                expandedSettings.hunterMode = false;
                expandedSettings.savePreviews = false;

                MatchResult expanded = findFloatingIslandInSeed(
                        seed,
                        expandedSettings,
                        borderWorkspace,
                        debugStats,
                        false,
                        false,
                        -1L,
                        false,
                        searchCenterChunkX,
                        searchCenterChunkZ
                );

                // If the bigger scan no longer has a valid floating island, the smaller
                // side-border hit was border bait, usually connected outside the old radius.
                if (expanded == null) {
                    return null;
                }

                best = new MatchResult(
                        seed,
                        expanded.component,
                        expanded.chunkRadiusUsed,
                        original.stage0Score,
                        original.stage0HighScore,
                        original.coarseScore,
                        searchCenterChunkX,
                        searchCenterChunkZ
                );

                if (!best.component.touchesSideBorder) {
                    return best;
                }
            }

            // Still touches side at the hard safety cap. Keep the largest verified scan
            // result and explicitly mark it as clipped in the logs/leaderboards.
            return best;
        } finally {
            if (borderStartNs != 0L) {
                perfStats.recordBorderVerify(System.nanoTime() - borderStartNs);
            }
        }
    }

    private boolean passesCoarseFloatingPrefilter(BetaTerrain173 terrain, SearchSettings settings) {
        int threshold = settings.hunterCoarseMinCells;
        if (threshold <= 0) {
            threshold = 180;
        }

        int bestCoarseFloatingCells = getBestCoarseFloatingCells(terrain, settings.chunkRadius, new Workspace());
        return bestCoarseFloatingCells >= threshold;
    }


    private BetaTerrain173.SparseGateCounts getStage0CombinedSparseReentrySamples(
            BetaTerrain173 terrain,
            SearchSettings settings,
            Workspace workspace
    ) {
        workspace.prepareStage0();
        return terrain.countSparseCoarseReentrySamplesAroundZeroCombined(
                settings.chunkRadius,
                getStage0Step(settings),
                getStage0MinUpperYIndex(settings),
                getStage0MinReentrySamples(settings),
                getStage0HighMinUpperYIndex(settings),
                getEffectiveHighMinSamples(settings),
                workspace.stage0Column
        );
    }

    private int getStage0SparseReentrySamples(BetaTerrain173 terrain, SearchSettings settings, Workspace workspace) {
        workspace.prepareStage0();
        return terrain.countSparseCoarseReentrySamplesAroundZero(
                settings.chunkRadius,
                getStage0Step(settings),
                getStage0MinUpperYIndex(settings),
                getStage0MinReentrySamples(settings),
                workspace.stage0Column
        );
    }

    private int getStage0HighSparseReentrySamples(BetaTerrain173 terrain, SearchSettings settings, Workspace workspace) {
        workspace.prepareStage0();
        return terrain.countSparseCoarseReentrySamplesAroundZero(
                settings.chunkRadius,
                getStage0Step(settings),
                getStage0HighMinUpperYIndex(settings),
                getEffectiveHighMinSamples(settings),
                workspace.stage0Column
        );
    }

    private int getBestCoarseFloatingCells(BetaTerrain173 terrain, int chunkRadius, Workspace workspace) {
        CoarseGrid grid = loadCoarseGridAroundZero(terrain, chunkRadius, workspace);
        return findBestCoarseFloatingCellsFast(grid, workspace);
    }

    private CoarseFeatureSummary getCoarseFeatures(BetaTerrain173 terrain, int chunkRadius, Workspace workspace) {
        CoarseGrid grid = loadCoarseGridAroundZero(terrain, chunkRadius, workspace);
        return findCoarseFeatureSummary(grid, workspace);
    }

    /**
     * P17 exact lazy coarse path.
     *
     * Any component that can score must reach Y index 8 or higher. Generate that
     * upper slice everywhere, then discover every positive cell connected to it.
     * Lower Y slices are generated in batches only for columns reached by that
     * connectivity search. Ungenerated lower cells cannot belong to a scoreable
     * component and are safely left negative.
     */
    private CoarseGrid loadRelevantCoarseGridAroundZero(
            BetaTerrain173 terrain,
            int chunkRadius,
            Workspace workspace
    ) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * coarseSize * COARSE_Y_LEVELS;
        workspace.prepareCoarse(coarseSize);

        Arrays.fill(workspace.coarseValues, 0, total, Double.NEGATIVE_INFINITY);
        CoarseGrid grid = new CoarseGrid(
                coarseSize,
                workspace.coarseValues,
                workspace.coarseFilled,
                true,
                chunkRadius
        );

        terrain.prepareCoarseDensityUpperSliceAroundZeroInto(
                workspace.coarseValues,
                workspace.coarseFilled,
                chunkRadius,
                MIN_INTERESTING_COARSE_Y_INDEX
        );
        return grid;
    }

    /**
     * P17 exact component-lazy scorer. Upper Y is known everywhere. Each upper
     * positive component is expanded into lower Y only along its own frontier.
     * Ground/side components abort immediately once disqualified, so they never
     * drag the search across the entire connected ground mass.
     */
    private int findBestCoarseFloatingCellsLazy(
            BetaTerrain173 terrain,
            CoarseGrid grid,
            Workspace workspace
    ) {
        int total = grid.size * grid.size * COARSE_Y_LEVELS;
        int columnTotal = grid.size * grid.size;
        int[] labels = workspace.coarseLabels;
        int[] queue = workspace.coarseQueue;
        Arrays.fill(labels, 0, total, 0);
        workspace.resetCoarseComponentColumns(columnTotal);

        int bestCells = 0;
        int nextId = 1;

        for (int x = 0; x < grid.size; x++) {
            for (int z = 0; z < grid.size; z++) {
                for (int y = MIN_INTERESTING_COARSE_Y_INDEX; y < COARSE_Y_LEVELS; y++) {
                    int start = grid.index(x, y, z);
                    if (labels[start] != 0 || grid.values[start] <= 0.0D) {
                        continue;
                    }

                    int id = nextId++;
                    int head = 0;
                    int tail = 0;
                    queue[tail++] = start;
                    labels[start] = id;
                    boolean invalid = false;
                    int expansionRounds = 0;
                    Arrays.fill(workspace.coarseLowerRequest, 0, columnTotal, false);

                    while (true) {
                        while (head < tail && !invalid) {
                            int idx = queue[head++];
                            int cy = idx % COARSE_Y_LEVELS;
                            int tmp = idx / COARSE_Y_LEVELS;
                            int cz = tmp % grid.size;
                            int cx = tmp / grid.size;

                            if (cy == 0 || cx == 0 || cz == 0 || cx == grid.size - 1 || cz == grid.size - 1) {
                                invalid = true;
                                break;
                            }

                            for (int[] d : COARSE_DIRS_3D) {
                                int nx = cx + d[0];
                                int ny = cy + d[1];
                                int nz = cz + d[2];
                                if (nx < 0 || nx >= grid.size || ny < 0 || ny >= COARSE_Y_LEVELS || nz < 0 || nz >= grid.size) {
                                    continue;
                                }

                                int ni = grid.index(nx, ny, nz);
                                if (ny < MIN_INTERESTING_COARSE_Y_INDEX && !grid.filled[ni]) {
                                    workspace.coarseLowerRequest[nx * grid.size + nz] = true;
                                    continue;
                                }

                                if (grid.values[ni] > 0.0D) {
                                    if (labels[ni] < 0) {
                                        invalid = true;
                                        break;
                                    }
                                    if (labels[ni] == 0) {
                                        labels[ni] = id;
                                        queue[tail++] = ni;
                                    }
                                }
                            }
                        }

                        if (invalid) {
                            break;
                        }

                        int requested = 0;
                        for (int column = 0; column < columnTotal; column++) {
                            if (workspace.coarseLowerRequest[column] && !grid.filled[column * COARSE_Y_LEVELS]) {
                                requested++;
                            } else {
                                workspace.coarseLowerRequest[column] = false;
                            }
                        }
                        if (requested == 0) {
                            break;
                        }

                        grid.lazyLowerColumnsGenerated += terrain.completeCoarseDensityLowerColumnsAroundZeroInto(
                                grid.values,
                                grid.filled,
                                grid.chunkRadius,
                                workspace.coarseLowerRequest,
                                MIN_INTERESTING_COARSE_Y_INDEX - 1
                        );
                        Arrays.fill(workspace.coarseLowerRequest, 0, columnTotal, false);
                        expansionRounds++;

                        if (grid.lazyLowerColumnsGenerated > P17_LAZY_COARSE_MAX_LOWER_COLUMNS
                                || grid.lazyExpansionRounds + expansionRounds > P17_LAZY_COARSE_MAX_EXPANSION_ROUNDS
                                || tail > P17_LAZY_COARSE_MAX_COMPONENT_QUEUE) {
                            return completeLazyCoarseGridAndScoreFull(terrain, grid, workspace);
                        }

                        // New lower cells can neighbor any previously reached cell.
                        // Re-scan the component; labels prevent duplicate queue entries.
                        head = 0;
                    }

                    grid.lazyExpansionRounds += expansionRounds;
                    if (invalid) {
                        for (int i = 0; i < tail; i++) {
                            labels[queue[i]] = -1;
                        }
                        continue;
                    }

                    CoarseComponent c = new CoarseComponent();
                    c.id = id;
                    c.cells = tail;
                    c.minYIndex = COARSE_Y_LEVELS;
                    c.maxYIndex = 0;

                    for (int i = 0; i < tail; i++) {
                        int idx = queue[i];
                        int py = idx % COARSE_Y_LEVELS;
                        int tmp = idx / COARSE_Y_LEVELS;
                        int pz = tmp % grid.size;
                        int px = tmp / grid.size;
                        if (py < c.minYIndex) c.minYIndex = py;
                        if (py > c.maxYIndex) c.maxYIndex = py;

                        int colIndex = px * grid.size + pz;
                        if (workspace.coarseColumnSeen[colIndex] != id) {
                            workspace.coarseColumnSeen[colIndex] = id;
                            workspace.coarseComponentColumns[c.columnCount++] = colIndex;
                            workspace.coarseColumnMinY[colIndex] = py;
                        } else if (py < workspace.coarseColumnMinY[colIndex]) {
                            workspace.coarseColumnMinY[colIndex] = py;
                        }
                    }

                    if (c.cells <= bestCells) {
                        continue;
                    }

                    // Re-entry checks need the lower slice in each component column,
                    // even when the component itself never descended below Y64.
                    Arrays.fill(workspace.coarseLowerRequest, 0, columnTotal, false);
                    int reentryColumnsToComplete = 0;
                    for (int i = 0; i < c.columnCount; i++) {
                        int col = workspace.coarseComponentColumns[i];
                        if (!grid.filled[col * COARSE_Y_LEVELS]) {
                            workspace.coarseLowerRequest[col] = true;
                            reentryColumnsToComplete++;
                        }
                    }
                    if (reentryColumnsToComplete > 0) {
                        grid.lazyLowerColumnsGenerated += terrain.completeCoarseDensityLowerColumnsAroundZeroInto(
                                grid.values,
                                grid.filled,
                                grid.chunkRadius,
                                workspace.coarseLowerRequest,
                                MIN_INTERESTING_COARSE_Y_INDEX - 1
                        );
                    }

                    if (hasCoarseReentryColumnForComponent(grid, labels, workspace, c.id, c.columnCount)) {
                        bestCells = c.cells;
                    }
                }
            }
        }

        return bestCells;
    }

    private int completeLazyCoarseGridAndScoreFull(
            BetaTerrain173 terrain,
            CoarseGrid grid,
            Workspace workspace
    ) {
        int columnTotal = grid.size * grid.size;
        Arrays.fill(workspace.coarseLowerRequest, 0, columnTotal, false);
        for (int column = 0; column < columnTotal; column++) {
            if (!grid.filled[column * COARSE_Y_LEVELS]) {
                workspace.coarseLowerRequest[column] = true;
            }
        }
        terrain.completeCoarseDensityLowerColumnsAroundZeroInto(
                grid.values,
                grid.filled,
                grid.chunkRadius,
                workspace.coarseLowerRequest,
                MIN_INTERESTING_COARSE_Y_INDEX - 1
        );
        grid.partialLower = false;
        grid.lazyFallbackToFull = true;
        return findBestCoarseFloatingCellsFast(grid, workspace);
    }

    private int getP17GeneratedPointCount(CoarseGrid grid) {
        int fullPoints = grid.size * grid.size * COARSE_Y_LEVELS;
        if (grid.lazyFallbackToFull || !grid.partialLower) {
            return fullPoints;
        }

        int upperLevels = COARSE_Y_LEVELS - MIN_INTERESTING_COARSE_Y_INDEX;
        int upperPoints = grid.size * grid.size * upperLevels;
        return upperPoints + grid.lazyLowerColumnsGenerated * MIN_INTERESTING_COARSE_Y_INDEX;
    }

    private void completeLazyCoarseGridForRichFeatures(
            BetaTerrain173 terrain,
            CoarseGrid grid,
            Workspace workspace
    ) {
        if (!grid.partialLower) return;

        int columnTotal = grid.size * grid.size;
        Arrays.fill(workspace.coarseLowerRequest, 0, columnTotal, false);
        for (int column = 0; column < columnTotal; column++) {
            if (!workspace.coarseFilled[column * COARSE_Y_LEVELS]) {
                workspace.coarseLowerRequest[column] = true;
            }
        }

        terrain.completeCoarseDensityLowerColumnsAroundZeroInto(
                workspace.coarseValues,
                workspace.coarseFilled,
                grid.chunkRadius,
                workspace.coarseLowerRequest,
                MIN_INTERESTING_COARSE_Y_INDEX - 1
        );
        grid.partialLower = false;
    }

    private CoarseGrid loadCoarseGridAroundZero(BetaTerrain173 terrain, int chunkRadius, Workspace workspace) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        workspace.prepareCoarse(coarseSize);
        CoarseGrid grid = new CoarseGrid(coarseSize, workspace.coarseValues, workspace.coarseFilled);

        // Faster path: generate the whole search area's coarse density lattice in one pass.
        // This avoids 225 separate chunk coarse generations at radius 7 and avoids duplicated
        // chunk-border lattice points.
        terrain.generateCoarseDensityGridAroundZeroInto(workspace.coarseValues, workspace.coarseFilled, chunkRadius);

        return grid;
    }

    /**
     * Score-only coarse path used by the live hunter.
     *
     * It skips every statistic that is only needed by hunter_features.csv. For
     * scoring, the hunter only needs the largest floating component with at least
     * one valid re-entry column.
     */
    private int findBestCoarseFloatingCellsFast(CoarseGrid grid, Workspace workspace) {
        int total = grid.size * grid.size * COARSE_Y_LEVELS;
        int[] labels = workspace.coarseLabels;
        int[] queue = workspace.coarseQueue;
        Arrays.fill(labels, 0, total, 0);
        workspace.resetCoarseComponentColumns(grid.size * grid.size);

        int bestCells = 0;
        int nextId = 1;

        for (int x = 0; x < grid.size; x++) {
            for (int z = 0; z < grid.size; z++) {
                for (int y = 0; y < COARSE_Y_LEVELS; y++) {
                    int start = grid.index(x, y, z);
                    if (labels[start] != 0 || grid.values[start] <= 0.0D) {
                        continue;
                    }

                    CoarseComponent c = floodFillCoarseComponentFast(
                            grid, labels, queue, workspace, nextId, start
                    );
                    nextId++;

                    // If it cannot beat the current maximum, an exact re-entry
                    // count cannot affect the final score.
                    if (c.cells <= bestCells || !isPossibleFloatingCoarseCandidate(c)) {
                        continue;
                    }

                    if (hasCoarseReentryColumnForComponent(grid, labels, workspace, c.id, c.columnCount)) {
                        bestCells = c.cells;
                    }
                }
            }
        }

        return bestCells;
    }

    private CoarseComponent floodFillCoarseComponentFast(
            CoarseGrid grid,
            int[] labels,
            int[] queue,
            Workspace workspace,
            int id,
            int start
    ) {
        CoarseComponent c = new CoarseComponent();
        c.id = id;
        c.minYIndex = COARSE_Y_LEVELS;
        c.maxYIndex = 0;

        int head = 0;
        int tail = 0;
        queue[tail++] = start;
        labels[start] = id;

        while (head < tail) {
            int idx = queue[head++];
            int y = idx % COARSE_Y_LEVELS;
            int tmp = idx / COARSE_Y_LEVELS;
            int z = tmp % grid.size;
            int x = tmp / grid.size;

            c.cells++;
            if (y < c.minYIndex) c.minYIndex = y;
            if (y > c.maxYIndex) c.maxYIndex = y;

            int colIndex = x * grid.size + z;
            if (workspace.coarseColumnSeen[colIndex] != id) {
                workspace.coarseColumnSeen[colIndex] = id;
                workspace.coarseComponentColumns[c.columnCount++] = colIndex;
                workspace.coarseColumnMinY[colIndex] = y;
            } else if (y < workspace.coarseColumnMinY[colIndex]) {
                workspace.coarseColumnMinY[colIndex] = y;
            }

            if (y == 0) {
                c.touchesBottom = true;
            }
            if (x == 0 || z == 0 || x == grid.size - 1 || z == grid.size - 1) {
                c.touchesSide = true;
            }

            for (int[] d : COARSE_DIRS_3D) {
                int nx = x + d[0];
                int ny = y + d[1];
                int nz = z + d[2];
                if (nx < 0 || nx >= grid.size || ny < 0 || ny >= COARSE_Y_LEVELS || nz < 0 || nz >= grid.size) {
                    continue;
                }

                int ni = grid.index(nx, ny, nz);
                if (labels[ni] == 0 && grid.values[ni] > 0.0D) {
                    labels[ni] = id;
                    queue[tail++] = ni;
                }
            }
        }

        return c;
    }

    private boolean hasCoarseReentryColumnForComponent(
            CoarseGrid grid,
            int[] labels,
            Workspace workspace,
            int componentId,
            int componentColumnCount
    ) {
        for (int i = 0; i < componentColumnCount; i++) {
            int colIndex = workspace.coarseComponentColumns[i];
            int x = colIndex / grid.size;
            int z = colIndex - x * grid.size;
            int componentMinYInColumn = workspace.coarseColumnMinY[colIndex];

            if (componentMinYInColumn <= 0 || componentMinYInColumn >= COARSE_Y_LEVELS) {
                continue;
            }

            int lowerPositiveY = -1;
            for (int y = componentMinYInColumn - 1; y >= 0; y--) {
                int idx = grid.index(x, y, z);
                if (grid.values[idx] > 0.0D && labels[idx] != componentId) {
                    lowerPositiveY = y;
                    break;
                }
            }

            if (lowerPositiveY >= 0
                    && componentMinYInColumn - lowerPositiveY - 1 >= MIN_FLOATING_GAP_STEPS) {
                return true;
            }
        }

        return false;
    }

    private CoarseFeatureSummary findCoarseFeatureSummary(CoarseGrid grid, Workspace workspace) {
        CoarseFeatureSummary summary = new CoarseFeatureSummary();
        int total = grid.values.length;
        int[] labels = workspace.coarseLabels;
        int[] queue = workspace.coarseQueue;
        Arrays.fill(labels, 0, total, 0);
        workspace.resetCoarseComponentColumns(grid.size * grid.size);

        // Cheap global density aggregates from the already generated coarse grid.
        boolean[] colY8 = workspace.featureColumnY8;
        boolean[] colY10 = workspace.featureColumnY10;
        boolean[] colY12 = workspace.featureColumnY12;
        int columnTotal = grid.size * grid.size;
        Arrays.fill(colY8, 0, columnTotal, false);
        Arrays.fill(colY10, 0, columnTotal, false);
        Arrays.fill(colY12, 0, columnTotal, false);

        for (int x = 0; x < grid.size; x++) {
            for (int z = 0; z < grid.size; z++) {
                int colIndex = x * grid.size + z;
                for (int y = 0; y < COARSE_Y_LEVELS; y++) {
                    double v = grid.values[grid.index(x, y, z)];
                    if (v <= 0.0D) {
                        continue;
                    }

                    summary.totalPositiveCells++;
                    if (y >= 8) {
                        summary.positiveCellsY8++;
                        colY8[colIndex] = true;
                    }
                    if (y >= 10) {
                        summary.positiveCellsY10++;
                        colY10[colIndex] = true;
                    }
                    if (y >= 12) {
                        summary.positiveCellsY12++;
                        colY12[colIndex] = true;
                    }
                }
            }
        }

        for (int i = 0; i < columnTotal; i++) {
            if (colY8[i]) summary.columnsPositiveY8++;
            if (colY10[i]) summary.columnsPositiveY10++;
            if (colY12[i]) summary.columnsPositiveY12++;
        }

        int nextId = 1;

        for (int x = 0; x < grid.size; x++) {
            for (int z = 0; z < grid.size; z++) {
                for (int y = 0; y < COARSE_Y_LEVELS; y++) {
                    int start = grid.index(x, y, z);
                    if (labels[start] != 0 || grid.values[start] <= 0.0D) {
                        continue;
                    }

                    summary.totalPositiveComponents++;
                    CoarseComponent c = floodFillCoarseComponent(grid, labels, queue, workspace, nextId, start);
                    nextId++;

                    // Most positive components are boring ground/side terrain.
                    // Do NOT run re-entry checks for those; that was the expensive part.
                    if (!isPossibleFloatingCoarseCandidate(c)) {
                        continue;
                    }

                    summary.possibleFloatingComponents++;
                    c.reentryColumns = countCoarseReentryColumnsForComponent(grid, labels, workspace, c.id, c.columnCount);

                    if (c.reentryColumns > 0 && c.cells > summary.bestCells) {
                        summary.bestCells = c.cells;
                        summary.bestColumns = c.columnCount;
                        summary.bestReentryColumns = c.reentryColumns;
                        summary.bestMinYIndex = c.minYIndex;
                        summary.bestMaxYIndex = c.maxYIndex;
                        summary.bestCellsY8 = c.cellsY8;
                        summary.bestCellsY10 = c.cellsY10;
                        summary.bestCellsY12 = c.cellsY12;
                        summary.bestMaxDensity = c.maxDensity;
                        summary.bestAvgPositiveDensity = c.cells == 0 ? 0.0D : c.sumDensity / c.cells;
                    }
                }
            }
        }

        return summary;
    }

    private CoarseComponent floodFillCoarseComponent(
            CoarseGrid grid,
            int[] labels,
            int[] queue,
            Workspace workspace,
            int id,
            int start
    ) {
        CoarseComponent c = new CoarseComponent();
        c.id = id;
        c.minYIndex = COARSE_Y_LEVELS;
        c.maxYIndex = 0;

        int head = 0;
        int tail = 0;
        queue[tail++] = start;
        labels[start] = id;

        while (head < tail) {
            int idx = queue[head++];
            int y = idx % COARSE_Y_LEVELS;
            int tmp = idx / COARSE_Y_LEVELS;
            int z = tmp % grid.size;
            int x = tmp / grid.size;

            double density = grid.values[idx];

            c.cells++;
            c.sumDensity += density;
            c.maxDensity = Math.max(c.maxDensity, density);
            if (y >= 8) c.cellsY8++;
            if (y >= 10) c.cellsY10++;
            if (y >= 12) c.cellsY12++;
            c.minYIndex = Math.min(c.minYIndex, y);
            c.maxYIndex = Math.max(c.maxYIndex, y);

            int colIndex = x * grid.size + z;
            if (workspace.coarseColumnSeen[colIndex] != id) {
                workspace.coarseColumnSeen[colIndex] = id;
                workspace.coarseComponentColumns[c.columnCount++] = colIndex;
                workspace.coarseColumnMinY[colIndex] = y;
            } else if (y < workspace.coarseColumnMinY[colIndex]) {
                workspace.coarseColumnMinY[colIndex] = y;
            }

            if (y == 0) {
                c.touchesBottom = true;
            }
            if (x == 0 || z == 0 || x == grid.size - 1 || z == grid.size - 1) {
                c.touchesSide = true;
            }

            for (int[] d : COARSE_DIRS_3D) {
                int nx = x + d[0];
                int ny = y + d[1];
                int nz = z + d[2];
                if (nx < 0 || nx >= grid.size || ny < 0 || ny >= COARSE_Y_LEVELS || nz < 0 || nz >= grid.size) {
                    continue;
                }

                int ni = grid.index(nx, ny, nz);
                if (labels[ni] == 0 && grid.values[ni] > 0.0D) {
                    labels[ni] = id;
                    queue[tail++] = ni;
                }
            }
        }

        return c;
    }

    private int countCoarseReentryColumnsForComponent(
            CoarseGrid grid,
            int[] labels,
            Workspace workspace,
            int componentId,
            int componentColumnCount
    ) {
        int count = 0;

        for (int i = 0; i < componentColumnCount; i++) {
            int colIndex = workspace.coarseComponentColumns[i];
            int x = colIndex / grid.size;
            int z = colIndex - x * grid.size;

            int componentMinYInColumn = workspace.coarseColumnMinY[colIndex];

            if (componentMinYInColumn <= 0 || componentMinYInColumn >= COARSE_Y_LEVELS) {
                continue;
            }

            int lowerPositiveY = -1;
            for (int y = componentMinYInColumn - 1; y >= 0; y--) {
                int idx = grid.index(x, y, z);
                if (grid.values[idx] > 0.0D && labels[idx] != componentId) {
                    lowerPositiveY = y;
                    break;
                }
            }

            if (lowerPositiveY < 0) {
                continue;
            }

            int gapSteps = componentMinYInColumn - lowerPositiveY - 1;
            if (gapSteps >= MIN_FLOATING_GAP_STEPS) {
                count++;
            }
        }

        return count;
    }

    private boolean isPossibleFloatingCoarseCandidate(CoarseComponent c) {
        return c.maxYIndex >= MIN_INTERESTING_COARSE_Y_INDEX
                && !c.touchesBottom
                && !c.touchesSide;
    }


    private boolean isMatch(Component c, SearchSettings settings) {
        if (c.blocks < settings.minBlocks) {
            return false;
        }

        if (c.columns < settings.minColumns) {
            return false;
        }

        if (c.maxY < settings.minYForMatch) {
            return false;
        }

        if (c.touchesBottom) {
            return false;
        }

        int width = c.maxWorldX - c.minWorldX + 1;
        int depth = c.maxWorldZ - c.minWorldZ + 1;

        if (width < settings.minWidth) {
            return false;
        }

        if (depth < settings.minDepth) {
            return false;
        }

        double avgThickness = c.blocks / (double) Math.max(1, c.columns);

        return avgThickness >= settings.minAvgThickness;
    }

private boolean isSampleCandidateColumn(int x, int z) {
    return (x == 4 || x == 12) && (z == 4 || z == 12);
}

private SearchSettings copySettingsForResearchExactAudit(SearchSettings settings) {
    SearchSettings copy = copySettingsWithRadius(settings, settings.chunkRadius);
    copy.hunterMode = false;
    copy.filterResearchEnabled = false;
    copy.savePreviews = false;
    copy.performanceProfilerEnabled = false;
    copy.featureLoggingEnabled = false;
    copy.hunterStage0AuditEnabled = false;
    return copy;
}

private SearchSettings copySettingsWithRadius(SearchSettings settings, int chunkRadius) {
    SearchSettings copy = new SearchSettings();

    copy.chunkRadius = chunkRadius;
    copy.seedsToCheck = settings.seedsToCheck;
    copy.threads = settings.threads;

    copy.minBlocks = settings.minBlocks;
    copy.minColumns = settings.minColumns;
    copy.minYForMatch = settings.minYForMatch;

    copy.minWidth = settings.minWidth;
    copy.minDepth = settings.minDepth;
    copy.minAvgThickness = settings.minAvgThickness;

    copy.topResultsToKeep = settings.topResultsToKeep;
    copy.savePreviews = settings.savePreviews;
    copy.hunterMode = settings.hunterMode;
    copy.megaMode = settings.megaMode;
    copy.huntProfile = settings.huntProfile;
    copy.recordHuntMode = settings.recordHuntMode;
    copy.extremeRecordHuntMode = settings.extremeRecordHuntMode;
    copy.record60P20MinPositiveColumns = settings.record60P20MinPositiveColumns;
    copy.record60UpperMinPositiveColumns = settings.record60UpperMinPositiveColumns;
    copy.record60HighMinReentryColumns = settings.record60HighMinReentryColumns;
    copy.record60MinP19Score = settings.record60MinP19Score;
    copy.record60CoarseMinCells = settings.record60CoarseMinCells;
    copy.record80P20MinPositiveColumns = settings.record80P20MinPositiveColumns;
    copy.record80UpperMinPositiveColumns = settings.record80UpperMinPositiveColumns;
    copy.record80HighMinReentryColumns = settings.record80HighMinReentryColumns;
    copy.record80MinP19Score = settings.record80MinP19Score;
    copy.record80CoarseMinCells = settings.record80CoarseMinCells;
    copy.megaUpperMinPositiveColumns = settings.megaUpperMinPositiveColumns;
    copy.megaHighMinReentryColumns = settings.megaHighMinReentryColumns;
    copy.megaTopologyMaxP19ScoreExclusive = settings.megaTopologyMaxP19ScoreExclusive;
    copy.megaTopologyMinY96LargestCluster = settings.megaTopologyMinY96LargestCluster;
    copy.megaTopologyRejectWhenFullY112Zero = settings.megaTopologyRejectWhenFullY112Zero;
    copy.filterResearchEnabled = settings.filterResearchEnabled;
    copy.hunterCoarseMinCells = settings.hunterCoarseMinCells;
    copy.hunterStage0Enabled = settings.hunterStage0Enabled;
    copy.hunterStage0Step = settings.hunterStage0Step;
    copy.hunterStage0MinReentrySamples = settings.hunterStage0MinReentrySamples;
    copy.hunterStage0MinUpperYIndex = settings.hunterStage0MinUpperYIndex;
    copy.hunterStage0HighEnabled = settings.hunterStage0HighEnabled;
    copy.hunterStage0HighMinUpperYIndex = settings.hunterStage0HighMinUpperYIndex;
    copy.hunterStage0HighMinReentrySamples = settings.hunterStage0HighMinReentrySamples;
    copy.hunterStage0AuditEnabled = settings.hunterStage0AuditEnabled;
    copy.hunterStage0AuditSampleMask = settings.hunterStage0AuditSampleMask;
    copy.performanceProfilerEnabled = settings.performanceProfilerEnabled;
    copy.performanceProfileSampleMask = settings.performanceProfileSampleMask;
    copy.deterministicSeedMode = settings.deterministicSeedMode;
    copy.deterministicSeedSequenceSeed = settings.deterministicSeedSequenceSeed;
    copy.runLabel = settings.runLabel;
    copy.featureLoggingEnabled = settings.featureLoggingEnabled;
    copy.debugLogInterval = settings.debugLogInterval;

    return copy;
    }

    private int compareComponents(Component a, Component b) {
        int blockCompare = Integer.compare(a.blocks, b.blocks);
        if (blockCompare != 0) {
            return blockCompare;
        }

        int columnCompare = Integer.compare(a.columns, b.columns);
        if (columnCompare != 0) {
            return columnCompare;
        }

        int areaA = (a.maxWorldX - a.minWorldX + 1) * (a.maxWorldZ - a.minWorldZ + 1);
        int areaB = (b.maxWorldX - b.minWorldX + 1) * (b.maxWorldZ - b.minWorldZ + 1);

        return Integer.compare(areaA, areaB);
    }

    private Component floodFillFlat(
        byte[] stone,
        byte[] visited,
        int[] queue,
        int[] columnSeen,
        int columnMark,
        int startX,
        int startY,
        int startZ,
        int sizeX,
        int sizeY,
        int sizeZ,
        int minWorldX,
        int minWorldZ,
        int floodMinY

) {
    int yzArea = sizeY * sizeZ;
    int offsetX = yzArea;
    int offsetY = sizeZ;
    int offsetZ = 1;

    int head = 0;
    int tail = 0;

    int startIndex = index3(startX, startY, startZ, sizeY, sizeZ);

    queue[tail++] = startIndex;
    visited[startIndex] = 1;

    Component c = new Component();

    while (head < tail) {
        int index = queue[head++];

        int x = index / yzArea;
        int remainder = index - x * yzArea;
        int y = remainder / sizeZ;
        int z = remainder - y * sizeZ;

        int worldX = minWorldX + x;
        int worldZ = minWorldZ + z;

        c.blocks++;

        int colIndex = x * sizeZ + z;

        if (columnSeen[colIndex] != columnMark) {
            columnSeen[colIndex] = columnMark;
            c.columns++;
            c.sumWorldX += worldX;
            c.sumWorldZ += worldZ;
        }

        c.minWorldX = Math.min(c.minWorldX, worldX);
        c.maxWorldX = Math.max(c.maxWorldX, worldX);
        c.minWorldZ = Math.min(c.minWorldZ, worldZ);
        c.maxWorldZ = Math.max(c.maxWorldZ, worldZ);
        c.minY = Math.min(c.minY, y);
        c.maxY = Math.max(c.maxY, y);

        if (y == 0) {
            c.touchesBottom = true;
        }

        if (x == 0 || x == sizeX - 1 || z == 0 || z == sizeZ - 1) {
            c.touchesSideBorder = true;
        }

        if (x > 0) {
            tail = tryAddNeighbor(index - offsetX, stone, visited, queue, tail);
        }

        if (x < sizeX - 1) {
            tail = tryAddNeighbor(index + offsetX, stone, visited, queue, tail);
        }

        if (y > floodMinY) {
            tail = tryAddNeighbor(index - offsetY, stone, visited, queue, tail);
        } else {
            if (y > 0 && stone[index - offsetY] != 0) {
                c.touchesBottom = true;
            }
        }

        if (y < sizeY - 1) {
            tail = tryAddNeighbor(index + offsetY, stone, visited, queue, tail);
        }

        if (z > 0) {
            tail = tryAddNeighbor(index - offsetZ, stone, visited, queue, tail);
        }

        if (z < sizeZ - 1) {
            tail = tryAddNeighbor(index + offsetZ, stone, visited, queue, tail);
        }
    }

    if (c.columns > 0) {
        c.centerX = (int) Math.round((double) c.sumWorldX / c.columns);
        c.centerZ = (int) Math.round((double) c.sumWorldZ / c.columns);
    }

    c.floodQueueLength = tail;

    return c;
    }

    private int tryAddNeighbor(
        int index,
        byte[] stone,
        byte[] visited,
        int[] queue,
        int tail
) {
    if (visited[index] != 0) {
        return tail;
    }

    if (stone[index] == 0) {
        return tail;
    }

    visited[index] = 1;
    queue[tail++] = index;

    return tail;
}

private int index3(int x, int y, int z, int sizeY, int sizeZ) {
    return (x * sizeY + y) * sizeZ + z;
}


    private void initializeRunOutput(SearchSettings settings) throws Exception {
        Path outRoot = AppPaths.outputRoot();
        Path runsRoot = outRoot.resolve("runs");
        Files.createDirectories(runsRoot);

        String baseName = LocalDateTime.now().format(RUN_TIME_FORMAT);
        if (settings.runLabel != null && !settings.runLabel.trim().isEmpty()) {
            baseName += "_" + sanitizeRunLabel(settings.runLabel.trim());
        }

        Path candidate = runsRoot.resolve(baseName);
        int suffix = 1;
        while (Files.exists(candidate)) {
            candidate = runsRoot.resolve(baseName + "_" + String.format(Locale.US, "%02d", suffix++));
        }

        runOutputDir = candidate;
        Files.createDirectories(runOutputDir);

        candidateCsvPath = runOutputDir.resolve("hunter_candidates.csv");
        featureCsvPath = runOutputDir.resolve("hunter_features.csv");
        stage0AuditCsvPath = runOutputDir.resolve("hunter_stage0_audit.csv");
        recordRejectAuditCsvPath = runOutputDir.resolve("record_reject_audits.csv");
        topResultsPath = runOutputDir.resolve("top_largest_results.txt");
        legacyTopResultsPath = runOutputDir.resolve("top_floating_results.txt");
        topFilledFootprintPath = runOutputDir.resolve("top_filled_footprint_results.txt");
        topRawFootprintPath = runOutputDir.resolve("top_raw_footprint_results.txt");
        topSideboardsCsvPath = runOutputDir.resolve("top_island_sideboards.csv");
        manifestPath = runOutputDir.resolve("manifest.txt");
        performancePath = runOutputDir.resolve("performance_profile.txt");
        eventLogPath = runOutputDir.resolve("event_log.txt");

        Files.writeString(
                topResultsPath,
                "Top largest floating islands\nNo results yet.\n",
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
        Files.writeString(
                legacyTopResultsPath,
                "Top largest floating islands\nNo results yet.\n",
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
        Files.writeString(
                topFilledFootprintPath,
                "Top filled-footprint floating islands\nNo results yet.\n",
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
        Files.writeString(
                topRawFootprintPath,
                "Top raw-footprint floating islands\nNo results yet.\n",
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
        Files.writeString(
                topSideboardsCsvPath,
                "board,rank,seed,blocks,columns,width,depth,footprintArea,fillPercent,avgThickness,minY,maxY,centerX,centerZ,chunkRadiusUsed,touchesSideBorder\n",
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );

        Files.writeString(
                outRoot.resolve("latest_run.txt"),
                runOutputDir.toAbsolutePath().normalize().toString() + System.lineSeparator(),
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
    }

    private String sanitizeRunLabel(String label) {
        String cleaned = label.replaceAll("[^A-Za-z0-9._-]+", "_");
        if (cleaned.length() > 40) {
            cleaned = cleaned.substring(0, 40);
        }
        return cleaned.isEmpty() ? "run" : cleaned;
    }

    private void writeManifest(
            SearchSettings settings,
            String status,
            long checked,
            int matches,
            int topUpdates,
            long startTime,
            long endTime
    ) {
        try {
            StringBuilder sb = new StringBuilder();
            sb.append("BetaSeedFinder run manifest\n");
            sb.append("build=").append(BUILD_ID).append("\n");
            sb.append("status=").append(status).append("\n");
            sb.append("runFolder=").append(runOutputDir.toAbsolutePath().normalize()).append("\n");
            sb.append("runLabel=").append(settings.runLabel == null ? "" : settings.runLabel).append("\n");
            sb.append("startEpochMs=").append(startTime).append("\n");
            sb.append("endEpochMs=").append(endTime).append("\n");
            if (endTime > startTime) {
                sb.append("elapsedSeconds=").append(String.format(Locale.US, "%.3f", (endTime - startTime) / 1000.0)).append("\n");
            }
            sb.append("checked=").append(checked).append("\n");
            sb.append("matches=").append(matches).append("\n");
            sb.append("topUpdates=").append(topUpdates).append("\n");
            sb.append("checkedUnit=").append(p28CoverageActive ? "radius7Regions" : "seedWorlds").append("\n");
            sb.append("regionBudget=").append(settings.seedsToCheck).append("\n");
            sb.append("gpuCoverageCentersPerWorld=").append(p28CoverageActive ? GpuStage0Scout.CENTER_COUNT : 1).append("\n");
            sb.append("gpuCoverageCenterSpacingChunks=").append(p28CoverageActive ? 15 : 0).append("\n");
            sb.append("gpuProtocol=").append(p28CoverageActive ? "ST0R3708" : "fallback").append("\n");
            sb.append("gpuTerrainPerlinCacheStates=").append(p28CoverageActive ? 66 : 0).append("\n");
            sb.append("gpuClimatePerlinCacheStates=").append(p28CoverageActive ? 10 : 0).append("\n");
            sb.append("gpuCachedCoarseInitialization=").append(p28CoverageActive).append("\n");
            sb.append("gpuCompactLowerThreads=").append(p28CoverageActive ? 32 : 0).append("\n");
            sb.append("gpuCompactUpperThreads=").append(p28CoverageActive ? 192 : 0).append("\n");
            sb.append("gpuSharedUpperYAxis=").append(p28CoverageActive).append("\n");
            sb.append("gpuDirectWriteNoise23=").append(p28CoverageActive).append("\n\n");

            sb.append("chunkRadius=").append(settings.chunkRadius).append("\n");
            sb.append("seedsToCheck=").append(settings.seedsToCheck).append("\n");
            sb.append("threads=").append(settings.threads).append("\n");
            sb.append("gpuStage0Enabled=").append(settings.gpuStage0Enabled).append("\n");
            sb.append("gpuStage0BatchSize=").append(settings.gpuStage0BatchSize).append("\n");
            sb.append("gpuStage0Compatible=").append(isGpuStage0Compatible(settings)).append("\n");
            sb.append("gpuStage0Executable=").append(GpuStage0Scout.executablePath()).append("\n");
            sb.append("gpuP20Enabled=").append(settings.gpuP20Enabled).append("\n");
            sb.append("gpuP20BatchSize=").append(settings.gpuP20BatchSize).append("\n");
            sb.append("gpuP20Compatible=").append(isGpuP20Compatible(settings)).append("\n");
            sb.append("gpuP20Executable=").append(GpuP20Scout.executablePath()).append("\n");
            sb.append("minBlocks=").append(settings.minBlocks).append("\n");
            sb.append("minColumns=").append(settings.minColumns).append("\n");
            sb.append("minYForMatch=").append(settings.minYForMatch).append("\n");
            sb.append("minWidth=").append(settings.minWidth).append("\n");
            sb.append("minDepth=").append(settings.minDepth).append("\n");
            sb.append("minAvgThickness=").append(settings.minAvgThickness).append("\n");
            sb.append("topResultsToKeep=").append(settings.topResultsToKeep).append("\n");
            sb.append("savePreviews=").append(settings.savePreviews).append("\n");
            sb.append("featureLoggingEnabled=").append(settings.featureLoggingEnabled).append("\n");
            sb.append("featureLogMinCoarseAlways=").append(FEATURE_LOG_MIN_COARSE_ALWAYS).append("\n");
            sb.append("featureLogLowCoarseSampleMask=").append(FEATURE_LOG_LOW_COARSE_SAMPLE_MASK).append("\n");
            sb.append("filterShadowResearchEnabled=").append(shadowResearch != null && shadowResearch.isEnabled()).append("\n");
            sb.append("filterShadowP21SampleMask=").append(FilterShadowResearch.P21_SAMPLE_MASK).append("\n");
            sb.append("overnightResearchTelemetryProtocol=ST0R3708\n");
            sb.append("nativeResponseMode=").append(settings.filterResearchEnabled ? "research" : "lean").append("\n");
            sb.append("nativeResponseBytesPerRegion=").append(settings.filterResearchEnabled ? 34 : 8).append("\n");
            sb.append("overnightResearchNativeBaseSampleMask=").append(FilterShadowResearch.NATIVE_BASE_SAMPLE_MASK).append("\n");
            sb.append("overnightResearchForcedAuditsEnabled=").append(shadowResearch != null && shadowResearch.isEnabled()).append("\n");
            sb.append("overnightResearchAuditInFlightCap=").append(Math.max(2, settings.threads * 2)).append("\n");
            sb.append("overnightResearchBackendBatchEvery=32\n");
            sb.append("overnightResearchNativeSamples=").append(shadowResearch == null ? "" : shadowResearch.getNativeSamplePath()).append("\n");
            sb.append("overnightResearchForcedAuditCsv=").append(shadowResearch == null ? "" : shadowResearch.getAuditPath()).append("\n");
            sb.append("overnightResearchHistograms=").append(shadowResearch == null ? "" : shadowResearch.getHistogramPath()).append("\n");
            sb.append("overnightResearchBackendBatches=").append(shadowResearch == null ? "" : shadowResearch.getBackendBatchPath()).append("\n");
            sb.append("debugLogInterval=").append(getDebugLogInterval(settings)).append("\n\n");

            sb.append("hunterMode=").append(settings.hunterMode).append("\n");
            sb.append("huntProfile=").append(settings.huntProfile).append("\n");
            sb.append("huntProfileName=").append(getHuntProfileName(settings)).append("\n");
            sb.append("megaMode=").append(settings.megaMode).append("\n");
            sb.append("recordHuntMode=").append(settings.recordHuntMode).append("\n");
            sb.append("extremeRecordHuntMode=").append(settings.extremeRecordHuntMode).append("\n");
            sb.append("effectiveP20MinPositiveColumns=").append(getEffectiveP20MinSamples(settings)).append("\n");
            sb.append("effectiveP19MinScore=").append(getEffectiveP19MinScore(settings)).append("\n");
            sb.append("filterResearchEnabled=").append(settings.filterResearchEnabled).append("\n");
            sb.append("megaUpperMinPositiveColumns=").append(getEffectiveUpperMinSamples(settings)).append("\n");
            sb.append("megaHighMinReentryColumns=").append(getEffectiveHighMinSamples(settings)).append("\n");
            sb.append("megaTopologyMaxP19ScoreExclusive=").append(settings.megaTopologyMaxP19ScoreExclusive).append("\n");
            sb.append("megaTopologyMinY96LargestCluster=").append(settings.megaTopologyMinY96LargestCluster).append("\n");
            sb.append("megaTopologyRejectWhenFullY112Zero=").append(settings.megaTopologyRejectWhenFullY112Zero).append("\n");
            sb.append("megaExtremeTopologyBypassPreserved=true\n");
            sb.append("hunterCoarseMinCells=").append(getHunterCoarseThreshold(settings)).append("\n");
            sb.append("stage0Enabled=").append(isStage0Enabled(settings)).append("\n");
            sb.append("stage0Step=").append(getStage0Step(settings)).append("\n");
            sb.append("stage0MinReentry=").append(getStage0MinReentrySamples(settings)).append("\n");
            sb.append("stage0MinUpperYIndex=").append(getStage0MinUpperYIndex(settings)).append("\n");
            sb.append("stage0HighEnabled=").append(isStage0HighEnabled(settings)).append("\n");
            sb.append("stage0HighConfiguredMinReentry=").append(getStage0HighMinReentrySamples(settings)).append("\n");
            sb.append("stage0HighProductionMinReentry=").append(getEffectiveHighMinSamples(settings)).append("\n");
            sb.append("stage0UpperProductionMinPositiveColumns=").append(getEffectiveUpperMinSamples(settings)).append("\n");
            sb.append("stage0HighMinUpperYIndex=").append(getStage0HighMinUpperYIndex(settings)).append("\n");
            sb.append("stage0FusedActive=").append(settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings)).append("\n");
            sb.append("stage025Progressive64PreScoutActive=").append(settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings)).append("\n");
            sb.append("stage025RejectRule=upperPositiveColumns==0\n");
            sb.append("stage025AuditSampleMask=").append(P20_PROGRESSIVE_SCOUT_AUDIT_MASK).append("\n");
            sb.append("stage0UpperYScoutActive=").append(settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings)).append("\n");
            sb.append("stage0CandidateColumnCompletionActive=").append(settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings)).append("\n");
            sb.append("stage075MonsterGateActive=").append(settings.hunterMode && isStage0Enabled(settings) && isStage0HighEnabled(settings)).append("\n");
            sb.append("stage075MonsterGateThreshold=").append(P19MonsterGate.THRESHOLD).append("\n");
            sb.append("stage075MonsterGateTrees=16\n");
            sb.append("stage0AuditEnabled=").append(isStage0AuditEnabled(settings)).append("\n");
            sb.append("stage0AuditSampleMask=").append(getStage0AuditMask(settings)).append("\n");
            sb.append("recordRejectAuditPath=").append(recordRejectAuditCsvPath).append("\n");
            sb.append("recordRejectAuditExactFeatures=true\n");
            sb.append("leanTelemetryP19Score=exactDouble\n");
            sb.append("borderVerifyRadii=");
            for (int i = 0; i < BORDER_VERIFY_RADII.length; i++) {
                if (i > 0) sb.append(',');
                sb.append(BORDER_VERIFY_RADII[i]);
            }
            sb.append("\n");
            sb.append("borderVerifyMaxRadius=").append(BORDER_VERIFY_MAX_RADIUS).append("\n");
            sb.append("dangerousRejectExactScanMinCoarse=").append(getHunterCoarseThreshold(settings)).append("\n");
            sb.append("overnightResearchForcedAuditRateMultiplier=").append(FilterShadowResearch.FORCED_AUDIT_RATE_MULTIPLIER).append("\n");
            sb.append("hotSeedScanEnabled=").append(HOT_SEED_SCAN_ENABLED).append("\n");
            sb.append("hotSeedCoarseThreshold=").append(HOT_SEED_COARSE_THRESHOLD).append("\n");
            sb.append("hotSeedScanRadius=").append(HOT_SEED_SCAN_RADIUS).append("\n\n");

            sb.append("performanceProfilerEnabled=").append(settings.performanceProfilerEnabled).append("\n");
            sb.append("performanceProfileSampleMask=").append(getPerformanceProfileMask(settings)).append("\n");
            sb.append("deterministicSeedMode=").append(settings.deterministicSeedMode).append("\n");
            sb.append("deterministicSeedSequenceSeed=").append(settings.deterministicSeedSequenceSeed).append("\n");

            Files.writeString(
                    manifestPath,
                    sb.toString(),
                    StandardOpenOption.CREATE,
                    StandardOpenOption.TRUNCATE_EXISTING
            );
        } catch (Exception ignored) {
            // Manifest failure must never kill a search.
        }
    }

    private void writePerformanceReport(SearchListener listener, long checked, long elapsedMs) {
        if (perfStats == null || !perfStats.enabled) {
            try {
                Files.writeString(
                        performancePath,
                        "Performance profiler disabled.\n",
                        StandardOpenOption.CREATE,
                        StandardOpenOption.TRUNCATE_EXISTING
                );
            } catch (Exception ignored) {
            }
            return;
        }

        String report = perfStats.formatReport(checked, elapsedMs);
        try {
            Files.writeString(
                    performancePath,
                    report,
                    StandardOpenOption.CREATE,
                    StandardOpenOption.TRUNCATE_EXISTING
            );
        } catch (Exception ignored) {
        }

        for (String line : report.split("\\R")) {
            if (!line.isEmpty()) {
                listener.onLog(line);
            }
        }
    }

    private void publishLatestRunCopies() {
        try {
            Path outRoot = AppPaths.outputRoot();
            Files.createDirectories(outRoot);
            copyLatest(candidateCsvPath, outRoot.resolve("hunter_candidates.csv"));
            copyLatest(featureCsvPath, outRoot.resolve("hunter_features.csv"));
            copyLatest(stage0AuditCsvPath, outRoot.resolve("hunter_stage0_audit.csv"));
            copyLatest(recordRejectAuditCsvPath, outRoot.resolve("record_reject_audits.csv"));
            if (shadowResearch != null && shadowResearch.isEnabled()) {
                copyLatest(shadowResearch.getSummaryPath(), outRoot.resolve("filter_shadow_summary.csv"));
                copyLatest(shadowResearch.getCandidatePath(), outRoot.resolve("filter_shadow_candidates.csv"));
                copyLatest(shadowResearch.getConfigPath(), outRoot.resolve("filter_shadow_config.txt"));
                copyLatest(shadowResearch.getP21SamplePath(), outRoot.resolve("p21_shadow_samples.csv"));
                copyLatest(shadowResearch.getNativeSamplePath(), outRoot.resolve("filter_research_native_samples.csv"));
                copyLatest(shadowResearch.getAuditPath(), outRoot.resolve("filter_research_forced_audits.csv"));
                copyLatest(shadowResearch.getHistogramPath(), outRoot.resolve("filter_research_histograms.csv"));
                copyLatest(shadowResearch.getBackendBatchPath(), outRoot.resolve("backend_pipeline_batches.csv"));
            }
            copyLatest(topResultsPath, outRoot.resolve("top_largest_results.txt"));
            copyLatest(topResultsPath, outRoot.resolve("top_floating_results.txt"));
            copyLatest(topFilledFootprintPath, outRoot.resolve("top_filled_footprint_results.txt"));
            copyLatest(topRawFootprintPath, outRoot.resolve("top_raw_footprint_results.txt"));
            copyLatest(topSideboardsCsvPath, outRoot.resolve("top_island_sideboards.csv"));
            copyLatest(manifestPath, outRoot.resolve("latest_manifest.txt"));
            copyLatest(performancePath, outRoot.resolve("latest_performance_profile.txt"));
            copyLatest(eventLogPath, outRoot.resolve("latest_event_log.txt"));
        } catch (Exception ignored) {
            // Latest mirrors are convenience copies only.
        }
    }

    private void copyLatest(Path source, Path target) throws Exception {
        if (source != null && Files.exists(source)) {
            Files.copy(source, target, StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private void initializeFeatureCsv() throws Exception {
        String header = "attempt,seed,sampleKind,stage0,stage0Y88,coarse,stage0ScoutPositiveColumns,stage0LowerCandidateColumns,p17GeneratedPoints,p17LowerColumnsGenerated,p17ExpansionRounds,p17FallbackToFull,bestCoarseColumns,bestCoarseReentry,bestCoarseMinYIndex,bestCoarseMaxYIndex,bestCoarseCellsY8,bestCoarseCellsY10,bestCoarseCellsY12,bestCoarseMaxDensity,bestCoarseAvgPositiveDensity,totalPositiveCells,positiveCellsY8,positiveCellsY10,positiveCellsY12,columnsPositiveY8,columnsPositiveY10,columnsPositiveY12,totalPositiveComponents,possibleFloatingComponents,stage0FullY64,stage0FullY72,stage0FullY80,stage0FullY88,stage0FullY96,stage0FullY104,stage0FullY112,stage0Y88LargestCluster,stage0Y88Width,stage0Y88Depth,stage0Y88TouchesBorder,stage0Y96LargestCluster,stage0Y96Width,stage0Y96Depth,stage0Y96TouchesBorder\n";
        Files.writeString(
                featureCsvPath,
                header,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
    }

    private void initializeRecordRejectAuditCsv() throws Exception {
        String header = "attempt,seed,searchCenterChunkX,searchCenterChunkZ,rejectStage,rejectReason,p20,upper,high,p19Pass,p19Score,p19Extreme,coarse,exactScanMs,exactMatched,exactBlocks,exactColumns,exactWidth,exactDepth,exactMinY,exactMaxY,exactRadiusUsed,exactTouchesSideBorder\n";
        Files.writeString(
                recordRejectAuditCsvPath,
                header,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
    }

    private void initializeStage0AuditCsv() throws Exception {
        String header = "attempt,seed,auditReason,stage0,stage0Y88,coarse,stage0ScoutPositiveColumns,stage0LowerCandidateColumns,p17GeneratedPoints,p17LowerColumnsGenerated,p17ExpansionRounds,p17FallbackToFull,bestCoarseColumns,bestCoarseReentry,bestCoarseMinYIndex,bestCoarseMaxYIndex,bestCoarseCellsY8,bestCoarseCellsY10,bestCoarseCellsY12,bestCoarseMaxDensity,bestCoarseAvgPositiveDensity,totalPositiveCells,positiveCellsY8,positiveCellsY10,positiveCellsY12,columnsPositiveY8,columnsPositiveY10,columnsPositiveY12,totalPositiveComponents,possibleFloatingComponents,stage0FullY64,stage0FullY72,stage0FullY80,stage0FullY88,stage0FullY96,stage0FullY104,stage0FullY112,stage0Y88LargestCluster,stage0Y88Width,stage0Y88Depth,stage0Y88TouchesBorder,stage0Y96LargestCluster,stage0Y96Width,stage0Y96Depth,stage0Y96TouchesBorder,exactAutoScanned,exactScanMs,exactMatched,exactBlocks,exactColumns,exactWidth,exactDepth,exactMinY,exactMaxY,exactRadiusUsed,exactTouchesSideBorder\n";
        Files.writeString(
                stage0AuditCsvPath,
                header,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
    }

    private void maybeAppendFeatureCsv(
            long attempt,
            long seed,
            SearchSettings settings,
            BetaTerrain173 terrain,
            Workspace workspace,
            CoarseGrid coarseGrid,
            int stage0Score,
            int stage0HighScore,
            int coarseScore,
            int stage0ScoutPositiveColumns,
            int stage0LowerCandidateColumns
    ) {
        if (!settings.featureLoggingEnabled) {
            return;
        }

        boolean alwaysLog = coarseScore >= FEATURE_LOG_MIN_COARSE_ALWAYS;
        boolean sampledLow = !alwaysLog && ((seed & FEATURE_LOG_LOW_COARSE_SAMPLE_MASK) == 0L);

        if (!alwaysLog && !sampledLow) {
            return;
        }

        long featureStartNs = perfStats != null && perfStats.enabled ? System.nanoTime() : 0L;
        int p17GeneratedPoints = getP17GeneratedPointCount(coarseGrid);
        int p17LowerColumnsGenerated = coarseGrid.lazyLowerColumnsGenerated;
        int p17ExpansionRounds = coarseGrid.lazyExpansionRounds;
        boolean p17FallbackToFull = coarseGrid.lazyFallbackToFull;
        try {
            // Rich analysis is paid only by rows that are actually written. P17
            // completes any intentionally omitted low-only terrain first so the
            // feature CSV remains bit-for-bit equivalent to the full-grid path.
            completeLazyCoarseGridForRichFeatures(terrain, coarseGrid, workspace);
            CoarseFeatureSummary coarseFeatures = findCoarseFeatureSummary(coarseGrid, workspace);
            if (coarseFeatures.bestCells != coarseScore) {
                throw new IllegalStateException(
                        "Fast/rich coarse mismatch for seed " + seed
                                + ": fast=" + coarseScore
                                + " rich=" + coarseFeatures.bestCells
                );
            }

            try {
                String sampleKind;
                if (coarseScore >= HOT_SEED_COARSE_THRESHOLD) {
                    sampleKind = "hot_coarse";
                } else if (coarseScore >= getHunterCoarseThreshold(settings)) {
                    sampleKind = "exact_candidate";
                } else if (alwaysLog) {
                    sampleKind = "coarse_30plus";
                } else {
                    sampleKind = "low_sample_1of256";
                }

                BetaTerrain173.SparseReentryStats stage0Stats = terrain.analyzeSparseCoarseReentryAroundZero(
                        settings.chunkRadius,
                        getStage0Step(settings)
                );

                String line = formatStage0FeatureLine(
                        attempt,
                        seed,
                        sampleKind,
                        stage0Score,
                        stage0HighScore,
                        stage0ScoutPositiveColumns,
                        stage0LowerCandidateColumns,
                        p17GeneratedPoints,
                        p17LowerColumnsGenerated,
                        p17ExpansionRounds,
                        p17FallbackToFull,
                        coarseFeatures,
                        stage0Stats
                );

                synchronized (featureCsvLock) {
                    Files.writeString(
                            featureCsvPath,
                            line + System.lineSeparator(),
                            StandardOpenOption.CREATE,
                            StandardOpenOption.APPEND
                    );
                }
            } catch (Exception ignored) {
                // File/diagnostic failures must never kill the search.
            }
        } finally {
            if (featureStartNs != 0L) {
                perfStats.recordFeatureLogging(System.nanoTime() - featureStartNs);
            }
        }
    }

    private void maybeAppendStage0AuditCsv(
            long attempt,
            long seed,
            SearchSettings settings,
            BetaTerrain173 terrain,
            Workspace workspace,
            DebugStats debugStats,
            String auditReason,
            int stage0Score,
            int stage0HighScore,
            int stage0ScoutPositiveColumns,
            int stage0LowerCandidateColumns
    ) {
        if (attempt < 0) {
            return;
        }

        // P19 gate rejects are sampled by attempt, not seed bits. This keeps the
        // audit independent from terrain features used by the model and gives a
        // uniform sample of rejected positions in the search stream.
        boolean stage025Audit = auditReason.startsWith("stage025_");
        boolean stage075Audit = auditReason.startsWith("stage075_");
        boolean sampled = stage025Audit
                ? isStage0AuditEnabled(settings) && ((attempt & getP20AuditMask(settings)) == 0L)
                : stage075Audit
                    ? isStage0AuditEnabled(settings) && ((attempt & getStage0AuditMask(settings)) == 0L)
                    : shouldAuditStage0Reject(seed, settings);
        if (!sampled) {
            return;
        }

        long auditStartNs = perfStats != null && perfStats.enabled ? System.nanoTime() : 0L;
        try {
            CoarseFeatureSummary coarseFeatures = getCoarseFeatures(terrain, settings.chunkRadius, workspace);
            BetaTerrain173.SparseReentryStats stage0Stats = terrain.analyzeSparseCoarseReentryAroundZero(
                    settings.chunkRadius,
                    getStage0Step(settings)
            );

            if (auditReason.startsWith("stage025_")) {
                debugStats.recordStage025AuditRejectedCoarse(coarseFeatures.bestCells);
            } else if (auditReason.startsWith("stage075_")) {
                debugStats.recordStage075AuditRejectedCoarse(coarseFeatures.bestCells);
            } else {
                debugStats.recordStage0AuditRejectedCoarse(coarseFeatures.bestCells);
            }

            boolean exactAutoScanned = coarseFeatures.bestCells >= getHunterCoarseThreshold(settings);
            MatchResult exactAuditResult = null;
            double exactScanMs = 0.0D;

            if (exactAutoScanned) {
                long exactAuditStartNs = System.nanoTime();
                SearchSettings exactAuditSettings = copySettingsForResearchExactAudit(settings);
                exactAuditResult = findFloatingIslandInSeed(
                        seed,
                        exactAuditSettings,
                        new Workspace(),
                        new DebugStats(),
                        true,
                        false,
                        -1L,
                        false
                );
                exactScanMs = (System.nanoTime() - exactAuditStartNs) / 1_000_000.0D;
            }

            Component exactComponent = exactAuditResult == null ? null : exactAuditResult.component;
            int exactWidth = exactComponent == null ? 0 : exactComponent.maxWorldX - exactComponent.minWorldX + 1;
            int exactDepth = exactComponent == null ? 0 : exactComponent.maxWorldZ - exactComponent.minWorldZ + 1;

            String line = formatStage0FeatureLine(
                    attempt,
                    seed,
                    auditReason,
                    stage0Score,
                    stage0HighScore,
                    stage0ScoutPositiveColumns,
                    stage0LowerCandidateColumns,
                    -1,
                    -1,
                    -1,
                    false,
                    coarseFeatures,
                    stage0Stats
            )
                    + "," + exactAutoScanned
                    + "," + String.format(Locale.US, "%.3f", exactScanMs)
                    + "," + (exactAuditResult != null)
                    + "," + (exactComponent == null ? 0 : exactComponent.blocks)
                    + "," + (exactComponent == null ? 0 : exactComponent.columns)
                    + "," + exactWidth
                    + "," + exactDepth
                    + "," + (exactComponent == null ? -1 : exactComponent.minY)
                    + "," + (exactComponent == null ? -1 : exactComponent.maxY)
                    + "," + (exactAuditResult == null ? 0 : exactAuditResult.chunkRadiusUsed)
                    + "," + (exactComponent != null && exactComponent.touchesSideBorder)
                    + System.lineSeparator();

            synchronized (stage0AuditCsvLock) {
                Files.writeString(
                        stage0AuditCsvPath,
                        line,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND
                );
            }
        } catch (Exception ignored) {
            // Audit logging is diagnostic only. Never let it kill the search.
        } finally {
            if (auditStartNs != 0L) {
                perfStats.recordAudit(System.nanoTime() - auditStartNs);
            }
        }
    }

    private String formatStage0FeatureLine(
            long attempt,
            long seed,
            String sampleKind,
            int stage0Score,
            int stage0HighScore,
            int stage0ScoutPositiveColumns,
            int stage0LowerCandidateColumns,
            int p17GeneratedPoints,
            int p17LowerColumnsGenerated,
            int p17ExpansionRounds,
            boolean p17FallbackToFull,
            CoarseFeatureSummary coarseFeatures,
            BetaTerrain173.SparseReentryStats stage0Stats
    ) {
        return String.format(
                Locale.US,
                "%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%s",
                attempt,
                seed,
                sampleKind,
                stage0Score,
                stage0HighScore,
                coarseFeatures.bestCells,
                stage0ScoutPositiveColumns,
                stage0LowerCandidateColumns,
                p17GeneratedPoints,
                p17LowerColumnsGenerated,
                p17ExpansionRounds,
                p17FallbackToFull,
                coarseFeatures.bestColumns,
                coarseFeatures.bestReentryColumns,
                coarseFeatures.bestMinYIndex,
                coarseFeatures.bestMaxYIndex,
                coarseFeatures.bestCellsY8,
                coarseFeatures.bestCellsY10,
                coarseFeatures.bestCellsY12,
                coarseFeatures.bestMaxDensity,
                coarseFeatures.bestAvgPositiveDensity,
                coarseFeatures.totalPositiveCells,
                coarseFeatures.positiveCellsY8,
                coarseFeatures.positiveCellsY10,
                coarseFeatures.positiveCellsY12,
                coarseFeatures.columnsPositiveY8,
                coarseFeatures.columnsPositiveY10,
                coarseFeatures.columnsPositiveY12,
                coarseFeatures.totalPositiveComponents,
                coarseFeatures.possibleFloatingComponents,
                stage0Stats.stage0FullY64,
                stage0Stats.stage0FullY72,
                stage0Stats.stage0FullY80,
                stage0Stats.stage0FullY88,
                stage0Stats.stage0FullY96,
                stage0Stats.stage0FullY104,
                stage0Stats.stage0FullY112,
                stage0Stats.stage0Y88LargestCluster,
                stage0Stats.stage0Y88Width,
                stage0Stats.stage0Y88Depth,
                stage0Stats.stage0Y88TouchesBorder,
                stage0Stats.stage0Y96LargestCluster,
                stage0Stats.stage0Y96Width,
                stage0Stats.stage0Y96Depth,
                stage0Stats.stage0Y96TouchesBorder
        );
    }

    private void initializeCandidateCsv() throws Exception {
        String header = "attempt,seed,stage0,stage0Y88,coarse,p20,upper,high,p19Pass,p19Score,p19Extreme,searchCenterChunkX,searchCenterChunkZ,matched,status,rawBlocks,verifiedBlocks,rawRadius,verifiedRadius,rawTouchesSide,verifiedTouchesSide,columns,width,depth,footprintArea,fillPercent,avgThickness,minY,maxY,centerX,centerZ,minX,maxX,minZ,maxZ,hotScanned,hotRadius,hotBlocks,hotTouchesSide,finalSource\n";
        Files.writeString(
                candidateCsvPath,
                header,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
    }

    private void appendCandidateCsv(
            long attempt,
            long seed,
            int stage0Score,
            int stage0HighScore,
            int coarseScore,
            int p20Count,
            int upperCount,
            int highCount,
            boolean p19Pass,
            double p19Score,
            boolean p19Extreme,
            int searchCenterChunkX,
            int searchCenterChunkZ,
            MatchResult rawResult,
            MatchResult verifiedResult,
            boolean hotScanRan,
            MatchResult hotResult,
            boolean duplicateMerged
    ) {
        long candidateStartNs = perfStats != null && perfStats.enabled ? System.nanoTime() : 0L;
        try {
            Component raw = rawResult == null ? null : rawResult.component;
            Component finalComponent = verifiedResult != null ? verifiedResult.component : raw;

            boolean matched = verifiedResult != null && !duplicateMerged;
            boolean finalIsHot = hotResult != null && verifiedResult == hotResult;
            boolean finalIsBorderVerified = verifiedResult != null
                    && rawResult != null
                    && verifiedResult != rawResult
                    && !finalIsHot;

            String finalSource;
            if (duplicateMerged) {
                finalSource = "duplicate_merged";
            } else if (finalIsHot) {
                finalSource = "hot_scan";
            } else if (finalIsBorderVerified) {
                finalSource = "border_verify";
            } else if (verifiedResult != null) {
                finalSource = "raw";
            } else {
                finalSource = "none";
            }

            String status;
            if (duplicateMerged) {
                status = "duplicate_region_merged";
            } else if (verifiedResult != null && finalIsHot && verifiedResult.component.touchesSideBorder) {
                status = "matched_hot_scan_still_touches_side";
            } else if (verifiedResult != null && finalIsHot) {
                status = "matched_hot_scan";
            } else if (verifiedResult != null && verifiedResult.component.touchesSideBorder) {
                status = "matched_still_touches_side";
            } else if (verifiedResult != null) {
                status = "matched";
            } else if (rawResult != null) {
                status = "border_rejected_or_invalid";
            } else if (hotScanRan) {
                status = "hot_scan_no_match";
            } else {
                status = "no_match";
            }

            int rawBlocks = raw == null ? 0 : raw.blocks;
            int verifiedBlocks = verifiedResult == null ? 0 : verifiedResult.component.blocks;
            int rawRadius = rawResult == null ? 0 : rawResult.chunkRadiusUsed;
            int verifiedRadius = verifiedResult == null ? 0 : verifiedResult.chunkRadiusUsed;
            boolean rawTouchesSide = raw != null && raw.touchesSideBorder;
            boolean verifiedTouchesSide = verifiedResult != null && verifiedResult.component.touchesSideBorder;

            int columns = finalComponent == null ? 0 : finalComponent.columns;
            int width = finalComponent == null ? 0 : finalComponent.maxWorldX - finalComponent.minWorldX + 1;
            int depth = finalComponent == null ? 0 : finalComponent.maxWorldZ - finalComponent.minWorldZ + 1;
            int footprintArea = width * depth;
            double fillPercent = finalComponent == null ? 0.0 : 100.0 * finalComponent.columns / (double) Math.max(1, footprintArea);
            double avgThickness = finalComponent == null ? 0.0 : finalComponent.blocks / (double) Math.max(1, finalComponent.columns);
            int minY = finalComponent == null ? 0 : finalComponent.minY;
            int maxY = finalComponent == null ? 0 : finalComponent.maxY;
            int centerX = finalComponent == null ? 0 : finalComponent.centerX;
            int centerZ = finalComponent == null ? 0 : finalComponent.centerZ;
            int minX = finalComponent == null ? 0 : finalComponent.minWorldX;
            int maxX = finalComponent == null ? 0 : finalComponent.maxWorldX;
            int minZ = finalComponent == null ? 0 : finalComponent.minWorldZ;
            int maxZ = finalComponent == null ? 0 : finalComponent.maxWorldZ;
            int hotRadius = hotResult == null ? 0 : hotResult.chunkRadiusUsed;
            int hotBlocks = hotResult == null ? 0 : hotResult.component.blocks;
            boolean hotTouchesSide = hotResult != null && hotResult.component.touchesSideBorder;

            String line = String.format(
                    Locale.US,
                    "%d,%d,%d,%d,%d,%d,%d,%d,%s,%.6f,%s,%d,%d,%s,%s,%d,%d,%d,%d,%s,%s,%d,%d,%d,%d,%.4f,%.4f,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%s,%s%n",
                    attempt,
                    seed,
                    stage0Score,
                    stage0HighScore,
                    coarseScore,
                    p20Count,
                    upperCount,
                    highCount,
                    p19Pass,
                    p19Score,
                    p19Extreme,
                    searchCenterChunkX,
                    searchCenterChunkZ,
                    matched,
                    status,
                    rawBlocks,
                    verifiedBlocks,
                    rawRadius,
                    verifiedRadius,
                    rawTouchesSide,
                    verifiedTouchesSide,
                    columns,
                    width,
                    depth,
                    footprintArea,
                    fillPercent,
                    avgThickness,
                    minY,
                    maxY,
                    centerX,
                    centerZ,
                    minX,
                    maxX,
                    minZ,
                    maxZ,
                    hotScanRan,
                    hotRadius,
                    hotBlocks,
                    hotTouchesSide,
                    finalSource
            );

            synchronized (candidateCsvLock) {
                Files.writeString(
                        candidateCsvPath,
                        line,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND
                );
            }
        } catch (Exception ignored) {
            // Candidate logging must never kill the search thread.
        } finally {
            if (candidateStartNs != 0L) {
                perfStats.recordCandidateLogging(System.nanoTime() - candidateStartNs);
            }
        }
    }

    private TopUpdate recordTopResult(long seed, Component c, SearchSettings settings, int chunkRadiusUsed) throws Exception {
        String previewPath = previewPathFor(seed, c).toString();
        ResultRecord record = new ResultRecord(seed, c, previewPath, chunkRadiusUsed);

        synchronized (topLock) {
            boolean enteredTop = false;

            if (topResults.size() < settings.topResultsToKeep) {
                topResults.add(record);
                enteredTop = true;
            } else {
                ResultRecord worstTop = topResults.peek();

                if (compareBetter(record, worstTop) > 0) {
                    topResults.poll();
                    topResults.add(record);
                    enteredTop = true;
                }
            }

            if (!enteredTop) {
                return null;
            }

            saveTopResultsLocked(settings);

            List<ResultRecord> sorted = getSortedTopResultsLocked();
            int rank = sorted.indexOf(record) + 1;

            return new TopUpdate(record, rank);
        }
    }

    private int compareLargestBetter(ResultRecord a, ResultRecord b) {
        int blockCompare = Integer.compare(a.blocks, b.blocks);
        if (blockCompare != 0) return blockCompare;
        int columnCompare = Integer.compare(a.columns, b.columns);
        if (columnCompare != 0) return columnCompare;
        int footprintCompare = Integer.compare(a.footprintArea, b.footprintArea);
        if (footprintCompare != 0) return footprintCompare;
        return Double.compare(a.avgThickness, b.avgThickness);
    }

    private int compareFilledFootprintBetter(ResultRecord a, ResultRecord b) {
        int filledCompare = Integer.compare(a.columns, b.columns);
        if (filledCompare != 0) return filledCompare;
        int footprintCompare = Integer.compare(a.footprintArea, b.footprintArea);
        if (footprintCompare != 0) return footprintCompare;
        int blockCompare = Integer.compare(a.blocks, b.blocks);
        if (blockCompare != 0) return blockCompare;
        return Double.compare(a.fillPercent, b.fillPercent);
    }

    private int compareRawFootprintBetter(ResultRecord a, ResultRecord b) {
        int footprintCompare = Integer.compare(a.footprintArea, b.footprintArea);
        if (footprintCompare != 0) return footprintCompare;
        int columnCompare = Integer.compare(a.columns, b.columns);
        if (columnCompare != 0) return columnCompare;
        int blockCompare = Integer.compare(a.blocks, b.blocks);
        if (blockCompare != 0) return blockCompare;
        return Double.compare(a.fillPercent, b.fillPercent);
    }

    private int compareBetter(ResultRecord a, ResultRecord b) {
        return compareLargestBetter(a, b);
    }

    private List<ResultRecord> getSortedTopResultsLocked() {
        return getSortedBoardLocked(topResults, this::compareLargestBetter);
    }

    private List<ResultRecord> getSortedBoardLocked(
            PriorityQueue<ResultRecord> board,
            Comparator<ResultRecord> betterComparator
    ) {
        List<ResultRecord> sorted = new ArrayList<>(board);
        sorted.sort((a, b) -> betterComparator.compare(b, a));
        return sorted;
    }

    private boolean addToBoard(
            PriorityQueue<ResultRecord> board,
            ResultRecord record,
            int keep,
            Comparator<ResultRecord> betterComparator
    ) {
        if (board.size() < keep) {
            board.add(record);
            return true;
        }

        ResultRecord worst = board.peek();
        if (betterComparator.compare(record, worst) > 0) {
            board.poll();
            board.add(record);
            return true;
        }
        return false;
    }

    private List<SideboardUpdate> recordSideboardResults(
            long seed,
            Component c,
            SearchSettings settings,
            int chunkRadiusUsed
    ) throws Exception {
        ResultRecord record = new ResultRecord(
                seed,
                c,
                previewPathFor(seed, c).toString(),
                chunkRadiusUsed
        );

        synchronized (topLock) {
            List<SideboardUpdate> updates = new ArrayList<>();

            if (addToBoard(topFilledFootprintResults, record, settings.topResultsToKeep, this::compareFilledFootprintBetter)) {
                List<ResultRecord> sorted = getSortedBoardLocked(topFilledFootprintResults, this::compareFilledFootprintBetter);
                updates.add(new SideboardUpdate("FILLED FOOTPRINT", record, sorted.indexOf(record) + 1));
            }

            if (addToBoard(topRawFootprintResults, record, settings.topResultsToKeep, this::compareRawFootprintBetter)) {
                List<ResultRecord> sorted = getSortedBoardLocked(topRawFootprintResults, this::compareRawFootprintBetter);
                updates.add(new SideboardUpdate("RAW FOOTPRINT", record, sorted.indexOf(record) + 1));
            }

            if (!updates.isEmpty()) {
                saveSideboardsLocked(settings);
            }

            return updates;
        }
    }

    private void saveTopResultsLocked(SearchSettings settings) {
        List<ResultRecord> sorted = getSortedTopResultsLocked();
        String contents = buildBoardText(
                "Top largest floating islands",
                "Sorted by blocks, then occupied columns, then footprint area, then thickness",
                sorted,
                settings.topResultsToKeep
        );

        boolean ok = true;
        ok &= safeReplaceTextFile(topResultsPath, contents);

        // Legacy compatibility mirror. This file is allowed to be stale while locked.
        // A failure here must never kill a long-running search.
        ok &= safeReplaceTextFile(legacyTopResultsPath, contents);
        ok &= saveSideboardsCsvLocked();

        if (!ok) {
            leaderboardSnapshotsDirty = true;
        }
    }

    private void saveSideboardsLocked(SearchSettings settings) {
        List<ResultRecord> filled = getSortedBoardLocked(topFilledFootprintResults, this::compareFilledFootprintBetter);
        List<ResultRecord> raw = getSortedBoardLocked(topRawFootprintResults, this::compareRawFootprintBetter);

        boolean ok = true;
        ok &= writeBoardText(
                topFilledFootprintPath,
                "Top filled-footprint floating islands",
                "Sorted by occupied X/Z columns. This is raw footprint area with fill ratio accounted for.",
                filled,
                settings.topResultsToKeep
        );
        ok &= writeBoardText(
                topRawFootprintPath,
                "Top raw-footprint floating islands",
                "Sorted by bounding-box footprint area. Fill percentage is shown so sparse boxes are obvious.",
                raw,
                settings.topResultsToKeep
        );
        ok &= saveSideboardsCsvLocked();

        if (!ok) {
            leaderboardSnapshotsDirty = true;
        }
    }

    private boolean writeBoardText(
            Path path,
            String title,
            String sortDescription,
            List<ResultRecord> sorted,
            int keep
    ) {
        return safeReplaceTextFile(path, buildBoardText(title, sortDescription, sorted, keep));
    }

    private String buildBoardText(
            String title,
            String sortDescription,
            List<ResultRecord> sorted,
            int keep
    ) {
        StringBuilder sb = new StringBuilder();
        sb.append(title).append("\n");
        sb.append(sortDescription).append("\n");
        sb.append("Keeping top ").append(keep).append("\n\n");

        for (int i = 0; i < sorted.size(); i++) {
            ResultRecord r = sorted.get(i);
            sb.append("#").append(i + 1).append("\n");
            appendResultDetails(sb, r);
            sb.append("\n");
        }

        return sb.toString();
    }

    private void appendResultDetails(StringBuilder sb, ResultRecord r) {
        sb.append("seed=").append(r.seed).append("\n");
        sb.append("blocks=").append(r.blocks).append("\n");
        sb.append("chunkRadiusUsed=").append(r.chunkRadiusUsed).append("\n");
        sb.append("touchesSideBorder=").append(r.touchesSideBorder).append("\n");
        sb.append("columns=").append(r.columns).append("\n");
        sb.append("filledFootprintColumns=").append(r.columns).append("\n");
        sb.append("footprint=").append(r.width).append("x").append(r.depth).append("\n");
        sb.append("footprintArea=").append(r.footprintArea).append("\n");
        sb.append("fillPercent=").append(String.format(Locale.US, "%.2f", r.fillPercent)).append("\n");
        sb.append("avgThickness=").append(String.format(Locale.US, "%.2f", r.avgThickness)).append("\n");
        sb.append("centerX=").append(r.centerX).append("\n");
        sb.append("centerZ=").append(r.centerZ).append("\n");
        sb.append("boundsX=").append(r.minWorldX).append(" to ").append(r.maxWorldX).append("\n");
        sb.append("boundsZ=").append(r.minWorldZ).append(" to ").append(r.maxWorldZ).append("\n");
        sb.append("yRange=").append(r.minY).append(" to ").append(r.maxY).append("\n");
        sb.append("preview=").append(r.previewPath).append("\n");
    }

    private boolean saveSideboardsCsvLocked() {
        StringBuilder sb = new StringBuilder();
        sb.append("board,rank,seed,blocks,columns,width,depth,footprintArea,fillPercent,avgThickness,minY,maxY,centerX,centerZ,chunkRadiusUsed,touchesSideBorder\n");
        appendBoardCsv(sb, "largest", getSortedTopResultsLocked());
        appendBoardCsv(sb, "filled_footprint", getSortedBoardLocked(topFilledFootprintResults, this::compareFilledFootprintBetter));
        appendBoardCsv(sb, "raw_footprint", getSortedBoardLocked(topRawFootprintResults, this::compareRawFootprintBetter));

        return safeReplaceTextFile(topSideboardsCsvPath, sb.toString());
    }

    private void retryDirtyLeaderboardSnapshots(SearchSettings settings) {
        if (!leaderboardSnapshotsDirty || topResults == null) {
            return;
        }

        synchronized (topLock) {
            if (!leaderboardSnapshotsDirty) {
                return;
            }

            boolean ok = true;

            String largestContents = buildBoardText(
                    "Top largest floating islands",
                    "Sorted by blocks, then occupied columns, then footprint area, then thickness",
                    getSortedTopResultsLocked(),
                    settings.topResultsToKeep
            );
            ok &= safeReplaceTextFile(topResultsPath, largestContents);
            ok &= safeReplaceTextFile(legacyTopResultsPath, largestContents);

            ok &= writeBoardText(
                    topFilledFootprintPath,
                    "Top filled-footprint floating islands",
                    "Sorted by occupied X/Z columns. This is raw footprint area with fill ratio accounted for.",
                    getSortedBoardLocked(topFilledFootprintResults, this::compareFilledFootprintBetter),
                    settings.topResultsToKeep
            );
            ok &= writeBoardText(
                    topRawFootprintPath,
                    "Top raw-footprint floating islands",
                    "Sorted by bounding-box footprint area. Fill percentage is shown so sparse boxes are obvious.",
                    getSortedBoardLocked(topRawFootprintResults, this::compareRawFootprintBetter),
                    settings.topResultsToKeep
            );
            ok &= saveSideboardsCsvLocked();

            leaderboardSnapshotsDirty = !ok;
        }
    }

    private boolean safeReplaceTextFile(Path target, String contents) {
        if (target == null) {
            return true;
        }

        Path parent = target.toAbsolutePath().normalize().getParent();
        Path temp = target.resolveSibling(target.getFileName().toString() + ".tmp");
        Exception lastError = null;

        try {
            if (parent != null) {
                Files.createDirectories(parent);
            }

            Files.writeString(
                    temp,
                    contents,
                    StandardOpenOption.CREATE,
                    StandardOpenOption.TRUNCATE_EXISTING
            );

            for (int attempt = 1; attempt <= LEADERBOARD_WRITE_RETRIES; attempt++) {
                try {
                    try {
                        Files.move(
                                temp,
                                target,
                                StandardCopyOption.REPLACE_EXISTING,
                                StandardCopyOption.ATOMIC_MOVE
                        );
                    } catch (java.nio.file.AtomicMoveNotSupportedException ignored) {
                        Files.move(temp, target, StandardCopyOption.REPLACE_EXISTING);
                    }
                    return true;
                } catch (Exception e) {
                    lastError = e;
                    if (attempt < LEADERBOARD_WRITE_RETRIES) {
                        try {
                            Thread.sleep(LEADERBOARD_WRITE_RETRY_DELAY_MS);
                        } catch (InterruptedException interrupted) {
                            Thread.currentThread().interrupt();
                            break;
                        }
                    }
                }
            }
        } catch (Exception e) {
            lastError = e;
        } finally {
            try {
                Files.deleteIfExists(temp);
            } catch (Exception ignored) {
            }
        }

        leaderboardSnapshotsDirty = true;
        logNonFatalLeaderboardWriteFailure(target, lastError);
        return false;
    }

    private void logNonFatalLeaderboardWriteFailure(Path target, Exception error) {
        long count = nonFatalLeaderboardWriteFailures.incrementAndGet();
        if (count <= 5 || count % 50 == 0) {
            String detail = error == null ? "unknown error" : error.getClass().getSimpleName() + ": " + error.getMessage();
            String message = "WARNING: Result file write skipped (search continues) | file=" + target + " | " + detail;
            System.err.println(message);
            appendEventLog(message);
        }
    }

    private void appendBoardCsv(StringBuilder sb, String board, List<ResultRecord> sorted) {
        for (int i = 0; i < sorted.size(); i++) {
            ResultRecord r = sorted.get(i);
            sb.append(board).append(',')
                    .append(i + 1).append(',')
                    .append(r.seed).append(',')
                    .append(r.blocks).append(',')
                    .append(r.columns).append(',')
                    .append(r.width).append(',')
                    .append(r.depth).append(',')
                    .append(r.footprintArea).append(',')
                    .append(String.format(Locale.US, "%.4f", r.fillPercent)).append(',')
                    .append(String.format(Locale.US, "%.4f", r.avgThickness)).append(',')
                    .append(r.minY).append(',')
                    .append(r.maxY).append(',')
                    .append(r.centerX).append(',')
                    .append(r.centerZ).append(',')
                    .append(r.chunkRadiusUsed).append(',')
                    .append(r.touchesSideBorder)
                    .append('\n');
        }
    }

    private Path previewPathFor(long seed, Component c) {
        return runOutputDir.resolve(
                "floating_seed_" + seed + "_x" + c.centerX + "_z" + c.centerZ + ".png"
        );
    }

    private boolean shouldSaveExceptionalPreview(Component c) {
        int width = c.maxWorldX - c.minWorldX + 1;
        int depth = c.maxWorldZ - c.minWorldZ + 1;
        int footprintArea = width * depth;

        return c.blocks >= PREVIEW_MIN_BLOCKS
                || footprintArea >= PREVIEW_MIN_FOOTPRINT_AREA
                || c.columns >= PREVIEW_MIN_COLUMNS;
    }

    private void trySavePreview(
            long seed,
            Component c,
            String path,
            SearchSettings settings,
            SearchListener listener,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
        try {
            savePreview(seed, c, path, settings, searchCenterChunkX, searchCenterChunkZ);
            listener.onLog(
                    "PREVIEW SAVED | seed " + seed
                            + " | blocks=" + c.blocks
                            + " | columns=" + c.columns
                            + " | footprint=" + (c.maxWorldX - c.minWorldX + 1)
                            + "x" + (c.maxWorldZ - c.minWorldZ + 1)
            );
        } catch (Exception e) {
            // Preview rendering is presentation-only and must never stop a hunt.
            listener.onLog("PREVIEW FAILED | seed " + seed + " | " + e.getMessage());
        }
    }

    private void savePreview(long seed, Component target, String path, SearchSettings settings) throws Exception {
        savePreview(seed, target, path, settings, 0, 0);
    }

    private void savePreview(
            long seed,
            Component target,
            String path,
            SearchSettings settings,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) throws Exception {
        PreviewData preview = buildPreviewData(
                seed, target, settings, searchCenterChunkX, searchCenterChunkZ
        );
        if (preview == null) {
            throw new IllegalStateException("exact island component could not be reconstructed");
        }

        BetaTerrain173 terrain = new BetaTerrain173(seed);
        int[][] heightmap = terrain.generateHeightmapAroundChunkCenterFast(
                searchCenterChunkX, searchCenterChunkZ, settings.chunkRadius
        );

        int sourceSize = preview.sizeX;
        int cropSize = Math.max(
                PREVIEW_MIN_CROP_SIZE,
                Math.max(
                        target.maxWorldX - target.minWorldX + 1,
                        target.maxWorldZ - target.minWorldZ + 1
                ) + PREVIEW_CONTEXT_MARGIN * 2
        );
        cropSize = Math.min(sourceSize, cropSize);

        int searchMinWorldX = (searchCenterChunkX - settings.chunkRadius) * 16;
        int searchMinWorldZ = (searchCenterChunkZ - settings.chunkRadius) * 16;
        int centerImageX = target.centerX - searchMinWorldX;
        int centerImageZ = target.centerZ - searchMinWorldZ;
        int cropX = clamp(centerImageX - cropSize / 2, 0, sourceSize - cropSize);
        int cropZ = clamp(centerImageZ - cropSize / 2, 0, sourceSize - cropSize);

        BufferedImage logical = new BufferedImage(cropSize, cropSize, BufferedImage.TYPE_INT_RGB);

        for (int ix = 0; ix < cropSize; ix++) {
            int worldArrayX = cropX + ix;
            for (int iz = 0; iz < cropSize; iz++) {
                int worldArrayZ = cropZ + iz;
                logical.setRGB(ix, iz, terrainReliefColor(heightmap, worldArrayX, worldArrayZ));
            }
        }

        // Dark halo first so the exact connected footprint remains readable even
        // over bright mountain ridges in the background heightmap.
        for (int ix = 0; ix < cropSize; ix++) {
            int sourceX = cropX + ix;
            for (int iz = 0; iz < cropSize; iz++) {
                int sourceZ = cropZ + iz;
                if (preview.thicknessAt(sourceX, sourceZ) > 0) {
                    continue;
                }
                if (hasIslandNeighbor(preview, sourceX, sourceZ)) {
                    logical.setRGB(ix, iz, 0x3A2818);
                }
            }
        }

        for (int ix = 0; ix < cropSize; ix++) {
            int sourceX = cropX + ix;
            for (int iz = 0; iz < cropSize; iz++) {
                int sourceZ = cropZ + iz;
                int thickness = preview.thicknessAt(sourceX, sourceZ);
                if (thickness <= 0) {
                    continue;
                }

                boolean edge = isIslandEdge(preview, sourceX, sourceZ);
                logical.setRGB(ix, iz, islandColor(thickness, preview.maxThickness, edge));
            }
        }

        BufferedImage output = new BufferedImage(PREVIEW_OUTPUT_SIZE, PREVIEW_OUTPUT_SIZE, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = output.createGraphics();
        try {
            g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR);
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
            g.drawImage(logical, 0, 0, PREVIEW_OUTPUT_SIZE, PREVIEW_OUTPUT_SIZE, null);
        } finally {
            g.dispose();
        }

        ImageIO.write(output, "png", new File(path));
    }

    private PreviewData buildPreviewData(long seed, Component target, SearchSettings settings) {
        return buildPreviewData(seed, target, settings, 0, 0);
    }

    private PreviewData buildPreviewData(
            long seed,
            Component target,
            SearchSettings settings,
            int searchCenterChunkX,
            int searchCenterChunkZ
    ) {
        BetaTerrain173 terrain = new BetaTerrain173(seed);

        int fullSize = (settings.chunkRadius * 2 + 1) * 16;
        int searchMinWorldX = (searchCenterChunkX - settings.chunkRadius) * 16;
        int searchMinWorldZ = (searchCenterChunkZ - settings.chunkRadius) * 16;
        int searchMaxWorldX = searchMinWorldX + fullSize - 1;
        int searchMaxWorldZ = searchMinWorldZ + fullSize - 1;

        // The exact component bounds are already known. A one-block margin is
        // enough to preserve every possible 6-neighbor connection while avoiding
        // a second full radius-16 volume allocation for rare hot-scan previews.
        int localMinWorldX = Math.max(searchMinWorldX, target.minWorldX - 1);
        int localMaxWorldX = Math.min(searchMaxWorldX, target.maxWorldX + 1);
        int localMinWorldZ = Math.max(searchMinWorldZ, target.minWorldZ - 1);
        int localMaxWorldZ = Math.min(searchMaxWorldZ, target.maxWorldZ + 1);

        int sizeX = localMaxWorldX - localMinWorldX + 1;
        int sizeZ = localMaxWorldZ - localMinWorldZ + 1;
        int sizeY = BetaTerrain173.WORLD_HEIGHT;
        int volume = sizeX * sizeY * sizeZ;
        int columnVolume = sizeX * sizeZ;

        byte[] stone = new byte[volume];
        byte[] visited = new byte[volume];
        int[] queue = new int[volume];
        int[] columnSeen = new int[columnVolume];
        int copyStartY = Math.max(0, ISLAND_FLOOR_Y - 1);

        int minChunkX = Math.floorDiv(localMinWorldX, 16);
        int maxChunkX = Math.floorDiv(localMaxWorldX, 16);
        int minChunkZ = Math.floorDiv(localMinWorldZ, 16);
        int maxChunkZ = Math.floorDiv(localMaxWorldZ, 16);

        for (int chunkX = minChunkX; chunkX <= maxChunkX; chunkX++) {
            for (int chunkZ = minChunkZ; chunkZ <= maxChunkZ; chunkZ++) {
                int[] blocks = terrain.generateChunkBaseTerrain(chunkX, chunkZ);

                for (int x = 0; x < 16; x++) {
                    int worldX = chunkX * 16 + x;
                    if (worldX < localMinWorldX || worldX > localMaxWorldX) continue;
                    int localX = worldX - localMinWorldX;

                    for (int z = 0; z < 16; z++) {
                        int worldZ = chunkZ * 16 + z;
                        if (worldZ < localMinWorldZ || worldZ > localMaxWorldZ) continue;
                        int localZ = worldZ - localMinWorldZ;

                        for (int y = copyStartY; y < sizeY; y++) {
                            if (blocks[BetaTerrain173.index(x, y, z)] == BetaTerrain173.STONE) {
                                stone[index3(localX, y, localZ, sizeY, sizeZ)] = 1;
                            }
                        }
                    }
                }
            }
        }

        int yzArea = sizeY * sizeZ;
        int columnMark = 1;
        int minStartY = Math.max(ISLAND_FLOOR_Y, target.minY);
        int maxStartY = Math.min(sizeY - 1, target.maxY);

        for (int worldX = target.minWorldX; worldX <= target.maxWorldX; worldX++) {
            int localX = worldX - localMinWorldX;
            int chunkLocalX = Math.floorMod(worldX, 16);

            for (int worldZ = target.minWorldZ; worldZ <= target.maxWorldZ; worldZ++) {
                int localZ = worldZ - localMinWorldZ;
                int chunkLocalZ = Math.floorMod(worldZ, 16);
                if (!isSampleCandidateColumn(chunkLocalX, chunkLocalZ)) continue;

                for (int y = minStartY; y <= maxStartY; y++) {
                    int start = index3(localX, y, localZ, sizeY, sizeZ);
                    if (stone[start] == 0 || visited[start] != 0) continue;
                    if (y <= 0 || stone[start - sizeZ] != 0) continue;

                    Component c = floodFillFlat(
                            stone,
                            visited,
                            queue,
                            columnSeen,
                            columnMark++,
                            localX,
                            y,
                            localZ,
                            sizeX,
                            sizeY,
                            sizeZ,
                            localMinWorldX,
                            localMinWorldZ,
                            ISLAND_FLOOR_Y
                    );

                    if (!sameComponentForPreview(c, target)) {
                        continue;
                    }

                    int[] thickness = new int[fullSize * fullSize];
                    int[] minYByColumn = new int[fullSize * fullSize];
                    int[] maxYByColumn = new int[fullSize * fullSize];
                    Arrays.fill(minYByColumn, Integer.MAX_VALUE);
                    Arrays.fill(maxYByColumn, Integer.MIN_VALUE);
                    int maxThickness = 0;
                    for (int q = 0; q < c.floodQueueLength; q++) {
                        int index = queue[q];
                        int qx = index / yzArea;
                        int qRemainder = index - qx * yzArea;
                        int qy = qRemainder / sizeZ;
                        int qz = qRemainder - qy * sizeZ;

                        int globalX = localMinWorldX + qx - searchMinWorldX;
                        int globalZ = localMinWorldZ + qz - searchMinWorldZ;
                        int col = globalX * fullSize + globalZ;
                        int count = ++thickness[col];
                        if (qy < minYByColumn[col]) minYByColumn[col] = qy;
                        if (qy > maxYByColumn[col]) maxYByColumn[col] = qy;
                        if (count > maxThickness) {
                            maxThickness = count;
                        }
                    }

                    return new PreviewData(
                            fullSize, fullSize, thickness, minYByColumn, maxYByColumn, maxThickness
                    );
                }
            }
        }

        return null;
    }

    private boolean sameComponentForPreview(Component a, Component b) {
        return a.blocks == b.blocks
                && a.columns == b.columns
                && a.minWorldX == b.minWorldX
                && a.maxWorldX == b.maxWorldX
                && a.minWorldZ == b.minWorldZ
                && a.maxWorldZ == b.maxWorldZ
                && a.minY == b.minY
                && a.maxY == b.maxY;
    }

    private int terrainReliefColor(int[][] heightmap, int x, int z) {
        int h = heightmap[x][z];
        int left = heightmap[Math.max(0, x - 1)][z];
        int right = heightmap[Math.min(heightmap.length - 1, x + 1)][z];
        int up = heightmap[x][Math.max(0, z - 1)];
        int down = heightmap[x][Math.min(heightmap[x].length - 1, z + 1)];

        int elevation = clamp((h - 48) * 2, 0, 100);
        int slopeLight = clamp((left - right) * 3 + (up - down) * 2, -28, 28);
        int base = clamp(48 + elevation + slopeLight, 34, 156);

        int r = clamp(base - 4, 0, 255);
        int g = clamp(base, 0, 255);
        int b = clamp(base + 3, 0, 255);
        return (r << 16) | (g << 8) | b;
    }

    private int islandColor(int thickness, int maxThickness, boolean edge) {
        double ratio = Math.log1p(thickness) / Math.log1p(Math.max(1, maxThickness));
        ratio = Math.max(0.0, Math.min(1.0, ratio));

        int r = (int) Math.round(174 + ratio * 74);
        int g = (int) Math.round(101 + ratio * 111);
        int b = (int) Math.round(38 + ratio * 83);

        if (edge) {
            r = Math.min(255, r + 22);
            g = Math.min(255, g + 22);
            b = Math.min(255, b + 12);
        }

        return (r << 16) | (g << 8) | b;
    }

    private boolean hasIslandNeighbor(PreviewData preview, int x, int z) {
        for (int dx = -1; dx <= 1; dx++) {
            for (int dz = -1; dz <= 1; dz++) {
                if (dx == 0 && dz == 0) continue;
                if (preview.thicknessAt(x + dx, z + dz) > 0) return true;
            }
        }
        return false;
    }

    private boolean isIslandEdge(PreviewData preview, int x, int z) {
        return preview.thicknessAt(x - 1, z) == 0
                || preview.thicknessAt(x + 1, z) == 0
                || preview.thicknessAt(x, z - 1) == 0
                || preview.thicknessAt(x, z + 1) == 0;
    }

    private int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private static class CoarseGrid {
        final int size;
        final double[] values;
        final boolean[] filled;
        boolean partialLower;
        final int chunkRadius;
        int lazyLowerColumnsGenerated;
        int lazyExpansionRounds;
        boolean lazyFallbackToFull;

        CoarseGrid(int size, double[] values, boolean[] filled) {
            this(size, values, filled, false, -1);
        }

        CoarseGrid(int size, double[] values, boolean[] filled, boolean partialLower, int chunkRadius) {
            this.size = size;
            this.values = values;
            this.filled = filled;
            this.partialLower = partialLower;
            this.chunkRadius = chunkRadius;
        }

        int index(int x, int y, int z) {
            return (x * size + z) * COARSE_Y_LEVELS + y;
        }
    }

    private static class CoarseFeatureSummary {
        int bestCells;
        int bestColumns;
        int bestReentryColumns;
        int bestMinYIndex;
        int bestMaxYIndex;
        int bestCellsY8;
        int bestCellsY10;
        int bestCellsY12;
        double bestMaxDensity;
        double bestAvgPositiveDensity;

        int totalPositiveCells;
        int positiveCellsY8;
        int positiveCellsY10;
        int positiveCellsY12;
        int columnsPositiveY8;
        int columnsPositiveY10;
        int columnsPositiveY12;
        int totalPositiveComponents;
        int possibleFloatingComponents;
    }

    private static class CoarseComponent {
        int id;
        int cells;
        int minYIndex;
        int maxYIndex;
        int columnCount;
        int reentryColumns;
        int cellsY8;
        int cellsY10;
        int cellsY12;
        double sumDensity;
        double maxDensity = Double.NEGATIVE_INFINITY;
        boolean touchesBottom;
        boolean touchesSide;
    }

    private static class DebugStats {
        private static final String[] BIN_LABELS = {
                "0", "1-9", "10-29", "30-49", "50-79", "80-99",
                "100-139", "140-179", "180-239", "240-279", "280+"
        };

        final AtomicLong coarseEvaluated = new AtomicLong(0);
        final AtomicLong coarseRejected = new AtomicLong(0);
        final AtomicLong coarseAccepted = new AtomicLong(0);
        final AtomicLong fullScans = new AtomicLong(0);
        final AtomicLong coarseScoreTotal = new AtomicLong(0);
        final AtomicInteger maxCoarseScore = new AtomicInteger(0);
        final AtomicInteger minAcceptedScore = new AtomicInteger(Integer.MAX_VALUE);
        final AtomicInteger maxAcceptedScore = new AtomicInteger(0);
        final AtomicLongArray scoreBins = new AtomicLongArray(BIN_LABELS.length);

        private static final String[] STAGE0_BIN_LABELS = {
                "0", "1", "2-3", "4-7", "8-15", "16+"
        };

        final AtomicLong stage0ScoutEvaluated = new AtomicLong(0);
        final AtomicLong stage0ScoutRejected = new AtomicLong(0);
        final AtomicLong stage0ScoutAccepted = new AtomicLong(0);

        final AtomicLong stage025Evaluated = new AtomicLong(0);
        final AtomicLong stage025Rejected = new AtomicLong(0);
        final AtomicLong stage025Accepted = new AtomicLong(0);

        final AtomicLong stage0Evaluated = new AtomicLong(0);
        final AtomicLong stage0Rejected = new AtomicLong(0);
        final AtomicLong stage0Accepted = new AtomicLong(0);
        final AtomicLong stage0ScoreTotal = new AtomicLong(0);
        final AtomicInteger maxStage0Score = new AtomicInteger(0);
        final AtomicLongArray stage0Bins = new AtomicLongArray(STAGE0_BIN_LABELS.length);

        final AtomicLong stage0HighEvaluated = new AtomicLong(0);
        final AtomicLong stage0HighRejected = new AtomicLong(0);
        final AtomicLong stage0HighAccepted = new AtomicLong(0);
        final AtomicLong stage0HighScoreTotal = new AtomicLong(0);
        final AtomicInteger maxStage0HighScore = new AtomicInteger(0);
        final AtomicLongArray stage0HighBins = new AtomicLongArray(STAGE0_BIN_LABELS.length);

        final AtomicLong stage075Evaluated = new AtomicLong(0);
        final AtomicLong stage075Rejected = new AtomicLong(0);
        final AtomicLong stage075Accepted = new AtomicLong(0);

        final AtomicLong stage025AuditSamples = new AtomicLong(0);
        final AtomicInteger maxStage025AuditRejectedCoarse = new AtomicInteger(0);
        final AtomicLong stage025AuditCoarse85Plus = new AtomicLong(0);
        final AtomicLong stage025AuditCoarse120Plus = new AtomicLong(0);
        final AtomicLong stage025AuditCoarse180Plus = new AtomicLong(0);
        final AtomicLong stage025AuditCoarse240Plus = new AtomicLong(0);
        final AtomicLong stage025AuditCoarse280Plus = new AtomicLong(0);

        final AtomicLong stage0AuditSamples = new AtomicLong(0);
        final AtomicInteger maxStage0AuditRejectedCoarse = new AtomicInteger(0);
        final AtomicLong stage0AuditCoarse85Plus = new AtomicLong(0);
        final AtomicLong stage0AuditCoarse120Plus = new AtomicLong(0);
        final AtomicLong stage0AuditCoarse180Plus = new AtomicLong(0);
        final AtomicLong stage0AuditCoarse240Plus = new AtomicLong(0);
        final AtomicLong stage0AuditCoarse280Plus = new AtomicLong(0);

        final AtomicLong stage075AuditSamples = new AtomicLong(0);
        final AtomicInteger maxStage075AuditRejectedCoarse = new AtomicInteger(0);
        final AtomicLong stage075AuditCoarse85Plus = new AtomicLong(0);
        final AtomicLong stage075AuditCoarse120Plus = new AtomicLong(0);
        final AtomicLong stage075AuditCoarse180Plus = new AtomicLong(0);
        final AtomicLong stage075AuditCoarse240Plus = new AtomicLong(0);
        final AtomicLong stage075AuditCoarse280Plus = new AtomicLong(0);

        private final Object topScoresLock = new Object();
        private final int[] topScores = new int[5];
        private final long[] topSeeds = new long[5];

        void recordStage0Score(int score) {
            stage0Evaluated.incrementAndGet();
            stage0ScoreTotal.addAndGet(score);
            updateMax(maxStage0Score, score);
            stage0Bins.incrementAndGet(stage0Bin(score));
        }

        void recordStage0HighScore(int score) {
            stage0HighEvaluated.incrementAndGet();
            stage0HighScoreTotal.addAndGet(score);
            updateMax(maxStage0HighScore, score);
            stage0HighBins.incrementAndGet(stage0Bin(score));
        }

        void recordStage025AuditRejectedCoarse(int coarseScore) {
            stage025AuditSamples.incrementAndGet();
            updateMax(maxStage025AuditRejectedCoarse, coarseScore);
            if (coarseScore >= 85) stage025AuditCoarse85Plus.incrementAndGet();
            if (coarseScore >= 120) stage025AuditCoarse120Plus.incrementAndGet();
            if (coarseScore >= 180) stage025AuditCoarse180Plus.incrementAndGet();
            if (coarseScore >= 240) stage025AuditCoarse240Plus.incrementAndGet();
            if (coarseScore >= 280) stage025AuditCoarse280Plus.incrementAndGet();
        }

        void recordStage0AuditRejectedCoarse(int coarseScore) {
            stage0AuditSamples.incrementAndGet();
            updateMax(maxStage0AuditRejectedCoarse, coarseScore);
            if (coarseScore >= 85) stage0AuditCoarse85Plus.incrementAndGet();
            if (coarseScore >= 120) stage0AuditCoarse120Plus.incrementAndGet();
            if (coarseScore >= 180) stage0AuditCoarse180Plus.incrementAndGet();
            if (coarseScore >= 240) stage0AuditCoarse240Plus.incrementAndGet();
            if (coarseScore >= 280) stage0AuditCoarse280Plus.incrementAndGet();
        }

        void recordStage075AuditRejectedCoarse(int coarseScore) {
            stage075AuditSamples.incrementAndGet();
            updateMax(maxStage075AuditRejectedCoarse, coarseScore);
            if (coarseScore >= 85) stage075AuditCoarse85Plus.incrementAndGet();
            if (coarseScore >= 120) stage075AuditCoarse120Plus.incrementAndGet();
            if (coarseScore >= 180) stage075AuditCoarse180Plus.incrementAndGet();
            if (coarseScore >= 240) stage075AuditCoarse240Plus.incrementAndGet();
            if (coarseScore >= 280) stage075AuditCoarse280Plus.incrementAndGet();
        }

        String formatStage0Distribution() {
            return formatStage0Distribution(stage0Evaluated, stage0Bins);
        }

        String formatStage0HighDistribution() {
            return formatStage0Distribution(stage0HighEvaluated, stage0HighBins);
        }

        private String formatStage0Distribution(AtomicLong evaluated, AtomicLongArray bins) {
            long total = Math.max(1L, evaluated.get());
            StringBuilder sb = new StringBuilder();

            for (int i = 0; i < STAGE0_BIN_LABELS.length; i++) {
                if (i > 0) {
                    sb.append(" | ");
                }

                long count = bins.get(i);
                double percent = count * 100.0 / total;

                sb.append(STAGE0_BIN_LABELS[i])
                        .append("=")
                        .append(count)
                        .append(" (")
                        .append(String.format("%.3f", percent))
                        .append("%)");
            }

            return sb.toString();
        }

        void recordCoarseScore(int score, long seed) {
            coarseEvaluated.incrementAndGet();
            coarseScoreTotal.addAndGet(score);
            updateMax(maxCoarseScore, score);
            scoreBins.incrementAndGet(scoreBin(score));
            recordTopScore(score, seed);
        }

        void recordAcceptedScore(int score) {
            updateMin(minAcceptedScore, score);
            updateMax(maxAcceptedScore, score);
        }

        String formatDistribution() {
            long total = Math.max(1L, coarseEvaluated.get());
            StringBuilder sb = new StringBuilder();

            for (int i = 0; i < BIN_LABELS.length; i++) {
                if (i > 0) {
                    sb.append(" | ");
                }

                long count = scoreBins.get(i);
                double percent = count * 100.0 / total;

                sb.append(BIN_LABELS[i])
                        .append("=")
                        .append(count)
                        .append(" (")
                        .append(String.format("%.3f", percent))
                        .append("%)");
            }

            return sb.toString();
        }

        String formatTopScores() {
            synchronized (topScoresLock) {
                StringBuilder sb = new StringBuilder();

                for (int i = 0; i < topScores.length; i++) {
                    if (i > 0) {
                        sb.append(" | ");
                    }

                    if (topScores[i] <= 0) {
                        sb.append("#").append(i + 1).append(" none");
                    } else {
                        sb.append("#")
                                .append(i + 1)
                                .append(" score=")
                                .append(topScores[i])
                                .append(" seed=")
                                .append(topSeeds[i]);
                    }
                }

                return sb.toString();
            }
        }

        private void recordTopScore(int score, long seed) {
            if (score <= 0) {
                return;
            }

            synchronized (topScoresLock) {
                if (score <= topScores[topScores.length - 1]) {
                    return;
                }

                int insertAt = topScores.length - 1;
                while (insertAt > 0 && score > topScores[insertAt - 1]) {
                    topScores[insertAt] = topScores[insertAt - 1];
                    topSeeds[insertAt] = topSeeds[insertAt - 1];
                    insertAt--;
                }

                topScores[insertAt] = score;
                topSeeds[insertAt] = seed;
            }
        }

        private static int stage0Bin(int score) {
            if (score <= 0) return 0;
            if (score == 1) return 1;
            if (score <= 3) return 2;
            if (score <= 7) return 3;
            if (score <= 15) return 4;
            return 5;
        }

        private static int scoreBin(int score) {
            if (score <= 0) return 0;
            if (score <= 9) return 1;
            if (score <= 29) return 2;
            if (score <= 49) return 3;
            if (score <= 79) return 4;
            if (score <= 99) return 5;
            if (score <= 139) return 6;
            if (score <= 179) return 7;
            if (score <= 239) return 8;
            if (score <= 279) return 9;
            return 10;
        }

        private static void updateMax(AtomicInteger atomic, int value) {
            int old;
            do {
                old = atomic.get();
                if (value <= old) {
                    return;
                }
            } while (!atomic.compareAndSet(old, value));
        }

        private static void updateMin(AtomicInteger atomic, int value) {
            int old;
            do {
                old = atomic.get();
                if (value >= old) {
                    return;
                }
            } while (!atomic.compareAndSet(old, value));
        }
    }

    private void appendEventLog(String message) {
        if (eventLogPath == null) {
            return;
        }

        try {
            String timestamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("HH:mm:ss"));
            String line = "[" + timestamp + "] " + message + System.lineSeparator();
            synchronized (eventLogLock) {
                Files.writeString(
                        eventLogPath,
                        line,
                        StandardOpenOption.CREATE,
                        StandardOpenOption.APPEND
                );
            }
        } catch (Exception ignored) {
            // Event logging is convenience only.
        }
    }

    private class RunLoggingListener implements SearchListener {
        private final SearchListener delegate;

        RunLoggingListener(SearchListener delegate) {
            this.delegate = delegate;
        }

        @Override
        public void onProgress(long checked, int matches, int topUpdates, double seedsPerSecond) {
            delegate.onProgress(checked, matches, topUpdates, seedsPerSecond);
        }

        @Override
        public void onHit(long checked, int blocks) {
            delegate.onHit(checked, blocks);
        }

        @Override
        public void onHit(long checked, SearchResult result) {
            delegate.onHit(checked, result);
        }

        @Override
        public void onTopResult(SearchResult result, int rank) {
            delegate.onTopResult(result, rank);
        }

        @Override
        public void onLog(String message) {
            appendEventLog(message);
            delegate.onLog(message);
        }

        @Override
        public void onFinished() {
            delegate.onFinished();
        }

        @Override
        public void onError(Throwable error) {
            appendEventLog("ERROR: " + error);
            delegate.onError(error);
        }
    }

    private static class PerfStats {
        final boolean enabled;
        final long commonSampleMask;

        final AtomicLong stage0Ns = new AtomicLong();
        final AtomicLong stage0Samples = new AtomicLong();
        final AtomicLong coarseGridNs = new AtomicLong();
        final AtomicLong coarseGridSamples = new AtomicLong();
        final AtomicLong coarseScoreNs = new AtomicLong();
        final AtomicLong coarseScoreSamples = new AtomicLong();

        final AtomicLong normalExactNs = new AtomicLong();
        final AtomicLong normalExactCount = new AtomicLong();
        final AtomicLong borderVerifyNs = new AtomicLong();
        final AtomicLong borderVerifyCount = new AtomicLong();
        final AtomicLong hotScanNs = new AtomicLong();
        final AtomicLong hotScanCount = new AtomicLong();

        final AtomicLong featureLoggingNs = new AtomicLong();
        final AtomicLong featureLoggingCount = new AtomicLong();
        final AtomicLong auditNs = new AtomicLong();
        final AtomicLong auditCount = new AtomicLong();
        final AtomicLong candidateLoggingNs = new AtomicLong();
        final AtomicLong candidateLoggingCount = new AtomicLong();

        PerfStats(boolean enabled, long commonSampleMask) {
            this.enabled = enabled;
            this.commonSampleMask = commonSampleMask;
        }

        void recordStage0(long ns) {
            stage0Ns.addAndGet(ns);
            stage0Samples.incrementAndGet();
        }

        void recordCoarseGrid(long ns) {
            coarseGridNs.addAndGet(ns);
            coarseGridSamples.incrementAndGet();
        }

        void recordCoarseScore(long ns) {
            coarseScoreNs.addAndGet(ns);
            coarseScoreSamples.incrementAndGet();
        }

        void recordNormalExact(long ns) {
            normalExactNs.addAndGet(ns);
            normalExactCount.incrementAndGet();
        }

        void recordBorderVerify(long ns) {
            borderVerifyNs.addAndGet(ns);
            borderVerifyCount.incrementAndGet();
        }

        void recordHotScan(long ns) {
            hotScanNs.addAndGet(ns);
            hotScanCount.incrementAndGet();
        }

        void recordFeatureLogging(long ns) {
            featureLoggingNs.addAndGet(ns);
            featureLoggingCount.incrementAndGet();
        }

        void recordAudit(long ns) {
            auditNs.addAndGet(ns);
            auditCount.incrementAndGet();
        }

        void recordCandidateLogging(long ns) {
            candidateLoggingNs.addAndGet(ns);
            candidateLoggingCount.incrementAndGet();
        }

        String formatReport(long checked, long elapsedMs) {
            long factor = commonSampleMask + 1L;
            long estimatedStage0Ns = stage0Ns.get() * factor;
            long estimatedCoarseGridNs = coarseGridNs.get() * factor;
            long estimatedCoarseScoreNs = coarseScoreNs.get() * factor;

            long totalMeasuredNs = estimatedStage0Ns
                    + estimatedCoarseGridNs
                    + estimatedCoarseScoreNs
                    + normalExactNs.get()
                    + borderVerifyNs.get()
                    + hotScanNs.get()
                    + featureLoggingNs.get()
                    + auditNs.get()
                    + candidateLoggingNs.get();

            StringBuilder sb = new StringBuilder();
            sb.append("PERF PROFILE | estimated worker-time share, not wall-clock share\n");
            sb.append("PERF META | checked=").append(checked)
                    .append(" | wall=").append(String.format(Locale.US, "%.1fs", elapsedMs / 1000.0))
                    .append(" | commonSample=1/").append(factor).append("\n");
            sb.append("PERF SAMPLES | stage0=").append(stage0Samples.get())
                    .append(" | coarseGrid=").append(coarseGridSamples.get())
                    .append(" | coarseScore=").append(coarseScoreSamples.get())
                    .append(" | exact=").append(normalExactCount.get())
                    .append(" | border=").append(borderVerifyCount.get())
                    .append(" | hot=").append(hotScanCount.get())
                    .append(" | featureLogs=").append(featureLoggingCount.get())
                    .append(" | audits=").append(auditCount.get())
                    .append(" | candidateLogs=").append(candidateLoggingCount.get())
                    .append("\n");

            appendPerfStage(sb, "Stage0 fused", estimatedStage0Ns, totalMeasuredNs, stage0Ns.get(), stage0Samples.get());
            appendPerfStage(sb, "Coarse grid", estimatedCoarseGridNs, totalMeasuredNs, coarseGridNs.get(), coarseGridSamples.get());
            appendPerfStage(sb, "Coarse fast score", estimatedCoarseScoreNs, totalMeasuredNs, coarseScoreNs.get(), coarseScoreSamples.get());
            appendPerfStage(sb, "Exact r-base", normalExactNs.get(), totalMeasuredNs, normalExactNs.get(), normalExactCount.get());
            appendPerfStage(sb, "Border verify", borderVerifyNs.get(), totalMeasuredNs, borderVerifyNs.get(), borderVerifyCount.get());
            appendPerfStage(sb, "Hot scan", hotScanNs.get(), totalMeasuredNs, hotScanNs.get(), hotScanCount.get());
            appendPerfStage(sb, "Feature logging", featureLoggingNs.get(), totalMeasuredNs, featureLoggingNs.get(), featureLoggingCount.get());
            appendPerfStage(sb, "Stage0 audit", auditNs.get(), totalMeasuredNs, auditNs.get(), auditCount.get());
            appendPerfStage(sb, "Candidate logging", candidateLoggingNs.get(), totalMeasuredNs, candidateLoggingNs.get(), candidateLoggingCount.get());

            return sb.toString();
        }

        private static void appendPerfStage(
                StringBuilder sb,
                String name,
                long estimatedNs,
                long totalEstimatedNs,
                long rawNs,
                long count
        ) {
            double percent = totalEstimatedNs == 0L ? 0.0 : estimatedNs * 100.0 / totalEstimatedNs;
            double totalSeconds = estimatedNs / 1_000_000_000.0;
            double avgUs = count == 0L ? 0.0 : rawNs / (double) count / 1_000.0;

            sb.append("PERF STAGE | ")
                    .append(name)
                    .append(" | share=").append(String.format(Locale.US, "%.2f%%", percent))
                    .append(" | estWorker=").append(String.format(Locale.US, "%.2fs", totalSeconds))
                    .append(" | avgMeasured=").append(String.format(Locale.US, "%.2fus", avgUs))
                    .append("\n");
        }
    }

    public static class P17CoarseDiagnostic {
        public final long seed;
        public final int fullScore;
        public final int lazyScore;
        public final int rawBitMismatches;
        public final int generatedPoints;
        public final int lowerColumnsGenerated;
        public final int expansionRounds;
        public final boolean fallbackToFull;
        public final long fullNs;
        public final long lazyNs;

        P17CoarseDiagnostic(
                long seed, int fullScore, int lazyScore, int rawBitMismatches,
                int generatedPoints, int lowerColumnsGenerated, int expansionRounds, boolean fallbackToFull,
                long fullNs, long lazyNs
        ) {
            this.seed = seed;
            this.fullScore = fullScore;
            this.lazyScore = lazyScore;
            this.rawBitMismatches = rawBitMismatches;
            this.generatedPoints = generatedPoints;
            this.lowerColumnsGenerated = lowerColumnsGenerated;
            this.expansionRounds = expansionRounds;
            this.fallbackToFull = fallbackToFull;
            this.fullNs = fullNs;
            this.lazyNs = lazyNs;
        }
    }

    public static class CoarsePathDiagnostic {
        public final long seed;
        public final int fastScore;
        public final int richScore;
        public final long fastNs;
        public final long richNs;

        CoarsePathDiagnostic(long seed, int fastScore, int richScore, long fastNs, long richNs) {
            this.seed = seed;
            this.fastScore = fastScore;
            this.richScore = richScore;
            this.fastNs = fastNs;
            this.richNs = richNs;
        }
    }

    public static class SeedDiagnostic {
        public final long seed;
        public final int stage0FullY72;
        public final int stage0FullY88;
        public final int stage0FullY96;
        public final int stage0Y88LargestCluster;
        public final int stage0Y96LargestCluster;
        public final int coarse;
        public final boolean currentPipelinePass;
        public final boolean experimentalY96Cluster3Pass;
        public final int blocks;
        public final int columns;
        public final int width;
        public final int depth;
        public final int minY;
        public final int maxY;
        public final int radiusUsed;
        public final boolean touchesSideBorder;

        SeedDiagnostic(
                long seed,
                int stage0FullY72,
                int stage0FullY88,
                int stage0FullY96,
                int stage0Y88LargestCluster,
                int stage0Y96LargestCluster,
                int coarse,
                boolean currentPipelinePass,
                boolean experimentalY96Cluster3Pass,
                int blocks,
                int columns,
                int width,
                int depth,
                int minY,
                int maxY,
                int radiusUsed,
                boolean touchesSideBorder
        ) {
            this.seed = seed;
            this.stage0FullY72 = stage0FullY72;
            this.stage0FullY88 = stage0FullY88;
            this.stage0FullY96 = stage0FullY96;
            this.stage0Y88LargestCluster = stage0Y88LargestCluster;
            this.stage0Y96LargestCluster = stage0Y96LargestCluster;
            this.coarse = coarse;
            this.currentPipelinePass = currentPipelinePass;
            this.experimentalY96Cluster3Pass = experimentalY96Cluster3Pass;
            this.blocks = blocks;
            this.columns = columns;
            this.width = width;
            this.depth = depth;
            this.minY = minY;
            this.maxY = maxY;
            this.radiusUsed = radiusUsed;
            this.touchesSideBorder = touchesSideBorder;
        }
    }

    private static class MatchResult {
        final long seed;
        final Component component;
        final int chunkRadiusUsed;
        final int stage0Score;
        final int stage0HighScore;
        final int coarseScore;
        final int searchCenterChunkX;
        final int searchCenterChunkZ;

        MatchResult(long seed, Component component, int chunkRadiusUsed, int stage0Score, int stage0HighScore, int coarseScore) {
            this(seed, component, chunkRadiusUsed, stage0Score, stage0HighScore, coarseScore, 0, 0);
        }

        MatchResult(
                long seed,
                Component component,
                int chunkRadiusUsed,
                int stage0Score,
                int stage0HighScore,
                int coarseScore,
                int searchCenterChunkX,
                int searchCenterChunkZ
        ) {
            this.seed = seed;
            this.component = component;
            this.chunkRadiusUsed = chunkRadiusUsed;
            this.stage0Score = stage0Score;
            this.stage0HighScore = stage0HighScore;
            this.coarseScore = coarseScore;
            this.searchCenterChunkX = searchCenterChunkX;
            this.searchCenterChunkZ = searchCenterChunkZ;
        }
    }
    private static class Workspace {
    BetaTerrain173 terrain;
    byte[] stone;
    byte[] visited;
    int[] queue;
    int[] columnSeen;
    int[] candidateStarts;

    double[] coarseValues;
    boolean[] coarseFilled;
    int[] coarseLabels;
    int[] coarseQueue;
    boolean[] coarseLowerRequest;
    int[] coarseColumnSeen;
    int[] coarseColumnMinY;
    int[] coarseComponentColumns;
    double[] coarseChunk;
    double[] stage0Column;
    boolean[] featureColumnY8;
    boolean[] featureColumnY10;
    boolean[] featureColumnY12;

    BetaTerrain173 terrainForSeed(long seed) {
        if (terrain == null) {
            terrain = new BetaTerrain173(seed);
        } else {
            terrain.reseed(seed);
        }
        return terrain;
    }

    void prepare(int volume, int columnVolume) {
        if (stone == null || stone.length < volume) {
            stone = new byte[volume];
        } else {
            Arrays.fill(stone, 0, volume, (byte) 0);
        }

        if (visited == null || visited.length < volume) {
            visited = new byte[volume];
        } else {
            Arrays.fill(visited, 0, volume, (byte) 0);
        }

        if (queue == null || queue.length < volume) {
            queue = new int[volume];
        }

        if (columnSeen == null || columnSeen.length < columnVolume) {
            columnSeen = new int[columnVolume];
        } else {
            Arrays.fill(columnSeen, 0, columnVolume, 0);
        }

        if (candidateStarts == null || candidateStarts.length < volume) {
            candidateStarts = new int[volume];
        }
    }

    void prepareStage0() {
        if (stage0Column == null || stage0Column.length < COARSE_Y_LEVELS) {
            stage0Column = new double[COARSE_Y_LEVELS];
        }
    }

    void prepareCoarse(int coarseSize) {
        int total = coarseSize * coarseSize * COARSE_Y_LEVELS;

        if (coarseValues == null || coarseValues.length < total) {
            coarseValues = new double[total];
        }
        // Values do not need clearing because coarseFilled tells us whether the slot
        // has been written for this seed.

        if (coarseFilled == null || coarseFilled.length < total) {
            coarseFilled = new boolean[total];
        } else {
            Arrays.fill(coarseFilled, 0, total, false);
        }

        if (coarseLabels == null || coarseLabels.length < total) {
            coarseLabels = new int[total];
        }

        if (coarseQueue == null || coarseQueue.length < total) {
            coarseQueue = new int[total];
        }
        int columnTotal = coarseSize * coarseSize;
        if (coarseLowerRequest == null || coarseLowerRequest.length < columnTotal) {
            coarseLowerRequest = new boolean[columnTotal];
        }
        if (coarseColumnSeen == null || coarseColumnSeen.length < columnTotal) {
            coarseColumnSeen = new int[columnTotal];
        }
        if (coarseColumnMinY == null || coarseColumnMinY.length < columnTotal) {
            coarseColumnMinY = new int[columnTotal];
        }
        if (coarseComponentColumns == null || coarseComponentColumns.length < columnTotal) {
            coarseComponentColumns = new int[columnTotal];
        }
        if (featureColumnY8 == null || featureColumnY8.length < columnTotal) {
            featureColumnY8 = new boolean[columnTotal];
        }
        if (featureColumnY10 == null || featureColumnY10.length < columnTotal) {
            featureColumnY10 = new boolean[columnTotal];
        }
        if (featureColumnY12 == null || featureColumnY12.length < columnTotal) {
            featureColumnY12 = new boolean[columnTotal];
        }

        int chunkCoarseTotal = 5 * COARSE_Y_LEVELS * 5;
        if (coarseChunk == null || coarseChunk.length < chunkCoarseTotal) {
            coarseChunk = new double[chunkCoarseTotal];
        }
    }

    void resetCoarseComponentColumns(int columnTotal) {
        if (coarseColumnSeen != null) {
            Arrays.fill(coarseColumnSeen, 0, columnTotal, 0);
        }
    }
}



    private static class Component {
        int blocks = 0;
        int columns = 0;
        int floodQueueLength = 0;

        int minWorldX = Integer.MAX_VALUE;
        int maxWorldX = Integer.MIN_VALUE;
        int minWorldZ = Integer.MAX_VALUE;
        int maxWorldZ = Integer.MIN_VALUE;
        int minY = Integer.MAX_VALUE;
        int maxY = Integer.MIN_VALUE;

        long sumWorldX = 0;
        long sumWorldZ = 0;

        int centerX = 0;
        int centerZ = 0;

        boolean touchesBottom = false;
        boolean touchesSideBorder = false;
    }

    private static class PreviewData {
        final int sizeX;
        final int sizeZ;
        final int[] thickness;
        final int[] minY;
        final int[] maxY;
        final int maxThickness;

        PreviewData(
                int sizeX,
                int sizeZ,
                int[] thickness,
                int[] minY,
                int[] maxY,
                int maxThickness
        ) {
            this.sizeX = sizeX;
            this.sizeZ = sizeZ;
            this.thickness = thickness;
            this.minY = minY;
            this.maxY = maxY;
            this.maxThickness = maxThickness;
        }

        int thicknessAt(int x, int z) {
            if (x < 0 || x >= sizeX || z < 0 || z >= sizeZ) {
                return 0;
            }
            return thickness[x * sizeZ + z];
        }
    }

    private static class TopUpdate {
        final ResultRecord record;
        final int rank;

        TopUpdate(ResultRecord record, int rank) {
            this.record = record;
            this.rank = rank;
        }
    }

    private static class SideboardUpdate {
        final String boardLabel;
        final ResultRecord record;
        final int rank;

        SideboardUpdate(String boardLabel, ResultRecord record, int rank) {
            this.boardLabel = boardLabel;
            this.record = record;
            this.rank = rank;
        }
    }

    private static class ResultRecord {
        final long seed;

        final int blocks;
        final int columns;

        final int centerX;
        final int centerZ;

        final int minWorldX;
        final int maxWorldX;
        final int minWorldZ;
        final int maxWorldZ;

        final int minY;
        final int maxY;

        final int width;
        final int depth;
        final int footprintArea;

        final double fillPercent;
        final double avgThickness;

        final int chunkRadiusUsed;
        final boolean touchesSideBorder;

        final String previewPath;

        ResultRecord(long seed, Component c, String previewPath, int chunkRadiusUsed) {
            this.seed = seed;
            this.chunkRadiusUsed = chunkRadiusUsed;
            this.touchesSideBorder = c.touchesSideBorder;

            this.blocks = c.blocks;
            this.columns = c.columns;

            this.centerX = c.centerX;
            this.centerZ = c.centerZ;

            this.minWorldX = c.minWorldX;
            this.maxWorldX = c.maxWorldX;
            this.minWorldZ = c.minWorldZ;
            this.maxWorldZ = c.maxWorldZ;

            this.minY = c.minY;
            this.maxY = c.maxY;

            this.width = c.maxWorldX - c.minWorldX + 1;
            this.depth = c.maxWorldZ - c.minWorldZ + 1;
            this.footprintArea = width * depth;

            this.fillPercent = 100.0 * c.columns / (double) Math.max(1, footprintArea);
            this.avgThickness = c.blocks / (double) Math.max(1, c.columns);

            this.previewPath = previewPath;
        }

        SearchResult toSearchResult() {
            return new SearchResult(
                    seed,
                    blocks,
                    columns,
                    width,
                    depth,
                    minY,
                    maxY,
                    centerX,
                    centerZ,
                    previewPath
            );
        }
    }
}