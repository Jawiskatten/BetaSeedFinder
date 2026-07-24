import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public class HunterBenchmark {
    public static void main(String[] args) throws InterruptedException {
        long seedCount = args.length >= 1 ? Long.parseLong(args[0]) : 1_000_000L;
        int threads = args.length >= 2
                ? Integer.parseInt(args[1])
                : Math.max(1, Math.min(10, Runtime.getRuntime().availableProcessors() - 1));
        long sequenceSeed = args.length >= 3 ? Long.parseLong(args[2]) : 123456789L;
        String profile = args.length >= 4 ? args[3].trim().toLowerCase() : "general";
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
        settings.topResultsToKeep = 50;
        settings.savePreviews = false;

        settings.hunterMode = true;
        settings.megaMode = profile.equals("mega");
        settings.filterResearchEnabled = false;
        settings.hunterCoarseMinCells = 85;
        settings.hunterStage0Enabled = true;
        settings.hunterStage0Step = 4;
        settings.hunterStage0MinReentrySamples = 2;
        settings.hunterStage0MinUpperYIndex = 9;
        settings.hunterStage0HighEnabled = true;
        settings.hunterStage0HighMinUpperYIndex = 11;
        settings.hunterStage0HighMinReentrySamples = 5;
        settings.hunterStage0AuditEnabled = false;
        settings.featureLoggingEnabled = false;
        settings.performanceProfilerEnabled = false;
        settings.deterministicSeedMode = true;
        settings.deterministicSeedSequenceSeed = sequenceSeed;
        settings.runLabel = "benchmark_" + seedCount;

        System.out.println("Deterministic Hunter benchmark");
        System.out.println("Seeds: " + seedCount);
        System.out.println("Threads: " + threads);
        System.out.println("Sequence seed: " + sequenceSeed);
        System.out.println("Profile: " + (settings.megaMode ? "MEGA 30k+" : "GENERAL"));
        System.out.println("Research telemetry: OFF (lean same-range A/B)");
        System.out.println("Run this exact command before and after a patch.");
        System.out.println();

        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Throwable> error = new AtomicReference<>();
        long started = System.nanoTime();

        IslandSearchEngine engine = new IslandSearchEngine();
        engine.start(settings, new SearchListener() {
            @Override
            public void onProgress(long checked, int matches, int topUpdates, double seedsPerSecond) {
                if (checked % 100_000L < 500L || checked == seedCount) {
                    System.out.printf("progress checked=%d matches=%d speed=%.1f seeds/s%n",
                            checked, matches, seedsPerSecond);
                }
            }

            @Override
            public void onTopResult(SearchResult result, int rank) {
                System.out.println("top rank=" + rank + " seed=" + result.seed + " blocks=" + result.blocks);
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

        System.out.printf("BENCHMARK DONE | %.2fs | %.1f seeds/s%n", seconds, seedCount / seconds);
        System.out.println("Compare speed, stage survivor counts, candidate counts, matches, and leaderboards between the General and Mega runs.");
    }
}
