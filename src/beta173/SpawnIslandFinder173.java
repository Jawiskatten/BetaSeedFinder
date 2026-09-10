package beta173;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Research CLI for the Beta 1.7.3 cave loophole:
 * can the guaranteed first spawn test at world (0,0) select sand that belongs
 * to a finite, cave-detached solid component with AIR underneath it?
 *
 * Water-supported ocean sand is tracked separately and is NOT counted as a
 * cave candidate.
 */
public final class SpawnIslandFinder173 {
    private static final int H = BetaChunk173.WORLD_HEIGHT;

    private SpawnIslandFinder173() {
    }

    public static void main(String[] args) throws Exception {
        Config config = Config.parse(args);
        if (config.help) {
            printUsage();
            return;
        }

        if (config.singleSeed != null) {
            BetaChunk173 generator = new BetaChunk173(config.singleSeed);
            Analysis result = analyzeOrigin(config.singleSeed, generator, config.maxRadius);
            printAnalysis(result);
            return;
        }

        System.out.printf(Locale.ROOT,
                "Beta 1.7.3 cave-spawn search: start=%d count=%d threads=%d maxRadius=%d%n",
                config.startSeed, config.count, config.threads, config.maxRadius);
        System.out.println("Target: vanilla first spawn coordinate (0,0), SAND spawn component detached from main terrain with AIR underneath.");

        AtomicLong cursor = new AtomicLong(0L);
        AtomicLong checked = new AtomicLong(0L);
        AtomicLong sandSpawns = new AtomicLong(0L);
        AtomicLong waterDetached = new AtomicLong(0L);
        AtomicLong airCandidates = new AtomicLong(0L);
        AtomicLong inconclusive = new AtomicLong(0L);
        AtomicBoolean done = new AtomicBoolean(false);
        List<Analysis> hitList = new ArrayList<>();
        Object hitLock = new Object();
        Instant startTime = Instant.now();

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
                double totalSeconds = Duration.between(startTime, Instant.now()).toMillis() / 1000.0D;
                double averageRate = totalSeconds > 0.0D ? nowChecked / totalSeconds : 0.0D;
                double percent = config.count == 0L ? 100.0D : 100.0D * nowChecked / (double) config.count;
                System.out.printf(Locale.ROOT,
                        "checked=%d/%d (%.3f%%) rate=%.1f/s avg=%.1f/s sandSpawn=%d waterDetached=%d airCandidates=%d inconclusive=%d%n",
                        nowChecked, config.count, percent, recentRate, averageRate,
                        sandSpawns.get(), waterDetached.get(), airCandidates.get(), inconclusive.get());
                previousChecked = nowChecked;
                previousNanos = now;
            }
        }, "spawn-island-reporter");
        reporter.setDaemon(true);
        reporter.start();

        Thread[] workers = new Thread[config.threads];
        for (int t = 0; t < workers.length; ++t) {
            workers[t] = new Thread(() -> {
                BetaChunk173 generator = new BetaChunk173(config.startSeed);
                while (true) {
                    long index = cursor.getAndIncrement();
                    if (index >= config.count) break;
                    long seed = config.startSeed + index;
                    generator.reseed(seed);
                    Analysis result = analyzeOrigin(seed, generator, config.maxRadius);
                    if (result.spawnIsSand) sandSpawns.incrementAndGet();
                    if (result.status == Status.DETACHED_OVER_WATER) waterDetached.incrementAndGet();
                    if (result.status == Status.INCONCLUSIVE_LARGE_COMPONENT) inconclusive.incrementAndGet();
                    if (isAirCandidate(result.status)) {
                        airCandidates.incrementAndGet();
                        synchronized (hitLock) {
                            hitList.add(result);
                            System.out.println();
                            System.out.println("*** AIR-BELOW DETACHED SPAWN CANDIDATE ***");
                            printAnalysis(result);
                            System.out.println("******************************************");
                        }
                    }
                    checked.incrementAndGet();
                }
            }, "spawn-island-worker-" + t);
            workers[t].start();
        }

        for (Thread worker : workers) worker.join();
        done.set(true);
        reporter.interrupt();

        double elapsed = Duration.between(startTime, Instant.now()).toMillis() / 1000.0D;
        double rate = elapsed > 0.0D ? checked.get() / elapsed : 0.0D;
        System.out.printf(Locale.ROOT,
                "DONE checked=%d sandSpawn=%d waterDetached=%d airCandidates=%d inconclusive=%d elapsed=%.3fs rate=%.1f seeds/s%n",
                checked.get(), sandSpawns.get(), waterDetached.get(), airCandidates.get(),
                inconclusive.get(), elapsed, rate);

        if (!hitList.isEmpty()) {
            synchronized (hitLock) {
                hitList.sort((a, b) -> Integer.compare(b.componentBlocks, a.componentBlocks));
                System.out.println("Air-below candidates ranked by detached solid component size:");
                for (Analysis hit : hitList) printAnalysis(hit);
            }
        }
    }

    private static boolean isAirCandidate(Status status) {
        return status == Status.DETACHED_OVER_AIR || status == Status.DETACHED_MIXED_AIR_WATER;
    }

    static Analysis analyzeOrigin(long seed, BetaChunk173 generator, int maxRadius) {
        int[] originChunk = generator.generateChunk(0, 0);
        int supportY = firstUncoveredY(originChunk, 0, 0);
        int supportBlock = blockAt(originChunk, 0, supportY, 0);
        if (supportBlock != BetaChunk173.SAND) {
            return new Analysis(seed, Status.NOT_SAND_SPAWN, false, supportY,
                    0, 0, false, false, 0, 0, 0);
        }

        for (int radius = 1; radius <= maxRadius; ++radius) {
            Region region = generateRegion(generator, radius, originChunk);
            Component component = floodSolidComponent(region, 0, supportY, 0);

            if (component.touchesBottom) {
                return analysisFromComponent(seed, Status.CONNECTED_TO_MAIN_TERRAIN,
                        supportY, radius, component);
            }
            if (!component.touchesBoundary) {
                Status detachedStatus;
                if (component.airBelowFaces > 0 && component.waterBelowFaces > 0) {
                    detachedStatus = Status.DETACHED_MIXED_AIR_WATER;
                } else if (component.airBelowFaces > 0) {
                    detachedStatus = Status.DETACHED_OVER_AIR;
                } else if (component.waterBelowFaces > 0) {
                    detachedStatus = Status.DETACHED_OVER_WATER;
                } else {
                    detachedStatus = Status.DETACHED_OTHER;
                }
                return analysisFromComponent(seed, detachedStatus, supportY, radius, component);
            }
        }

        Region region = generateRegion(generator, maxRadius, originChunk);
        Component component = floodSolidComponent(region, 0, supportY, 0);
        return analysisFromComponent(seed, Status.INCONCLUSIVE_LARGE_COMPONENT,
                supportY, maxRadius, component);
    }

    private static Analysis analysisFromComponent(long seed, Status status, int supportY,
                                                  int radius, Component component) {
        return new Analysis(seed, status, true, supportY,
                component.blocks, radius, component.touchesBottom, component.touchesBoundary,
                component.airBelowFaces, component.waterBelowFaces, component.lavaBelowFaces);
    }

    /** Mirrors World.getFirstUncoveredBlock: start at y=63, walk up while y+1 is non-air. */
    private static int firstUncoveredY(int[] chunk, int x, int z) {
        int y = 63;
        while (y < 127 && blockAt(chunk, x, y + 1, z) != BetaChunk173.AIR) ++y;
        return y;
    }

    private static int blockAt(int[] chunk, int x, int y, int z) {
        if (y < 0 || y >= 128) return BetaChunk173.AIR;
        return chunk[BetaChunk173.index(x, y, z)];
    }

    private static Region generateRegion(BetaChunk173 generator, int radius, int[] originChunk) {
        int chunksWide = radius * 2 + 1;
        int width = chunksWide * 16;
        int minBlock = -radius * 16;
        int[] blocks = new int[width * width * H];

        for (int chunkX = -radius; chunkX <= radius; ++chunkX) {
            for (int chunkZ = -radius; chunkZ <= radius; ++chunkZ) {
                int[] chunk = (chunkX == 0 && chunkZ == 0)
                        ? originChunk
                        : generator.generateChunk(chunkX, chunkZ);
                int regionX = (chunkX + radius) * 16;
                int regionZ = (chunkZ + radius) * 16;

                for (int x = 0; x < 16; ++x) {
                    for (int z = 0; z < 16; ++z) {
                        int source = BetaChunk173.index(x, 0, z);
                        int target = regionIndex(regionX + x, 0, regionZ + z, width);
                        System.arraycopy(chunk, source, blocks, target, H);
                    }
                }
            }
        }
        return new Region(minBlock, width, blocks);
    }

    private static Component floodSolidComponent(Region region, int worldX, int worldY, int worldZ) {
        int sx = worldX - region.minBlock;
        int sz = worldZ - region.minBlock;
        if (sx < 0 || sx >= region.width || sz < 0 || sz >= region.width
                || worldY < 0 || worldY >= H) {
            return new Component(0, false, true, 0, 0, 0);
        }

        int start = regionIndex(sx, worldY, sz, region.width);
        if (!BetaChunk173.isSolid(region.blocks[start])) {
            return new Component(0, false, false, 0, 0, 0);
        }

        boolean[] visited = new boolean[region.blocks.length];
        int[] queue = new int[region.blocks.length];
        int head = 0;
        int tail = 0;
        queue[tail++] = start;
        visited[start] = true;
        int count = 0;
        boolean bottom = false;
        boolean boundary = false;
        int airBelowFaces = 0;
        int waterBelowFaces = 0;
        int lavaBelowFaces = 0;

        final int plane = region.width * H;
        while (head < tail) {
            int index = queue[head++];
            ++count;

            int x = index / plane;
            int rem = index - x * plane;
            int z = rem / H;
            int y = rem - z * H;

            if (y == 0) bottom = true;
            if (x == 0 || z == 0 || x == region.width - 1 || z == region.width - 1) boundary = true;

            if (y > 0) {
                int below = region.blocks[index - 1];
                if (!BetaChunk173.isSolid(below)) {
                    if (below == BetaChunk173.AIR) {
                        ++airBelowFaces;
                    } else if (below == BetaChunk173.WATER_MOVING || below == BetaChunk173.WATER_STILL) {
                        ++waterBelowFaces;
                    } else if (below == BetaChunk173.LAVA_MOVING || below == BetaChunk173.LAVA_STILL) {
                        ++lavaBelowFaces;
                    }
                }
            }

            if (x > 0) tail = enqueue(region, visited, queue, tail, index - plane);
            if (x + 1 < region.width) tail = enqueue(region, visited, queue, tail, index + plane);
            if (z > 0) tail = enqueue(region, visited, queue, tail, index - H);
            if (z + 1 < region.width) tail = enqueue(region, visited, queue, tail, index + H);
            if (y > 0) tail = enqueue(region, visited, queue, tail, index - 1);
            if (y + 1 < H) tail = enqueue(region, visited, queue, tail, index + 1);

            if (bottom) break;
        }

        return new Component(count, bottom, boundary,
                airBelowFaces, waterBelowFaces, lavaBelowFaces);
    }

    private static int enqueue(Region region, boolean[] visited, int[] queue, int tail, int index) {
        if (!visited[index] && BetaChunk173.isSolid(region.blocks[index])) {
            visited[index] = true;
            queue[tail++] = index;
        }
        return tail;
    }

    private static int regionIndex(int x, int y, int z, int width) {
        return (x * width + z) * H + y;
    }

    private static void printAnalysis(Analysis result) {
        System.out.printf(Locale.ROOT,
                "seed=%d status=%s spawnSand=%s supportY=%d componentBlocks=%d radius=%d touchesBottom=%s touchesBoundary=%s airBelowFaces=%d waterBelowFaces=%d lavaBelowFaces=%d%n",
                result.seed, result.status, result.spawnIsSand, result.supportY,
                result.componentBlocks, result.radiusUsed, result.touchesBottom, result.touchesBoundary,
                result.airBelowFaces, result.waterBelowFaces, result.lavaBelowFaces);
    }

    private static void printUsage() {
        System.out.println("Beta 1.7.3 cave-detached spawn island finder");
        System.out.println();
        System.out.println("Search:");
        System.out.println("  java -cp <jar-or-classes> beta173.SpawnIslandFinder173 --start 0 --count 100000 --threads 8");
        System.out.println();
        System.out.println("Verify one seed:");
        System.out.println("  java -cp <jar-or-classes> beta173.SpawnIslandFinder173 --seed 12345 --max-radius 2");
        System.out.println();
        System.out.println("Only detached components with at least one AIR block directly beneath an exposed bottom face count as airCandidates.");
        System.out.println("Water-only detached sand is reported separately as waterDetached.");
        System.out.println();
        System.out.println("Options:");
        System.out.println("  --start <long>       first world seed (default 0)");
        System.out.println("  --count <long>       number of sequential seeds (default 10000)");
        System.out.println("  --threads <int>      worker threads (default available processors)");
        System.out.println("  --max-radius <int>   max chunk radius for topology proof (default 2; allowed 1..4)");
        System.out.println("  --seed <long>        analyze one seed only");
        System.out.println("  --help               show this text");
    }

    enum Status {
        NOT_SAND_SPAWN,
        CONNECTED_TO_MAIN_TERRAIN,
        DETACHED_OVER_WATER,
        DETACHED_OVER_AIR,
        DETACHED_MIXED_AIR_WATER,
        DETACHED_OTHER,
        INCONCLUSIVE_LARGE_COMPONENT
    }

    static final class Analysis {
        final long seed;
        final Status status;
        final boolean spawnIsSand;
        final int supportY;
        final int componentBlocks;
        final int radiusUsed;
        final boolean touchesBottom;
        final boolean touchesBoundary;
        final int airBelowFaces;
        final int waterBelowFaces;
        final int lavaBelowFaces;

        Analysis(long seed, Status status, boolean spawnIsSand, int supportY,
                 int componentBlocks, int radiusUsed, boolean touchesBottom, boolean touchesBoundary,
                 int airBelowFaces, int waterBelowFaces, int lavaBelowFaces) {
            this.seed = seed;
            this.status = status;
            this.spawnIsSand = spawnIsSand;
            this.supportY = supportY;
            this.componentBlocks = componentBlocks;
            this.radiusUsed = radiusUsed;
            this.touchesBottom = touchesBottom;
            this.touchesBoundary = touchesBoundary;
            this.airBelowFaces = airBelowFaces;
            this.waterBelowFaces = waterBelowFaces;
            this.lavaBelowFaces = lavaBelowFaces;
        }
    }

    private static final class Component {
        final int blocks;
        final boolean touchesBottom;
        final boolean touchesBoundary;
        final int airBelowFaces;
        final int waterBelowFaces;
        final int lavaBelowFaces;

        Component(int blocks, boolean touchesBottom, boolean touchesBoundary,
                  int airBelowFaces, int waterBelowFaces, int lavaBelowFaces) {
            this.blocks = blocks;
            this.touchesBottom = touchesBottom;
            this.touchesBoundary = touchesBoundary;
            this.airBelowFaces = airBelowFaces;
            this.waterBelowFaces = waterBelowFaces;
            this.lavaBelowFaces = lavaBelowFaces;
        }
    }

    private static final class Region {
        final int minBlock;
        final int width;
        final int[] blocks;

        Region(int minBlock, int width, int[] blocks) {
            this.minBlock = minBlock;
            this.width = width;
            this.blocks = blocks;
        }
    }

    private static final class Config {
        long startSeed = 0L;
        long count = 10_000L;
        int threads = Math.max(1, Runtime.getRuntime().availableProcessors());
        int maxRadius = 2;
        Long singleSeed;
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
                    case "--seed": c.singleSeed = Long.parseLong(requireValue(args, ++i, arg)); break;
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
