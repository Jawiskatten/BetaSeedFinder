package beta173;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Companion explorer for unusual Beta 1.7.3 water-detached spawn terrain.
 *
 * This intentionally reuses SpawnIslandFinder173.analyzeOrigin so the worldgen
 * and topology rules stay identical to the main cave-spawn finder. It does not
 * change or filter the main search; it only records DETACHED_OVER_WATER hits
 * and presents richer ranking/telemetry for them.
 */
public final class WaterDetachedFinder173 {
    private static final long WORK_BATCH = 256L;
    private static final int SEA_LEVEL = BetaChunk173.SEA_LEVEL;

    private WaterDetachedFinder173() {}

    public static void main(String[] args) throws Exception {
        Config config = Config.parse(args);
        if (config.help) {
            printUsage();
            return;
        }

        System.out.printf(Locale.ROOT,
                "Beta 1.7.3 water-detached explorer: start=%d count=%d threads=%d maxRadius=%d top=%d%n",
                config.startSeed, config.count, config.threads, config.maxRadius, config.top);
        System.out.println("Uses the exact same analyzer as SpawnIslandFinder173; only DETACHED_OVER_WATER results are collected.");

        AtomicLong cursor = new AtomicLong(0L);
        AtomicLong checked = new AtomicLong(0L);
        AtomicLong sandSpawns = new AtomicLong(0L);
        AtomicLong waterHits = new AtomicLong(0L);
        AtomicBoolean done = new AtomicBoolean(false);
        List<SpawnIslandFinder173.Analysis> hits = new ArrayList<>();
        Object hitLock = new Object();
        Instant started = Instant.now();

        Thread reporter = new Thread(() -> {
            long previousChecked = 0L;
            long previousNanos = System.nanoTime();
            while (!done.get()) {
                try {
                    Thread.sleep(2000L);
                } catch (InterruptedException ignored) {
                    return;
                }
                long nowChecked = checked.get();
                long now = System.nanoTime();
                double seconds = (now - previousNanos) / 1_000_000_000.0D;
                double recentRate = seconds > 0.0D ? (nowChecked - previousChecked) / seconds : 0.0D;
                double totalSeconds = Duration.between(started, Instant.now()).toMillis() / 1000.0D;
                double averageRate = totalSeconds > 0.0D ? nowChecked / totalSeconds : 0.0D;
                double percent = config.count == 0L ? 100.0D : 100.0D * nowChecked / (double) config.count;
                System.out.printf(Locale.ROOT,
                        "checked=%d/%d (%.3f%%) rate=%.1f/s avg=%.1f/s sandSpawn=%d waterHits=%d%n",
                        nowChecked, config.count, percent, recentRate, averageRate,
                        sandSpawns.get(), waterHits.get());
                previousChecked = nowChecked;
                previousNanos = now;
            }
        }, "water-detached-reporter");
        reporter.setDaemon(true);
        reporter.start();

        Thread[] workers = new Thread[config.threads];
        for (int t = 0; t < workers.length; ++t) {
            workers[t] = new Thread(() -> {
                BetaChunk173 generator = new BetaChunk173(config.startSeed);
                while (true) {
                    long batchStart = cursor.getAndAdd(WORK_BATCH);
                    if (batchStart >= config.count) break;
                    long batchEnd = Math.min(config.count, batchStart + WORK_BATCH);
                    long localChecked = 0L;
                    long localSand = 0L;
                    long localWater = 0L;

                    for (long index = batchStart; index < batchEnd; ++index) {
                        long seed = config.startSeed + index;
                        generator.reseed(seed);
                        SpawnIslandFinder173.Analysis result =
                                SpawnIslandFinder173.analyzeOrigin(seed, generator, config.maxRadius);
                        ++localChecked;
                        if (result.spawnIsSand) ++localSand;

                        if (result.status == SpawnIslandFinder173.Status.DETACHED_OVER_WATER) {
                            ++localWater;
                            synchronized (hitLock) {
                                hits.add(result);
                                System.out.println();
                                System.out.println("*** WATER-DETACHED SPAWN ***");
                                printAnalysis(result);
                                System.out.println("****************************");
                            }
                        }
                    }

                    checked.addAndGet(localChecked);
                    sandSpawns.addAndGet(localSand);
                    waterHits.addAndGet(localWater);
                }
            }, "water-detached-worker-" + t);
            workers[t].start();
        }

        for (Thread worker : workers) worker.join();
        done.set(true);
        reporter.interrupt();

        double elapsed = Duration.between(started, Instant.now()).toMillis() / 1000.0D;
        double rate = elapsed > 0.0D ? checked.get() / elapsed : 0.0D;
        System.out.printf(Locale.ROOT,
                "DONE checked=%d sandSpawn=%d waterHits=%d elapsed=%.3fs rate=%.1f seeds/s%n",
                checked.get(), sandSpawns.get(), waterHits.get(), elapsed, rate);

        if (!hits.isEmpty()) {
            hits.sort(Comparator
                    .comparingInt((SpawnIslandFinder173.Analysis a) -> a.componentBlocks).reversed()
                    .thenComparing(Comparator.comparingInt((SpawnIslandFinder173.Analysis a) -> a.maxY).reversed())
                    .thenComparingInt(a -> a.diagonalExternalContacts)
                    .thenComparing(Comparator.comparingInt((SpawnIslandFinder173.Analysis a) -> a.nearestExternalSolidCheb).reversed()));

            int limit = config.top <= 0 ? hits.size() : Math.min(config.top, hits.size());
            System.out.printf(Locale.ROOT,
                    "Water-detached hits ranked by component size, then height/separation (showing %d/%d):%n",
                    limit, hits.size());
            for (int i = 0; i < limit; ++i) {
                System.out.printf(Locale.ROOT, "#%d ", i + 1);
                printAnalysis(hits.get(i));
            }
        }
    }

    private static void printAnalysis(SpawnIslandFinder173.Analysis a) {
        int widthX = a.componentBlocks == 0 ? 0 : a.maxX - a.minX + 1;
        int heightY = a.componentBlocks == 0 ? 0 : a.maxY - a.minY + 1;
        int widthZ = a.componentBlocks == 0 ? 0 : a.maxZ - a.minZ + 1;
        int topAboveSea = a.componentBlocks == 0 ? 0 : a.maxY - SEA_LEVEL;
        String nearest = a.nearestExternalSolidCheb == 0
                ? "n/a"
                : (a.nearestExternalSolidCheb > 4 ? ">=" + a.nearestExternalSolidCheb
                : Integer.toString(a.nearestExternalSolidCheb));

        System.out.printf(Locale.ROOT,
                "seed=%d supportY=%d componentBlocks=%d waterBelowFaces=%d "
                        + "diagonalExternalContacts=%d nearestExternalSolidCheb=%s "
                        + "bbox=%dx%dx%d yRange=%d..%d topAboveSea=%+d "
                        + "blocks[sand=%d sandstone=%d stone=%d dirtGrass=%d other=%d]%n",
                a.seed, a.supportY, a.componentBlocks, a.waterBelowFaces,
                a.diagonalExternalContacts, nearest,
                widthX, heightY, widthZ, a.minY, a.maxY, topAboveSea,
                a.sandBlocks, a.sandstoneBlocks, a.stoneBlocks,
                a.dirtGrassBlocks, a.otherSolidBlocks);
    }

    private static void printUsage() {
        System.out.println("Beta 1.7.3 water-detached spawn explorer");
        System.out.println();
        System.out.println("Example:");
        System.out.println("  java -cp build/java/classes beta173.WaterDetachedFinder173 --start 0 --count 100000000 --threads 16 --max-radius 1 --top 50");
        System.out.println();
        System.out.println("Options:");
        System.out.println("  --start <long>       first world seed (default 0)");
        System.out.println("  --count <long>       number of sequential seeds (default 1000000)");
        System.out.println("  --threads <int>      worker threads (default available processors)");
        System.out.println("  --max-radius <int>   topology proof radius, 1..4 (default 1)");
        System.out.println("  --top <int>          ranked results printed at end; 0 means all (default 50)");
        System.out.println("  --help               show this text");
    }

    private static final class Config {
        long startSeed = 0L;
        long count = 1_000_000L;
        int threads = Math.max(1, Runtime.getRuntime().availableProcessors());
        int maxRadius = 1;
        int top = 50;
        boolean help;

        static Config parse(String[] args) {
            Config c = new Config();
            for (int i = 0; i < args.length; ++i) {
                String arg = args[i];
                switch (arg) {
                    case "--start": c.startSeed = Long.parseLong(requireValue(args, ++i, arg)); break;
                    case "--count": c.count = Long.parseLong(requireValue(args, ++i, arg)); break;
                    case "--threads": c.threads = Integer.parseInt(requireValue(args, ++i, arg)); break;
                    case "--max-radius": c.maxRadius = Integer.parseInt(requireValue(args, ++i, arg)); break;
                    case "--top": c.top = Integer.parseInt(requireValue(args, ++i, arg)); break;
                    case "--help":
                    case "-h": c.help = true; break;
                    default: throw new IllegalArgumentException("Unknown option: " + arg);
                }
            }
            if (c.count < 0L) throw new IllegalArgumentException("--count must be >= 0");
            if (c.threads < 1) throw new IllegalArgumentException("--threads must be >= 1");
            if (c.maxRadius < 1 || c.maxRadius > 4) throw new IllegalArgumentException("--max-radius must be 1..4");
            if (c.top < 0) throw new IllegalArgumentException("--top must be >= 0");
            return c;
        }

        private static String requireValue(String[] args, int index, String option) {
            if (index >= args.length) throw new IllegalArgumentException("Missing value for " + option);
            return args[index];
        }
    }
}
