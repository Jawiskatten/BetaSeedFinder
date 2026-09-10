package beta173;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Research CLI for the Beta 1.7.3 cave loophole:
 * can the guaranteed first spawn test at world (0,0) select sand that belongs
 * to a finite, cave-detached solid component?
 *
 * A hit here is stronger than "some spawn candidate near zero": Beta tests
 * (0,0) first, so a valid hit is deterministic and does not depend on the
 * unseeded fallback random walk used when (0,0) is invalid.
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
                "Beta 1.7.3 detached-spawn search: start=%d count=%d threads=%d maxRadius=%d%n",
                config.startSeed, config.count, config.threads, config.maxRadius);
        System.out.println("Target: vanilla first spawn coordinate (0,0), top block SAND, solid support component detached from bedrock/main terrain.");

        AtomicLong cursor = new AtomicLong(0L);
        AtomicLong checked = new AtomicLong(0L);
        AtomicLong sandSpawns = new AtomicLong(0L);
        AtomicLong expanded = new AtomicLong(0L);
        AtomicLong inconclusive = new AtomicLong(0L);
        AtomicLong hits = new AtomicLong(0L);
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
                        "checked=%d/%d (%.3f%%) rate=%.1f/s avg=%.1f/s sandSpawn=%d expanded=%d inconclusive=%d hits=%d%n",
                        nowChecked, config.count, percent, recentRate, averageRate,
                        sandSpawns.get(), expanded.get(), inconclusive.get(), hits.get());
                previousChecked = nowChecked;
                previousNanos = now;
            }
        }, "spawn-island-reporter");
        reporter.setDaemon(true);
        reporter.start();

        Thread[] workers = new Thread[config.threads];
        for (int t = 0; t < workers.length; ++t) {
            final int workerId = t;
            workers[t] = new Thread(() -> {
                BetaChunk173 generator = new BetaChunk173(config.startSeed + workerId);
                while (true) {
                    long index = cursor.getAndIncrement();
                    if (index >= config.count) break;
                    long seed = config.startSeed + index;
                    generator.reseed(seed);
                    Analysis result = analyzeOrigin(seed, generator, config.maxRadius);
                    if (result.spawnIsSand) sandSpawns.incrementAndGet();
                    if (result.radiusUsed > 0) expanded.incrementAndGet();
                    if (result.status == Status.INCONCLUSIVE_LARGE_COMPONENT) inconclusive.incrementAndGet();
                    if (result.status == Status.DETACHED) {
                        hits.incrementAndGet();
                        synchronized (hitLock) {
                            hitList.add(result);
                            System.out.println();
                            System.out.println("*** DETACHED SPAWN HIT ***");
                            printAnalysis(result);
                            System.out.println("**************************");
                        }
                    } else if (result.status == Status.INCONCLUSIVE_LARGE_COMPONENT) {
                        synchronized (hitLock) {
                            System.out.printf(Locale.ROOT,
                                    "candidate-too-large seed=%d supportY=%d componentBlocks>=%d reached radius=%d%n",
                                    result.seed, result.supportY, result.componentBlocks, result.radiusUsed);
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
                "DONE checked=%d sandSpawn=%d expanded=%d inconclusive=%d hits=%d elapsed=%.3fs rate=%.1f seeds/s%n",
                checked.get(), sandSpawns.get(), expanded.get(), inconclusive.get(), hits.get(), elapsed, rate);

        if (!hitList.isEmpty()) {
            synchronized (hitLock) {
                hitList.sort((a, b) -> Integer.compare(b.componentBlocks, a.componentBlocks));
                System.out.println("Hits ranked by detached solid component size:");
                for (Analysis hit : hitList) printAnalysis(hit);
            }
        }
    }

    static Analysis analyzeOrigin(long seed, BetaChunk173 generator, int maxRadius) {
        int[] originChunk = generator.generateChunk(0, 0);
        int supportY = firstUncoveredY(originChunk, 0, 0);
        int supportBlock = blockAt(originChunk, 0, supportY, 0);
        if (supportBlock != BetaChunk173.SAND) {
            return new Analysis(seed, Status.NOT_SAND_SPAWN, false, supportY, 0, 0, false, false);
        }

        // Since (0,0) is a corner of chunk (0,0), a genuinely detached component
        // often crosses into negative chunks. Build symmetric regions around the
        // world origin and expand only when the component reaches the current edge.
        for (int radius = 1; radius <= maxRadius; ++radius) {
            Region region = generateRegion(generator, radius, originChunk);
            Component component = floodSolidComponent(region, 0, supportY, 0);

            if (component.touchesBottom) {
                return new Analysis(seed, Status.CONNECTED_TO_MAIN_TERRAIN, true, supportY,
                        component.blocks, radius, true, component.touchesBoundary);
            }
            if (!component.touchesBoundary) {
                return new Analysis(seed, Status.DETACHED, true, supportY,
                        component.blocks, radius, false, false);
            }
        }

        Region region = generateRegion(generator, maxRadius, originChunk);
        Component component = floodSolidComponent(region, 0, supportY, 0);
        return new Analysis(seed, Status.INCONCLUSIVE_LARGE_COMPONENT, true, supportY,
                component.blocks, maxRadius, component.touchesBottom, component.touchesBoundary);
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
            return new Component(0, false, true);
        }

        int start = regionIndex(sx, worldY, sz, region.width);
        if (!BetaChunk173.isSolid(region.blocks[start])) {
            return new Component(0, false, false);
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

            if (x > 0) tail = enqueue(region, visited, queue, tail, index - plane);
            if (x + 1 < region.width) tail = enqueue(region, visited, queue, tail, index + plane);
            if (z > 0) tail = enqueue(region, visited, queue, tail, index - H);
            if (z + 1 < region.width) tail = enqueue(region, visited, queue, tail, index + H);
            if (y > 0) tail = enqueue(region, visited, queue, tail, index - 1);
            if (y + 1 < H) tail = enqueue(region, visited, queue, tail, index + 1);

            // Reaching bedrock proves this is ordinary/main terrain, so there is
            // no reason to flood millions more blocks in an expanded region.
            if (bottom) break;
        }

        return new Component(count, bottom, boundary);
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
                "seed=%d status=%s spawnSand=%s supportY=%d componentBlocks=%d radius=%d touchesBottom=%s touchesBoundary=%s%n",
                result.seed, result.status, result.spawnIsSand, result.supportY,
                result.componentBlocks, result.radiusUsed, result.touchesBottom, result.touchesBoundary);
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
        DETACHED,
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

        Analysis(long seed, Status status, boolean spawnIsSand, int supportY,
                 int componentBlocks, int radiusUsed, boolean touchesBottom, boolean touchesBoundary) {
            this.seed = seed;
            this.status = status;
            this.spawnIsSand = spawnIsSand;
            this.supportY = supportY;
            this.componentBlocks = componentBlocks;
            this.radiusUsed = radiusUsed;
            this.touchesBottom = touchesBottom;
            this.touchesBoundary = touchesBoundary;
        }
    }

    private static final class Component {
        final int blocks;
        final boolean touchesBottom;
        final boolean touchesBoundary;

        Component(int blocks, boolean touchesBottom, boolean touchesBoundary) {
            this.blocks = blocks;
            this.touchesBottom = touchesBottom;
            this.touchesBoundary = touchesBoundary;
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
