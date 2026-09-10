package beta173;

import beta173.noise.NoiseGeneratorOctaves173;

import java.util.Arrays;
import java.util.Random;

/**
 * Exact pre-population Beta 1.7.3 overworld chunk generator used by the
 * spawn-island research tool.
 *
 * Generation order matches ChunkProviderGenerate.provideChunk:
 *   base terrain -> biome surface replacement -> MapGenCaves.
 *
 * The existing BetaTerrain173 deliberately stops at AIR/STONE/WATER/ICE.
 * This class layers the missing surface and cave passes on top without
 * changing that hot-path implementation.
 */
public final class BetaChunk173 {
    public static final int AIR = 0;
    public static final int STONE = 1;
    public static final int GRASS = 2;
    public static final int DIRT = 3;
    public static final int BEDROCK = 7;
    public static final int WATER_MOVING = 8;
    public static final int WATER_STILL = 9;
    public static final int LAVA_MOVING = 10;
    public static final int LAVA_STILL = 11;
    public static final int SAND = 12;
    public static final int GRAVEL = 13;
    public static final int SANDSTONE = 24;
    public static final int ICE = 79;

    public static final int CHUNK_SIZE = 16;
    public static final int WORLD_HEIGHT = 128;
    public static final int SEA_LEVEL = 64;

    private long worldSeed;
    private final BetaTerrain173 baseTerrain;
    private final WorldChunkManager173 worldChunkManager;
    private final Random chunkRandom = new Random();
    private final Random surfaceInitRandom;
    private final NoiseGeneratorOctaves173 sandGravelNoiseGenerator;
    private final NoiseGeneratorOctaves173 stoneNoiseGenerator;
    private final CaveGenerator caves = new CaveGenerator();

    private BiomeBase173[] biomes;
    private double[] sandNoise = new double[256];
    private double[] gravelNoise = new double[256];
    private double[] stoneNoise = new double[256];

    public BetaChunk173(long seed) {
        this.worldSeed = seed;
        this.baseTerrain = new BetaTerrain173(seed);
        this.worldChunkManager = new WorldChunkManager173(seed);

        // Match the exact ChunkProviderGenerate constructor order up to the two
        // four-octave surface generators. BetaTerrain173 owns a separate copy of
        // the terrain generators; duplicating their constructor consumption here
        // lets us recover the exact surface-noise states independently.
        this.surfaceInitRandom = new Random(seed);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 16);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 16);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 8);
        this.sandGravelNoiseGenerator = new NoiseGeneratorOctaves173(this.surfaceInitRandom, 4);
        this.stoneNoiseGenerator = new NoiseGeneratorOctaves173(this.surfaceInitRandom, 4);
    }

    public void reseed(long seed) {
        this.worldSeed = seed;
        this.baseTerrain.reseed(seed);
        this.worldChunkManager.reseed(seed);

        this.surfaceInitRandom.setSeed(seed);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 16);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 16);
        NoiseGeneratorOctaves173.consumeConstructorRandom(this.surfaceInitRandom, 8);
        this.sandGravelNoiseGenerator.reseed(this.surfaceInitRandom);
        this.stoneNoiseGenerator.reseed(this.surfaceInitRandom);
    }

    /** Generates a vanilla Beta 1.7.3 chunk through the cave-carving pass. */
    public int[] generateChunk(int chunkX, int chunkZ) {
        int[] compact = this.baseTerrain.generateChunkBaseTerrain(chunkX, chunkZ);
        int[] blocks = new int[compact.length];
        for (int i = 0; i < compact.length; ++i) {
            int b = compact[i];
            if (b == BetaTerrain173.STONE) blocks[i] = STONE;
            else if (b == BetaTerrain173.WATER) blocks[i] = WATER_STILL;
            else if (b == BetaTerrain173.ICE) blocks[i] = ICE;
            else blocks[i] = AIR;
        }

        this.biomes = this.worldChunkManager.getBiomeNoise(
                this.biomes, chunkX * 16, chunkZ * 16, 16, 16);

        // Vanilla resets this Random to chunk coordinates before base generation.
        // Base generation itself does not consume it, so resetting immediately
        // before the surface pass is equivalent.
        this.chunkRandom.setSeed((long) chunkX * 341873128712L + (long) chunkZ * 132897987541L);
        replaceBlocksForBiome(chunkX, chunkZ, blocks, this.biomes);
        this.caves.generate(this.worldSeed, chunkX, chunkZ, blocks);
        return blocks;
    }

    private void replaceBlocksForBiome(int chunkX, int chunkZ, int[] blocks, BiomeBase173[] biomeData) {
        final int sea = 64;
        final double scale = 0.03125D;

        this.sandNoise = this.sandGravelNoiseGenerator.generateNoise(
                this.sandNoise,
                (double) (chunkX * 16),
                (double) (chunkZ * 16),
                0.0D,
                16, 16, 1,
                scale, scale, 1.0D);

        this.gravelNoise = this.sandGravelNoiseGenerator.generateNoise(
                this.gravelNoise,
                (double) (chunkX * 16),
                109.0134D,
                (double) (chunkZ * 16),
                16, 1, 16,
                scale, 1.0D, scale);

        this.stoneNoise = this.stoneNoiseGenerator.generateNoise(
                this.stoneNoise,
                (double) (chunkX * 16),
                (double) (chunkZ * 16),
                0.0D,
                16, 16, 1,
                scale * 2.0D, scale * 2.0D, scale * 2.0D);

        // Source order is z outer, x inner in terms of the byte-array index:
        // block index = (x * 16 + z) * 128 + y.
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                BiomeBase173 biome = biomeData[z + x * 16];
                boolean sandPatch = this.sandNoise[z + x * 16] + this.chunkRandom.nextDouble() * 0.2D > 0.0D;
                boolean gravelPatch = this.gravelNoise[z + x * 16] + this.chunkRandom.nextDouble() * 0.2D > 3.0D;
                int depth = (int) (this.stoneNoise[z + x * 16] / 3.0D + 3.0D + this.chunkRandom.nextDouble() * 0.25D);
                int remaining = -1;
                int top = biomeTop(biome);
                int filler = biomeFiller(biome);

                for (int y = 127; y >= 0; --y) {
                    int index = index(x, y, z);
                    if (y <= this.chunkRandom.nextInt(5)) {
                        blocks[index] = BEDROCK;
                    } else {
                        int current = blocks[index];
                        if (current == AIR) {
                            remaining = -1;
                        } else if (current == STONE) {
                            if (remaining == -1) {
                                if (depth <= 0) {
                                    top = AIR;
                                    filler = STONE;
                                } else if (y >= sea - 4 && y <= sea + 1) {
                                    top = biomeTop(biome);
                                    filler = biomeFiller(biome);

                                    if (gravelPatch) top = AIR;
                                    if (gravelPatch) filler = GRAVEL;
                                    if (sandPatch) top = SAND;
                                    if (sandPatch) filler = SAND;
                                }

                                if (y < sea && top == AIR) {
                                    top = WATER_STILL;
                                }

                                remaining = depth;
                                if (y >= sea - 1) blocks[index] = top;
                                else blocks[index] = filler;
                            } else if (remaining > 0) {
                                --remaining;
                                blocks[index] = filler;
                                if (remaining == 0 && filler == SAND) {
                                    remaining = this.chunkRandom.nextInt(4);
                                    filler = SANDSTONE;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    private static int biomeTop(BiomeBase173 biome) {
        return biome == BiomeBase173.DESERT || biome == BiomeBase173.ICE_DESERT ? SAND : GRASS;
    }

    private static int biomeFiller(BiomeBase173 biome) {
        return biome == BiomeBase173.DESERT || biome == BiomeBase173.ICE_DESERT ? SAND : DIRT;
    }

    public static int index(int x, int y, int z) {
        return (x * 16 + z) * 128 + y;
    }

    public static boolean isAir(int block) {
        return block == AIR;
    }

    /** Collision-solid enough for terrain-component topology. */
    public static boolean isSolid(int block) {
        return block != AIR
                && block != WATER_MOVING
                && block != WATER_STILL
                && block != LAVA_MOVING
                && block != LAVA_STILL;
    }

    /** Exact Beta 1.7.3 MapGenBase + MapGenCaves port for int[] chunks. */
    private static final class CaveGenerator {
        private static final int RANGE = 8;
        private static final float PI = 3.1415927F;
        private static final float[] SIN_TABLE = new float[65536];

        static {
            for (int i = 0; i < SIN_TABLE.length; ++i) {
                SIN_TABLE[i] = (float) Math.sin((double) i * Math.PI * 2.0D / 65536.0D);
            }
        }

        private final Random rand = new Random();

        void generate(long worldSeed, int targetChunkX, int targetChunkZ, int[] blocks) {
            this.rand.setSeed(worldSeed);
            long oddX = this.rand.nextLong() / 2L * 2L + 1L;
            long oddZ = this.rand.nextLong() / 2L * 2L + 1L;

            for (int sourceChunkX = targetChunkX - RANGE; sourceChunkX <= targetChunkX + RANGE; ++sourceChunkX) {
                for (int sourceChunkZ = targetChunkZ - RANGE; sourceChunkZ <= targetChunkZ + RANGE; ++sourceChunkZ) {
                    this.rand.setSeed((long) sourceChunkX * oddX + (long) sourceChunkZ * oddZ ^ worldSeed);
                    recursiveGenerate(sourceChunkX, sourceChunkZ, targetChunkX, targetChunkZ, blocks);
                }
            }
        }

        private void recursiveGenerate(int sourceChunkX, int sourceChunkZ,
                                       int targetChunkX, int targetChunkZ, int[] blocks) {
            int count = this.rand.nextInt(this.rand.nextInt(this.rand.nextInt(40) + 1) + 1);
            if (this.rand.nextInt(15) != 0) count = 0;

            for (int i = 0; i < count; ++i) {
                double x = (double) (sourceChunkX * 16 + this.rand.nextInt(16));
                double y = (double) this.rand.nextInt(this.rand.nextInt(120) + 8);
                double z = (double) (sourceChunkZ * 16 + this.rand.nextInt(16));
                int tunnels = 1;

                if (this.rand.nextInt(4) == 0) {
                    generateLargeCaveNode(targetChunkX, targetChunkZ, blocks, x, y, z);
                    tunnels += this.rand.nextInt(4);
                }

                for (int j = 0; j < tunnels; ++j) {
                    float yaw = this.rand.nextFloat() * PI * 2.0F;
                    float pitch = (this.rand.nextFloat() - 0.5F) * 2.0F / 8.0F;
                    float width = this.rand.nextFloat() * 2.0F + this.rand.nextFloat();
                    generateCaveNode(targetChunkX, targetChunkZ, blocks, x, y, z,
                            width, yaw, pitch, 0, 0, 1.0D);
                }
            }
        }

        private void generateLargeCaveNode(int targetChunkX, int targetChunkZ, int[] blocks,
                                           double x, double y, double z) {
            generateCaveNode(targetChunkX, targetChunkZ, blocks, x, y, z,
                    1.0F + this.rand.nextFloat() * 6.0F,
                    0.0F, 0.0F, -1, -1, 0.5D);
        }

        private void generateCaveNode(int targetChunkX, int targetChunkZ, int[] blocks,
                                      double x, double y, double z,
                                      float width, float yaw, float pitch,
                                      int step, int maxStep, double verticalScale) {
            double centerX = (double) (targetChunkX * 16 + 8);
            double centerZ = (double) (targetChunkZ * 16 + 8);
            float yawVelocity = 0.0F;
            float pitchVelocity = 0.0F;
            Random local = new Random(this.rand.nextLong());

            if (maxStep <= 0) {
                int max = RANGE * 16 - 16;
                maxStep = max - local.nextInt(max / 4);
            }

            boolean singleNode = false;
            if (step == -1) {
                step = maxStep / 2;
                singleNode = true;
            }

            int branchStep = local.nextInt(maxStep / 2) + maxStep / 4;
            boolean gentlePitch = local.nextInt(6) == 0;

            for (; step < maxStep; ++step) {
                double radiusXZ = 1.5D + (double) (sin((float) step * PI / (float) maxStep) * width * 1.0F);
                double radiusY = radiusXZ * verticalScale;
                float cosPitch = cos(pitch);
                float sinPitch = sin(pitch);
                x += (double) (cos(yaw) * cosPitch);
                y += (double) sinPitch;
                z += (double) (sin(yaw) * cosPitch);

                if (gentlePitch) pitch *= 0.92F;
                else pitch *= 0.7F;

                pitch += pitchVelocity * 0.1F;
                yaw += yawVelocity * 0.1F;
                pitchVelocity *= 0.9F;
                yawVelocity *= 0.75F;
                pitchVelocity += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 2.0F;
                yawVelocity += (local.nextFloat() - local.nextFloat()) * local.nextFloat() * 4.0F;

                if (!singleNode && step == branchStep && width > 1.0F) {
                    generateCaveNode(targetChunkX, targetChunkZ, blocks, x, y, z,
                            local.nextFloat() * 0.5F + 0.5F,
                            yaw - 1.5707964F, pitch / 3.0F,
                            step, maxStep, 1.0D);
                    generateCaveNode(targetChunkX, targetChunkZ, blocks, x, y, z,
                            local.nextFloat() * 0.5F + 0.5F,
                            yaw + 1.5707964F, pitch / 3.0F,
                            step, maxStep, 1.0D);
                    return;
                }

                if (singleNode || local.nextInt(4) != 0) {
                    double dx = x - centerX;
                    double dz = z - centerZ;
                    double remaining = (double) (maxStep - step);
                    double maxReach = (double) (width + 2.0F + 16.0F);
                    if (dx * dx + dz * dz - remaining * remaining > maxReach * maxReach) return;

                    if (x >= centerX - 16.0D - radiusXZ * 2.0D
                            && z >= centerZ - 16.0D - radiusXZ * 2.0D
                            && x <= centerX + 16.0D + radiusXZ * 2.0D
                            && z <= centerZ + 16.0D + radiusXZ * 2.0D) {
                        int minX = floor(x - radiusXZ) - targetChunkX * 16 - 1;
                        int maxX = floor(x + radiusXZ) - targetChunkX * 16 + 1;
                        int minY = floor(y - radiusY) - 1;
                        int maxY = floor(y + radiusY) + 1;
                        int minZ = floor(z - radiusXZ) - targetChunkZ * 16 - 1;
                        int maxZ = floor(z + radiusXZ) - targetChunkZ * 16 + 1;

                        if (minX < 0) minX = 0;
                        if (maxX > 16) maxX = 16;
                        if (minY < 1) minY = 1;
                        if (maxY > 120) maxY = 120;
                        if (minZ < 0) minZ = 0;
                        if (maxZ > 16) maxZ = 16;

                        boolean waterFound = false;
                        for (int bx = minX; !waterFound && bx < maxX; ++bx) {
                            for (int bz = minZ; !waterFound && bz < maxZ; ++bz) {
                                for (int by = maxY + 1; !waterFound && by >= minY - 1; --by) {
                                    if (by >= 0 && by < 128) {
                                        int b = blocks[index(bx, by, bz)];
                                        if (b == WATER_MOVING || b == WATER_STILL) waterFound = true;
                                        if (by != minY - 1
                                                && bx != minX && bx != maxX - 1
                                                && bz != minZ && bz != maxZ - 1) {
                                            by = minY;
                                        }
                                    }
                                }
                            }
                        }

                        if (!waterFound) {
                            for (int bx = minX; bx < maxX; ++bx) {
                                double nx = ((double) (bx + targetChunkX * 16) + 0.5D - x) / radiusXZ;
                                for (int bz = minZ; bz < maxZ; ++bz) {
                                    double nz = ((double) (bz + targetChunkZ * 16) + 0.5D - z) / radiusXZ;
                                    int blockIndex = index(bx, maxY, bz);
                                    boolean hitGrass = false;

                                    if (nx * nx + nz * nz < 1.0D) {
                                        for (int by = maxY - 1; by >= minY; --by) {
                                            double ny = ((double) by + 0.5D - y) / radiusY;
                                            if (ny > -0.7D && nx * nx + ny * ny + nz * nz < 1.0D) {
                                                int block = blocks[blockIndex];
                                                if (block == GRASS) hitGrass = true;

                                                if (block == STONE || block == DIRT || block == GRASS) {
                                                    if (by < 10) {
                                                        blocks[blockIndex] = LAVA_MOVING;
                                                    } else {
                                                        blocks[blockIndex] = AIR;
                                                        if (hitGrass && blockIndex - 1 >= 0 && blocks[blockIndex - 1] == DIRT) {
                                                            blocks[blockIndex - 1] = GRASS;
                                                        }
                                                    }
                                                }
                                            }
                                            --blockIndex;
                                        }
                                    }
                                }
                            }

                            if (singleNode) break;
                        }
                    }
                }
            }
        }

        private static float sin(float value) {
            return SIN_TABLE[(int) (value * 10430.378F) & 65535];
        }

        private static float cos(float value) {
            return SIN_TABLE[(int) (value * 10430.378F + 16384.0F) & 65535];
        }

        private static int floor(double value) {
            int i = (int) value;
            return value < (double) i ? i - 1 : i;
        }
    }
}
