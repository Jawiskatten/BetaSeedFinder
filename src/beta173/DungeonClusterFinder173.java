package beta173;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.PriorityQueue;
import java.util.Random;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Minecraft Beta 1.7.3 same-population-chunk dungeon cluster finder.
 *
 * Goal: find multiple surviving dungeon rooms packed as tightly as possible,
 * using physical room geometry rather than the 16-block spawner activation range.
 *
 * Validation status:
 *   - base terrain + biome surface + caves come from BetaChunk173
 *   - population RNG seeding matches ChunkProviderGenerate
 *   - water/lava lake passes immediately before dungeons are replayed
 *   - all eight dungeon attempts are replayed sequentially, including RNG consumed
 *     by room size, mossy floors, chest placement/loot, and spawner type
 *   - later dungeon attempts may overwrite earlier rooms; final spawner survival is checked
 *
 * IMPORTANT: this class currently starts from an isolated pre-population 32x32 region.
 * That is a useful reference oracle, but it is NOT yet validated as equivalent to a
 * freshly generated vanilla Beta 1.7.3 world, because neighbouring population chunks
 * can mutate shared chunks before this population chunk runs. A different early
 * success/failure also changes the shared Random stream and therefore shifts later
 * dungeon attempt coordinates. Long searches are gated behind
 * --allow-unvalidated-search until parity is established.
 */
public final class DungeonClusterFinder173 {
    private static final int H = BetaChunk173.WORLD_HEIGHT;

    private static final int COBBLESTONE = 4;
    private static final int MOSSY_COBBLESTONE = 48;
    private static final int SPAWNER = 52;
    private static final int CHEST = 54;

    private DungeonClusterFinder173() {}

    public static void main(String[] args) throws Exception {
        Config config;
        try {
            config = Config.parse(args);
        } catch (IllegalArgumentException ex) {
            System.err.println(ex.getMessage());
            printUsage();
            System.exit(2);
            return;
        }

        if (config.help) {
            printUsage();
            return;
        }

        if (config.singleSeed != null) {
            long seed = config.singleSeed.longValue();
            BetaChunk173 generator = new BetaChunk173(seed);
            if (config.scanRadius == 0) {
                Analysis analysis = analyze(
                        seed,
                        config.populationChunkX,
                        config.populationChunkZ,
                        config.minDungeons,
                        generator);
                printAnalysis(analysis);
            } else {
                for (int chunkX = config.populationChunkX - config.scanRadius;
                     chunkX <= config.populationChunkX + config.scanRadius; ++chunkX) {
                    for (int chunkZ = config.populationChunkZ - config.scanRadius;
                         chunkZ <= config.populationChunkZ + config.scanRadius; ++chunkZ) {
                        Analysis analysis = analyze(seed, chunkX, chunkZ, config.minDungeons, generator);
                        printAnalysis(analysis);
                    }
                }
            }
            return;
        }

        if (config.seedFile != null) {
            verifySeedFile(config);
            return;
        }

        if (!config.allowUnvalidatedSearch) {
            System.err.println("Long search is disabled: isolated population parity is not validated against a fresh vanilla Beta 1.7.3 world.");
            System.err.println("Use --seed <seed> for parity tracing, or explicitly add --allow-unvalidated-search for research-only runs.");
            System.exit(2);
            return;
        }

        search(config);
    }

    private static void verifySeedFile(Config config) throws Exception {
        List<Long> seeds = new ArrayList<>();
        for (String raw : Files.readAllLines(config.seedFile, StandardCharsets.UTF_8)) {
            String line = raw.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String first = line.split(",", 2)[0].trim();
            if (first.equalsIgnoreCase("seed")) continue;
            try {
                seeds.add(Long.parseLong(first));
            } catch (NumberFormatException ignored) {
                // Allow scout metadata/header lines without turning verifier input into a fragile format.
            }
        }

        if (seeds.isEmpty()) {
            System.out.println("No candidate seeds in " + config.seedFile.toAbsolutePath());
            return;
        }

        final AtomicLong cursor = new AtomicLong();
        final AtomicLong processed = new AtomicLong();
        final TopBoards boards = new TopBoards(config.minDungeons, 8, config.top);
        final long started = System.nanoTime();

        ExecutorService pool = Executors.newFixedThreadPool(config.threads);
        for (int thread = 0; thread < config.threads; ++thread) {
            pool.submit(() -> {
                BetaChunk173 generator = new BetaChunk173(0L);
                while (true) {
                    long index = cursor.getAndIncrement();
                    if (index >= seeds.size()) return;

                    long seed = seeds.get((int) index);
                    Analysis analysis = analyze(
                            seed,
                            config.populationChunkX,
                            config.populationChunkZ,
                            config.minDungeons,
                            generator);
                    for (Cluster cluster : analysis.bestClusters.values()) {
                        boards.offer(cluster);
                    }

                    long done = processed.incrementAndGet();
                    if ((done & 255L) == 0L || done == seeds.size()) {
                        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
                        double rate = seconds <= 0.0 ? 0.0 : done / seconds;
                        synchronized (System.out) {
                            System.out.printf(Locale.ROOT,
                                    "verify progress=%d/%d rate=%.1f candidates/s best=%s%n",
                                    done, seeds.size(), rate, boards.summary());
                        }
                    }
                }
            });
        }

        pool.shutdown();
        pool.awaitTermination(Long.MAX_VALUE, TimeUnit.DAYS);

        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        System.out.printf(Locale.ROOT,
                "%nVERIFY DONE candidates=%d elapsed=%.3fs rate=%.1f candidates/s populationChunk=(%d,%d)%n",
                processed.get(), seconds,
                seconds <= 0.0 ? 0.0 : processed.get() / seconds,
                config.populationChunkX, config.populationChunkZ);

        List<Cluster> results = boards.allSorted();
        printLeaderboard(results, config);
        if (config.csvPath != null) {
            writeCsv(results, config.csvPath);
            System.out.println("CSV: " + config.csvPath.toAbsolutePath());
        }
    }

    private static void search(Config config) throws Exception {
        final AtomicLong cursor = new AtomicLong();
        final AtomicLong processed = new AtomicLong();
        final AtomicLong nextProgressNanos = new AtomicLong(System.nanoTime() + TimeUnit.SECONDS.toNanos(5));
        final TopBoards boards = new TopBoards(config.minDungeons, 8, config.top);
        final long started = System.nanoTime();

        ExecutorService pool = Executors.newFixedThreadPool(config.threads);
        for (int thread = 0; thread < config.threads; ++thread) {
            pool.submit(() -> {
                BetaChunk173 generator = new BetaChunk173(0L);
                while (true) {
                    long offset = cursor.getAndIncrement();
                    if (offset >= config.count) {
                        return;
                    }

                    long seed = config.startSeed + offset;
                    Analysis analysis = analyze(
                            seed,
                            config.populationChunkX,
                            config.populationChunkZ,
                            config.minDungeons,
                            generator);

                    for (Cluster cluster : analysis.bestClusters.values()) {
                        boards.offer(cluster);
                    }

                    long done = processed.incrementAndGet();
                    long now = System.nanoTime();
                    long due = nextProgressNanos.get();
                    if (now >= due && nextProgressNanos.compareAndSet(due, now + TimeUnit.SECONDS.toNanos(5))) {
                        double seconds = (now - started) / 1_000_000_000.0;
                        double rate = seconds <= 0.0 ? 0.0 : done / seconds;
                        double pct = config.count == 0L ? 100.0 : done * 100.0 / config.count;
                        synchronized (System.out) {
                            System.out.printf(Locale.ROOT,
                                    "progress checked=%d/%d (%.2f%%) rate=%.1f seeds/s best=%s%n",
                                    done, config.count, pct, rate, boards.summary());
                        }
                    }
                }
            });
        }

        pool.shutdown();
        pool.awaitTermination(Long.MAX_VALUE, TimeUnit.DAYS);

        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        double rate = seconds <= 0.0 ? 0.0 : processed.get() / seconds;

        System.out.printf(Locale.ROOT,
                "%nDONE checked=%d elapsed=%.3fs rate=%.1f seeds/s populationChunk=(%d,%d)%n",
                processed.get(), seconds, rate, config.populationChunkX, config.populationChunkZ);

        List<Cluster> results = boards.allSorted();
        printLeaderboard(results, config);
        if (config.csvPath != null) {
            writeCsv(results, config.csvPath);
            System.out.println("CSV: " + config.csvPath.toAbsolutePath());
        }
    }

    static Analysis analyze(long seed, int populationChunkX, int populationChunkZ,
                            int minDungeons, BetaChunk173 generator) {
        generator.reseed(seed);
        Region world = Region.generate(generator, populationChunkX, populationChunkZ);
        TraceRandom random = new TraceRandom(seed);

        long oddX = random.nextLong() / 2L * 2L + 1L;
        long oddZ = random.nextLong() / 2L * 2L + 1L;
        random.setSeed((long) populationChunkX * oddX
                + (long) populationChunkZ * oddZ ^ seed);

        int baseX = populationChunkX * 16;
        int baseZ = populationChunkZ * 16;

        boolean waterLakeAttempted = false;
        boolean waterLakeGenerated = false;
        if (random.nextInt(4) == 0) {
            waterLakeAttempted = true;
            int x = baseX + random.nextInt(16) + 8;
            int y = random.nextInt(H);
            int z = baseZ + random.nextInt(16) + 8;
            waterLakeGenerated = generateLake(world, random, x, y, z, BetaChunk173.WATER_STILL);
        }

        boolean lavaLakeAttempted = false;
        boolean lavaLakeGenerated = false;
        if (random.nextInt(8) == 0) {
            int x = baseX + random.nextInt(16) + 8;
            int y = random.nextInt(random.nextInt(H - 8) + 8);
            int z = baseZ + random.nextInt(16) + 8;
            if (y < BetaChunk173.SEA_LEVEL || random.nextInt(10) == 0) {
                lavaLakeAttempted = true;
                lavaLakeGenerated = generateLake(world, random, x, y, z, BetaChunk173.LAVA_STILL);
            }
        }

        List<DungeonAttempt> attempts = new ArrayList<>();
        List<DungeonRoom> generated = new ArrayList<>();
        for (int attempt = 0; attempt < 8; ++attempt) {
            long stateBeforeCoords = random.state48();
            int x = baseX + random.nextInt(16) + 8;
            int y = random.nextInt(H);
            int z = baseZ + random.nextInt(16) + 8;
            long stateAfterCoords = random.state48();
            DungeonAttempt dungeonAttempt = tryGenerateDungeon(
                    world, random, x, y, z, attempt, stateBeforeCoords, stateAfterCoords);
            attempts.add(dungeonAttempt);
            if (dungeonAttempt.room != null) {
                generated.add(dungeonAttempt.room);
            }
        }

        Map<Long, DungeonRoom> survivingByPos = new LinkedHashMap<>();
        for (DungeonRoom room : generated) {
            if (world.get(room.centerX, room.centerY, room.centerZ) == SPAWNER) {
                survivingByPos.put(pack(room.centerX, room.centerY, room.centerZ), room);
            }
        }
        List<DungeonRoom> surviving = new ArrayList<>(survivingByPos.values());

        Map<Integer, Cluster> bestClusters = new LinkedHashMap<>();
        for (int n = Math.max(2, minDungeons); n <= surviving.size(); ++n) {
            Cluster best = bestClusterOfSize(seed, populationChunkX, populationChunkZ, surviving, n);
            if (best != null) {
                bestClusters.put(n, best);
            }
        }

        return new Analysis(
                seed,
                populationChunkX,
                populationChunkZ,
                waterLakeAttempted,
                waterLakeGenerated,
                lavaLakeAttempted,
                lavaLakeGenerated,
                attempts,
                generated,
                surviving,
                bestClusters);
    }

    private static DungeonAttempt tryGenerateDungeon(
            Region world, TraceRandom random, int centerX, int centerY, int centerZ, int attempt,
            long stateBeforeCoords, long stateAfterCoords) {
        final int roomHeight = 3;
        int halfX = random.nextInt(2) + 2;
        int halfZ = random.nextInt(2) + 2;
        long stateAfterRoomSize = random.state48();
        int openings = 0;

        for (int x = centerX - halfX - 1; x <= centerX + halfX + 1; ++x) {
            for (int y = centerY - 1; y <= centerY + roomHeight + 1; ++y) {
                for (int z = centerZ - halfZ - 1; z <= centerZ + halfZ + 1; ++z) {
                    int block = world.get(x, y, z);
                    if (y == centerY - 1 && !isBuildable(block)) {
                        return DungeonAttempt.failure(
                                attempt, centerX, centerY, centerZ, halfX, halfZ, openings,
                                "FLOOR_NOT_BUILDABLE", stateBeforeCoords, stateAfterCoords,
                                stateAfterRoomSize, random.state48());
                    }
                    if (y == centerY + roomHeight + 1 && !isBuildable(block)) {
                        return DungeonAttempt.failure(
                                attempt, centerX, centerY, centerZ, halfX, halfZ, openings,
                                "CEILING_NOT_BUILDABLE", stateBeforeCoords, stateAfterCoords,
                                stateAfterRoomSize, random.state48());
                    }
                    if ((x == centerX - halfX - 1
                            || x == centerX + halfX + 1
                            || z == centerZ - halfZ - 1
                            || z == centerZ + halfZ + 1)
                            && y == centerY
                            && isAir(world.get(x, y, z))
                            && isAir(world.get(x, y + 1, z))) {
                        ++openings;
                    }
                }
            }
        }

        if (openings < 1 || openings > 5) {
            return DungeonAttempt.failure(
                    attempt, centerX, centerY, centerZ, halfX, halfZ, openings,
                    openings < 1 ? "NO_OPENING" : "TOO_MANY_OPENINGS",
                    stateBeforeCoords, stateAfterCoords, stateAfterRoomSize, random.state48());
        }

        for (int x = centerX - halfX - 1; x <= centerX + halfX + 1; ++x) {
            for (int y = centerY + roomHeight; y >= centerY - 1; --y) {
                for (int z = centerZ - halfZ - 1; z <= centerZ + halfZ + 1; ++z) {
                    boolean interior = x != centerX - halfX - 1
                            && y != centerY - 1
                            && z != centerZ - halfZ - 1
                            && x != centerX + halfX + 1
                            && y != centerY + roomHeight + 1
                            && z != centerZ + halfZ + 1;

                    if (interior) {
                        world.set(x, y, z, BetaChunk173.AIR);
                    } else if (y >= 0 && !isBuildable(world.get(x, y - 1, z))) {
                        world.set(x, y, z, BetaChunk173.AIR);
                    } else if (isBuildable(world.get(x, y, z))) {
                        if (y == centerY - 1 && random.nextInt(4) != 0) {
                            world.set(x, y, z, MOSSY_COBBLESTONE);
                        } else {
                            world.set(x, y, z, COBBLESTONE);
                        }
                    }
                }
            }
        }

        for (int chest = 0; chest < 2; ++chest) {
            boolean placed = false;
            for (int tries = 0; tries < 3 && !placed; ++tries) {
                int x = centerX + random.nextInt(halfX * 2 + 1) - halfX;
                int z = centerZ + random.nextInt(halfZ * 2 + 1) - halfZ;
                if (isAir(world.get(x, centerY, z))) {
                    int solidSides = 0;
                    if (isBuildable(world.get(x - 1, centerY, z))) ++solidSides;
                    if (isBuildable(world.get(x + 1, centerY, z))) ++solidSides;
                    if (isBuildable(world.get(x, centerY, z - 1))) ++solidSides;
                    if (isBuildable(world.get(x, centerY, z + 1))) ++solidSides;

                    if (solidSides == 1) {
                        world.set(x, centerY, z, CHEST);
                        for (int loot = 0; loot < 8; ++loot) {
                            if (consumeRandomDungeonItem(random)) {
                                random.nextInt(27);
                            }
                        }
                        placed = true;
                    }
                }
            }
        }

        world.set(centerX, centerY, centerZ, SPAWNER);
        random.nextInt(4);

        DungeonRoom room = new DungeonRoom(
                attempt,
                centerX, centerY, centerZ,
                halfX, halfZ, openings,
                centerX - halfX - 1,
                centerX + halfX + 1,
                centerY - 1,
                centerY + roomHeight,
                centerZ - halfZ - 1,
                centerZ + halfZ + 1);
        return DungeonAttempt.success(
                room, stateBeforeCoords, stateAfterCoords, stateAfterRoomSize, random.state48());
    }

    private static boolean consumeRandomDungeonItem(Random random) {
        int item = random.nextInt(11);
        switch (item) {
            case 0:
            case 2:
            case 6:
            case 10:
                return true;
            case 1:
            case 3:
            case 4:
            case 5:
                random.nextInt(4);
                return true;
            case 7:
                return random.nextInt(100) == 0;
            case 8:
                if (random.nextInt(2) == 0) {
                    random.nextInt(4);
                    return true;
                }
                return false;
            case 9:
                if (random.nextInt(10) == 0) {
                    random.nextInt(2);
                    return true;
                }
                return false;
            default:
                throw new AssertionError(item);
        }
    }

    private static boolean generateLake(
            Region world, Random random, int centerX, int centerY, int centerZ, int lakeBlock) {
        centerX -= 8;
        centerZ -= 8;

        while (centerY > 0 && isAir(world.get(centerX, centerY, centerZ))) {
            --centerY;
        }

        centerY -= 4;
        boolean[] mask = new boolean[2048];
        int blobs = random.nextInt(4) + 4;

        for (int blob = 0; blob < blobs; ++blob) {
            double sizeX = random.nextDouble() * 6.0D + 3.0D;
            double sizeY = random.nextDouble() * 4.0D + 2.0D;
            double sizeZ = random.nextDouble() * 6.0D + 3.0D;
            double cx = random.nextDouble() * (16.0D - sizeX - 2.0D) + 1.0D + sizeX / 2.0D;
            double cy = random.nextDouble() * (8.0D - sizeY - 4.0D) + 2.0D + sizeY / 2.0D;
            double cz = random.nextDouble() * (16.0D - sizeZ - 2.0D) + 1.0D + sizeZ / 2.0D;

            for (int x = 1; x < 15; ++x) {
                for (int z = 1; z < 15; ++z) {
                    for (int y = 1; y < 7; ++y) {
                        double dx = ((double) x - cx) / (sizeX / 2.0D);
                        double dy = ((double) y - cy) / (sizeY / 2.0D);
                        double dz = ((double) z - cz) / (sizeZ / 2.0D);
                        if (dx * dx + dy * dy + dz * dz < 1.0D) {
                            mask[(x * 16 + z) * 8 + y] = true;
                        }
                    }
                }
            }
        }

        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (int y = 0; y < 8; ++y) {
                    boolean boundary = !mask[(x * 16 + z) * 8 + y]
                            && (x < 15 && mask[((x + 1) * 16 + z) * 8 + y]
                            || x > 0 && mask[((x - 1) * 16 + z) * 8 + y]
                            || z < 15 && mask[(x * 16 + z + 1) * 8 + y]
                            || z > 0 && mask[(x * 16 + z - 1) * 8 + y]
                            || y < 7 && mask[(x * 16 + z) * 8 + y + 1]
                            || y > 0 && mask[(x * 16 + z) * 8 + y - 1]);

                    if (boundary) {
                        int block = world.get(centerX + x, centerY + y, centerZ + z);
                        if (y >= 4 && isLiquid(block)) {
                            return false;
                        }
                        if (y < 4 && !isBuildable(block) && block != lakeBlock) {
                            return false;
                        }
                    }
                }
            }
        }

        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (int y = 0; y < 8; ++y) {
                    if (mask[(x * 16 + z) * 8 + y]) {
                        world.set(centerX + x, centerY + y, centerZ + z,
                                y >= 4 ? BetaChunk173.AIR : lakeBlock);
                    }
                }
            }
        }

        if (lakeBlock == BetaChunk173.LAVA_STILL || lakeBlock == BetaChunk173.LAVA_MOVING) {
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < 8; ++y) {
                        boolean boundary = !mask[(x * 16 + z) * 8 + y]
                                && (x < 15 && mask[((x + 1) * 16 + z) * 8 + y]
                                || x > 0 && mask[((x - 1) * 16 + z) * 8 + y]
                                || z < 15 && mask[(x * 16 + z + 1) * 8 + y]
                                || z > 0 && mask[(x * 16 + z - 1) * 8 + y]
                                || y < 7 && mask[(x * 16 + z) * 8 + y + 1]
                                || y > 0 && mask[(x * 16 + z) * 8 + y - 1]);

                        if (boundary
                                && (y < 4 || random.nextInt(2) != 0)
                                && isBuildable(world.get(centerX + x, centerY + y, centerZ + z))) {
                            world.set(centerX + x, centerY + y, centerZ + z, BetaChunk173.STONE);
                        }
                    }
                }
            }
        }

        return true;
    }

    private static Cluster bestClusterOfSize(
            long seed, int populationChunkX, int populationChunkZ,
            List<DungeonRoom> rooms, int wanted) {
        if (wanted < 2 || wanted > rooms.size()) {
            return null;
        }

        Cluster[] best = new Cluster[1];
        DungeonRoom[] selection = new DungeonRoom[wanted];
        chooseCluster(seed, populationChunkX, populationChunkZ, rooms, wanted, 0, 0, selection, best);
        return best[0];
    }

    private static void chooseCluster(
            long seed, int populationChunkX, int populationChunkZ,
            List<DungeonRoom> rooms, int wanted,
            int sourceIndex, int selected,
            DungeonRoom[] selection, Cluster[] best) {
        if (selected == wanted) {
            Cluster cluster = Cluster.from(seed, populationChunkX, populationChunkZ, selection);
            // User-facing target is a physical dungeon complex, not merely N dungeons
            // that happen to exist in the same population chunk. Require the room
            // boxes to form one graph through overlap or direct face/edge adjacency.
            if (!cluster.roomGraphConnected) {
                return;
            }
            if (best[0] == null || Cluster.BEST_FIRST.compare(cluster, best[0]) < 0) {
                best[0] = cluster;
            }
            return;
        }

        int remainingNeeded = wanted - selected;
        for (int i = sourceIndex; i <= rooms.size() - remainingNeeded; ++i) {
            selection[selected] = rooms.get(i);
            chooseCluster(seed, populationChunkX, populationChunkZ, rooms, wanted,
                    i + 1, selected + 1, selection, best);
        }
    }

    private static int intervalGap(int aMin, int aMax, int bMin, int bMax) {
        if (aMax < bMin) return bMin - aMax - 1;
        if (bMax < aMin) return aMin - bMax - 1;
        return 0;
    }

    private static boolean intervalsOverlap(int aMin, int aMax, int bMin, int bMax) {
        return aMin <= bMax && bMin <= aMax;
    }

    private static boolean isAir(int block) {
        return block == BetaChunk173.AIR;
    }

    private static boolean isLiquid(int block) {
        return block == BetaChunk173.WATER_MOVING
                || block == BetaChunk173.WATER_STILL
                || block == BetaChunk173.LAVA_MOVING
                || block == BetaChunk173.LAVA_STILL;
    }

    private static boolean isBuildable(int block) {
        return !isAir(block) && !isLiquid(block);
    }

    private static long pack(int x, int y, int z) {
        long a = ((long) x & 0x3FFFFFFL) << 38;
        long b = ((long) z & 0x3FFFFFFL) << 12;
        long c = (long) y & 0xFFFL;
        return a ^ b ^ c;
    }

    private static void printAnalysis(Analysis a) {
        System.out.printf(Locale.ROOT,
                "seed=%d populationChunk=(%d,%d) generated=%d surviving=%d "
                        + "waterLake[attempt=%s generated=%s] lavaLake[attempt=%s generated=%s]%n",
                a.seed, a.populationChunkX, a.populationChunkZ,
                a.generated.size(), a.surviving.size(),
                a.waterLakeAttempted, a.waterLakeGenerated,
                a.lavaLakeAttempted, a.lavaLakeGenerated);

        System.out.println("  dungeon attempt trace (shared population RNG; one divergence shifts later coordinates):");
        for (DungeonAttempt attempt : a.attempts) {
            boolean survives = attempt.room != null && a.surviving.stream().anyMatch(r ->
                    r.centerX == attempt.centerX && r.centerY == attempt.centerY && r.centerZ == attempt.centerZ);
            if (attempt.room == null) {
                System.out.printf(Locale.ROOT,
                        "  attempt=%d candidate=(%d,%d,%d) half=(%d,%d) result=FAIL[%s] openings=%d "
                                + "rng48[before=%012x coords=%012x size=%012x after=%012x]%n",
                        attempt.attempt, attempt.centerX, attempt.centerY, attempt.centerZ,
                        attempt.halfX, attempt.halfZ, attempt.failureReason, attempt.openings,
                        attempt.stateBeforeCoords, attempt.stateAfterCoords,
                        attempt.stateAfterRoomSize, attempt.stateAfterDungeon);
            } else {
                DungeonRoom room = attempt.room;
                System.out.printf(Locale.ROOT,
                        "  attempt=%d candidate=(%d,%d,%d) half=(%d,%d) result=SUCCESS "
                                + "room=%dx5x%d bbox=[%d..%d,%d..%d,%d..%d] openings=%d survives=%s "
                                + "rng48[before=%012x coords=%012x size=%012x after=%012x]%n",
                        attempt.attempt, attempt.centerX, attempt.centerY, attempt.centerZ,
                        attempt.halfX, attempt.halfZ,
                        room.maxX - room.minX + 1,
                        room.maxZ - room.minZ + 1,
                        room.minX, room.maxX,
                        room.minY, room.maxY,
                        room.minZ, room.maxZ,
                        room.openings, survives,
                        attempt.stateBeforeCoords, attempt.stateAfterCoords,
                        attempt.stateAfterRoomSize, attempt.stateAfterDungeon);
            }
        }

        if (a.bestClusters.isEmpty()) {
            System.out.println("  no qualifying multi-dungeon cluster");
        } else {
            for (Cluster cluster : a.bestClusters.values()) {
                System.out.println("  " + cluster.describe());
            }
        }
    }

    private static void printLeaderboard(List<Cluster> results, Config config) {
        if (results.isEmpty()) {
            System.out.println("No qualifying clusters found.");
            return;
        }

        int current = -1;
        int rank = 0;
        for (Cluster c : results) {
            if (c.count != current) {
                current = c.count;
                rank = 0;
                System.out.printf(Locale.ROOT, "%n=== BEST %d-DUNGEON CLUSTERS ===%n", current);
            }
            ++rank;
            System.out.printf(Locale.ROOT, "#%d %s%n", rank, c.describe());
        }
    }

    private static void writeCsv(List<Cluster> results, Path path) throws IOException {
        Path parent = path.toAbsolutePath().getParent();
        if (parent != null) Files.createDirectories(parent);

        StringBuilder out = new StringBuilder();
        out.append("seed,pop_chunk_x,pop_chunk_z,count,room_graph_connected,spawner_y_span,max_room_gap_sq,max_room_gap,")
                .append("sum_room_gap_sq,overlap_pairs,touch_pairs,cluster_x,cluster_y,cluster_z,")
                .append("cluster_volume,max_spawner_dist_sq,max_spawner_dist,spawners\n");

        for (Cluster c : results) {
            out.append(c.seed).append(',')
                    .append(c.populationChunkX).append(',')
                    .append(c.populationChunkZ).append(',')
                    .append(c.count).append(',')
                    .append(c.roomGraphConnected).append(',')
                    .append(c.spawnerYSpan).append(',')
                    .append(c.maxRoomGapSq).append(',')
                    .append(String.format(Locale.ROOT, "%.4f", Math.sqrt(c.maxRoomGapSq))).append(',')
                    .append(c.sumRoomGapSq).append(',')
                    .append(c.overlapPairs).append(',')
                    .append(c.touchPairs).append(',')
                    .append(c.clusterX).append(',')
                    .append(c.clusterY).append(',')
                    .append(c.clusterZ).append(',')
                    .append(c.clusterVolume).append(',')
                    .append(c.maxSpawnerDistSq).append(',')
                    .append(String.format(Locale.ROOT, "%.4f", Math.sqrt(c.maxSpawnerDistSq))).append(',')
                    .append('"').append(c.spawnerList()).append('"')
                    .append('\n');
        }

        Files.writeString(path, out.toString(), StandardCharsets.UTF_8);
    }

    private static void printUsage() {
        System.out.println("Beta 1.7.3 dungeon cluster finder");
        System.out.println();
        System.out.println("Research-only isolated search (explicit opt-in required):");
        System.out.println("  java -cp build/java/classes beta173.DungeonClusterFinder173 --start 0 --count 1000000 --threads 16 --allow-unvalidated-search");
        System.out.println();
        System.out.println("Exact-verify a GPU scout candidate CSV:");
        System.out.println("  java -cp build/java/classes beta173.DungeonClusterFinder173 --seed-file out\\dungeon_gpu_candidates.csv --threads 8");
        System.out.println();
        System.out.println("Parity-trace one seed:");
        System.out.println("  java -cp build/java/classes beta173.DungeonClusterFinder173 --seed 501789");
        System.out.println("Scan nearby population chunks for the same seed:");
        System.out.println("  java -cp build/java/classes beta173.DungeonClusterFinder173 --seed 501789 --scan-chunks 1");
        System.out.println();
        System.out.println("Ranking is physical dungeon packing, not spawner activation range.");
        System.out.println("Only room-graph CONNECTED clusters are retained: every room must join the same");
        System.out.println("component through overlapping or directly adjacent dungeon bounding volumes.");
        System.out.println("For each N independently: minimize worst room gap, then Y span, maximize overlaps,");
        System.out.println("then minimize cluster bounding volume and worst spawner distance.");
        System.out.println();
        System.out.println("Options:");
        System.out.println("  --start <long>          first world seed (default 0)");
        System.out.println("  --count <long>          sequential seeds to check (default 100000)");
        System.out.println("  --threads <int>         worker threads (default available processors)");
        System.out.println("  --chunk-x <int>         population chunk X (default 0)");
        System.out.println("  --chunk-z <int>         population chunk Z (default 0)");
        System.out.println("  --min-dungeons <int>    minimum cluster size to retain (default 2; 2..8)");
        System.out.println("  --top <int>             leaderboard entries per cluster size (default 20)");
        System.out.println("  --csv <path|off>        output CSV (default out/dungeon_cluster_results.csv)");
        System.out.println("  --seed <long>           parity-trace one seed only");
        System.out.println("  --seed-file <path>      exact-verify first CSV column as candidate seeds");
        System.out.println("  --scan-chunks <int>     with --seed, trace +/- radius population chunks (default 0; max 4)");
        System.out.println("  --allow-unvalidated-search  explicitly enable isolated long searches");
        System.out.println("  --help                  show this text");
    }

    static final class Analysis {
        final long seed;
        final int populationChunkX;
        final int populationChunkZ;
        final boolean waterLakeAttempted;
        final boolean waterLakeGenerated;
        final boolean lavaLakeAttempted;
        final boolean lavaLakeGenerated;
        final List<DungeonAttempt> attempts;
        final List<DungeonRoom> generated;
        final List<DungeonRoom> surviving;
        final Map<Integer, Cluster> bestClusters;

        Analysis(long seed, int populationChunkX, int populationChunkZ,
                 boolean waterLakeAttempted, boolean waterLakeGenerated,
                 boolean lavaLakeAttempted, boolean lavaLakeGenerated,
                 List<DungeonAttempt> attempts,
                 List<DungeonRoom> generated, List<DungeonRoom> surviving,
                 Map<Integer, Cluster> bestClusters) {
            this.seed = seed;
            this.populationChunkX = populationChunkX;
            this.populationChunkZ = populationChunkZ;
            this.waterLakeAttempted = waterLakeAttempted;
            this.waterLakeGenerated = waterLakeGenerated;
            this.lavaLakeAttempted = lavaLakeAttempted;
            this.lavaLakeGenerated = lavaLakeGenerated;
            this.attempts = attempts;
            this.generated = generated;
            this.surviving = surviving;
            this.bestClusters = bestClusters;
        }
    }

    static final class DungeonAttempt {
        final int attempt;
        final int centerX, centerY, centerZ;
        final int halfX, halfZ;
        final int openings;
        final String failureReason;
        final long stateBeforeCoords;
        final long stateAfterCoords;
        final long stateAfterRoomSize;
        final long stateAfterDungeon;
        final DungeonRoom room;

        private DungeonAttempt(int attempt,
                               int centerX, int centerY, int centerZ,
                               int halfX, int halfZ, int openings,
                               String failureReason,
                               long stateBeforeCoords, long stateAfterCoords,
                               long stateAfterRoomSize, long stateAfterDungeon,
                               DungeonRoom room) {
            this.attempt = attempt;
            this.centerX = centerX;
            this.centerY = centerY;
            this.centerZ = centerZ;
            this.halfX = halfX;
            this.halfZ = halfZ;
            this.openings = openings;
            this.failureReason = failureReason;
            this.stateBeforeCoords = stateBeforeCoords;
            this.stateAfterCoords = stateAfterCoords;
            this.stateAfterRoomSize = stateAfterRoomSize;
            this.stateAfterDungeon = stateAfterDungeon;
            this.room = room;
        }

        static DungeonAttempt failure(int attempt,
                                      int centerX, int centerY, int centerZ,
                                      int halfX, int halfZ, int openings,
                                      String failureReason,
                                      long stateBeforeCoords, long stateAfterCoords,
                                      long stateAfterRoomSize, long stateAfterDungeon) {
            return new DungeonAttempt(
                    attempt, centerX, centerY, centerZ, halfX, halfZ, openings,
                    failureReason, stateBeforeCoords, stateAfterCoords,
                    stateAfterRoomSize, stateAfterDungeon, null);
        }

        static DungeonAttempt success(DungeonRoom room,
                                      long stateBeforeCoords, long stateAfterCoords,
                                      long stateAfterRoomSize, long stateAfterDungeon) {
            return new DungeonAttempt(
                    room.attempt, room.centerX, room.centerY, room.centerZ,
                    room.halfX, room.halfZ, room.openings,
                    null, stateBeforeCoords, stateAfterCoords,
                    stateAfterRoomSize, stateAfterDungeon, room);
        }
    }

    static final class DungeonRoom {
        final int attempt;
        final int centerX, centerY, centerZ;
        final int halfX, halfZ;
        final int openings;
        final int minX, maxX, minY, maxY, minZ, maxZ;

        DungeonRoom(int attempt,
                    int centerX, int centerY, int centerZ,
                    int halfX, int halfZ, int openings,
                    int minX, int maxX, int minY, int maxY, int minZ, int maxZ) {
            this.attempt = attempt;
            this.centerX = centerX;
            this.centerY = centerY;
            this.centerZ = centerZ;
            this.halfX = halfX;
            this.halfZ = halfZ;
            this.openings = openings;
            this.minX = minX;
            this.maxX = maxX;
            this.minY = minY;
            this.maxY = maxY;
            this.minZ = minZ;
            this.maxZ = maxZ;
        }
    }

    static final class Cluster {
        static final Comparator<Cluster> BEST_FIRST =
                Comparator.comparingInt((Cluster c) -> c.maxRoomGapSq)
                        .thenComparingInt(c -> c.spawnerYSpan)
                        .thenComparing(Comparator.comparingInt((Cluster c) -> c.overlapPairs).reversed())
                        .thenComparingLong(c -> c.clusterVolume)
                        .thenComparingInt(c -> c.maxSpawnerDistSq)
                        .thenComparingInt(c -> c.sumRoomGapSq)
                        .thenComparingLong(c -> c.seed);

        final long seed;
        final int populationChunkX;
        final int populationChunkZ;
        final List<DungeonRoom> rooms;
        final int count;
        final int maxRoomGapSq;
        final int sumRoomGapSq;
        final int overlapPairs;
        final int touchPairs;
        final boolean roomGraphConnected;
        final int spawnerYSpan;
        final int clusterX, clusterY, clusterZ;
        final long clusterVolume;
        final int maxSpawnerDistSq;

        Cluster(long seed, int populationChunkX, int populationChunkZ,
                List<DungeonRoom> rooms,
                int maxRoomGapSq, int sumRoomGapSq,
                int overlapPairs, int touchPairs,
                boolean roomGraphConnected, int spawnerYSpan,
                int clusterX, int clusterY, int clusterZ,
                long clusterVolume, int maxSpawnerDistSq) {
            this.seed = seed;
            this.populationChunkX = populationChunkX;
            this.populationChunkZ = populationChunkZ;
            this.rooms = rooms;
            this.count = rooms.size();
            this.maxRoomGapSq = maxRoomGapSq;
            this.sumRoomGapSq = sumRoomGapSq;
            this.overlapPairs = overlapPairs;
            this.touchPairs = touchPairs;
            this.roomGraphConnected = roomGraphConnected;
            this.spawnerYSpan = spawnerYSpan;
            this.clusterX = clusterX;
            this.clusterY = clusterY;
            this.clusterZ = clusterZ;
            this.clusterVolume = clusterVolume;
            this.maxSpawnerDistSq = maxSpawnerDistSq;
        }

        static Cluster from(long seed, int populationChunkX, int populationChunkZ,
                            DungeonRoom[] selected) {
            List<DungeonRoom> rooms = List.of(selected.clone());

            int minX = Integer.MAX_VALUE, minY = Integer.MAX_VALUE, minZ = Integer.MAX_VALUE;
            int maxX = Integer.MIN_VALUE, maxY = Integer.MIN_VALUE, maxZ = Integer.MIN_VALUE;
            int maxGapSq = 0;
            int sumGapSq = 0;
            int overlaps = 0;
            int touches = 0;
            int maxSpawnerSq = 0;
            int minSpawnerY = Integer.MAX_VALUE;
            int maxSpawnerY = Integer.MIN_VALUE;
            boolean[][] linked = new boolean[rooms.size()][rooms.size()];

            for (DungeonRoom r : rooms) {
                minX = Math.min(minX, r.minX);
                minY = Math.min(minY, r.minY);
                minZ = Math.min(minZ, r.minZ);
                maxX = Math.max(maxX, r.maxX);
                maxY = Math.max(maxY, r.maxY);
                maxZ = Math.max(maxZ, r.maxZ);
                minSpawnerY = Math.min(minSpawnerY, r.centerY);
                maxSpawnerY = Math.max(maxSpawnerY, r.centerY);
            }

            for (int i = 0; i < rooms.size(); ++i) {
                DungeonRoom a = rooms.get(i);
                for (int j = i + 1; j < rooms.size(); ++j) {
                    DungeonRoom b = rooms.get(j);

                    int gx = intervalGap(a.minX, a.maxX, b.minX, b.maxX);
                    int gy = intervalGap(a.minY, a.maxY, b.minY, b.maxY);
                    int gz = intervalGap(a.minZ, a.maxZ, b.minZ, b.maxZ);
                    int gapSq = gx * gx + gy * gy + gz * gz;
                    maxGapSq = Math.max(maxGapSq, gapSq);
                    sumGapSq += gapSq;
                    if (gapSq == 0) {
                        ++touches;
                        linked[i][j] = true;
                        linked[j][i] = true;
                    }

                    if (intervalsOverlap(a.minX, a.maxX, b.minX, b.maxX)
                            && intervalsOverlap(a.minY, a.maxY, b.minY, b.maxY)
                            && intervalsOverlap(a.minZ, a.maxZ, b.minZ, b.maxZ)) {
                        ++overlaps;
                    }

                    int dx = a.centerX - b.centerX;
                    int dy = a.centerY - b.centerY;
                    int dz = a.centerZ - b.centerZ;
                    int spawnerSq = dx * dx + dy * dy + dz * dz;
                    maxSpawnerSq = Math.max(maxSpawnerSq, spawnerSq);
                }
            }

            boolean roomGraphConnected = isConnectedGraph(linked);
            int spawnerYSpan = maxSpawnerY - minSpawnerY;

            int sizeX = maxX - minX + 1;
            int sizeY = maxY - minY + 1;
            int sizeZ = maxZ - minZ + 1;
            long volume = (long) sizeX * sizeY * sizeZ;

            return new Cluster(
                    seed, populationChunkX, populationChunkZ, rooms,
                    maxGapSq, sumGapSq, overlaps, touches,
                    roomGraphConnected, spawnerYSpan,
                    sizeX, sizeY, sizeZ, volume, maxSpawnerSq);
        }

        private static boolean isConnectedGraph(boolean[][] linked) {
            if (linked.length <= 1) return true;
            boolean[] seen = new boolean[linked.length];
            int[] queue = new int[linked.length];
            int head = 0;
            int tail = 0;
            queue[tail++] = 0;
            seen[0] = true;
            int count = 1;

            while (head < tail) {
                int at = queue[head++];
                for (int next = 0; next < linked.length; ++next) {
                    if (linked[at][next] && !seen[next]) {
                        seen[next] = true;
                        queue[tail++] = next;
                        ++count;
                    }
                }
            }
            return count == linked.length;
        }

        String spawnerList() {
            StringBuilder s = new StringBuilder();
            for (int i = 0; i < rooms.size(); ++i) {
                if (i != 0) s.append(';');
                DungeonRoom r = rooms.get(i);
                s.append(r.centerX).append('/').append(r.centerY).append('/').append(r.centerZ);
            }
            return s.toString();
        }

        String describe() {
            int pairs = count * (count - 1) / 2;
            return String.format(Locale.ROOT,
                    "seed=%d N=%d roomGraph=%s ySpan=%d roomGapMax=%.3f blocks overlapPairs=%d/%d touchPairs=%d/%d "
                            + "cluster=%dx%dx%d volume=%d spawnerMax=%.3f spawners=%s",
                    seed, count, roomGraphConnected ? "CONNECTED" : "DISCONNECTED",
                    spawnerYSpan, Math.sqrt(maxRoomGapSq),
                    overlapPairs, pairs, touchPairs, pairs,
                    clusterX, clusterY, clusterZ, clusterVolume,
                    Math.sqrt(maxSpawnerDistSq), spawnerList());
        }
    }

    private static final class TopBoards {
        private final int min;
        private final int max;
        private final int top;
        private final Map<Integer, PriorityQueue<Cluster>> queues = new HashMap<>();

        TopBoards(int min, int max, int top) {
            this.min = min;
            this.max = max;
            this.top = top;
            Comparator<Cluster> worstFirst = Cluster.BEST_FIRST.reversed();
            for (int n = min; n <= max; ++n) {
                queues.put(n, new PriorityQueue<>(worstFirst));
            }
        }

        synchronized void offer(Cluster cluster) {
            PriorityQueue<Cluster> q = queues.get(cluster.count);
            if (q == null) return;

            if (q.size() < top) {
                q.add(cluster);
            } else if (Cluster.BEST_FIRST.compare(cluster, q.peek()) < 0) {
                q.poll();
                q.add(cluster);
            }
        }

        synchronized String summary() {
            StringBuilder s = new StringBuilder();
            for (int n = max; n >= min; --n) {
                PriorityQueue<Cluster> q = queues.get(n);
                if (q != null && !q.isEmpty()) {
                    Cluster best = q.stream().min(Cluster.BEST_FIRST).orElseThrow();
                    if (s.length() != 0) s.append(" | ");
                    s.append(n).append("x gap=")
                            .append(String.format(Locale.ROOT, "%.2f", Math.sqrt(best.maxRoomGapSq)));
                }
            }
            return s.length() == 0 ? "none" : s.toString();
        }

        synchronized List<Cluster> allSorted() {
            List<Cluster> out = new ArrayList<>();
            for (int n = max; n >= min; --n) {
                PriorityQueue<Cluster> q = queues.get(n);
                if (q == null) continue;
                List<Cluster> one = new ArrayList<>(q);
                one.sort(Cluster.BEST_FIRST);
                out.addAll(one);
            }
            return out;
        }
    }

    private static final class Region {
        final int minX;
        final int minZ;
        final int width;
        final int[] blocks;

        Region(int minX, int minZ, int width, int[] blocks) {
            this.minX = minX;
            this.minZ = minZ;
            this.width = width;
            this.blocks = blocks;
        }

        static Region generate(BetaChunk173 generator, int populationChunkX, int populationChunkZ) {
            int minX = populationChunkX * 16;
            int minZ = populationChunkZ * 16;
            int width = 32;
            int[] blocks = new int[width * width * H];

            for (int cx = populationChunkX; cx <= populationChunkX + 1; ++cx) {
                for (int cz = populationChunkZ; cz <= populationChunkZ + 1; ++cz) {
                    int[] chunk = generator.generateChunk(cx, cz);
                    int offsetX = (cx - populationChunkX) * 16;
                    int offsetZ = (cz - populationChunkZ) * 16;

                    for (int x = 0; x < 16; ++x) {
                        for (int z = 0; z < 16; ++z) {
                            int source = BetaChunk173.index(x, 0, z);
                            int target = indexLocal(offsetX + x, 0, offsetZ + z, width);
                            System.arraycopy(chunk, source, blocks, target, H);
                        }
                    }
                }
            }

            return new Region(minX, minZ, width, blocks);
        }

        int get(int worldX, int y, int worldZ) {
            if (y < 0 || y >= H) return BetaChunk173.AIR;
            int x = worldX - minX;
            int z = worldZ - minZ;
            if (x < 0 || x >= width || z < 0 || z >= width) {
                return BetaChunk173.AIR;
            }
            return blocks[indexLocal(x, y, z, width)];
        }

        void set(int worldX, int y, int worldZ, int block) {
            if (y < 0 || y >= H) return;
            int x = worldX - minX;
            int z = worldZ - minZ;
            if (x < 0 || x >= width || z < 0 || z >= width) {
                throw new IllegalStateException(String.format(Locale.ROOT,
                        "feature write escaped 32x32 population region: (%d,%d,%d) min=(%d,%d)",
                        worldX, y, worldZ, minX, minZ));
            }
            blocks[indexLocal(x, y, z, width)] = block;
        }

        private static int indexLocal(int x, int y, int z, int width) {
            return (x * width + z) * H + y;
        }
    }

    /**
     * java.util.Random-compatible 48-bit LCG with exposed internal state for parity traces.
     * Only methods inherited from Random are used by the generator; overriding next(int)
     * preserves Java Random semantics while making cascade points observable.
     */
    private static final class TraceRandom extends Random {
        private static final long serialVersionUID = 1L;
        private static final long MULTIPLIER = 0x5DEECE66DL;
        private static final long ADDEND = 0xBL;
        private static final long MASK = (1L << 48) - 1L;
        private long state;

        TraceRandom(long seed) {
            super(0L);
            setSeed(seed);
        }

        @Override
        public synchronized void setSeed(long seed) {
            state = (seed ^ MULTIPLIER) & MASK;
        }

        @Override
        protected synchronized int next(int bits) {
            state = (state * MULTIPLIER + ADDEND) & MASK;
            return (int) (state >>> (48 - bits));
        }

        synchronized long state48() {
            return state;
        }
    }

    private static final class Config {
        long startSeed = 0L;
        long count = 100_000L;
        int threads = Math.max(1, Runtime.getRuntime().availableProcessors());
        int populationChunkX = 0;
        int populationChunkZ = 0;
        int minDungeons = 2;
        int top = 20;
        int scanRadius = 0;
        Path csvPath = Paths.get("out", "dungeon_cluster_results.csv");
        Path seedFile;
        Long singleSeed;
        boolean allowUnvalidatedSearch;
        boolean help;

        static Config parse(String[] args) {
            Config c = new Config();
            for (int i = 0; i < args.length; ++i) {
                String arg = args[i];
                switch (arg) {
                    case "--start":
                        c.startSeed = Long.parseLong(requireValue(args, ++i, arg));
                        break;
                    case "--count":
                        c.count = Long.parseLong(requireValue(args, ++i, arg));
                        break;
                    case "--threads":
                        c.threads = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--chunk-x":
                        c.populationChunkX = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--chunk-z":
                        c.populationChunkZ = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--min-dungeons":
                        c.minDungeons = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--top":
                        c.top = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--csv": {
                        String value = requireValue(args, ++i, arg);
                        c.csvPath = value.equalsIgnoreCase("off") ? null : Paths.get(value);
                        break;
                    }
                    case "--seed":
                        c.singleSeed = Long.parseLong(requireValue(args, ++i, arg));
                        break;
                    case "--seed-file":
                        c.seedFile = Paths.get(requireValue(args, ++i, arg));
                        break;
                    case "--scan-chunks":
                        c.scanRadius = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--allow-unvalidated-search":
                        c.allowUnvalidatedSearch = true;
                        break;
                    case "--help":
                    case "-h":
                        c.help = true;
                        break;
                    default:
                        throw new IllegalArgumentException("Unknown option: " + arg);
                }
            }

            if (c.count < 0L) throw new IllegalArgumentException("--count must be >= 0");
            if (c.threads < 1) throw new IllegalArgumentException("--threads must be >= 1");
            if (c.minDungeons < 2 || c.minDungeons > 8) {
                throw new IllegalArgumentException("--min-dungeons must be 2..8");
            }
            if (c.top < 1 || c.top > 10_000) {
                throw new IllegalArgumentException("--top must be 1..10000");
            }
            if (c.scanRadius < 0 || c.scanRadius > 4) {
                throw new IllegalArgumentException("--scan-chunks must be 0..4");
            }
            if (c.scanRadius != 0 && c.singleSeed == null) {
                throw new IllegalArgumentException("--scan-chunks requires --seed");
            }
            if (c.singleSeed != null && c.seedFile != null) {
                throw new IllegalArgumentException("--seed and --seed-file are mutually exclusive");
            }
            if (c.seedFile != null && !Files.isRegularFile(c.seedFile)) {
                throw new IllegalArgumentException("--seed-file not found: " + c.seedFile);
            }
            return c;
        }

        private static String requireValue(String[] args, int index, String option) {
            if (index >= args.length) {
                throw new IllegalArgumentException("Missing value for " + option);
            }
            return args[index];
        }
    }
}
