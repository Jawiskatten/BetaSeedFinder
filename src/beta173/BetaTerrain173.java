package beta173;

import beta173.noise.NoiseGeneratorOctaves173;

import java.util.Random;
import java.util.Arrays;

/**
 * Standalone stripped Beta 1.7.3 overworld base-terrain generator.
 *
 * This is NOT a Bukkit chunk provider.
 * It only generates base terrain into int arrays or heightmaps:
 * AIR / STONE / WATER / ICE.
 *
 * No caves, no trees, no ores, no surface decoration.
 */
public class BetaTerrain173 {
    public static final int AIR = 0;
    public static final int STONE = 1;
    public static final int WATER = 2;
    public static final int ICE = 3;

    public static final int CHUNK_SIZE = 16;
    public static final int WORLD_HEIGHT = 128;
    public static final int SEA_LEVEL = 64;

    private final Random random;

    private final NoiseGeneratorOctaves173 terrainNoise2Generator;
    private final NoiseGeneratorOctaves173 terrainNoise3Generator;
    private final NoiseGeneratorOctaves173 terrainNoise1Generator;
    private final NoiseGeneratorOctaves173 terrainNoise4Generator;
    private final NoiseGeneratorOctaves173 terrainNoise5Generator;

    private final WorldChunkManager173 worldChunkManager;

    private double[] terrainNoise;
    private double[] terrainNoise1;
    private double[] terrainNoise2;
    private double[] terrainNoise3;
    private double[] terrainNoise4;
    private double[] terrainNoise5;
    private byte[] terrainBlendMask;
    private int[] terrainBlendNoise2ActiveY;
    private int[] terrainBlendNoise3ActiveY;

    private BiomeBase173[] biomeNoiseCache;
    private double[] coarseGridTemperature;
    private double[] coarseGridRain;
    private double[] sparseCoarseTemperature;
    private double[] sparseCoarseRain;
    private double[] stage0SampleTemperature;
    private double[] stage0SampleRain;
    private double[] stage0SparseDensity;
    private double[] stage0ScoutDensity;
    // P19 hot-path scratch: exact monster-gate features derived from the already
    // prepared P15/P16 Stage0 density cache. Reused per worker to avoid allocations.
    private boolean[] stage0MonsterY88Grid;
    private boolean[] stage0MonsterY96Grid;
    private boolean[] stage0MonsterVisited;
    private int[] stage0MonsterQueue;
    private int[] stage0RequestedActiveY;
    // P20 research scratch. Progressive upper-scout tiers reuse the exact same
    // Stage0 climate/shape context and 17-level Y semantics as the full scout.
    private boolean[] stage0ProgressiveGeneratedColumns;
    private int stage0ProgressiveSampleSize;
    private int stage0ProgressiveUpperMask;
    private int stage0ProgressiveFromCoarseX;
    private int stage0ProgressiveFromCoarseZ;
    private int stage0ProgressiveStep;
    private boolean stage0ProgressivePrepared;
    private int stage0LowerCandidateColumns;
    private int[] coarseRequestedActiveY;
    private int coarsePreparedChunkRadius = Integer.MIN_VALUE;
    private final boolean[] directStonePrevious = new boolean[16 * 16];

    public BetaTerrain173(long seed) {
        this.worldChunkManager = new WorldChunkManager173(seed);

        /*
         * Keep this exact creation order.
         * Changing the order changes Java Random consumption and gives fake terrain.
         */
        this.random = new Random(seed);
        this.terrainNoise2Generator = new NoiseGeneratorOctaves173(this.random, 16);
        this.terrainNoise3Generator = new NoiseGeneratorOctaves173(this.random, 16);
        this.terrainNoise1Generator = new NoiseGeneratorOctaves173(this.random, 8);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.random, 4);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.random, 4);
        this.terrainNoise4Generator = new NoiseGeneratorOctaves173(this.random, 10);
        this.terrainNoise5Generator = new NoiseGeneratorOctaves173(this.random, 16);
        // treeCountNoise comes after all terrain generators and is never read here.
    }

    /**
     * Patch 13: rebuilds all seed-dependent generator state in place.
     * Buffers, octave objects, permutation arrays, and axis caches are retained.
     */
    public void reseed(long seed) {
        this.worldChunkManager.reseed(seed);
        this.random.setSeed(seed);

        this.terrainNoise2Generator.reseed(this.random);
        this.terrainNoise3Generator.reseed(this.random);
        this.terrainNoise1Generator.reseed(this.random);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.random, 4);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.random, 4);
        this.terrainNoise4Generator.reseed(this.random);
        this.terrainNoise5Generator.reseed(this.random);
    }

    /**
     * Generates one 16x128x16 chunk.
     *
     * Index order:
     * index = (x * 16 + z) * 128 + y
     *
     * This is slower than heightmap-only generation, but useful for verification/debugging.
     */
    public int[] generateChunkBaseTerrain(int chunkX, int chunkZ) {
        int[] blocks = new int[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT];

        /*
         * This fills worldChunkManager.temperature and worldChunkManager.rain.
         * generateTerrainNoise depends on those arrays.
         */
        this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                this.biomeNoiseCache,
                chunkX * 16,
                chunkZ * 16,
                16,
                16
        );

        generateBareTerrain(chunkX, chunkZ, blocks, this.worldChunkManager.temperature);

        return blocks;
    }
    public int writeChunkStoneMask(
        int chunkX,
        int chunkZ,
        byte[] stone,
        int baseX,
        int baseZ,
        int sizeY,
        int sizeZ,
        int copyStartY,
        int candidateMinY,
        int[] candidateStarts,
        int candidateCount,
        boolean sampledCandidates
) {
    /*
     * This fills worldChunkManager.temperature and worldChunkManager.rain.
     * generateTerrainNoise depends on those arrays.
     */
    this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
            this.biomeNoiseCache,
            chunkX * 16,
            chunkZ * 16,
            16,
            16
    );

    return writeBareTerrainStoneMask(
            chunkX,
            chunkZ,
            stone,
            baseX,
            baseZ,
            sizeY,
            sizeZ,
            copyStartY,
            candidateMinY,
            candidateStarts,
            candidateCount,
            sampledCandidates
    );
}

private int writeBareTerrainStoneMask(
        int chunkX,
        int chunkZ,
        byte[] stone,
        int baseX,
        int baseZ,
        int sizeY,
        int sizeZ,
        int copyStartY,
        int candidateMinY,
        int[] candidateStarts,
        int candidateCount,
        boolean sampledCandidates
) {
    byte b0 = 4;
    int k = b0 + 1;
    byte b2 = 17;
    int l = b0 + 1;

    Arrays.fill(this.directStonePrevious, false);

    this.terrainNoise = this.generateTerrainNoise(
            this.terrainNoise,
            chunkX * b0,
            0,
            chunkZ * b0,
            k,
            b2,
            l
    );

    for (int i1 = 0; i1 < b0; ++i1) {
        for (int j1 = 0; j1 < b0; ++j1) {
            for (int k1 = 0; k1 < 16; ++k1) {
                double d0 = 0.125D;

                double d1 = this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 0];
                double d2 = this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 0];
                double d3 = this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 0];
                double d4 = this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 0];

                double d5 = (this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 1] - d1) * d0;
                double d6 = (this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 1] - d2) * d0;
                double d7 = (this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 1] - d3) * d0;
                double d8 = (this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 1] - d4) * d0;

                for (int l1 = 0; l1 < 8; ++l1) {
                    double d9 = 0.25D;
                    double d10 = d1;
                    double d11 = d2;
                    double d12 = (d3 - d1) * d9;
                    double d13 = (d4 - d2) * d9;

                    for (int i2 = 0; i2 < 4; ++i2) {
                        int x = i2 + i1 * 4;
                        int y = k1 * 8 + l1;

                        double d14 = 0.25D;
                        double d15 = d10;
                        double d16 = (d11 - d10) * d14;

                        for (int k2 = 0; k2 < 4; ++k2) {
                            int z = j1 * 4 + k2;

                            boolean isStone = d15 > 0.0D;
                            int columnIndex = x * 16 + z;
                            boolean belowStone = this.directStonePrevious[columnIndex];

                            this.directStonePrevious[columnIndex] = isStone;

                            if (isStone && y >= copyStartY) {
                                int flatIndex = index3(baseX + x, y, baseZ + z, sizeY, sizeZ);
                                stone[flatIndex] = 1;

                                if (y >= candidateMinY) {
                                    boolean allowCandidate = !sampledCandidates || isSampleCandidateColumn(x, z);

                                    if (allowCandidate && !belowStone) {
                                        candidateStarts[candidateCount++] = flatIndex;
                                    }
                                }
                            }

                            d15 += d16;
                        }

                        d10 += d12;
                        d11 += d13;
                    }

                    d1 += d5;
                    d2 += d6;
                    d3 += d7;
                    d4 += d8;
                }
            }
        }
    }

    return candidateCount;
}

    public String getBiomeNameAtWorldXZ(int worldX, int worldZ) {
        return this.worldChunkManager.getBiome(worldX, worldZ).name();
    }
    public double[] getClimateStatsAroundWorldXZ(int centerX, int centerZ, int radius, int step) {
    double sumTemp = 0.0;
    double sumRain = 0.0;
    double sumHumidity = 0.0;

    double maxHumidity = Double.NEGATIVE_INFINITY;

    int hotCount = 0;
    int wetCount = 0;
    int humidCount = 0;
    int count = 0;

    for (int x = centerX - radius; x <= centerX + radius; x += step) {
        for (int z = centerZ - radius; z <= centerZ + radius; z += step) {
            this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                    this.biomeNoiseCache,
                    x,
                    z,
                    1,
                    1
            );

            double temp = this.worldChunkManager.temperature[0];
            double rain = this.worldChunkManager.rain[0];
            double humidity = temp * rain;

            sumTemp += temp;
            sumRain += rain;
            sumHumidity += humidity;

            maxHumidity = Math.max(maxHumidity, humidity);

            if (temp >= 0.75) {
                hotCount++;
            }

            if (rain >= 0.55) {
                wetCount++;
            }

            if (humidity >= 0.45) {
                humidCount++;
            }

            count++;
        }
    }

    if (count == 0) {
        return new double[] {0, 0, 0, 0, 0, 0, 0};
    }

    return new double[] {
            sumTemp / count,
            sumRain / count,
            sumHumidity / count,
            maxHumidity,
            hotCount * 100.0 / count,
            wetCount * 100.0 / count,
            humidCount * 100.0 / count
    };
}

    public int getHeightAtWorldXZ(int worldX, int worldZ) {
        int chunkX = Math.floorDiv(worldX, 16);
        int chunkZ = Math.floorDiv(worldZ, 16);

        int localX = Math.floorMod(worldX, 16);
        int localZ = Math.floorMod(worldZ, 16);

        int[] heights = generateChunkHeightmapOnly(chunkX, chunkZ);

        return heights[localX * 16 + localZ];
    }

    /**
     * Slow/full block-array heightmap.
     * Kept for compatibility and debugging.
     */
    public int[][] generateHeightmapAroundZero(int chunkRadius) {
        int chunkCount = chunkRadius * 2 + 1;
        int size = chunkCount * 16;

        int[][] heightmap = new int[size][size];

        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; chunkX++) {
            for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; chunkZ++) {
                int[] blocks = generateChunkBaseTerrain(chunkX, chunkZ);

                int baseX = (chunkX + chunkRadius) * 16;
                int baseZ = (chunkZ + chunkRadius) * 16;

                for (int x = 0; x < 16; x++) {
                    for (int z = 0; z < 16; z++) {
                        heightmap[baseX + x][baseZ + z] = getSurfaceStoneY(blocks, x, z);
                    }
                }
            }
        }

        return heightmap;
    }

    /**
     * Fast heightmap generation.
     * Does not allocate/write a full 16x128x16 block array per chunk.
     */
    public int[][] generateHeightmapAroundZeroFast(int chunkRadius) {
        int chunkCount = chunkRadius * 2 + 1;
        int size = chunkCount * 16;

        int[][] heightmap = new int[size][size];

        for (int chunkX = -chunkRadius; chunkX <= chunkRadius; chunkX++) {
            for (int chunkZ = -chunkRadius; chunkZ <= chunkRadius; chunkZ++) {
                int[] chunkHeights = generateChunkHeightmapOnly(chunkX, chunkZ);

                int baseX = (chunkX + chunkRadius) * 16;
                int baseZ = (chunkZ + chunkRadius) * 16;

                for (int x = 0; x < 16; x++) {
                    for (int z = 0; z < 16; z++) {
                        heightmap[baseX + x][baseZ + z] = chunkHeights[x * 16 + z];
                    }
                }
            }
        }

        return heightmap;
    }

    /** Fast heightmap around an arbitrary chunk center. */
    public int[][] generateHeightmapAroundChunkCenterFast(int centerChunkX, int centerChunkZ, int chunkRadius) {
        int chunkCount = chunkRadius * 2 + 1;
        int size = chunkCount * 16;
        int[][] heightmap = new int[size][size];

        int minChunkX = centerChunkX - chunkRadius;
        int minChunkZ = centerChunkZ - chunkRadius;
        for (int chunkX = minChunkX; chunkX <= centerChunkX + chunkRadius; chunkX++) {
            for (int chunkZ = minChunkZ; chunkZ <= centerChunkZ + chunkRadius; chunkZ++) {
                int[] chunkHeights = generateChunkHeightmapOnly(chunkX, chunkZ);
                int baseX = (chunkX - minChunkX) * 16;
                int baseZ = (chunkZ - minChunkZ) * 16;
                for (int x = 0; x < 16; x++) {
                    for (int z = 0; z < 16; z++) {
                        heightmap[baseX + x][baseZ + z] = chunkHeights[x * 16 + z];
                    }
                }
            }
        }
        return heightmap;
    }

    /**
     * Fast sampled heightmap around 0,0.
     *
     * This is meant as a cheap prefilter for rare searches.
     * Example: step=8 samples every 8 blocks.
     */
    public int[][] generateSampledHeightmapAroundZeroFast(int chunkRadius, int step) {
    if (step <= 16) {
        throw new IllegalArgumentException("Sparse sample step should be > 16. Use 25 for 50x50 search.");
    }

    int minWorld = -chunkRadius * 16;
    int maxWorld = chunkRadius * 16 + 15;
    int worldSize = maxWorld - minWorld + 1;

    int sampleWidth = (worldSize + step - 1) / step;

    int[][] sampled = new int[sampleWidth][sampleWidth];

    for (int sx = 0; sx < sampleWidth; sx++) {
        int worldX = minWorld + sx * step;

        for (int sz = 0; sz < sampleWidth; sz++) {
            int worldZ = minWorld + sz * step;

            sampled[sx][sz] = getHeightAtWorldXZ(worldX, worldZ);
        }
    }

    return sampled;
}

    /**
     * Generates only the highest STONE Y for each x/z column in a chunk.
     *
     * Output index:
     * heights[x * 16 + z]
     */
    public int[] generateChunkHeightmapOnly(int chunkX, int chunkZ) {
        int[] heights = new int[16 * 16];

        this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                this.biomeNoiseCache,
                chunkX * 16,
                chunkZ * 16,
                16,
                16
        );

        generateBareTerrainHeightsOnly(chunkX, chunkZ, heights);

        return heights;
    }

    private void generateBareTerrainHeightsOnly(int chunkX, int chunkZ, int[] heights) {
        byte b0 = 4;
        int k = b0 + 1;
        byte b2 = 17;
        int l = b0 + 1;

        this.terrainNoise = this.generateTerrainNoise(
                this.terrainNoise,
                chunkX * b0,
                0,
                chunkZ * b0,
                k,
                b2,
                l
        );

        for (int i1 = 0; i1 < b0; ++i1) {
            for (int j1 = 0; j1 < b0; ++j1) {
                for (int k1 = 0; k1 < 16; ++k1) {
                    double d0 = 0.125D;

                    double d1 = this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 0];
                    double d2 = this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 0];
                    double d3 = this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 0];
                    double d4 = this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 0];

                    double d5 = (this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 1] - d1) * d0;
                    double d6 = (this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 1] - d2) * d0;
                    double d7 = (this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 1] - d3) * d0;
                    double d8 = (this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 1] - d4) * d0;

                    for (int l1 = 0; l1 < 8; ++l1) {
                        double d9 = 0.25D;
                        double d10 = d1;
                        double d11 = d2;
                        double d12 = (d3 - d1) * d9;
                        double d13 = (d4 - d2) * d9;

                        for (int i2 = 0; i2 < 4; ++i2) {
                            int x = i2 + i1 * 4;
                            int y = k1 * 8 + l1;

                            double d14 = 0.25D;
                            double d15 = d10;
                            double d16 = (d11 - d10) * d14;

                            for (int k2 = 0; k2 < 4; ++k2) {
                                int z = j1 * 4 + k2;

                                if (d15 > 0.0D) {
                                    int index = x * 16 + z;

                                    if (y > heights[index]) {
                                        heights[index] = y;
                                    }
                                }

                                d15 += d16;
                            }

                            d10 += d12;
                            d11 += d13;
                        }

                        d1 += d5;
                        d2 += d6;
                        d3 += d7;
                        d4 += d8;
                    }
                }
            }
        }
    }

    public static int getSurfaceStoneY(int[] blocks, int x, int z) {
        for (int y = WORLD_HEIGHT - 1; y >= 0; y--) {
            int block = blocks[index(x, y, z)];
            if (block == STONE) {
                return y;
            }
        }
        return 0;
    }

    public static int index(int x, int y, int z) {
        return (x * 16 + z) * 128 + y;
    }

    private void generateBareTerrain(int chunkX, int chunkZ, int[] blocks, double[] temperatures) {
        byte b0 = 4;
        byte b1 = 64;
        int k = b0 + 1;
        byte b2 = 17;
        int l = b0 + 1;

        this.terrainNoise = this.generateTerrainNoise(
                this.terrainNoise,
                chunkX * b0,
                0,
                chunkZ * b0,
                k,
                b2,
                l
        );

        for (int i1 = 0; i1 < b0; ++i1) {
            for (int j1 = 0; j1 < b0; ++j1) {
                for (int k1 = 0; k1 < 16; ++k1) {
                    double d0 = 0.125D;

                    double d1 = this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 0];
                    double d2 = this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 0];
                    double d3 = this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 0];
                    double d4 = this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 0];

                    double d5 = (this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 1] - d1) * d0;
                    double d6 = (this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 1] - d2) * d0;
                    double d7 = (this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 1] - d3) * d0;
                    double d8 = (this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 1] - d4) * d0;

                    for (int l1 = 0; l1 < 8; ++l1) {
                        double d9 = 0.25D;
                        double d10 = d1;
                        double d11 = d2;
                        double d12 = (d3 - d1) * d9;
                        double d13 = (d4 - d2) * d9;

                        for (int i2 = 0; i2 < 4; ++i2) {
                            int x = i2 + i1 * 4;
                            int y = k1 * 8 + l1;

                            double d14 = 0.25D;
                            double d15 = d10;
                            double d16 = (d11 - d10) * d14;

                            for (int k2 = 0; k2 < 4; ++k2) {
                                int z = j1 * 4 + k2;

                                double temperature = temperatures[(i1 * 4 + i2) * 16 + j1 * 4 + k2];

                                int block = AIR;

                                if (y < b1) {
                                    if (temperature < 0.5D && y >= b1 - 1) {
                                        block = ICE;
                                    } else {
                                        block = WATER;
                                    }
                                }

                                if (d15 > 0.0D) {
                                    block = STONE;
                                }

                                blocks[index(x, y, z)] = block;

                                d15 += d16;
                            }

                            d10 += d12;
                            d11 += d13;
                        }

                        d1 += d5;
                        d2 += d6;
                        d3 += d7;
                        d4 += d8;
                    }
                }
            }
        }
    }

    private double[] generateTerrainNoise(double[] noise, int fromX, int fromY, int fromZ, int xLen, int yLen, int zLen) {
        if (noise == null || noise.length < xLen * yLen * zLen) {
            noise = new double[xLen * yLen * zLen];
        }

        double d0 = 684.412D;
        double d1 = 684.412D;

        double[] temperatures = this.worldChunkManager.temperature;
        double[] rain = this.worldChunkManager.rain;

        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise(
                this.terrainNoise4,
                fromX,
                fromZ,
                xLen,
                zLen,
                1.121D,
                1.121D,
                0.5D
        );

        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise(
                this.terrainNoise5,
                fromX,
                fromZ,
                xLen,
                zLen,
                200.0D,
                200.0D,
                0.5D
        );

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoise(
                this.terrainNoise1,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0 / 80.0D,
                d1 / 160.0D,
                d0 / 80.0D
        );

        this.terrainNoise2 = this.terrainNoise2Generator.generateNoise(
                this.terrainNoise2,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0,
                d1,
                d0
        );

        this.terrainNoise3 = this.terrainNoise3Generator.generateNoise(
                this.terrainNoise3,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0,
                d1,
                d0
        );

        int k1 = 0;
        int l1 = 0;
        int i2 = 16 / xLen;

        for (int j2 = 0; j2 < xLen; ++j2) {
            int k2 = j2 * i2 + i2 / 2;

            for (int l2 = 0; l2 < zLen; ++l2) {
                int i3 = l2 * i2 + i2 / 2;

                double d2 = temperatures[k2 * 16 + i3];
                double d3 = rain[k2 * 16 + i3] * d2;
                double d4 = 1.0D - d3;

                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[l1] + 256.0D) / 512.0D;
                d5 *= d4;

                if (d5 > 1.0D) {
                    d5 = 1.0D;
                }

                double d6 = this.terrainNoise5[l1] / 8000.0D;

                if (d6 < 0.0D) {
                    d6 = -d6 * 0.3D;
                }

                d6 = d6 * 3.0D - 2.0D;

                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) {
                        d6 = -1.0D;
                    }

                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) {
                        d6 = 1.0D;
                    }

                    d6 /= 8.0D;
                }

                if (d5 < 0.0D) {
                    d5 = 0.0D;
                }

                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;

                double d7 = (double) yLen / 2.0D + d6 * 4.0D;

                ++l1;

                for (int j3 = 0; j3 < yLen; ++j3) {
                    double d8;
                    double d9 = ((double) j3 - d7) * 12.0D / d5;

                    if (d9 < 0.0D) {
                        d9 *= 4.0D;
                    }

                    double d10 = this.terrainNoise2[k1] / 512.0D;
                    double d11 = this.terrainNoise3[k1] / 512.0D;
                    double d12 = (this.terrainNoise1[k1] / 10.0D + 1.0D) / 2.0D;

                    if (d12 < 0.0D) {
                        d8 = d10;
                    } else if (d12 > 1.0D) {
                        d8 = d11;
                    } else {
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;

                    if (j3 > yLen - 4) {
                        double d13 = (double) ((float) (j3 - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }

                    noise[k1] = d8;
                    ++k1;
                }
            }
        }

        return noise;
    }

    /**
     * Debug/analysis helper.
     *
     * Generates the final interpolated terrain density for one chunk.
     * This is the raw value that base terrain turns into stone when density > 0.0D.
     *
     * Index order matches generateChunkBaseTerrain:
     * index = (x * 16 + z) * 128 + y
     */
    public double[] generateChunkDensityField(int chunkX, int chunkZ) {
        double[] density = new double[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT];

        /*
         * This fills worldChunkManager.temperature and worldChunkManager.rain.
         * generateTerrainNoise depends on those arrays.
         */
        this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                this.biomeNoiseCache,
                chunkX * 16,
                chunkZ * 16,
                16,
                16
        );

        generateBareTerrainDensityField(chunkX, chunkZ, density);

        return density;
    }

    private void generateBareTerrainDensityField(int chunkX, int chunkZ, double[] density) {
        byte b0 = 4;
        int k = b0 + 1;
        byte b2 = 17;
        int l = b0 + 1;

        this.terrainNoise = this.generateTerrainNoise(
                this.terrainNoise,
                chunkX * b0,
                0,
                chunkZ * b0,
                k,
                b2,
                l
        );

        for (int i1 = 0; i1 < b0; ++i1) {
            for (int j1 = 0; j1 < b0; ++j1) {
                for (int k1 = 0; k1 < 16; ++k1) {
                    double d0 = 0.125D;

                    double d1 = this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 0];
                    double d2 = this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 0];
                    double d3 = this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 0];
                    double d4 = this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 0];

                    double d5 = (this.terrainNoise[((i1 + 0) * l + j1 + 0) * b2 + k1 + 1] - d1) * d0;
                    double d6 = (this.terrainNoise[((i1 + 0) * l + j1 + 1) * b2 + k1 + 1] - d2) * d0;
                    double d7 = (this.terrainNoise[((i1 + 1) * l + j1 + 0) * b2 + k1 + 1] - d3) * d0;
                    double d8 = (this.terrainNoise[((i1 + 1) * l + j1 + 1) * b2 + k1 + 1] - d4) * d0;

                    for (int l1 = 0; l1 < 8; ++l1) {
                        double d9 = 0.25D;
                        double d10 = d1;
                        double d11 = d2;
                        double d12 = (d3 - d1) * d9;
                        double d13 = (d4 - d2) * d9;

                        for (int i2 = 0; i2 < 4; ++i2) {
                            int x = i2 + i1 * 4;
                            int y = k1 * 8 + l1;

                            double d14 = 0.25D;
                            double d15 = d10;
                            double d16 = (d11 - d10) * d14;

                            for (int k2 = 0; k2 < 4; ++k2) {
                                int z = j1 * 4 + k2;

                                density[index(x, y, z)] = d15;

                                d15 += d16;
                            }

                            d10 += d12;
                            d11 += d13;
                        }

                        d1 += d5;
                        d2 += d6;
                        d3 += d7;
                        d4 += d8;
                    }
                }
            }
        }
    }




    /**
     * Generates the full coarse density grid for the whole search area in one noise call.
     *
     * This is used by Hunter v2 as a faster replacement for generating 5x17x5
     * chunk coarse grids one chunk at a time. It uses the same coarse coordinates
     * that chunk terrain uses: coarse world X/Z coordinates are chunk*4 + local0..4,
     * and the biome/climate sample for each coarse point is at block coordinate
     * coarse*4 + 2, matching the old per-chunk code.
     *
     * Output index:
     * index = (coarseX * coarseSize + coarseZ) * 17 + coarseY
     */
    public void generateCoarseDensityGridAroundZeroInto(double[] outValues, boolean[] outFilled, int chunkRadius) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * 17 * coarseSize;

        if (outValues == null || outValues.length < total) {
            throw new IllegalArgumentException("outValues buffer too small for coarse grid");
        }
        if (outFilled == null || outFilled.length < total) {
            throw new IllegalArgumentException("outFilled buffer too small for coarse grid");
        }

        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;

        prepareCoarseGridClimate(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);
        generateTerrainNoiseForCoarseGridCachedAxes(outValues, fromCoarseX, 0, fromCoarseZ, coarseSize, 17, coarseSize,
                this.coarseGridTemperature, this.coarseGridRain);

        Arrays.fill(outFilled, 0, total, true);
    }

    /**
     * P17 phase 1: prepares the full coarse-grid climate/shape context and generates
     * only Y=minUpperYIndex..16 for every X/Z column. Lower cells are left
     * untouched except for outFilled=false so they can be completed selectively.
     */
    public void prepareCoarseDensityUpperSliceAroundZeroInto(
            double[] outValues,
            boolean[] outFilled,
            int chunkRadius,
            int minUpperYIndex
    ) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * 17 * coarseSize;
        int columns = coarseSize * coarseSize;

        if (outValues == null || outValues.length < total) {
            throw new IllegalArgumentException("outValues buffer too small for coarse grid");
        }
        if (outFilled == null || outFilled.length < total) {
            throw new IllegalArgumentException("outFilled buffer too small for coarse grid");
        }

        minUpperYIndex = Math.max(0, Math.min(16, minUpperYIndex));
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;

        prepareCoarseGridClimate(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);
        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise(
                this.terrainNoise4, fromCoarseX, fromCoarseZ, coarseSize, coarseSize, 1.121D, 1.121D, 0.5D
        );
        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise(
                this.terrainNoise5, fromCoarseX, fromCoarseZ, coarseSize, coarseSize, 200.0D, 200.0D, 0.5D
        );

        if (this.coarseRequestedActiveY == null || this.coarseRequestedActiveY.length < columns) {
            this.coarseRequestedActiveY = new int[columns];
        }
        int upperMask = (-1 << minUpperYIndex) & 0x1FFFF;
        Arrays.fill(this.coarseRequestedActiveY, 0, columns, upperMask);

        generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
                outValues,
                fromCoarseX,
                0,
                fromCoarseZ,
                coarseSize,
                17,
                coarseSize,
                1,
                1,
                this.coarseRequestedActiveY,
                this.coarseGridTemperature,
                this.coarseGridRain
        );

        Arrays.fill(outFilled, 0, total, false);
        for (int column = 0; column < columns; column++) {
            int base = column * 17;
            for (int y = minUpperYIndex; y < 17; y++) {
                outFilled[base + y] = true;
            }
        }
        this.coarsePreparedChunkRadius = chunkRadius;
    }

    /**
     * P17 phase 2: completes Y=0..maxLowerYIndex for selected coarse columns,
     * reusing the climate and 2D shape-noise context prepared by the upper slice.
     * Returns the number of newly completed columns.
     */
    public int completeCoarseDensityLowerColumnsAroundZeroInto(
            double[] outValues,
            boolean[] outFilled,
            int chunkRadius,
            boolean[] requestedColumns,
            int maxLowerYIndex
    ) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * 17 * coarseSize;
        int columns = coarseSize * coarseSize;

        if (this.coarsePreparedChunkRadius != chunkRadius) {
            throw new IllegalStateException("coarse upper slice must be prepared before lower completion");
        }
        if (outValues == null || outValues.length < total) {
            throw new IllegalArgumentException("outValues buffer too small for coarse grid");
        }
        if (outFilled == null || outFilled.length < total) {
            throw new IllegalArgumentException("outFilled buffer too small for coarse grid");
        }
        if (requestedColumns == null || requestedColumns.length < columns) {
            throw new IllegalArgumentException("requestedColumns buffer too small");
        }

        maxLowerYIndex = Math.max(-1, Math.min(16, maxLowerYIndex));
        if (maxLowerYIndex < 0) return 0;
        int lowerMask = (1 << (maxLowerYIndex + 1)) - 1;

        if (this.coarseRequestedActiveY == null || this.coarseRequestedActiveY.length < columns) {
            this.coarseRequestedActiveY = new int[columns];
        }

        int newlyRequested = 0;
        for (int column = 0; column < columns; column++) {
            int base = column * 17;
            boolean needs = requestedColumns[column] && !outFilled[base];
            this.coarseRequestedActiveY[column] = needs ? lowerMask : 0;
            if (needs) newlyRequested++;
        }
        if (newlyRequested == 0) return 0;

        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
                outValues,
                fromCoarseX,
                0,
                fromCoarseZ,
                coarseSize,
                17,
                coarseSize,
                1,
                1,
                this.coarseRequestedActiveY,
                this.coarseGridTemperature,
                this.coarseGridRain
        );

        for (int column = 0; column < columns; column++) {
            if (this.coarseRequestedActiveY[column] == 0) continue;
            int base = column * 17;
            for (int y = 0; y <= maxLowerYIndex; y++) {
                outFilled[base + y] = true;
            }
        }
        return newlyRequested;
    }

    /**
     * Legacy 3D terrain-noise path used only by CoarseNoiseAxisCacheCheck.
     * Climate sampling stays on the Patch 4 strided path so the check isolates
     * only the new cached-axis Perlin optimization.
     */
    public void generateCoarseDensityGridAroundZeroLegacyNoiseInto(
            double[] outValues,
            boolean[] outFilled,
            int chunkRadius
    ) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * 17 * coarseSize;

        if (outValues == null || outValues.length < total) {
            throw new IllegalArgumentException("outValues buffer too small for coarse grid");
        }
        if (outFilled == null || outFilled.length < total) {
            throw new IllegalArgumentException("outFilled buffer too small for coarse grid");
        }

        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;

        prepareCoarseGridClimate(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);
        generateTerrainNoiseForCoarseGrid(outValues, fromCoarseX, 0, fromCoarseZ, coarseSize, 17, coarseSize,
                this.coarseGridTemperature, this.coarseGridRain);

        Arrays.fill(outFilled, 0, total, true);
    }

    /**
     * Legacy dense-climate version used only by CoarseClimateStrideCheck.
     * It generates the exact same coarse density lattice through the old
     * 241x241-at-radius-7 climate preparation path.
     */
    public void generateCoarseDensityGridAroundZeroDenseClimateInto(
            double[] outValues,
            boolean[] outFilled,
            int chunkRadius
    ) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int total = coarseSize * 17 * coarseSize;

        if (outValues == null || outValues.length < total) {
            throw new IllegalArgumentException("outValues buffer too small for coarse grid");
        }
        if (outFilled == null || outFilled.length < total) {
            throw new IllegalArgumentException("outFilled buffer too small for coarse grid");
        }

        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;

        prepareCoarseGridClimateDenseLegacy(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);
        generateTerrainNoiseForCoarseGrid(outValues, fromCoarseX, 0, fromCoarseZ, coarseSize, 17, coarseSize,
                this.coarseGridTemperature, this.coarseGridRain);

        Arrays.fill(outFilled, 0, total, true);
    }


    /**
     * Stage-0 helper for Hunter v2.
     *
     * Samples sparse vertical columns across the full radius and counts columns
     * with a positive -> negative -> positive density re-entry pattern. This is
     * much cheaper than building the whole 3D coarse component grid, but it is
     * only a sniff test: it can reject obvious trash, not prove a good island.
     */
    public int countSparseCoarseReentrySamplesAroundZero(
            int chunkRadius,
            int step,
            int minUpperYIndex,
            int minSamplesNeeded,
            double[] columnBuffer
    ) {
        if (columnBuffer == null || columnBuffer.length < 17) {
            throw new IllegalArgumentException("columnBuffer must have length at least 17");
        }

        if (step < 1) {
            step = 1;
        }

        minUpperYIndex = Math.max(0, Math.min(16, minUpperYIndex));
        minSamplesNeeded = Math.max(1, minSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;

        // Same climate shaping used by the full coarse grid, prepared once.
        prepareCoarseGridClimate(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);

        int reentrySamples = 0;

        for (int localX = 0; localX < coarseSize; localX += step) {
            int coarseX = fromCoarseX + localX;

            for (int localZ = 0; localZ < coarseSize; localZ += step) {
                int coarseZ = fromCoarseZ + localZ;
                int climateIndex = localX * coarseSize + localZ;

                samplePreparedCoarseDensityColumnInto(
                        columnBuffer,
                        coarseX,
                        coarseZ,
                        this.coarseGridTemperature[climateIndex],
                        this.coarseGridRain[climateIndex]
                );

                if (hasSparseReentryPattern(columnBuffer, minUpperYIndex)) {
                    reentrySamples++;

                    if (reentrySamples >= minSamplesNeeded) {
                        return reentrySamples;
                    }
                }
            }
        }

        return reentrySamples;
    }

    /**
     * Combined Stage0 + Stage0.5 helper.
     *
     * The old pipeline sampled the same sparse X/Z columns twice: once for the
     * low Y gate and again for the high Y gate. This method prepares climate once,
     * generates the complete sparse 16x17x16 density lattice in one strided
     * cached-axis batch, then updates both counters in the original sample order.
     *
     * It is result-equivalent to the two old early-stop calls: a seed passes only
     * when both requested counts reach their thresholds. Counts are returned as
     * observed; callers may clamp them to the configured thresholds to preserve
     * the old debug/CSV score ranges.
     */
    public SparseGateCounts countSparseCoarseReentrySamplesAroundZeroCombined(
            int chunkRadius,
            int step,
            int lowMinUpperYIndex,
            int lowSamplesNeeded,
            int highMinUpperYIndex,
            int highSamplesNeeded,
            double[] columnBuffer
    ) {
        if (columnBuffer == null || columnBuffer.length < 17) {
            throw new IllegalArgumentException("columnBuffer must have length at least 17");
        }

        if (step < 1) step = 1;

        lowMinUpperYIndex = Math.max(0, Math.min(16, lowMinUpperYIndex));
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        lowSamplesNeeded = Math.max(1, lowSamplesNeeded);
        highSamplesNeeded = Math.max(1, highSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);

        int required = sampleSize * sampleSize * 17;
        if (this.stage0SparseDensity == null || this.stage0SparseDensity.length < required) {
            this.stage0SparseDensity = new double[required];
        }

        generateTerrainNoiseForSparseStage0BatchCachedAxes(
                this.stage0SparseDensity,
                fromCoarseX,
                0,
                fromCoarseZ,
                sampleSize,
                17,
                sampleSize,
                step,
                step,
                this.stage0SampleTemperature,
                this.stage0SampleRain
        );

        int lowCount = 0;
        int highCount = 0;
        int columnIndex = 0;

        for (int sampleX = 0; sampleX < sampleSize; sampleX++) {
            for (int sampleZ = 0; sampleZ < sampleSize; sampleZ++) {
                int highestReentryY = highestSparseReentryYIndex(this.stage0SparseDensity, columnIndex * 17);
                if (highestReentryY >= lowMinUpperYIndex) lowCount++;
                if (highestReentryY >= highMinUpperYIndex) highCount++;

                if (lowCount >= lowSamplesNeeded && highCount >= highSamplesNeeded) {
                    return new SparseGateCounts(lowCount, highCount);
                }
                columnIndex++;
            }
        }

        return new SparseGateCounts(lowCount, highCount);
    }

    /**
     * P15 exact upper-Y scout.
     *
     * The normal high Stage0 gate requires highSamplesNeeded distinct columns with
     * a positive->negative->positive re-entry whose final positive sample is at or
     * above highMinUpperYIndex. Every such column must therefore contain at least
     * one positive density sample in [highMinUpperYIndex, 16].
     *
     * This method generates only those upper Y samples (while preserving the full
     * 17-level Beta Y-cell cache semantics) and counts columns with any positive
     * upper density. Fewer than highSamplesNeeded is a mathematically safe reject.
     */
    public int prepareStage0UpperPositiveScoutAroundZero(
            int chunkRadius,
            int step,
            int highMinUpperYIndex,
            int highSamplesNeeded
    ) {
        if (step < 1) step = 1;
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        highSamplesNeeded = Math.max(1, highSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;
        int required = sampleSize * sampleSize * 17;

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);
        prepareSparseStage0ShapeNoise(fromCoarseX, fromCoarseZ, sampleSize, sampleSize, step, step);

        if (this.stage0ScoutDensity == null || this.stage0ScoutDensity.length < required) {
            this.stage0ScoutDensity = new double[required];
        }

        int upperMask = (-1 << highMinUpperYIndex) & 0x1FFFF;
        generateTerrainNoiseForSparseStage0YMaskCachedAxes(
                this.stage0ScoutDensity,
                fromCoarseX,
                0,
                fromCoarseZ,
                sampleSize,
                17,
                sampleSize,
                step,
                step,
                upperMask,
                this.stage0SampleTemperature,
                this.stage0SampleRain
        );

        int positiveColumns = 0;
        int columns = sampleSize * sampleSize;
        for (int column = 0; column < columns; column++) {
            int offset = column * 17;
            boolean anyPositive = false;
            for (int y = highMinUpperYIndex; y < 17; y++) {
                if (this.stage0ScoutDensity[offset + y] > 0.0D) {
                    anyPositive = true;
                    break;
                }
            }
            if (anyPositive) {
                positiveColumns++;
            }
        }
        return positiveColumns;
    }


    /** P20 research: exact regular 8x8 mini-context at even/odd full-lattice indices. */
    public int analyzeRegularStage0MiniScout64AroundZero(
            int chunkRadius, int step, int highMinUpperYIndex, int offsetParity
    ) {
        if (step < 1) step = 1;
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        offsetParity = offsetParity == 0 ? 0 : 1;
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fullSize = (coarseSize + step - 1) / step;
        if (fullSize != 16) throw new IllegalArgumentException("regular P20 mini scout requires 16x16 full lattice");
        int miniSize = 8;
        int fromCoarseX = -chunkRadius * 4 + offsetParity * step;
        int fromCoarseZ = -chunkRadius * 4 + offsetParity * step;
        int miniStep = step * 2;
        int required = miniSize * miniSize * 17;
        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, miniStep, miniSize);
        prepareSparseStage0ShapeNoise(fromCoarseX, fromCoarseZ, miniSize, miniSize, miniStep, miniStep);
        if (this.stage0ScoutDensity == null || this.stage0ScoutDensity.length < required) this.stage0ScoutDensity = new double[required];
        int upperMask = (-1 << highMinUpperYIndex) & 0x1FFFF;
        generateTerrainNoiseForSparseStage0YMaskCachedAxes(
                this.stage0ScoutDensity, fromCoarseX, 0, fromCoarseZ,
                miniSize, 17, miniSize, miniStep, miniStep, upperMask,
                this.stage0SampleTemperature, this.stage0SampleRain
        );
        int positives = 0;
        for (int column = 0; column < miniSize * miniSize; column++) {
            int base = column * 17;
            for (int y = highMinUpperYIndex; y < 17; y++) {
                if (this.stage0ScoutDensity[base + y] > 0.0D) { positives++; break; }
            }
        }
        return positives;
    }

    /**
     * P20 production pre-scout: generate the exact nested 8x8 / 64-column upper
     * subset and return its features. If the gate accepts, the remaining 192
     * columns can be completed by completeStage0UpperPositiveScoutAfterProgressiveTiers.
     */
    public ProgressiveStage0TierFeatures prepareStage0UpperPositiveScoutTier64AroundZero(
            int chunkRadius,
            int step,
            int highMinUpperYIndex
    ) {
        prepareProgressiveStage0UpperScoutContextAroundZero(chunkRadius, step, highMinUpperYIndex);
        return generateAndAnalyzeProgressiveStage0Tier(64);
    }

    /**
     * P20 research path: prepares the exact Stage0 context and generates only a
     * nested 4x4 (16-column) then 8x8 (64-column) subset of the normal 16x16
     * sparse scout. The selected columns use the same world coordinates and the
     * same full yLen=17 active-Y noise semantics as the production P15 scout.
     *
     * This method intentionally does not make a gate decision. It exists so the
     * progressive tiers can be measured and trained before any production filter
     * is shipped.
     */
    public ProgressiveStage0ScoutFeatures analyzeProgressiveStage0UpperTiersAroundZero(
            int chunkRadius,
            int step,
            int highMinUpperYIndex
    ) {
        prepareProgressiveStage0UpperScoutContextAroundZero(chunkRadius, step, highMinUpperYIndex);

        ProgressiveStage0ScoutFeatures result = new ProgressiveStage0ScoutFeatures();
        result.tier16 = generateAndAnalyzeProgressiveStage0Tier(16);
        result.tier64 = generateAndAnalyzeProgressiveStage0Tier(64);
        return result;
    }

    /**
     * Continues from analyzeProgressiveStage0UpperTiersAroundZero(...) and fills
     * the remaining upper-scout columns exactly. The resulting cache is suitable
     * for the existing P16 lower completion path.
     */
    public int completeStage0UpperPositiveScoutAfterProgressiveTiers(
            int chunkRadius,
            int step,
            int highMinUpperYIndex
    ) {
        if (!this.stage0ProgressivePrepared) {
            throw new IllegalStateException("progressive Stage0 context was not prepared");
        }
        if (step < 1) step = 1;
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int sampleSize = (coarseSize + step - 1) / step;
        if (sampleSize != this.stage0ProgressiveSampleSize) {
            throw new IllegalStateException("progressive Stage0 sample size changed");
        }
        int expectedMask = (-1 << highMinUpperYIndex) & 0x1FFFF;
        if (expectedMask != this.stage0ProgressiveUpperMask) {
            throw new IllegalStateException("progressive Stage0 upper mask changed");
        }

        int columns = sampleSize * sampleSize;
        Arrays.fill(this.stage0RequestedActiveY, 0, columns, 0);
        int remaining = 0;
        for (int column = 0; column < columns; column++) {
            if (!this.stage0ProgressiveGeneratedColumns[column]) {
                this.stage0RequestedActiveY[column] = this.stage0ProgressiveUpperMask;
                remaining++;
            }
        }

        if (remaining > 0) {
            int fromCoarseX = -chunkRadius * 4;
            int fromCoarseZ = -chunkRadius * 4;
            generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
                    this.stage0ScoutDensity,
                    fromCoarseX,
                    0,
                    fromCoarseZ,
                    sampleSize,
                    17,
                    sampleSize,
                    step,
                    step,
                    this.stage0RequestedActiveY,
                    this.stage0SampleTemperature,
                    this.stage0SampleRain
            );
            for (int column = 0; column < columns; column++) {
                if (this.stage0RequestedActiveY[column] != 0) {
                    this.stage0ProgressiveGeneratedColumns[column] = true;
                }
            }
        }

        int positiveColumns = 0;
        for (int column = 0; column < columns; column++) {
            int offset = column * 17;
            for (int y = highMinUpperYIndex; y < 17; y++) {
                if (this.stage0ScoutDensity[offset + y] > 0.0D) {
                    positiveColumns++;
                    break;
                }
            }
        }
        return positiveColumns;
    }

    private void prepareProgressiveStage0UpperScoutContextAroundZero(
            int chunkRadius,
            int step,
            int highMinUpperYIndex
    ) {
        if (step < 1) step = 1;
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;
        int columns = sampleSize * sampleSize;
        int required = columns * 17;

        // P20's current nested layouts are deliberately defined for the normal
        // radius-7/step-4 16x16 Stage0 lattice. Fail loudly in research instead
        // of silently measuring a different geometry.
        if (sampleSize != 16) {
            throw new IllegalArgumentException("P20 progressive scout currently requires a 16x16 Stage0 lattice");
        }

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);
        prepareSparseStage0ShapeNoise(fromCoarseX, fromCoarseZ, sampleSize, sampleSize, step, step);

        if (this.stage0ScoutDensity == null || this.stage0ScoutDensity.length < required) {
            this.stage0ScoutDensity = new double[required];
        }
        if (this.stage0RequestedActiveY == null || this.stage0RequestedActiveY.length < columns) {
            this.stage0RequestedActiveY = new int[columns];
        }
        if (this.stage0ProgressiveGeneratedColumns == null
                || this.stage0ProgressiveGeneratedColumns.length < columns) {
            this.stage0ProgressiveGeneratedColumns = new boolean[columns];
        }

        Arrays.fill(this.stage0ProgressiveGeneratedColumns, 0, columns, false);
        Arrays.fill(this.stage0RequestedActiveY, 0, columns, 0);
        this.stage0ProgressiveSampleSize = sampleSize;
        this.stage0ProgressiveUpperMask = (-1 << highMinUpperYIndex) & 0x1FFFF;
        this.stage0ProgressiveFromCoarseX = fromCoarseX;
        this.stage0ProgressiveFromCoarseZ = fromCoarseZ;
        this.stage0ProgressiveStep = step;
        this.stage0ProgressivePrepared = true;
    }

    private ProgressiveStage0TierFeatures generateAndAnalyzeProgressiveStage0Tier(int tierColumns) {
        if (!this.stage0ProgressivePrepared) {
            throw new IllegalStateException("progressive Stage0 context was not prepared");
        }
        int[] axis = progressiveStage0AxisForTier(tierColumns);
        int sampleSize = this.stage0ProgressiveSampleSize;
        int columns = sampleSize * sampleSize;

        Arrays.fill(this.stage0RequestedActiveY, 0, columns, 0);
        int newlyRequested = 0;
        for (int gx : axis) {
            for (int gz : axis) {
                int column = gx * sampleSize + gz;
                if (!this.stage0ProgressiveGeneratedColumns[column]) {
                    this.stage0RequestedActiveY[column] = this.stage0ProgressiveUpperMask;
                    newlyRequested++;
                }
            }
        }

        if (newlyRequested > 0) {
            generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
                    this.stage0ScoutDensity,
                    this.stage0ProgressiveFromCoarseX,
                    0,
                    this.stage0ProgressiveFromCoarseZ,
                    sampleSize,
                    17,
                    sampleSize,
                    this.stage0ProgressiveStep,
                    this.stage0ProgressiveStep,
                    this.stage0RequestedActiveY,
                    this.stage0SampleTemperature,
                    this.stage0SampleRain
            );
            for (int column = 0; column < columns; column++) {
                if (this.stage0RequestedActiveY[column] != 0) {
                    this.stage0ProgressiveGeneratedColumns[column] = true;
                }
            }
        }

        return analyzeProgressiveStage0Tier(axis);
    }

    private ProgressiveStage0TierFeatures analyzeProgressiveStage0Tier(int[] axis) {
        ProgressiveStage0TierFeatures f = new ProgressiveStage0TierFeatures();
        f.axisSize = axis.length;
        f.columnsSampled = axis.length * axis.length;

        boolean[] upper = new boolean[f.columnsSampled];
        boolean[] y96plus = new boolean[f.columnsSampled];
        int[] highest = new int[f.columnsSampled];
        Arrays.fill(highest, -1);

        int tierIndex = 0;
        for (int ix = 0; ix < axis.length; ix++) {
            int x = axis[ix];
            for (int iz = 0; iz < axis.length; iz++) {
                int z = axis[iz];
                int column = x * this.stage0ProgressiveSampleSize + z;
                int offset = column * 17;

                boolean anyUpper = false;
                boolean any96 = false;
                boolean any104 = false;
                boolean any112 = false;
                int highestPositiveY = -1;

                for (int y = 11; y < 17; y++) {
                    double density = this.stage0ScoutDensity[offset + y];
                    if (density > 0.0D) {
                        anyUpper = true;
                        if (y >= 12) any96 = true;
                        if (y >= 13) any104 = true;
                        if (y >= 14) any112 = true;
                        highestPositiveY = y;
                        f.positiveDensityCells++;
                        f.sumPositiveDensity += density;
                        if (density > f.maxPositiveDensity) f.maxPositiveDensity = density;
                    }
                }

                if (this.stage0ScoutDensity[offset + 11] > 0.0D) f.positiveAtY88++;
                if (this.stage0ScoutDensity[offset + 12] > 0.0D) f.positiveAtY96++;
                if (anyUpper) f.upperPositiveColumns++;
                if (any96) f.y96PlusColumns++;
                if (any104) f.y104PlusColumns++;
                if (any112) f.y112PlusColumns++;
                if (highestPositiveY > f.highestPositiveYIndex) f.highestPositiveYIndex = highestPositiveY;

                upper[tierIndex] = anyUpper;
                y96plus[tierIndex] = any96;
                highest[tierIndex] = highestPositiveY;
                tierIndex++;
            }
        }

        fillProgressiveTierSpatialStats(upper, axis, f, false);
        fillProgressiveTierSpatialStats(y96plus, axis, f, true);

        if (f.positiveDensityCells > 0) {
            f.avgPositiveDensity = f.sumPositiveDensity / f.positiveDensityCells;
        }
        return f;
    }

    private static void fillProgressiveTierSpatialStats(
            boolean[] grid,
            int[] axis,
            ProgressiveStage0TierFeatures out,
            boolean y96
    ) {
        int n = axis.length;
        boolean[] visited = new boolean[n * n];
        int[] queue = new int[n * n];
        int largest = 0;
        int bestMinX = 0;
        int bestMaxX = -1;
        int bestMinZ = 0;
        int bestMaxZ = -1;
        int adjacentEdges = 0;
        int occupiedRows = 0;
        int occupiedCols = 0;
        int quadrantsMask = 0;

        for (int x = 0; x < n; x++) {
            boolean row = false;
            boolean col = false;
            for (int z = 0; z < n; z++) {
                int index = x * n + z;
                if (grid[index]) {
                    row = true;
                    if (x < n / 2) quadrantsMask |= (z < n / 2 ? 1 : 2);
                    else quadrantsMask |= (z < n / 2 ? 4 : 8);
                    if (x + 1 < n && grid[(x + 1) * n + z]) adjacentEdges++;
                    if (z + 1 < n && grid[x * n + z + 1]) adjacentEdges++;
                }
                if (grid[z * n + x]) col = true;
            }
            if (row) occupiedRows++;
            if (col) occupiedCols++;
        }

        for (int start = 0; start < grid.length; start++) {
            if (!grid[start] || visited[start]) continue;
            int head = 0;
            int tail = 0;
            visited[start] = true;
            queue[tail++] = start;
            int size = 0;
            int minX = n;
            int maxX = -1;
            int minZ = n;
            int maxZ = -1;

            while (head < tail) {
                int current = queue[head++];
                size++;
                int cx = current / n;
                int cz = current % n;
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cz < minZ) minZ = cz;
                if (cz > maxZ) maxZ = cz;

                if (cx > 0) tail = addProgressiveNeighbor(grid, visited, queue, tail, n, cx - 1, cz);
                if (cx + 1 < n) tail = addProgressiveNeighbor(grid, visited, queue, tail, n, cx + 1, cz);
                if (cz > 0) tail = addProgressiveNeighbor(grid, visited, queue, tail, n, cx, cz - 1);
                if (cz + 1 < n) tail = addProgressiveNeighbor(grid, visited, queue, tail, n, cx, cz + 1);
            }

            if (size > largest) {
                largest = size;
                bestMinX = minX;
                bestMaxX = maxX;
                bestMinZ = minZ;
                bestMaxZ = maxZ;
            }
        }

        int width = largest == 0 ? 0 : axis[bestMaxX] - axis[bestMinX] + 1;
        int depth = largest == 0 ? 0 : axis[bestMaxZ] - axis[bestMinZ] + 1;
        int quadrants = Integer.bitCount(quadrantsMask);

        if (y96) {
            out.y96LargestCluster = largest;
            out.y96ClusterWidth = width;
            out.y96ClusterDepth = depth;
            out.y96OccupiedRows = occupiedRows;
            out.y96OccupiedCols = occupiedCols;
            out.y96Quadrants = quadrants;
            out.y96AdjacentEdges = adjacentEdges;
        } else {
            out.upperLargestCluster = largest;
            out.upperClusterWidth = width;
            out.upperClusterDepth = depth;
            out.upperOccupiedRows = occupiedRows;
            out.upperOccupiedCols = occupiedCols;
            out.upperQuadrants = quadrants;
            out.upperAdjacentEdges = adjacentEdges;
        }
    }

    private static int addProgressiveNeighbor(
            boolean[] grid,
            boolean[] visited,
            int[] queue,
            int tail,
            int n,
            int x,
            int z
    ) {
        int index = x * n + z;
        if (!grid[index] || visited[index]) return tail;
        visited[index] = true;
        queue[tail++] = index;
        return tail;
    }

    private static int[] progressiveStage0AxisForTier(int tierColumns) {
        if (tierColumns == 16) {
            return new int[] {0, 5, 10, 15};
        }
        if (tierColumns == 64) {
            return new int[] {0, 2, 5, 7, 8, 10, 13, 15};
        }
        throw new IllegalArgumentException("unsupported P20 progressive tier columns: " + tierColumns);
    }

    /**
     * P16 exact candidate-column completion.
     *
     * A Y88+ re-entry is impossible in a column that has no positive density at
     * Y88 or above. P15 already generated the complete upper slice for every
     * Stage0 column, so P16 builds one lower-Y mask only for columns that can still
     * satisfy the high gate. Lower samples for impossible columns are never
     * generated.
     *
     * The returned highCount is exact up to the configured early-stop threshold.
     * lowCount is only decision-equivalent: any high-gate passer is automatically
     * a low-gate passer because highMinUpperYIndex >= lowMinUpperYIndex.
     */
    public SparseGateCounts completeHighSparseReentryAfterUpperScoutCandidateColumns(
            int chunkRadius,
            int step,
            int lowMinUpperYIndex,
            int lowSamplesNeeded,
            int highMinUpperYIndex,
            int highSamplesNeeded
    ) {
        if (step < 1) step = 1;
        lowMinUpperYIndex = Math.max(0, Math.min(16, lowMinUpperYIndex));
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        lowSamplesNeeded = Math.max(1, lowSamplesNeeded);
        highSamplesNeeded = Math.max(1, highSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;
        int columns = sampleSize * sampleSize;

        if (this.stage0RequestedActiveY == null || this.stage0RequestedActiveY.length < columns) {
            this.stage0RequestedActiveY = new int[columns];
        }

        int lowerMask = highMinUpperYIndex == 0 ? 0 : ((1 << highMinUpperYIndex) - 1);
        int candidateColumns = 0;
        for (int column = 0; column < columns; column++) {
            int offset = column * 17;
            boolean upperPositive = false;
            for (int y = highMinUpperYIndex; y < 17; y++) {
                if (this.stage0ScoutDensity[offset + y] > 0.0D) {
                    upperPositive = true;
                    break;
                }
            }
            this.stage0RequestedActiveY[column] = upperPositive ? lowerMask : 0;
            if (upperPositive) candidateColumns++;
        }
        this.stage0LowerCandidateColumns = candidateColumns;

        if (lowerMask != 0 && candidateColumns > 0) {
            generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
                    this.stage0ScoutDensity,
                    fromCoarseX,
                    0,
                    fromCoarseZ,
                    sampleSize,
                    17,
                    sampleSize,
                    step,
                    step,
                    this.stage0RequestedActiveY,
                    this.stage0SampleTemperature,
                    this.stage0SampleRain
            );
        }

        int highCount = 0;
        for (int column = 0; column < columns; column++) {
            if (this.stage0RequestedActiveY[column] == 0) continue;
            int highestReentryY = highestSparseReentryYIndex(this.stage0ScoutDensity, column * 17);
            if (highestReentryY >= highMinUpperYIndex && ++highCount >= highSamplesNeeded) {
                return new SparseGateCounts(lowSamplesNeeded, highCount);
            }
        }

        return new SparseGateCounts(Math.min(highCount, lowSamplesNeeded), highCount);
    }

    /**
     * Shadow-research helper: after P16 has filled the exact lower slices for all
     * possible Y88+ candidate columns, count every high re-entry instead of
     * stopping at the production threshold. No new terrain noise is generated.
     */
    public int countPreparedHighSparseReentryColumns(int highMinUpperYIndex) {
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        if (this.stage0RequestedActiveY == null || this.stage0ScoutDensity == null) return 0;

        int columns = Math.min(this.stage0RequestedActiveY.length, this.stage0ScoutDensity.length / 17);
        int highCount = 0;
        for (int column = 0; column < columns; column++) {
            if (this.stage0RequestedActiveY[column] == 0) continue;
            int highestReentryY = highestSparseReentryYIndex(this.stage0ScoutDensity, column * 17);
            if (highestReentryY >= highMinUpperYIndex) highCount++;
        }
        return highCount;
    }

    /** Number of columns whose lower Y slice P16 generated for the last scout survivor. */
    public int getLastStage0LowerCandidateColumns() {
        return this.stage0LowerCandidateColumns;
    }

    /**
     * P19 Stage0.75 features, derived only from the exact P15/P16 density cache.
     *
     * No new terrain/climate noise is generated here. P15 has already filled Y88+
     * for every sparse column, and P16 has filled lower Y for every column that can
     * possibly contain a Y88+ re-entry. Columns outside that candidate set are
     * mathematically incapable of contributing to these features.
     */
    public PreparedStage0MonsterFeatures analyzePreparedStage0MonsterFeatures(
            int chunkRadius,
            int step
    ) {
        if (step < 1) step = 1;

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int sampleSize = (coarseSize + step - 1) / step;
        int columns = sampleSize * sampleSize;

        ensureStage0MonsterScratch(columns);
        Arrays.fill(this.stage0MonsterY88Grid, 0, columns, false);
        Arrays.fill(this.stage0MonsterY96Grid, 0, columns, false);

        PreparedStage0MonsterFeatures stats = new PreparedStage0MonsterFeatures();
        stats.sampleSize = sampleSize;
        stats.step = step;

        for (int column = 0; column < columns; column++) {
            if (this.stage0RequestedActiveY[column] == 0) continue;

            int highestReentryY = highestSparseReentryYIndex(this.stage0ScoutDensity, column * 17);
            if (highestReentryY >= 11) {
                stats.stage0FullY88++;
                this.stage0MonsterY88Grid[column] = true;
            }
            if (highestReentryY >= 12) {
                stats.stage0FullY96++;
                this.stage0MonsterY96Grid[column] = true;
            }
            if (highestReentryY >= 13) stats.stage0FullY104++;
            if (highestReentryY >= 14) stats.stage0FullY112++;
        }

        fillPreparedStage0ShapeStats(
                this.stage0MonsterY88Grid,
                sampleSize,
                stats,
                true
        );
        fillPreparedStage0ShapeStats(
                this.stage0MonsterY96Grid,
                sampleSize,
                stats,
                false
        );

        return stats;
    }

    private void ensureStage0MonsterScratch(int columns) {
        if (this.stage0MonsterY88Grid == null || this.stage0MonsterY88Grid.length < columns) {
            this.stage0MonsterY88Grid = new boolean[columns];
            this.stage0MonsterY96Grid = new boolean[columns];
            this.stage0MonsterVisited = new boolean[columns];
            this.stage0MonsterQueue = new int[columns];
        }
    }

    private void fillPreparedStage0ShapeStats(
            boolean[] grid,
            int size,
            PreparedStage0MonsterFeatures out,
            boolean y88
    ) {
        int total = size * size;
        Arrays.fill(this.stage0MonsterVisited, 0, total, false);

        int largestCluster = 0;
        int minX = Integer.MAX_VALUE;
        int maxX = Integer.MIN_VALUE;
        int minZ = Integer.MAX_VALUE;
        int maxZ = Integer.MIN_VALUE;
        boolean touchesBorder = false;

        for (int x = 0; x < size; x++) {
            for (int z = 0; z < size; z++) {
                int index = x * size + z;
                if (!grid[index]) continue;

                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (z < minZ) minZ = z;
                if (z > maxZ) maxZ = z;
                if (x == 0 || z == 0 || x == size - 1 || z == size - 1) {
                    touchesBorder = true;
                }

                if (this.stage0MonsterVisited[index]) continue;

                int head = 0;
                int tail = 0;
                int cluster = 0;
                this.stage0MonsterVisited[index] = true;
                this.stage0MonsterQueue[tail++] = index;

                while (head < tail) {
                    int current = this.stage0MonsterQueue[head++];
                    cluster++;
                    int cx = current / size;
                    int cz = current % size;
                    tail = tryAddSparseNeighbor(
                            grid, this.stage0MonsterVisited, this.stage0MonsterQueue, tail, size, cx - 1, cz
                    );
                    tail = tryAddSparseNeighbor(
                            grid, this.stage0MonsterVisited, this.stage0MonsterQueue, tail, size, cx + 1, cz
                    );
                    tail = tryAddSparseNeighbor(
                            grid, this.stage0MonsterVisited, this.stage0MonsterQueue, tail, size, cx, cz - 1
                    );
                    tail = tryAddSparseNeighbor(
                            grid, this.stage0MonsterVisited, this.stage0MonsterQueue, tail, size, cx, cz + 1
                    );
                }

                if (cluster > largestCluster) largestCluster = cluster;
            }
        }

        int width = largestCluster == 0 ? 0 : maxX - minX + 1;
        int depth = largestCluster == 0 ? 0 : maxZ - minZ + 1;
        if (y88) {
            out.stage0Y88LargestCluster = largestCluster;
            out.stage0Y88Width = width;
            out.stage0Y88Depth = depth;
            out.stage0Y88TouchesBorder = touchesBorder;
        } else {
            out.stage0Y96LargestCluster = largestCluster;
            out.stage0Y96Width = width;
            out.stage0Y96Depth = depth;
            out.stage0Y96TouchesBorder = touchesBorder;
        }
    }

    /**
     * Completes exact Stage0 after a successful upper scout by generating only the
     * lower Y samples that the scout skipped. The upper samples, climate, and 2D
     * terrain-shape noise are reused, so survivors never regenerate the upper slice.
     */
    public SparseGateCounts completeSparseCoarseReentrySamplesAroundZeroCombinedAfterUpperScout(
            int chunkRadius,
            int step,
            int lowMinUpperYIndex,
            int lowSamplesNeeded,
            int highMinUpperYIndex,
            int highSamplesNeeded
    ) {
        if (step < 1) step = 1;
        lowMinUpperYIndex = Math.max(0, Math.min(16, lowMinUpperYIndex));
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        lowSamplesNeeded = Math.max(1, lowSamplesNeeded);
        highSamplesNeeded = Math.max(1, highSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;

        int lowerMask = highMinUpperYIndex == 0 ? 0 : ((1 << highMinUpperYIndex) - 1);
        if (lowerMask != 0) {
            generateTerrainNoiseForSparseStage0YMaskCachedAxes(
                    this.stage0ScoutDensity,
                    fromCoarseX,
                    0,
                    fromCoarseZ,
                    sampleSize,
                    17,
                    sampleSize,
                    step,
                    step,
                    lowerMask,
                    this.stage0SampleTemperature,
                    this.stage0SampleRain
            );
        }

        int lowCount = 0;
        int highCount = 0;
        int columns = sampleSize * sampleSize;
        for (int column = 0; column < columns; column++) {
            int highestReentryY = highestSparseReentryYIndex(this.stage0ScoutDensity, column * 17);
            if (highestReentryY >= lowMinUpperYIndex) lowCount++;
            if (highestReentryY >= highMinUpperYIndex) highCount++;

            if (lowCount >= lowSamplesNeeded && highCount >= highSamplesNeeded) {
                return new SparseGateCounts(lowCount, highCount);
            }
        }
        return new SparseGateCounts(lowCount, highCount);
    }

    /** Copies the full-layout scout buffer for strict raw-bit equivalence tests. */
    public void copyPreparedStage0ScoutDensityInto(double[] out, int chunkRadius, int step) {
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int sampleSize = (coarseSize + Math.max(1, step) - 1) / Math.max(1, step);
        int required = sampleSize * sampleSize * 17;
        if (out == null || out.length < required) {
            throw new IllegalArgumentException("out buffer too small for prepared Stage0 scout density");
        }
        System.arraycopy(this.stage0ScoutDensity, 0, out, 0, required);
    }

    /**
     * Patch-5 per-column Stage0 path retained only for equivalence testing.
     */
    public SparseGateCounts countSparseCoarseReentrySamplesAroundZeroCombinedLegacy(
            int chunkRadius,
            int step,
            int lowMinUpperYIndex,
            int lowSamplesNeeded,
            int highMinUpperYIndex,
            int highSamplesNeeded,
            double[] columnBuffer
    ) {
        if (columnBuffer == null || columnBuffer.length < 17) {
            throw new IllegalArgumentException("columnBuffer must have length at least 17");
        }

        if (step < 1) {
            step = 1;
        }

        lowMinUpperYIndex = Math.max(0, Math.min(16, lowMinUpperYIndex));
        highMinUpperYIndex = Math.max(0, Math.min(16, highMinUpperYIndex));
        lowSamplesNeeded = Math.max(1, lowSamplesNeeded);
        highSamplesNeeded = Math.max(1, highSamplesNeeded);

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);

        int lowCount = 0;
        int highCount = 0;
        int sampleX = 0;

        for (int localX = 0; localX < coarseSize; localX += step) {
            int coarseX = fromCoarseX + localX;
            int sampleZ = 0;

            for (int localZ = 0; localZ < coarseSize; localZ += step) {
                int coarseZ = fromCoarseZ + localZ;
                int climateIndex = sampleX * sampleSize + sampleZ;

                samplePreparedCoarseDensityColumnInto(
                        columnBuffer,
                        coarseX,
                        coarseZ,
                        this.stage0SampleTemperature[climateIndex],
                        this.stage0SampleRain[climateIndex]
                );

                int highestReentryY = highestSparseReentryYIndex(columnBuffer);
                if (highestReentryY >= lowMinUpperYIndex) {
                    lowCount++;
                }
                if (highestReentryY >= highMinUpperYIndex) {
                    highCount++;
                }

                if (lowCount >= lowSamplesNeeded && highCount >= highSamplesNeeded) {
                    return new SparseGateCounts(lowCount, highCount);
                }

                sampleZ++;
            }
            sampleX++;
        }

        return new SparseGateCounts(lowCount, highCount);
    }

    /**
     * Generates the complete sparse Stage0 density lattice through the old
     * one-column-at-a-time path. Used only by Stage0SparseBatchCheck.
     */
    public void generateStage0SparseDensityGridAroundZeroLegacyInto(
            double[] outValues,
            int chunkRadius,
            int step
    ) {
        if (step < 1) step = 1;
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;
        int required = sampleSize * sampleSize * 17;
        if (outValues == null || outValues.length < required) {
            throw new IllegalArgumentException("outValues buffer too small for sparse Stage0 grid");
        }

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);
        double[] column = new double[17];
        int out = 0;
        int sampleX = 0;
        for (int localX = 0; localX < coarseSize; localX += step) {
            int coarseX = fromCoarseX + localX;
            int sampleZ = 0;
            for (int localZ = 0; localZ < coarseSize; localZ += step) {
                int coarseZ = fromCoarseZ + localZ;
                int climateIndex = sampleX * sampleSize + sampleZ;
                samplePreparedCoarseDensityColumnInto(
                        column,
                        coarseX,
                        coarseZ,
                        this.stage0SampleTemperature[climateIndex],
                        this.stage0SampleRain[climateIndex]
                );
                System.arraycopy(column, 0, outValues, out, 17);
                out += 17;
                sampleZ++;
            }
            sampleX++;
        }
    }

    /**
     * Generates the same sparse Stage0 density lattice in one strided cached-axis
     * noise batch. Output order is X, Z, then Y, matching the legacy sampler.
     */
    public void generateStage0SparseDensityGridAroundZeroBatchedInto(
            double[] outValues,
            int chunkRadius,
            int step
    ) {
        if (step < 1) step = 1;
        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;
        int required = sampleSize * sampleSize * 17;
        if (outValues == null || outValues.length < required) {
            throw new IllegalArgumentException("outValues buffer too small for sparse Stage0 grid");
        }

        prepareSparseStage0Climate(fromCoarseX, fromCoarseZ, step, sampleSize);
        generateTerrainNoiseForSparseStage0BatchCachedAxes(
                outValues,
                fromCoarseX,
                0,
                fromCoarseZ,
                sampleSize,
                17,
                sampleSize,
                step,
                step,
                this.stage0SampleTemperature,
                this.stage0SampleRain
        );
    }

    /**
     * Full sparse Stage0 diagnostics for filter research.
     *
     * Unlike countSparseCoarseReentrySamplesAroundZero(...), this does not stop
     * early. It samples every sparse coarse column once and returns counts for
     * several upper Y cutoffs plus simple 2D shape stats for Y88/Y96 hits.
     */
    public SparseReentryStats analyzeSparseCoarseReentryAroundZero(int chunkRadius, int step) {
        if (step < 1) {
            step = 1;
        }

        int chunkCount = chunkRadius * 2 + 1;
        int coarseSize = chunkCount * 4 + 1;
        int fromCoarseX = -chunkRadius * 4;
        int fromCoarseZ = -chunkRadius * 4;
        int sampleSize = (coarseSize + step - 1) / step;

        SparseReentryStats stats = new SparseReentryStats();
        stats.sampleSize = sampleSize;
        stats.step = step;

        boolean[] y88Grid = new boolean[sampleSize * sampleSize];
        boolean[] y96Grid = new boolean[sampleSize * sampleSize];
        double[] columnBuffer = new double[17];

        prepareCoarseGridClimate(fromCoarseX, fromCoarseZ, coarseSize, coarseSize);

        int sx = 0;
        for (int localX = 0; localX < coarseSize; localX += step) {
            int coarseX = fromCoarseX + localX;

            int sz = 0;
            for (int localZ = 0; localZ < coarseSize; localZ += step) {
                int coarseZ = fromCoarseZ + localZ;
                int climateIndex = localX * coarseSize + localZ;

                samplePreparedCoarseDensityColumnInto(
                        columnBuffer,
                        coarseX,
                        coarseZ,
                        this.coarseGridTemperature[climateIndex],
                        this.coarseGridRain[climateIndex]
                );

                if (hasSparseReentryPattern(columnBuffer, 8)) stats.stage0FullY64++;
                if (hasSparseReentryPattern(columnBuffer, 9)) stats.stage0FullY72++;
                if (hasSparseReentryPattern(columnBuffer, 10)) stats.stage0FullY80++;

                boolean y88 = hasSparseReentryPattern(columnBuffer, 11);
                boolean y96 = hasSparseReentryPattern(columnBuffer, 12);

                if (y88) {
                    stats.stage0FullY88++;
                    y88Grid[sx * sampleSize + sz] = true;
                }
                if (y96) {
                    stats.stage0FullY96++;
                    y96Grid[sx * sampleSize + sz] = true;
                }

                if (hasSparseReentryPattern(columnBuffer, 13)) stats.stage0FullY104++;
                if (hasSparseReentryPattern(columnBuffer, 14)) stats.stage0FullY112++;

                sz++;
            }
            sx++;
        }

        int[] y88Shape = sparseShapeStats(y88Grid, sampleSize);
        stats.stage0Y88LargestCluster = y88Shape[0];
        stats.stage0Y88Width = y88Shape[1];
        stats.stage0Y88Depth = y88Shape[2];
        stats.stage0Y88TouchesBorder = y88Shape[3] != 0;

        int[] y96Shape = sparseShapeStats(y96Grid, sampleSize);
        stats.stage0Y96LargestCluster = y96Shape[0];
        stats.stage0Y96Width = y96Shape[1];
        stats.stage0Y96Depth = y96Shape[2];
        stats.stage0Y96TouchesBorder = y96Shape[3] != 0;

        return stats;
    }

    private static int[] sparseShapeStats(boolean[] grid, int size) {
        int total = size * size;
        boolean[] visited = new boolean[total];
        int[] queue = new int[total];

        int largestCluster = 0;
        int minX = Integer.MAX_VALUE;
        int maxX = Integer.MIN_VALUE;
        int minZ = Integer.MAX_VALUE;
        int maxZ = Integer.MIN_VALUE;
        boolean touchesBorder = false;

        for (int x = 0; x < size; x++) {
            for (int z = 0; z < size; z++) {
                int index = x * size + z;
                if (!grid[index]) {
                    continue;
                }

                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (z < minZ) minZ = z;
                if (z > maxZ) maxZ = z;

                if (x == 0 || z == 0 || x == size - 1 || z == size - 1) {
                    touchesBorder = true;
                }

                if (visited[index]) {
                    continue;
                }

                int head = 0;
                int tail = 0;
                int cluster = 0;
                visited[index] = true;
                queue[tail++] = index;

                while (head < tail) {
                    int current = queue[head++];
                    cluster++;

                    int cx = current / size;
                    int cz = current % size;

                    tail = tryAddSparseNeighbor(grid, visited, queue, tail, size, cx - 1, cz);
                    tail = tryAddSparseNeighbor(grid, visited, queue, tail, size, cx + 1, cz);
                    tail = tryAddSparseNeighbor(grid, visited, queue, tail, size, cx, cz - 1);
                    tail = tryAddSparseNeighbor(grid, visited, queue, tail, size, cx, cz + 1);
                }

                if (cluster > largestCluster) {
                    largestCluster = cluster;
                }
            }
        }

        int width = largestCluster == 0 ? 0 : maxX - minX + 1;
        int depth = largestCluster == 0 ? 0 : maxZ - minZ + 1;

        return new int[] {largestCluster, width, depth, touchesBorder ? 1 : 0};
    }

    private static int tryAddSparseNeighbor(
            boolean[] grid,
            boolean[] visited,
            int[] queue,
            int tail,
            int size,
            int x,
            int z
    ) {
        if (x < 0 || z < 0 || x >= size || z >= size) {
            return tail;
        }

        int index = x * size + z;
        if (!grid[index] || visited[index]) {
            return tail;
        }

        visited[index] = true;
        queue[tail++] = index;
        return tail;
    }

    private void samplePreparedCoarseDensityColumnInto(
            double[] outColumn,
            int coarseX,
            int coarseZ,
            double temperature,
            double rain
    ) {
        if (this.sparseCoarseTemperature == null) {
            this.sparseCoarseTemperature = new double[1];
        }
        if (this.sparseCoarseRain == null) {
            this.sparseCoarseRain = new double[1];
        }

        this.sparseCoarseTemperature[0] = temperature;
        this.sparseCoarseRain[0] = rain;

        generateTerrainNoiseForCoarseGrid(
                outColumn,
                coarseX,
                0,
                coarseZ,
                1,
                17,
                1,
                this.sparseCoarseTemperature,
                this.sparseCoarseRain
        );
    }


    private static int highestSparseReentryYIndex(double[] column) {
        return highestSparseReentryYIndex(column, 0);
    }

    private static int highestSparseReentryYIndex(double[] values, int offset) {
        boolean sawPositive = false;
        boolean sawGapAfterPositive = false;
        int highestReentryY = -1;

        for (int y = 0; y < 17; y++) {
            if (values[offset + y] > 0.0D) {
                if (sawPositive && sawGapAfterPositive) highestReentryY = y;
                sawPositive = true;
            } else if (sawPositive) {
                sawGapAfterPositive = true;
            }
        }
        return highestReentryY;
    }

    private static boolean hasSparseReentryPattern(double[] column, int minUpperYIndex) {
        boolean sawPositive = false;
        boolean sawGapAfterPositive = false;

        for (int y = 0; y < 17; y++) {
            if (column[y] > 0.0D) {
                if (sawPositive && sawGapAfterPositive && y >= minUpperYIndex) {
                    return true;
                }
                sawPositive = true;
            } else if (sawPositive) {
                sawGapAfterPositive = true;
            }
        }

        return false;
    }

    private void prepareSparseStage0Climate(
            int fromCoarseX,
            int fromCoarseZ,
            int step,
            int sampleSize
    ) {
        int total = sampleSize * sampleSize;
        if (this.stage0SampleTemperature == null || this.stage0SampleTemperature.length < total) {
            this.stage0SampleTemperature = new double[total];
        }
        if (this.stage0SampleRain == null || this.stage0SampleRain.length < total) {
            this.stage0SampleRain = new double[total];
        }

        int climateStartX = fromCoarseX * 4 + 2;
        int climateStartZ = fromCoarseZ * 4 + 2;
        int climateStride = step * 4;

        this.worldChunkManager.getClimateNoiseStrided(
                this.stage0SampleTemperature,
                this.stage0SampleRain,
                climateStartX,
                climateStartZ,
                sampleSize,
                sampleSize,
                climateStride,
                climateStride
        );
    }

    /**
     * Prepares exactly the climate samples consumed by the coarse density grid.
     *
     * The old path generated a dense block-climate square, then kept only every
     * fourth point in X/Z. At radius 7 that meant generating 241x241 = 58,081
     * climate points to use only 61x61 = 3,721. The strided sampler evaluates the
     * exact same block coordinates directly.
     */
    private void prepareCoarseGridClimate(int fromCoarseX, int fromCoarseZ, int xLen, int zLen) {
        int coarseTotal = xLen * zLen;
        if (this.coarseGridTemperature == null || this.coarseGridTemperature.length < coarseTotal) {
            this.coarseGridTemperature = new double[coarseTotal];
        }
        if (this.coarseGridRain == null || this.coarseGridRain.length < coarseTotal) {
            this.coarseGridRain = new double[coarseTotal];
        }

        int climateStartX = fromCoarseX * 4 + 2;
        int climateStartZ = fromCoarseZ * 4 + 2;

        this.worldChunkManager.getClimateNoiseStrided(
                this.coarseGridTemperature,
                this.coarseGridRain,
                climateStartX,
                climateStartZ,
                xLen,
                zLen,
                4,
                4
        );
    }

    /**
     * Legacy dense climate path retained only for equivalence testing.
     */
    private void prepareCoarseGridClimateDenseLegacy(int fromCoarseX, int fromCoarseZ, int xLen, int zLen) {
        int climateStartX = fromCoarseX * 4 + 2;
        int climateStartZ = fromCoarseZ * 4 + 2;
        int climateSizeX = (xLen - 1) * 4 + 1;
        int climateSizeZ = (zLen - 1) * 4 + 1;

        this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                this.biomeNoiseCache,
                climateStartX,
                climateStartZ,
                climateSizeX,
                climateSizeZ
        );

        int coarseTotal = xLen * zLen;
        if (this.coarseGridTemperature == null || this.coarseGridTemperature.length < coarseTotal) {
            this.coarseGridTemperature = new double[coarseTotal];
        }
        if (this.coarseGridRain == null || this.coarseGridRain.length < coarseTotal) {
            this.coarseGridRain = new double[coarseTotal];
        }

        double[] temp = this.worldChunkManager.temperature;
        double[] rain = this.worldChunkManager.rain;

        int out = 0;
        for (int x = 0; x < xLen; x++) {
            int bx = x * 4;
            for (int z = 0; z < zLen; z++) {
                int bz = z * 4;
                int climateIndex = bx * climateSizeZ + bz;
                this.coarseGridTemperature[out] = temp[climateIndex];
                this.coarseGridRain[out] = rain[climateIndex];
                out++;
            }
        }
    }


    private byte[] buildTerrainBlendMask(int requiredLength) {
        if (this.terrainBlendMask == null || this.terrainBlendMask.length < requiredLength) {
            this.terrainBlendMask = new byte[requiredLength];
        }
        for (int i = 0; i < requiredLength; i++) {
            double blend = (this.terrainNoise1[i] / 10.0D + 1.0D) / 2.0D;
            this.terrainBlendMask[i] = (byte) (blend < 0.0D ? 1 : (blend > 1.0D ? 2 : 3));
        }
        return this.terrainBlendMask;
    }

    private void prepareSparseStage0ShapeNoise(
            int fromX,
            int fromZ,
            int xLen,
            int zLen,
            int xStep,
            int zStep
    ) {
        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise2DStridedCachedAxes(
                this.terrainNoise4,
                (double) fromX,
                (double) fromZ,
                xLen,
                zLen,
                (double) xStep,
                (double) zStep,
                1.121D,
                1.121D
        );
        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise2DStridedCachedAxes(
                this.terrainNoise5,
                (double) fromX,
                (double) fromZ,
                xLen,
                zLen,
                (double) xStep,
                (double) zStep,
                200.0D,
                200.0D
        );
    }

    private byte[] buildTerrainBlendMaskAndActiveYForYMask(int xLen, int yLen, int zLen, int requestedYMask) {
        int requiredLength = xLen * yLen * zLen;
        int columns = xLen * zLen;
        if (yLen > 32) throw new IllegalArgumentException("active-Y mask supports at most 32 Y samples");
        if (this.terrainBlendMask == null || this.terrainBlendMask.length < requiredLength) {
            this.terrainBlendMask = new byte[requiredLength];
        }
        if (this.terrainBlendNoise2ActiveY == null || this.terrainBlendNoise2ActiveY.length < columns) {
            this.terrainBlendNoise2ActiveY = new int[columns];
        }
        if (this.terrainBlendNoise3ActiveY == null || this.terrainBlendNoise3ActiveY.length < columns) {
            this.terrainBlendNoise3ActiveY = new int[columns];
        }

        for (int column = 0; column < columns; column++) {
            int active2 = 0;
            int active3 = 0;
            int active = requestedYMask;
            int base = column * yLen;
            while (active != 0) {
                int y = Integer.numberOfTrailingZeros(active);
                active &= active - 1;
                int index = base + y;
                double blend = (this.terrainNoise1[index] / 10.0D + 1.0D) / 2.0D;
                int blendCase = blend < 0.0D ? 1 : (blend > 1.0D ? 2 : 3);
                this.terrainBlendMask[index] = (byte) blendCase;
                int yBit = 1 << y;
                if ((blendCase & 1) != 0) active2 |= yBit;
                if ((blendCase & 2) != 0) active3 |= yBit;
            }
            this.terrainBlendNoise2ActiveY[column] = active2;
            this.terrainBlendNoise3ActiveY[column] = active3;
        }
        return this.terrainBlendMask;
    }

    private byte[] buildTerrainBlendMaskAndActiveYForColumnMasks(
            int xLen, int yLen, int zLen, int[] requestedYByColumn
    ) {
        int requiredLength = xLen * yLen * zLen;
        int columns = xLen * zLen;
        if (yLen > 32) throw new IllegalArgumentException("active-Y mask supports at most 32 Y samples");
        if (requestedYByColumn == null || requestedYByColumn.length < columns) {
            throw new IllegalArgumentException("requestedYByColumn buffer too small");
        }
        if (this.terrainBlendMask == null || this.terrainBlendMask.length < requiredLength) {
            this.terrainBlendMask = new byte[requiredLength];
        }
        if (this.terrainBlendNoise2ActiveY == null || this.terrainBlendNoise2ActiveY.length < columns) {
            this.terrainBlendNoise2ActiveY = new int[columns];
        }
        if (this.terrainBlendNoise3ActiveY == null || this.terrainBlendNoise3ActiveY.length < columns) {
            this.terrainBlendNoise3ActiveY = new int[columns];
        }

        for (int column = 0; column < columns; column++) {
            int active2 = 0;
            int active3 = 0;
            int active = requestedYByColumn[column];
            int base = column * yLen;
            while (active != 0) {
                int y = Integer.numberOfTrailingZeros(active);
                active &= active - 1;
                int index = base + y;
                double blend = (this.terrainNoise1[index] / 10.0D + 1.0D) / 2.0D;
                int blendCase = blend < 0.0D ? 1 : (blend > 1.0D ? 2 : 3);
                this.terrainBlendMask[index] = (byte) blendCase;
                int yBit = 1 << y;
                if ((blendCase & 1) != 0) active2 |= yBit;
                if ((blendCase & 2) != 0) active3 |= yBit;
            }
            this.terrainBlendNoise2ActiveY[column] = active2;
            this.terrainBlendNoise3ActiveY[column] = active3;
        }
        return this.terrainBlendMask;
    }

    private double[] generateTerrainNoiseForSparseStage0ColumnYMasksCachedAxes(
            double[] noise,
            int fromX,
            int fromY,
            int fromZ,
            int xLen,
            int yLen,
            int zLen,
            int xStep,
            int zStep,
            int[] requestedYByColumn,
            double[] temperatures,
            double[] rain
    ) {
        int required = xLen * yLen * zLen;
        if (noise == null || noise.length < required) {
            noise = new double[required];
        }
        int columns = xLen * zLen;
        if (requestedYByColumn == null || requestedYByColumn.length < columns) {
            throw new IllegalArgumentException("requestedYByColumn buffer too small");
        }

        double d0 = 684.412D;
        double d1 = 684.412D;

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise1,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0 / 80.0D,
                d1 / 160.0D,
                d0 / 80.0D,
                requestedYByColumn
        );
        byte[] blendMask = buildTerrainBlendMaskAndActiveYForColumnMasks(xLen, yLen, zLen, requestedYByColumn);
        this.terrainNoise2 = this.terrainNoise2Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise2,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise2ActiveY
        );
        this.terrainNoise3 = this.terrainNoise3Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise3,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise3ActiveY
        );

        int climateIndex = 0;
        int shapeIndex = 0;
        for (int x = 0; x < xLen; x++) {
            for (int z = 0; z < zLen; z++) {
                int column = x * zLen + z;
                int active = requestedYByColumn[column];
                if (active == 0) {
                    climateIndex++;
                    shapeIndex++;
                    continue;
                }

                double d2 = temperatures[climateIndex];
                double d3 = rain[climateIndex] * d2;
                climateIndex++;

                double d4 = 1.0D - d3;
                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[shapeIndex] + 256.0D) / 512.0D;
                d5 *= d4;
                if (d5 > 1.0D) d5 = 1.0D;

                double d6 = this.terrainNoise5[shapeIndex] / 8000.0D;
                if (d6 < 0.0D) d6 = -d6 * 0.3D;
                d6 = d6 * 3.0D - 2.0D;
                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) d6 = -1.0D;
                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) d6 = 1.0D;
                    d6 /= 8.0D;
                }
                if (d5 < 0.0D) d5 = 0.0D;
                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;
                double d7 = (double) yLen / 2.0D + d6 * 4.0D;
                shapeIndex++;

                int base = column * yLen;
                while (active != 0) {
                    int y = Integer.numberOfTrailingZeros(active);
                    active &= active - 1;
                    int densityIndex = base + y;

                    double d9 = ((double) y - d7) * 12.0D / d5;
                    if (d9 < 0.0D) d9 *= 4.0D;

                    int blendCase = blendMask[densityIndex];
                    double d8;
                    if (blendCase == 1) {
                        d8 = this.terrainNoise2[densityIndex] / 512.0D;
                    } else if (blendCase == 2) {
                        d8 = this.terrainNoise3[densityIndex] / 512.0D;
                    } else {
                        double d10 = this.terrainNoise2[densityIndex] / 512.0D;
                        double d11 = this.terrainNoise3[densityIndex] / 512.0D;
                        double d12 = (this.terrainNoise1[densityIndex] / 10.0D + 1.0D) / 2.0D;
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;
                    if (y > yLen - 4) {
                        double d13 = (double) ((float) (y - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }
                    noise[densityIndex] = d8;
                }
            }
        }
        return noise;
    }

    private double[] generateTerrainNoiseForSparseStage0YMaskCachedAxes(
            double[] noise,
            int fromX,
            int fromY,
            int fromZ,
            int xLen,
            int yLen,
            int zLen,
            int xStep,
            int zStep,
            int requestedYMask,
            double[] temperatures,
            double[] rain
    ) {
        int required = xLen * yLen * zLen;
        if (noise == null || noise.length < required) {
            noise = new double[required];
        }
        int columns = xLen * zLen;
        if (this.stage0RequestedActiveY == null || this.stage0RequestedActiveY.length < columns) {
            this.stage0RequestedActiveY = new int[columns];
        }
        Arrays.fill(this.stage0RequestedActiveY, 0, columns, requestedYMask);

        double d0 = 684.412D;
        double d1 = 684.412D;

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise1,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0 / 80.0D,
                d1 / 160.0D,
                d0 / 80.0D,
                this.stage0RequestedActiveY
        );
        byte[] blendMask = buildTerrainBlendMaskAndActiveYForYMask(xLen, yLen, zLen, requestedYMask);
        this.terrainNoise2 = this.terrainNoise2Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise2,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise2ActiveY
        );
        this.terrainNoise3 = this.terrainNoise3Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise3,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise3ActiveY
        );

        int climateIndex = 0;
        int shapeIndex = 0;
        for (int x = 0; x < xLen; x++) {
            for (int z = 0; z < zLen; z++) {
                double d2 = temperatures[climateIndex];
                double d3 = rain[climateIndex] * d2;
                climateIndex++;

                double d4 = 1.0D - d3;
                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[shapeIndex] + 256.0D) / 512.0D;
                d5 *= d4;
                if (d5 > 1.0D) d5 = 1.0D;

                double d6 = this.terrainNoise5[shapeIndex] / 8000.0D;
                if (d6 < 0.0D) d6 = -d6 * 0.3D;
                d6 = d6 * 3.0D - 2.0D;
                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) d6 = -1.0D;
                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) d6 = 1.0D;
                    d6 /= 8.0D;
                }
                if (d5 < 0.0D) d5 = 0.0D;
                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;
                double d7 = (double) yLen / 2.0D + d6 * 4.0D;
                shapeIndex++;

                int base = (x * zLen + z) * yLen;
                int active = requestedYMask;
                while (active != 0) {
                    int y = Integer.numberOfTrailingZeros(active);
                    active &= active - 1;
                    int densityIndex = base + y;

                    double d9 = ((double) y - d7) * 12.0D / d5;
                    if (d9 < 0.0D) d9 *= 4.0D;

                    int blendCase = blendMask[densityIndex];
                    double d8;
                    if (blendCase == 1) {
                        d8 = this.terrainNoise2[densityIndex] / 512.0D;
                    } else if (blendCase == 2) {
                        d8 = this.terrainNoise3[densityIndex] / 512.0D;
                    } else {
                        double d10 = this.terrainNoise2[densityIndex] / 512.0D;
                        double d11 = this.terrainNoise3[densityIndex] / 512.0D;
                        double d12 = (this.terrainNoise1[densityIndex] / 10.0D + 1.0D) / 2.0D;
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;
                    if (y > yLen - 4) {
                        double d13 = (double) ((float) (y - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }
                    noise[densityIndex] = d8;
                }
            }
        }
        return noise;
    }

    /**
     * Patch 12: compile the point blend mask into one 17-bit active-Y mask per X/Z column.
     * The final byte mask is retained for exact blend selection, while the Perlin octave
     * loops can jump directly between needed Y samples instead of checking every point.
     */
    private byte[] buildTerrainBlendMaskAndActiveY(int xLen, int yLen, int zLen) {
        int requiredLength = xLen * yLen * zLen;
        int columns = xLen * zLen;
        if (yLen > 32) throw new IllegalArgumentException("active-Y mask supports at most 32 Y samples");
        if (this.terrainBlendMask == null || this.terrainBlendMask.length < requiredLength) {
            this.terrainBlendMask = new byte[requiredLength];
        }
        if (this.terrainBlendNoise2ActiveY == null || this.terrainBlendNoise2ActiveY.length < columns) {
            this.terrainBlendNoise2ActiveY = new int[columns];
        }
        if (this.terrainBlendNoise3ActiveY == null || this.terrainBlendNoise3ActiveY.length < columns) {
            this.terrainBlendNoise3ActiveY = new int[columns];
        }

        int index = 0;
        for (int column = 0; column < columns; column++) {
            int active2 = 0;
            int active3 = 0;
            for (int y = 0; y < yLen; y++) {
                double blend = (this.terrainNoise1[index] / 10.0D + 1.0D) / 2.0D;
                int blendCase = blend < 0.0D ? 1 : (blend > 1.0D ? 2 : 3);
                this.terrainBlendMask[index++] = (byte) blendCase;
                int yBit = 1 << y;
                if ((blendCase & 1) != 0) active2 |= yBit;
                if ((blendCase & 2) != 0) active3 |= yBit;
            }
            this.terrainBlendNoise2ActiveY[column] = active2;
            this.terrainBlendNoise3ActiveY[column] = active3;
        }
        return this.terrainBlendMask;
    }

    private double[] generateTerrainNoiseForSparseStage0BatchCachedAxes(
            double[] noise,
            int fromX,
            int fromY,
            int fromZ,
            int xLen,
            int yLen,
            int zLen,
            int xStep,
            int zStep,
            double[] temperatures,
            double[] rain
    ) {
        int required = xLen * yLen * zLen;
        if (noise == null || noise.length < required) {
            noise = new double[required];
        }

        double d0 = 684.412D;
        double d1 = 684.412D;

        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise2DStridedCachedAxes(
                this.terrainNoise4,
                (double) fromX,
                (double) fromZ,
                xLen,
                zLen,
                (double) xStep,
                (double) zStep,
                1.121D,
                1.121D
        );
        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise2DStridedCachedAxes(
                this.terrainNoise5,
                (double) fromX,
                (double) fromZ,
                xLen,
                zLen,
                (double) xStep,
                (double) zStep,
                200.0D,
                200.0D
        );

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoiseCachedAxesStrided(
                this.terrainNoise1,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0 / 80.0D,
                d1 / 160.0D,
                d0 / 80.0D
        );
        byte[] blendMask = buildTerrainBlendMaskAndActiveY(xLen, yLen, zLen);
        this.terrainNoise2 = this.terrainNoise2Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise2,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise2ActiveY
        );
        this.terrainNoise3 = this.terrainNoise3Generator.generateNoiseCachedAxesStridedActiveY(
                this.terrainNoise3,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                (double) xStep,
                1.0D,
                (double) zStep,
                d0,
                d1,
                d0,
                this.terrainBlendNoise3ActiveY
        );

        int densityIndex = 0;
        int climateIndex = 0;
        int shapeIndex = 0;
        for (int x = 0; x < xLen; x++) {
            for (int z = 0; z < zLen; z++) {
                double d2 = temperatures[climateIndex];
                double d3 = rain[climateIndex] * d2;
                climateIndex++;

                double d4 = 1.0D - d3;
                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[shapeIndex] + 256.0D) / 512.0D;
                d5 *= d4;
                if (d5 > 1.0D) d5 = 1.0D;

                double d6 = this.terrainNoise5[shapeIndex] / 8000.0D;
                if (d6 < 0.0D) d6 = -d6 * 0.3D;
                d6 = d6 * 3.0D - 2.0D;
                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) d6 = -1.0D;
                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) d6 = 1.0D;
                    d6 /= 8.0D;
                }
                if (d5 < 0.0D) d5 = 0.0D;
                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;
                double d7 = (double) yLen / 2.0D + d6 * 4.0D;
                shapeIndex++;

                for (int y = 0; y < yLen; y++) {
                    double d9 = ((double) y - d7) * 12.0D / d5;
                    if (d9 < 0.0D) d9 *= 4.0D;

                    int blendCase = blendMask[densityIndex];
                    double d8;
                    if (blendCase == 1) {
                        d8 = this.terrainNoise2[densityIndex] / 512.0D;
                    } else if (blendCase == 2) {
                        d8 = this.terrainNoise3[densityIndex] / 512.0D;
                    } else {
                        double d10 = this.terrainNoise2[densityIndex] / 512.0D;
                        double d11 = this.terrainNoise3[densityIndex] / 512.0D;
                        double d12 = (this.terrainNoise1[densityIndex] / 10.0D + 1.0D) / 2.0D;
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;
                    if (y > yLen - 4) {
                        double d13 = (double) ((float) (y - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }
                    noise[densityIndex++] = d8;
                }
            }
        }
        return noise;
    }

    private double[] generateTerrainNoiseForCoarseGridCachedAxes(
            double[] noise,
            int fromX,
            int fromY,
            int fromZ,
            int xLen,
            int yLen,
            int zLen,
            double[] temperatures,
            double[] rain
    ) {
        if (noise == null || noise.length < xLen * yLen * zLen) {
            throw new IllegalArgumentException("noise buffer too small for coarse grid");
        }

        double d0 = 684.412D;
        double d1 = 684.412D;

        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise(
                this.terrainNoise4, fromX, fromZ, xLen, zLen, 1.121D, 1.121D, 0.5D
        );
        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise(
                this.terrainNoise5, fromX, fromZ, xLen, zLen, 200.0D, 200.0D, 0.5D
        );

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoiseCachedAxes(
                this.terrainNoise1, (double) fromX, (double) fromY, (double) fromZ,
                xLen, yLen, zLen, d0 / 80.0D, d1 / 160.0D, d0 / 80.0D
        );
        int required = xLen * yLen * zLen;
        byte[] blendMask = buildTerrainBlendMaskAndActiveY(xLen, yLen, zLen);
        this.terrainNoise2 = this.terrainNoise2Generator.generateNoiseCachedAxesActiveY(
                this.terrainNoise2, (double) fromX, (double) fromY, (double) fromZ,
                xLen, yLen, zLen, d0, d1, d0, this.terrainBlendNoise2ActiveY
        );
        this.terrainNoise3 = this.terrainNoise3Generator.generateNoiseCachedAxesActiveY(
                this.terrainNoise3, (double) fromX, (double) fromY, (double) fromZ,
                xLen, yLen, zLen, d0, d1, d0, this.terrainBlendNoise3ActiveY
        );

        int k1 = 0;
        int l1 = 0;
        int climateIndex = 0;

        for (int j2 = 0; j2 < xLen; ++j2) {
            for (int l2 = 0; l2 < zLen; ++l2) {
                double d2 = temperatures[climateIndex];
                double d3 = rain[climateIndex] * d2;
                climateIndex++;

                double d4 = 1.0D - d3;
                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[l1] + 256.0D) / 512.0D;
                d5 *= d4;
                if (d5 > 1.0D) d5 = 1.0D;

                double d6 = this.terrainNoise5[l1] / 8000.0D;
                if (d6 < 0.0D) d6 = -d6 * 0.3D;
                d6 = d6 * 3.0D - 2.0D;

                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) d6 = -1.0D;
                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) d6 = 1.0D;
                    d6 /= 8.0D;
                }

                if (d5 < 0.0D) d5 = 0.0D;
                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;
                double d7 = (double) yLen / 2.0D + d6 * 4.0D;
                ++l1;

                for (int j3 = 0; j3 < yLen; ++j3) {
                    double d9 = ((double) j3 - d7) * 12.0D / d5;
                    if (d9 < 0.0D) d9 *= 4.0D;

                    int blendCase = blendMask[k1];
                    double d8;
                    if (blendCase == 1) {
                        d8 = this.terrainNoise2[k1] / 512.0D;
                    } else if (blendCase == 2) {
                        d8 = this.terrainNoise3[k1] / 512.0D;
                    } else {
                        double d10 = this.terrainNoise2[k1] / 512.0D;
                        double d11 = this.terrainNoise3[k1] / 512.0D;
                        double d12 = (this.terrainNoise1[k1] / 10.0D + 1.0D) / 2.0D;
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;
                    if (j3 > yLen - 4) {
                        double d13 = (double) ((float) (j3 - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }
                    noise[k1++] = d8;
                }
            }
        }
        return noise;
    }

    private double[] generateTerrainNoiseForCoarseGrid(
            double[] noise,
            int fromX,
            int fromY,
            int fromZ,
            int xLen,
            int yLen,
            int zLen,
            double[] temperatures,
            double[] rain
    ) {
        if (noise == null || noise.length < xLen * yLen * zLen) {
            throw new IllegalArgumentException("noise buffer too small for coarse grid");
        }

        double d0 = 684.412D;
        double d1 = 684.412D;

        this.terrainNoise4 = this.terrainNoise4Generator.generateNoise(
                this.terrainNoise4,
                fromX,
                fromZ,
                xLen,
                zLen,
                1.121D,
                1.121D,
                0.5D
        );

        this.terrainNoise5 = this.terrainNoise5Generator.generateNoise(
                this.terrainNoise5,
                fromX,
                fromZ,
                xLen,
                zLen,
                200.0D,
                200.0D,
                0.5D
        );

        this.terrainNoise1 = this.terrainNoise1Generator.generateNoise(
                this.terrainNoise1,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0 / 80.0D,
                d1 / 160.0D,
                d0 / 80.0D
        );

        this.terrainNoise2 = this.terrainNoise2Generator.generateNoise(
                this.terrainNoise2,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0,
                d1,
                d0
        );

        this.terrainNoise3 = this.terrainNoise3Generator.generateNoise(
                this.terrainNoise3,
                (double) fromX,
                (double) fromY,
                (double) fromZ,
                xLen,
                yLen,
                zLen,
                d0,
                d1,
                d0
        );

        int k1 = 0;
        int l1 = 0;
        int climateIndex = 0;

        for (int j2 = 0; j2 < xLen; ++j2) {
            for (int l2 = 0; l2 < zLen; ++l2) {
                double d2 = temperatures[climateIndex];
                double d3 = rain[climateIndex] * d2;
                climateIndex++;

                double d4 = 1.0D - d3;

                d4 *= d4;
                d4 *= d4;
                d4 = 1.0D - d4;

                double d5 = (this.terrainNoise4[l1] + 256.0D) / 512.0D;
                d5 *= d4;

                if (d5 > 1.0D) {
                    d5 = 1.0D;
                }

                double d6 = this.terrainNoise5[l1] / 8000.0D;

                if (d6 < 0.0D) {
                    d6 = -d6 * 0.3D;
                }

                d6 = d6 * 3.0D - 2.0D;

                if (d6 < 0.0D) {
                    d6 /= 2.0D;
                    if (d6 < -1.0D) {
                        d6 = -1.0D;
                    }

                    d6 /= 1.4D;
                    d6 /= 2.0D;
                    d5 = 0.0D;
                } else {
                    if (d6 > 1.0D) {
                        d6 = 1.0D;
                    }

                    d6 /= 8.0D;
                }

                if (d5 < 0.0D) {
                    d5 = 0.0D;
                }

                d5 += 0.5D;
                d6 = d6 * (double) yLen / 16.0D;

                double d7 = (double) yLen / 2.0D + d6 * 4.0D;

                ++l1;

                for (int j3 = 0; j3 < yLen; ++j3) {
                    double d8;
                    double d9 = ((double) j3 - d7) * 12.0D / d5;

                    if (d9 < 0.0D) {
                        d9 *= 4.0D;
                    }

                    double d10 = this.terrainNoise2[k1] / 512.0D;
                    double d11 = this.terrainNoise3[k1] / 512.0D;
                    double d12 = (this.terrainNoise1[k1] / 10.0D + 1.0D) / 2.0D;

                    if (d12 < 0.0D) {
                        d8 = d10;
                    } else if (d12 > 1.0D) {
                        d8 = d11;
                    } else {
                        d8 = d10 + (d11 - d10) * d12;
                    }

                    d8 -= d9;

                    if (j3 > yLen - 4) {
                        double d13 = (double) ((float) (j3 - (yLen - 4)) / 3.0F);
                        d8 = d8 * (1.0D - d13) + -10.0D * d13;
                    }

                    noise[k1] = d8;
                    ++k1;
                }
            }
        }

        return noise;
    }

    /**
     * Debug/analysis helper.
     *
     * Generates the raw 5x17x5 coarse density lattice used by Beta terrain before
     * block-level interpolation. This is much cheaper than generateChunkDensityField().
     *
     * Index order:
     * index = (coarseX * 5 + coarseZ) * 17 + coarseY
     * coarseX/coarseZ are 0..4 inside the chunk lattice.
     * coarseY is 0..16 and roughly corresponds to block Y coarseY * 8.
     */
    public double[] generateChunkCoarseDensityField(int chunkX, int chunkZ) {
        byte b0 = 4;
        int xLen = b0 + 1;
        byte yLen = 17;
        int zLen = b0 + 1;

        double[] coarse = generateChunkCoarseDensityFieldInto(null, chunkX, chunkZ);
        return Arrays.copyOf(coarse, xLen * yLen * zLen);
    }

    /**
     * Allocation-friendly version used by Hunter v2. The returned array is the
     * same buffer passed in when it is large enough. Read it before calling this
     * method again with the same buffer.
     */
    public double[] generateChunkCoarseDensityFieldInto(double[] buffer, int chunkX, int chunkZ) {
        byte b0 = 4;
        int xLen = b0 + 1;
        byte yLen = 17;
        int zLen = b0 + 1;

        this.biomeNoiseCache = this.worldChunkManager.getBiomeNoise(
                this.biomeNoiseCache,
                chunkX * 16,
                chunkZ * 16,
                16,
                16
        );

        return this.generateTerrainNoise(
                buffer,
                chunkX * b0,
                0,
                chunkZ * b0,
                xLen,
                yLen,
                zLen
        );
    }

    public static int coarseIndex(int coarseX, int coarseY, int coarseZ) {
        return (coarseX * 5 + coarseZ) * 17 + coarseY;
    }



    public static class ProgressiveStage0ScoutFeatures {
        public ProgressiveStage0TierFeatures tier16;
        public ProgressiveStage0TierFeatures tier64;
    }

    public static class ProgressiveStage0TierFeatures {
        public int axisSize;
        public int columnsSampled;

        public int upperPositiveColumns;
        public int y96PlusColumns;
        public int y104PlusColumns;
        public int y112PlusColumns;
        public int positiveAtY88;
        public int positiveAtY96;
        public int highestPositiveYIndex = -1;

        public int positiveDensityCells;
        public double sumPositiveDensity;
        public double avgPositiveDensity;
        public double maxPositiveDensity;

        public int upperLargestCluster;
        public int upperClusterWidth;
        public int upperClusterDepth;
        public int upperOccupiedRows;
        public int upperOccupiedCols;
        public int upperQuadrants;
        public int upperAdjacentEdges;

        public int y96LargestCluster;
        public int y96ClusterWidth;
        public int y96ClusterDepth;
        public int y96OccupiedRows;
        public int y96OccupiedCols;
        public int y96Quadrants;
        public int y96AdjacentEdges;
    }

    public static class SparseGateCounts {
        public final int lowCount;
        public final int highCount;

        public SparseGateCounts(int lowCount, int highCount) {
            this.lowCount = lowCount;
            this.highCount = highCount;
        }
    }

    public static class PreparedStage0MonsterFeatures {
        public int sampleSize;
        public int step;

        public int stage0FullY88;
        public int stage0FullY96;
        public int stage0FullY104;
        public int stage0FullY112;

        public int stage0Y88LargestCluster;
        public int stage0Y88Width;
        public int stage0Y88Depth;
        public boolean stage0Y88TouchesBorder;

        public int stage0Y96LargestCluster;
        public int stage0Y96Width;
        public int stage0Y96Depth;
        public boolean stage0Y96TouchesBorder;
    }

    public static class SparseReentryStats {
        public int sampleSize;
        public int step;

        public int stage0FullY64;
        public int stage0FullY72;
        public int stage0FullY80;
        public int stage0FullY88;
        public int stage0FullY96;
        public int stage0FullY104;
        public int stage0FullY112;

        public int stage0Y88LargestCluster;
        public int stage0Y88Width;
        public int stage0Y88Depth;
        public boolean stage0Y88TouchesBorder;

        public int stage0Y96LargestCluster;
        public int stage0Y96Width;
        public int stage0Y96Depth;
        public boolean stage0Y96TouchesBorder;
    }

    private boolean isSampleCandidateColumn(int x, int z) {
    return (x == 4 || x == 12) && (z == 4 || z == 12);
}

private static int index3(int x, int y, int z, int sizeY, int sizeZ) {
    return (x * sizeY + y) * sizeZ + z;
}
}
