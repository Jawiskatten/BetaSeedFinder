import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Long random-seed monster hunt with only the logs needed for reviewing results.
 *
 * Default: 30,000,000 seeds, which is roughly one 8-hour night around 1,000 seeds/s.
 * Usage: java -cp bin OvernightHunter [seedCount] [threads] [general|mega]
 */
public class OvernightHunter {
    public static void main(String[] args) throws InterruptedException {
        long seedCount = args.length >= 1 ? Long.parseLong(args[0]) : 30_000_000L;
        int threads = args.length >= 2
                ? Integer.parseInt(args[1])
                : Math.max(1, Math.min(10, Runtime.getRuntime().availableProcessors() - 1));
        String profile = args.length >= 3 ? args[2].trim().toLowerCase() : "mega";
        if (!profile.equals("general") && !profile.equals("mega")) {
            throw new IllegalArgumentException("Profile must be general or mega");
        }

        SearchSettings settings = new SearchSettings();
        settings.chunkRadius = 7;
        settings.seedsToCheck = seedCount;
        settings.threads = threads;

        settings.minBlocks = 1000;
        settings.minColumns = 100;
        settings.minYForMatch = 60;
        settings.minWidth = 10;
        settings.minDepth = 10;
        settings.minAvgThickness = 2.0;

        // Keep enough entries to browse tomorrow, but do not generate previews overnight.
        settings.topResultsToKeep = 100;
        settings.savePreviews = false;

        settings.hunterMode = true;
        settings.megaMode = profile.equals("mega");
        settings.filterResearchEnabled = true;
        settings.hunterCoarseMinCells = 85;
        settings.hunterStage0Enabled = true;
        settings.hunterStage0Step = 4;
        settings.hunterStage0MinReentrySamples = 2;
        settings.hunterStage0MinUpperYIndex = 9;
        settings.hunterStage0HighEnabled = true;
        settings.hunterStage0HighMinUpperYIndex = 11;
        settings.hunterStage0HighMinReentrySamples = 5;

        // Overnight research mode: collect broad future-test data at low sample rates.
        // The profiler stays off because it is not needed for filter/model research.
        settings.hunterStage0AuditEnabled = true;
        settings.hunterStage0AuditSampleMask = 4095L;
        settings.featureLoggingEnabled = true;
        settings.performanceProfilerEnabled = false;
        settings.debugLogInterval = 1_000_000L;

        settings.deterministicSeedMode = false;
        settings.runLabel = "overnight_" + seedCount;

        System.out.println("Overnight Hunter");
        System.out.println("Random seeds: " + seedCount);
        System.out.println("Threads: " + threads);
        System.out.println("Profile: " + (settings.megaMode ? "MEGA 30k+" : "GENERAL"));
        System.out.println("Top entries kept per board: " + settings.topResultsToKeep);
        System.out.println("Boards: largest blocks | filled footprint | raw footprint");
        System.out.println("Research telemetry: features ON | Stage0 audit ~1/4096 | profiler OFF");
        if (settings.megaMode) {
            System.out.println("Mega live gates: Upper>=8 | High>=6 | validated topology union before coarse");
        }
        System.out.println("Feature sampling: all coarse >=30 + ~1/256 lower-score rows");
        System.out.println();

        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Throwable> error = new AtomicReference<>();
        AtomicLong lastPrinted = new AtomicLong(0L);
        long started = System.nanoTime();

        IslandSearchEngine engine = new IslandSearchEngine();
        Runtime.getRuntime().addShutdownHook(new Thread(engine::stop, "overnight-stop"));

        engine.start(settings, new SearchListener() {
            @Override
            public void onProgress(long checked, int matches, int topUpdates, double seedsPerSecond) {
                long previous = lastPrinted.get();
                if (checked == seedCount || checked - previous >= 1_000_000L) {
                    if (lastPrinted.compareAndSet(previous, checked)) {
                        System.out.printf(
                                "progress checked=%d matches=%d largestUpdates=%d speed=%.1f seeds/s%n",
                                checked,
                                matches,
                                topUpdates,
                                seedsPerSecond
                        );
                    }
                }
            }

            @Override
            public void onTopResult(SearchResult result, int rank) {
                System.out.println(
                        "largest rank=" + rank
                                + " seed=" + result.seed
                                + " blocks=" + result.blocks
                                + " footprint=" + result.width + "x" + result.depth
                );
            }

            @Override
            public void onLog(String message) {
                System.out.println(message);
            }

            @Override
            public void onFinished() {
                done.countDown();
            }

            @Override
            public void onError(Throwable throwable) {
                error.set(throwable);
                throwable.printStackTrace();
                done.countDown();
            }
        });

        done.await();
        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;

        if (error.get() != null) {
            System.exit(2);
        }

        System.out.printf("OVERNIGHT DONE | %.2fh | %.1f seeds/s%n", seconds / 3600.0, seedCount / seconds);
    }
}
