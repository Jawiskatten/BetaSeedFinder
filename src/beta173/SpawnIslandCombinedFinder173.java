package beta173;

import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Combined Beta 1.7.3 spawn-island search runner.
 *
 * Uses SpawnIslandFinder173.analyzeOrigin unchanged, so candidate classification
 * and the optimized exact worldgen/topology path are identical to the main
 * finder. Air candidates are printed/ranked normally, while every detached hit
 * involving water is also persisted to a text file for later inspection.
 */
public final class SpawnIslandCombinedFinder173 {
    private static final long WORK_BATCH = 256L;
    private static final int DIAGNOSTIC_GAP_RADIUS = 4;

    private SpawnIslandCombinedFinder173() {}

    public static void main(String[] args) throws Exception {
        Config config = Config.parse(args);
        if (config.help) {
            printUsage();
            return;
        }

        long endExclusive = config.startSeed + config.count;
        Path waterLog = config.waterLog != null
                ? Path.of(config.waterLog)
                : Path.of(String.format(Locale.ROOT, "water-hits-%d-%d.txt", config.startSeed, endExclusive - 1L));
        Path parent = waterLog.toAbsolutePath().getParent();
        if (parent != null) Files.createDirectories(parent);

        System.out.printf(Locale.ROOT,
                "Beta 1.7.3 combined spawn search: start=%d count=%d threads=%d maxRadius=%d%n",
                config.startSeed, config.count, config.threads, config.maxRadius);
        System.out.println("Same exact analyzer and hit criteria as SpawnIslandFinder173.");
        System.out.println("Air candidates print to console; water-involved detached hits are also saved to:");
        System.out.println("  " + waterLog.toAbsolutePath());

        AtomicLong cursor = new AtomicLong(0L);
        AtomicLong checked = new AtomicLong(0L);
        AtomicLong sandSpawns = new AtomicLong(0L);
        AtomicLong fastConnected = new AtomicLong(0L);
        AtomicLong expanded = new AtomicLong(0L);
        AtomicLong waterDetached = new AtomicLong(0L);
        AtomicLong mixedDetached = new AtomicLong(0L);
        AtomicLong airCandidates = new AtomicLong(0L);
        AtomicLong inconclusive = new AtomicLong(0L);
        AtomicBoolean done = new AtomicBoolean(false);

        List<SpawnIslandFinder173.Analysis> airHits = new ArrayList<>();
        List<SpawnIslandFinder173.Analysis> inconclusiveHits = new ArrayList<>();
        Object outputLock = new Object();
        Instant started = Instant.now();

        try (BufferedWriter waterWriter = Files.newBufferedWriter(
                waterLog,
                StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING,
                StandardOpenOption.WRITE)) {

            waterWriter.write("# Beta 1.7.3 water-involved detached spawn hits\n");
            waterWriter.write(String.format(Locale.ROOT,
                    "# start=%d count=%d endInclusive=%d maxRadius=%d\n",
                    config.startSeed, config.count, endExclusive - 1L, config.maxRadius));
            waterWriter.write("# Includes DETACHED_OVER_WATER and DETACHED_MIXED_AIR_WATER.\n");
            waterWriter.flush();

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
                            "checked=%d/%d (%.3f%%) rate=%.1f/s avg=%.1f/s sandSpawn=%d fastConnected=%d expanded=%d waterDetached=%d mixed=%d airCandidates=%d inconclusive=%d%n",
                            nowChecked, config.count, percent, recentRate, averageRate,
                            sandSpawns.get(), fastConnected.get(), expanded.get(), waterDetached.get(),
                            mixedDetached.get(), airCandidates.get(), inconclusive.get());
                    previousChecked = nowChecked;
                    previousNanos = now;
                }
            }, "combined-spawn-reporter");
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
                        long localFastConnected = 0L;
                        long localExpanded = 0L;
                        long localWater = 0L;
                        long localMixed = 0L;
                        long localAir = 0L;
                        long localInconclusive = 0L;

                        for (long index = batchStart; index < batchEnd; ++index) {
                            long seed = config.startSeed + index;
                            generator.reseed(seed);
                            SpawnIslandFinder173.Analysis result =
                                    SpawnIslandFinder173.analyzeOrigin(seed, generator, config.maxRadius);
                            ++localChecked;

                            if (result.spawnIsSand) ++localSand;
                            if (result.status == SpawnIslandFinder173.Status.CONNECTED_TO_MAIN_TERRAIN
                                    && result.radiusUsed == 0) ++localFastConnected;
                            if (result.radiusUsed > 0) ++localExpanded;

                            boolean pureWater = result.status == SpawnIslandFinder173.Status.DETACHED_OVER_WATER;
                            boolean mixed = result.status == SpawnIslandFinder173.Status.DETACHED_MIXED_AIR_WATER;
                            boolean air = result.status == SpawnIslandFinder173.Status.DETACHED_OVER_AIR || mixed;

                            if (pureWater) ++localWater;
                            if (mixed) ++localMixed;

                            if (pureWater || mixed) {
                                synchronized (outputLock) {
                                    try {
                                        waterWriter.write(formatAnalysis(result));
                                        waterWriter.newLine();
                                        // Hits are extremely rare; flush each one so a crash/reboot does not lose it.
                                        waterWriter.flush();
                                    } catch (IOException e) {
                                        throw new RuntimeException("Failed to write water hit log", e);
                                    }
                                }
                            }

                            if (result.status == SpawnIslandFinder173.Status.INCONCLUSIVE_LARGE_COMPONENT) {
                                ++localInconclusive;
                                synchronized (outputLock) {
                                    inconclusiveHits.add(result);
                                    System.out.println();
                                    System.out.println("*** INCONCLUSIVE COMPONENT (rerun with larger --max-radius) ***");
                                    System.out.println(formatAnalysis(result));
                                    System.out.println("***************************************************************");
                                }
                            }

                            if (air) {
                                ++localAir;
                                synchronized (outputLock) {
                                    airHits.add(result);
                                    System.out.println();
                                    System.out.println("*** AIR-BELOW DETACHED SPAWN CANDIDATE ***");
                                    System.out.println(formatAnalysis(result));
                                    if (mixed) System.out.println("(also saved to water-hit log)");
                                    System.out.println("******************************************");
                                }
                            }
                        }

                        sandSpawns.addAndGet(localSand);
                        fastConnected.addAndGet(localFastConnected);
                        expanded.addAndGet(localExpanded);
                        waterDetached.addAndGet(localWater);
                        mixedDetached.addAndGet(localMixed);
                        airCandidates.addAndGet(localAir);
                        inconclusive.addAndGet(localInconclusive);
                        checked.addAndGet(localChecked);
                    }
                }, "combined-spawn-worker-" + t);
                workers[t].start();
            }

            for (Thread worker : workers) worker.join();
            done.set(true);
            reporter.interrupt();

            double elapsed = Duration.between(started, Instant.now()).toMillis() / 1000.0D;
            double rate = elapsed > 0.0D ? checked.get() / elapsed : 0.0D;
            System.out.printf(Locale.ROOT,
                    "DONE checked=%d sandSpawn=%d fastConnected=%d expanded=%d waterDetached=%d mixed=%d airCandidates=%d inconclusive=%d elapsed=%.3fs rate=%.1f seeds/s%n",
                    checked.get(), sandSpawns.get(), fastConnected.get(), expanded.get(), waterDetached.get(),
                    mixedDetached.get(), airCandidates.get(), inconclusive.get(), elapsed, rate);
            System.out.println("Water-hit log: " + waterLog.toAbsolutePath());

            if (!airHits.isEmpty()) {
                synchronized (outputLock) {
                    airHits.sort((a, b) -> {
                        int clean = Integer.compare(a.diagonalExternalContacts, b.diagonalExternalContacts);
                        if (clean != 0) return clean;
                        int gap = Integer.compare(b.nearestExternalSolidCheb, a.nearestExternalSolidCheb);
                        if (gap != 0) return gap;
                        return Integer.compare(b.componentBlocks, a.componentBlocks);
                    });
                    System.out.println("Air-below candidates ranked by visual separation, then size:");
                    for (SpawnIslandFinder173.Analysis hit : airHits) {
                        System.out.println(formatAnalysis(hit));
                    }
                }
            }

            if (!inconclusiveHits.isEmpty()) {
                synchronized (outputLock) {
                    inconclusiveHits.sort((a, b) -> Long.compare(a.seed, b.seed));
                    System.out.println("Inconclusive seeds (rerun individually with --max-radius 2..4):");
                    for (SpawnIslandFinder173.Analysis hit : inconclusiveHits) {
                        System.out.println(formatAnalysis(hit));
                    }
                }
            }
        }
    }

    private static String formatAnalysis(SpawnIslandFinder173.Analysis r) {
        String nearest = r.nearestExternalSolidCheb == 0
                ? "n/a"
                : (r.nearestExternalSolidCheb > DIAGNOSTIC_GAP_RADIUS
                    ? ">=" + r.nearestExternalSolidCheb
                    : Integer.toString(r.nearestExternalSolidCheb));
        int widthX = r.componentBlocks == 0 ? 0 : r.maxX - r.minX + 1;
        int heightY = r.componentBlocks == 0 ? 0 : r.maxY - r.minY + 1;
        int widthZ = r.componentBlocks == 0 ? 0 : r.maxZ - r.minZ + 1;
        return String.format(Locale.ROOT,
                "seed=%d status=%s spawnSand=%s supportY=%d componentBlocks=%d radius=%d touchesBottom=%s touchesBoundary=%s "
                        + "airBelowFaces=%d waterBelowFaces=%d lavaBelowFaces=%d diagonalExternalContacts=%d nearestExternalSolidCheb=%s "
                        + "bbox=%dx%dx%d yRange=%d..%d blocks[sand=%d sandstone=%d stone=%d dirtGrass=%d other=%d]",
                r.seed, r.status, r.spawnIsSand, r.supportY,
                r.componentBlocks, r.radiusUsed, r.touchesBottom, r.touchesBoundary,
                r.airBelowFaces, r.waterBelowFaces, r.lavaBelowFaces,
                r.diagonalExternalContacts, nearest,
                widthX, heightY, widthZ, r.minY, r.maxY,
                r.sandBlocks, r.sandstoneBlocks, r.stoneBlocks,
                r.dirtGrassBlocks, r.otherSolidBlocks);
    }

    private static void printUsage() {
        System.out.println("Combined Beta 1.7.3 spawn-island finder + water-hit logger");
        System.out.println();
        System.out.println("  java -cp <classes> beta173.SpawnIslandCombinedFinder173 --start 106100000 --count 100000000 --threads 16 --max-radius 1");
        System.out.println();
        System.out.println("Options:");
        System.out.println("  --start <long>       first world seed (default 0)");
        System.out.println("  --count <long>       number of sequential seeds (default 10000)");
        System.out.println("  --threads <int>      worker threads (default available processors)");
        System.out.println("  --max-radius <int>   max chunk radius for topology proof (default 1; allowed 1..4)");
        System.out.println("  --water-log <path>   output file; default water-hits-START-END.txt");
        System.out.println("  --help               show this text");
    }

    private static final class Config {
        long startSeed = 0L;
        long count = 10_000L;
        int threads = Math.max(1, Runtime.getRuntime().availableProcessors());
        int maxRadius = 1;
        String waterLog;
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
                    case "--water-log": c.waterLog = requireValue(args, ++i, arg); break;
                    case "--help":
                    case "-h": c.help = true; break;
                    default: throw new IllegalArgumentException("Unknown option: " + arg);
                }
            }
            if (c.count < 0L) throw new IllegalArgumentException("--count must be >= 0");
            if (c.threads < 1) throw new IllegalArgumentException("--threads must be >= 1");
            if (c.maxRadius < 1 || c.maxRadius > 4) throw new IllegalArgumentException("--max-radius must be 1..4");
            return c;
        }

        private static String requireValue(String[] args, int index, String option) {
            if (index >= args.length) throw new IllegalArgumentException("Missing value for " + option);
            return args[index];
        }
    }
}
