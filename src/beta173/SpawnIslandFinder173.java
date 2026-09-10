package beta173;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Exact Beta 1.7.3 spawn-island research CLI.
 *
 * Hit criteria are intentionally unchanged: a spawn is interesting when the
 * vanilla first coordinate (0,0) selects SAND and its 6-connected collision-
 * solid component is detached from bedrock/main terrain. AIR-vs-water under
 * exposed bottom faces is classification/telemetry only, never a rejection.
 */
public final class SpawnIslandFinder173 {
    private static final int H = BetaChunk173.WORLD_HEIGHT;
    private static final long WORK_BATCH = 256L;
    private static final int DIAGNOSTIC_GAP_RADIUS = 4;

    private SpawnIslandFinder173() {}

    public static void main(String[] args) throws Exception {
        Config config = Config.parse(args);
        if (config.help) {
            printUsage();
            return;
        }

        if (config.singleSeed != null) {
            BetaChunk173 generator = new BetaChunk173(config.singleSeed);
            printAnalysis(analyzeOrigin(config.singleSeed, generator, config.maxRadius));
            return;
        }

        System.out.printf(Locale.ROOT,
                "Beta 1.7.3 cave-spawn search: start=%d count=%d threads=%d maxRadius=%d%n",
                config.startSeed, config.count, config.threads, config.maxRadius);
        System.out.println("Hit criteria unchanged. Extra diagnostics rank visual separation; they do not filter candidates.");
        System.out.println("Fast path: direct solid-column proof -> origin-chunk flood -> expanded region only when still unresolved.");

        AtomicLong cursor = new AtomicLong(0L);
        AtomicLong checked = new AtomicLong(0L);
        AtomicLong sandSpawns = new AtomicLong(0L);
        AtomicLong fastConnected = new AtomicLong(0L);
        AtomicLong expanded = new AtomicLong(0L);
        AtomicLong waterDetached = new AtomicLong(0L);
        AtomicLong airCandidates = new AtomicLong(0L);
        AtomicLong inconclusive = new AtomicLong(0L);
        AtomicBoolean done = new AtomicBoolean(false);
        List<Analysis> hitList = new ArrayList<>();
        List<Analysis> inconclusiveList = new ArrayList<>();
        Object outputLock = new Object();
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
                        "checked=%d/%d (%.3f%%) rate=%.1f/s avg=%.1f/s sandSpawn=%d fastConnected=%d expanded=%d waterDetached=%d airCandidates=%d inconclusive=%d%n",
                        nowChecked, config.count, percent, recentRate, averageRate,
                        sandSpawns.get(), fastConnected.get(), expanded.get(), waterDetached.get(),
                        airCandidates.get(), inconclusive.get());
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
                    long batchStart = cursor.getAndAdd(WORK_BATCH);
                    if (batchStart >= config.count) break;
                    long batchEnd = Math.min(config.count, batchStart + WORK_BATCH);

                    long localChecked = 0L;
                    long localSand = 0L;
                    long localFastConnected = 0L;
                    long localExpanded = 0L;
                    long localWaterDetached = 0L;
                    long localAir = 0L;
                    long localInconclusive = 0L;

                    for (long index = batchStart; index < batchEnd; ++index) {
                        long seed = config.startSeed + index;
                        generator.reseed(seed);
                        Analysis result = analyzeOrigin(seed, generator, config.maxRadius);
                        ++localChecked;
                        if (result.spawnIsSand) ++localSand;
                        if (result.status == Status.CONNECTED_TO_MAIN_TERRAIN && result.radiusUsed == 0) {
                            ++localFastConnected;
                        }
                        if (result.radiusUsed > 0) ++localExpanded;
                        if (result.status == Status.DETACHED_OVER_WATER) ++localWaterDetached;
                        if (result.status == Status.INCONCLUSIVE_LARGE_COMPONENT) {
                            ++localInconclusive;
                            synchronized (outputLock) {
                                inconclusiveList.add(result);
                                System.out.println();
                                System.out.println("*** INCONCLUSIVE COMPONENT (rerun with larger --max-radius) ***");
                                printAnalysis(result);
                                System.out.println("***************************************************************");
                            }
                        }
                        if (isAirCandidate(result.status)) {
                            ++localAir;
                            synchronized (outputLock) {
                                hitList.add(result);
                                System.out.println();
                                System.out.println("*** AIR-BELOW DETACHED SPAWN CANDIDATE ***");
                                printAnalysis(result);
                                System.out.println("******************************************");
                            }
                        }
                    }

                    sandSpawns.addAndGet(localSand);
                    fastConnected.addAndGet(localFastConnected);
                    expanded.addAndGet(localExpanded);
                    waterDetached.addAndGet(localWaterDetached);
                    airCandidates.addAndGet(localAir);
                    inconclusive.addAndGet(localInconclusive);
                    checked.addAndGet(localChecked);
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
                "DONE checked=%d sandSpawn=%d fastConnected=%d expanded=%d waterDetached=%d airCandidates=%d inconclusive=%d elapsed=%.3fs rate=%.1f seeds/s%n",
                checked.get(), sandSpawns.get(), fastConnected.get(), expanded.get(), waterDetached.get(),
                airCandidates.get(), inconclusive.get(), elapsed, rate);

        if (!hitList.isEmpty()) {
            synchronized (outputLock) {
                hitList.sort((a, b) -> {
                    int clean = Integer.compare(a.diagonalExternalContacts, b.diagonalExternalContacts);
                    if (clean != 0) return clean;
                    int gap = Integer.compare(b.nearestExternalSolidCheb, a.nearestExternalSolidCheb);
                    if (gap != 0) return gap;
                    return Integer.compare(b.componentBlocks, a.componentBlocks);
                });
                System.out.println("Air-below candidates ranked by visual separation, then size:");
                for (Analysis hit : hitList) printAnalysis(hit);
            }
        }

        if (!inconclusiveList.isEmpty()) {
            synchronized (outputLock) {
                inconclusiveList.sort((a, b) -> Long.compare(a.seed, b.seed));
                System.out.println("Inconclusive seeds (rerun individually with --max-radius 2..4):");
                for (Analysis candidate : inconclusiveList) printAnalysis(candidate);
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
            return Analysis.notSand(seed, supportY);
        }

        // Exact one-way proof: a fully solid vertical path to y=0 means this is
        // definitely main terrain. It cannot hide a detached candidate.
        if (solidColumnToBottom(originChunk, 0, supportY, 0)) {
            return Analysis.fastConnected(seed, supportY);
        }

        // Second cheap proof: flood only chunk (0,0). If that reaches y=0, the
        // component is connected. Otherwise preserve the old expanded-region
        // logic exactly; local boundary contact never rejects a candidate.
        Region originRegion = new Region(0, 16, originChunk);
        Component local = floodSolidComponent(originRegion, 0, supportY, 0, false);
        if (local.touchesBottom) {
            return analysisFromComponent(seed, Status.CONNECTED_TO_MAIN_TERRAIN, supportY, 0, local);
        }

        for (int radius = 1; radius <= maxRadius; ++radius) {
            Region region = generateRegion(generator, radius, originChunk);
            Component component = floodSolidComponent(region, 0, supportY, 0, true);

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
        Component component = floodSolidComponent(region, 0, supportY, 0, true);
        return analysisFromComponent(seed, Status.INCONCLUSIVE_LARGE_COMPONENT,
                supportY, maxRadius, component);
    }

    private static boolean solidColumnToBottom(int[] chunk, int x, int supportY, int z) {
        for (int y = supportY; y >= 0; --y) {
            if (!BetaChunk173.isSolid(blockAt(chunk, x, y, z))) return false;
        }
        return true;
    }

    private static Analysis analysisFromComponent(long seed, Status status, int supportY,
                                                  int radius, Component component) {
        return new Analysis(seed, status, true, supportY,
                component.blocks, radius, component.touchesBottom, component.touchesBoundary,
                component.airBelowFaces, component.waterBelowFaces, component.lavaBelowFaces,
                component.diagonalExternalContacts, component.nearestExternalSolidCheb,
                component.minX, component.maxX, component.minY, component.maxY,
                component.minZ, component.maxZ,
                component.sandBlocks, component.sandstoneBlocks, component.stoneBlocks,
                component.dirtGrassBlocks, component.otherSolidBlocks);
    }

    /** Mirrors World.getFirstUncoveredBlock: start at y=63, walk up while y+1 is non-air. */
    private static int firstUncoveredY(int[] chunk, int x, int z) {
        int y = 63;
        while (y < 127 && blockAt(chunk, x, y + 1, z) != BetaChunk173.AIR) ++y;
        return y;
    }

    private static int blockAt(int[] chunk, int x, int y, int z) {
        if (y < 0 || y >= H) return BetaChunk173.AIR;
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

    private static Component floodSolidComponent(Region region, int worldX, int worldY, int worldZ,
                                                 boolean collectDiagnostics) {
        int sx = worldX - region.minBlock;
        int sz = worldZ - region.minBlock;
        if (sx < 0 || sx >= region.width || sz < 0 || sz >= region.width || worldY < 0 || worldY >= H) {
            return Component.empty(true);
        }

        int start = regionIndex(sx, worldY, sz, region.width);
        if (!BetaChunk173.isSolid(region.blocks[start])) return Component.empty(false);

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
        int sandBlocks = 0;
        int sandstoneBlocks = 0;
        int stoneBlocks = 0;
        int dirtGrassBlocks = 0;
        int otherSolidBlocks = 0;
        int minX = Integer.MAX_VALUE, minY = Integer.MAX_VALUE, minZ = Integer.MAX_VALUE;
        int maxX = Integer.MIN_VALUE, maxY = Integer.MIN_VALUE, maxZ = Integer.MIN_VALUE;

        final int plane = region.width * H;
        while (head < tail) {
            int index = queue[head++];
            ++count;

            int x = index / plane;
            int rem = index - x * plane;
            int z = rem / H;
            int y = rem - z * H;
            int worldBlockX = region.minBlock + x;
            int worldBlockZ = region.minBlock + z;

            minX = Math.min(minX, worldBlockX); maxX = Math.max(maxX, worldBlockX);
            minY = Math.min(minY, y); maxY = Math.max(maxY, y);
            minZ = Math.min(minZ, worldBlockZ); maxZ = Math.max(maxZ, worldBlockZ);

            int block = region.blocks[index];
            if (block == BetaChunk173.SAND) ++sandBlocks;
            else if (block == BetaChunk173.SANDSTONE) ++sandstoneBlocks;
            else if (block == BetaChunk173.STONE) ++stoneBlocks;
            else if (block == BetaChunk173.DIRT || block == BetaChunk173.GRASS) ++dirtGrassBlocks;
            else ++otherSolidBlocks;

            if (y == 0) bottom = true;
            if (x == 0 || z == 0 || x == region.width - 1 || z == region.width - 1) boundary = true;

            if (y > 0) {
                int below = region.blocks[index - 1];
                if (!BetaChunk173.isSolid(below)) {
                    if (below == BetaChunk173.AIR) ++airBelowFaces;
                    else if (below == BetaChunk173.WATER_MOVING || below == BetaChunk173.WATER_STILL) ++waterBelowFaces;
                    else if (below == BetaChunk173.LAVA_MOVING || below == BetaChunk173.LAVA_STILL) ++lavaBelowFaces;
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

        int diagonalContacts = 0;
        int nearestExternalCheb = 0;
        if (collectDiagnostics && !bottom) {
            diagonalContacts = countDiagonalExternalContacts(region, visited, queue, tail);
            nearestExternalCheb = nearestExternalSolidChebyshev(region, visited, queue, tail,
                    DIAGNOSTIC_GAP_RADIUS);
        }

        return new Component(count, bottom, boundary,
                airBelowFaces, waterBelowFaces, lavaBelowFaces,
                diagonalContacts, nearestExternalCheb,
                minX, maxX, minY, maxY, minZ, maxZ,
                sandBlocks, sandstoneBlocks, stoneBlocks, dirtGrassBlocks, otherSolidBlocks);
    }

    private static int countDiagonalExternalContacts(Region region, boolean[] visited, int[] queue, int tail) {
        int contacts = 0;
        int plane = region.width * H;
        for (int qi = 0; qi < tail; ++qi) {
            int index = queue[qi];
            int x = index / plane;
            int rem = index - x * plane;
            int z = rem / H;
            int y = rem - z * H;
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        if (dx == 0 && dz == 0 && dy == 0) continue;
                        if (Math.abs(dx) + Math.abs(dz) + Math.abs(dy) == 1) continue;
                        int nx = x + dx, nz = z + dz, ny = y + dy;
                        if (nx < 0 || nx >= region.width || nz < 0 || nz >= region.width || ny < 0 || ny >= H) continue;
                        int ni = regionIndex(nx, ny, nz, region.width);
                        if (!visited[ni] && BetaChunk173.isSolid(region.blocks[ni])) ++contacts;
                    }
                }
            }
        }
        return contacts;
    }

    /**
     * Returns nearest Chebyshev distance 1..radius to any solid block outside the
     * component. radius+1 means no external solid was found inside the measured
     * radius. This is a visual-clearance metric only; it never changes hit status.
     */
    private static int nearestExternalSolidChebyshev(Region region, boolean[] visited, int[] queue,
                                                     int tail, int radius) {
        int plane = region.width * H;
        for (int r = 1; r <= radius; ++r) {
            for (int qi = 0; qi < tail; ++qi) {
                int index = queue[qi];
                int x = index / plane;
                int rem = index - x * plane;
                int z = rem / H;
                int y = rem - z * H;
                for (int dx = -r; dx <= r; ++dx) {
                    for (int dz = -r; dz <= r; ++dz) {
                        for (int dy = -r; dy <= r; ++dy) {
                            if (Math.max(Math.max(Math.abs(dx), Math.abs(dz)), Math.abs(dy)) != r) continue;
                            int nx = x + dx, nz = z + dz, ny = y + dy;
                            if (nx < 0 || nx >= region.width || nz < 0 || nz >= region.width || ny < 0 || ny >= H) continue;
                            int ni = regionIndex(nx, ny, nz, region.width);
                            if (!visited[ni] && BetaChunk173.isSolid(region.blocks[ni])) return r;
                        }
                    }
                }
            }
        }
        return radius + 1;
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
        String nearest = result.nearestExternalSolidCheb == 0
                ? "n/a"
                : (result.nearestExternalSolidCheb > DIAGNOSTIC_GAP_RADIUS
                    ? ">=" + result.nearestExternalSolidCheb
                    : Integer.toString(result.nearestExternalSolidCheb));
        int widthX = result.componentBlocks == 0 ? 0 : result.maxX - result.minX + 1;
        int heightY = result.componentBlocks == 0 ? 0 : result.maxY - result.minY + 1;
        int widthZ = result.componentBlocks == 0 ? 0 : result.maxZ - result.minZ + 1;
        System.out.printf(Locale.ROOT,
                "seed=%d status=%s spawnSand=%s supportY=%d componentBlocks=%d radius=%d touchesBottom=%s touchesBoundary=%s "
                        + "airBelowFaces=%d waterBelowFaces=%d lavaBelowFaces=%d diagonalExternalContacts=%d nearestExternalSolidCheb=%s "
                        + "bbox=%dx%dx%d yRange=%d..%d blocks[sand=%d sandstone=%d stone=%d dirtGrass=%d other=%d]%n",
                result.seed, result.status, result.spawnIsSand, result.supportY,
                result.componentBlocks, result.radiusUsed, result.touchesBottom, result.touchesBoundary,
                result.airBelowFaces, result.waterBelowFaces, result.lavaBelowFaces,
                result.diagonalExternalContacts, nearest,
                widthX, heightY, widthZ, result.minY, result.maxY,
                result.sandBlocks, result.sandstoneBlocks, result.stoneBlocks,
                result.dirtGrassBlocks, result.otherSolidBlocks);
    }

    private static void printUsage() {
        System.out.println("Beta 1.7.3 cave-detached spawn island finder");
        System.out.println();
        System.out.println("Search:");
        System.out.println("  java -cp <jar-or-classes> beta173.SpawnIslandFinder173 --start 0 --count 100000000 --threads 16 --max-radius 1");
        System.out.println();
        System.out.println("Verify one seed:");
        System.out.println("  java -cp <jar-or-classes> beta173.SpawnIslandFinder173 --seed 58222042 --max-radius 3");
        System.out.println();
        System.out.println("Hit criteria are unchanged. diagonalExternalContacts and nearestExternalSolidCheb are display/ranking metrics only.");
        System.out.println("Inconclusive seeds are always printed so they can be rerun later.");
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
        final int diagonalExternalContacts;
        final int nearestExternalSolidCheb;
        final int minX, maxX, minY, maxY, minZ, maxZ;
        final int sandBlocks, sandstoneBlocks, stoneBlocks, dirtGrassBlocks, otherSolidBlocks;

        Analysis(long seed, Status status, boolean spawnIsSand, int supportY,
                 int componentBlocks, int radiusUsed, boolean touchesBottom, boolean touchesBoundary,
                 int airBelowFaces, int waterBelowFaces, int lavaBelowFaces,
                 int diagonalExternalContacts, int nearestExternalSolidCheb,
                 int minX, int maxX, int minY, int maxY, int minZ, int maxZ,
                 int sandBlocks, int sandstoneBlocks, int stoneBlocks, int dirtGrassBlocks, int otherSolidBlocks) {
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
            this.diagonalExternalContacts = diagonalExternalContacts;
            this.nearestExternalSolidCheb = nearestExternalSolidCheb;
            this.minX = minX; this.maxX = maxX;
            this.minY = minY; this.maxY = maxY;
            this.minZ = minZ; this.maxZ = maxZ;
            this.sandBlocks = sandBlocks;
            this.sandstoneBlocks = sandstoneBlocks;
            this.stoneBlocks = stoneBlocks;
            this.dirtGrassBlocks = dirtGrassBlocks;
            this.otherSolidBlocks = otherSolidBlocks;
        }

        static Analysis notSand(long seed, int supportY) {
            return new Analysis(seed, Status.NOT_SAND_SPAWN, false, supportY,
                    0, 0, false, false, 0, 0, 0, 0, 0,
                    0, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0);
        }

        static Analysis fastConnected(long seed, int supportY) {
            return new Analysis(seed, Status.CONNECTED_TO_MAIN_TERRAIN, true, supportY,
                    0, 0, true, false, 0, 0, 0, 0, 0,
                    0, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0);
        }
    }

    private static final class Component {
        final int blocks;
        final boolean touchesBottom;
        final boolean touchesBoundary;
        final int airBelowFaces, waterBelowFaces, lavaBelowFaces;
        final int diagonalExternalContacts, nearestExternalSolidCheb;
        final int minX, maxX, minY, maxY, minZ, maxZ;
        final int sandBlocks, sandstoneBlocks, stoneBlocks, dirtGrassBlocks, otherSolidBlocks;

        Component(int blocks, boolean touchesBottom, boolean touchesBoundary,
                  int airBelowFaces, int waterBelowFaces, int lavaBelowFaces,
                  int diagonalExternalContacts, int nearestExternalSolidCheb,
                  int minX, int maxX, int minY, int maxY, int minZ, int maxZ,
                  int sandBlocks, int sandstoneBlocks, int stoneBlocks, int dirtGrassBlocks, int otherSolidBlocks) {
            this.blocks = blocks;
            this.touchesBottom = touchesBottom;
            this.touchesBoundary = touchesBoundary;
            this.airBelowFaces = airBelowFaces;
            this.waterBelowFaces = waterBelowFaces;
            this.lavaBelowFaces = lavaBelowFaces;
            this.diagonalExternalContacts = diagonalExternalContacts;
            this.nearestExternalSolidCheb = nearestExternalSolidCheb;
            this.minX = minX; this.maxX = maxX;
            this.minY = minY; this.maxY = maxY;
            this.minZ = minZ; this.maxZ = maxZ;
            this.sandBlocks = sandBlocks;
            this.sandstoneBlocks = sandstoneBlocks;
            this.stoneBlocks = stoneBlocks;
            this.dirtGrassBlocks = dirtGrassBlocks;
            this.otherSolidBlocks = otherSolidBlocks;
        }

        static Component empty(boolean boundary) {
            return new Component(0, false, boundary, 0, 0, 0, 0, 0,
                    0, -1, 0, -1, 0, -1, 0, 0, 0, 0, 0);
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
