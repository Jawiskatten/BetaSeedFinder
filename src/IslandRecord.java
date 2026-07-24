import java.nio.file.Files;
import java.nio.file.Path;

public class IslandRecord {
    public final RunRecord run;
    public final long attempt;
    public final long seed;
    public final int stage0;
    public final int stage0Y88;
    public final int coarse;
    public final int blocks;
    public final int columns;
    public final int width;
    public final int depth;
    public final int footprintArea;
    public final double fillPercent;
    public final double avgThickness;
    public final int minY;
    public final int maxY;
    public final int centerX;
    public final int centerZ;
    public final int searchCenterChunkX;
    public final int searchCenterChunkZ;
    public final int radius;
    public final boolean touchesSide;
    public final String finalSource;

    public IslandRecord(
            RunRecord run,
            long attempt,
            long seed,
            int stage0,
            int stage0Y88,
            int coarse,
            int blocks,
            int columns,
            int width,
            int depth,
            int footprintArea,
            double fillPercent,
            double avgThickness,
            int minY,
            int maxY,
            int centerX,
            int centerZ,
            int searchCenterChunkX,
            int searchCenterChunkZ,
            int radius,
            boolean touchesSide,
            String finalSource
    ) {
        this.run = run;
        this.attempt = attempt;
        this.seed = seed;
        this.stage0 = stage0;
        this.stage0Y88 = stage0Y88;
        this.coarse = coarse;
        this.blocks = blocks;
        this.columns = columns;
        this.width = width;
        this.depth = depth;
        this.footprintArea = footprintArea > 0 ? footprintArea : Math.max(1, width * depth);
        this.fillPercent = fillPercent;
        this.avgThickness = avgThickness;
        this.minY = minY;
        this.maxY = maxY;
        this.centerX = centerX;
        this.centerZ = centerZ;
        this.searchCenterChunkX = searchCenterChunkX;
        this.searchCenterChunkZ = searchCenterChunkZ;
        this.radius = radius;
        this.touchesSide = touchesSide;
        this.finalSource = finalSource == null ? "" : finalSource;
    }

    public int yRange() {
        return maxY - minY;
    }

    public Path previewPath() {
        if (run == null || run.folder == null) {
            return null;
        }

        Path coordinatePath = run.folder.resolve(
                "floating_seed_" + seed + "_x" + centerX + "_z" + centerZ + ".png"
        );
        Path legacyPath = run.folder.resolve("floating_seed_" + seed + ".png");

        // Prefer whatever was actually saved. New multi-center builds always use the
        // coordinate-qualified filename, while old single-center runs used seed-only.
        if (Files.isRegularFile(coordinatePath)) {
            return coordinatePath;
        }
        if (Files.isRegularFile(legacyPath)) {
            return legacyPath;
        }

        // Do not enumerate build IDs here. That caused every new production build
        // (P37/P38 and future versions) to silently fall back to the legacy filename.
        if (hasSavedSearchCenter() || manifestInt("gpuCoverageCentersPerWorld", 1) > 1) {
            return coordinatePath;
        }
        return legacyPath;
    }

    private boolean hasSavedSearchCenter() {
        return searchCenterChunkX != Integer.MIN_VALUE && searchCenterChunkZ != Integer.MIN_VALUE;
    }

    private int manifestInt(String key, int fallback) {
        if (run == null || run.manifest == null) return fallback;
        try {
            return Integer.parseInt(run.manifest.getOrDefault(key, Integer.toString(fallback)).trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }
}
