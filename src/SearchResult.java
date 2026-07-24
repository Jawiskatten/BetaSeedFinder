public class SearchResult {
    public final long seed;
    public final int blocks;
    public final int columns;
    public final int width;
    public final int depth;
    public final int minY;
    public final int maxY;
    public final int centerX;
    public final int centerZ;
    public final String previewPath;

    public SearchResult(
            long seed,
            int blocks,
            int columns,
            int width,
            int depth,
            int minY,
            int maxY,
            int centerX,
            int centerZ,
            String previewPath
    ) {
        this.seed = seed;
        this.blocks = blocks;
        this.columns = columns;
        this.width = width;
        this.depth = depth;
        this.minY = minY;
        this.maxY = maxY;
        this.centerX = centerX;
        this.centerZ = centerZ;
        this.previewPath = previewPath;
    }
}