import java.util.Arrays;

/**
 * Compact exact-component column model used by the interactive preview.
 * Each occupied X/Z column stores the lowest and highest connected stone Y.
 */
public final class Island3DData {
    public final long seed;
    public final int width;
    public final int depth;
    public final int worldMinX;
    public final int worldMinZ;
    public final int globalMinY;
    public final int globalMaxY;
    public final int blocks;
    public final int columns;

    private final int[] minY;
    private final int[] maxY;

    public Island3DData(
            long seed,
            int width,
            int depth,
            int worldMinX,
            int worldMinZ,
            int globalMinY,
            int globalMaxY,
            int blocks,
            int columns,
            int[] minY,
            int[] maxY
    ) {
        if (width <= 0 || depth <= 0) {
            throw new IllegalArgumentException("Invalid 3D preview dimensions");
        }
        if (minY.length != width * depth || maxY.length != width * depth) {
            throw new IllegalArgumentException("Invalid 3D preview column arrays");
        }
        this.seed = seed;
        this.width = width;
        this.depth = depth;
        this.worldMinX = worldMinX;
        this.worldMinZ = worldMinZ;
        this.globalMinY = globalMinY;
        this.globalMaxY = globalMaxY;
        this.blocks = blocks;
        this.columns = columns;
        this.minY = Arrays.copyOf(minY, minY.length);
        this.maxY = Arrays.copyOf(maxY, maxY.length);
    }

    public boolean occupied(int x, int z) {
        int index = index(x, z);
        return index >= 0 && minY[index] != Integer.MAX_VALUE;
    }

    public int minYAt(int x, int z) {
        int index = index(x, z);
        return index < 0 ? Integer.MAX_VALUE : minY[index];
    }

    public int maxYAt(int x, int z) {
        int index = index(x, z);
        return index < 0 ? Integer.MIN_VALUE : maxY[index];
    }

    public int thicknessAt(int x, int z) {
        int index = index(x, z);
        if (index < 0 || minY[index] == Integer.MAX_VALUE) return 0;
        return maxY[index] - minY[index] + 1;
    }

    public String cacheKey() {
        return seed + "|" + worldMinX + "|" + worldMinZ + "|" + width + "|" + depth
                + "|" + globalMinY + "|" + globalMaxY + "|" + blocks;
    }

    private int index(int x, int z) {
        if (x < 0 || x >= width || z < 0 || z >= depth) return -1;
        return x * depth + z;
    }
}
