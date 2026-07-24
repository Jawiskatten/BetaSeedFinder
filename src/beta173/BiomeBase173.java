package beta173;

/**
 * Minimal standalone Beta 1.7.3 biome lookup.
 *
 * This intentionally has no Bukkit blocks, no Bukkit biomes, and no tree generators.
 * For bare terrain seedfinding, we only need the climate lookup to exist.
 */
public enum BiomeBase173 {
    RAINFOREST,
    SWAMPLAND,
    SEASONAL_FOREST,
    FOREST,
    SAVANNA,
    SHRUBLAND,
    TAIGA,
    DESERT,
    PLAINS,
    ICE_DESERT,
    TUNDRA,
    HELL,
    SKY;

    private static final BiomeBase173[] LOOKUP = new BiomeBase173[64 * 64];

    static {
        for (int tempIndex = 0; tempIndex < 64; ++tempIndex) {
            for (int rainIndex = 0; rainIndex < 64; ++rainIndex) {
                LOOKUP[tempIndex + rainIndex * 64] = getByRainTempUncached(
                        (float) tempIndex / 63.0F,
                        (float) rainIndex / 63.0F
                );
            }
        }
    }

    public static BiomeBase173 getByRainTempUncached(float temp, float rain) {
        rain *= temp;

        if (temp < 0.1F) {
            return TUNDRA;
        }

        if (rain < 0.2F) {
            if (temp < 0.5F) {
                return TUNDRA;
            }

            if (temp < 0.95F) {
                return SAVANNA;
            }

            return DESERT;
        }

        if (rain > 0.5F && temp < 0.7F) {
            return SWAMPLAND;
        }

        if (temp < 0.5F) {
            return TAIGA;
        }

        if (temp < 0.97F) {
            if (rain < 0.35F) {
                return SHRUBLAND;
            }

            return FOREST;
        }

        if (rain < 0.45F) {
            return PLAINS;
        }

        if (rain < 0.9F) {
            return SEASONAL_FOREST;
        }

        return RAINFOREST;
    }

    public static BiomeBase173 a(double temp, double rain) {
        int tempIndex = (int)(temp * 63.0D);
        int rainIndex = (int)(rain * 63.0D);

        if (tempIndex < 0) tempIndex = 0;
        if (rainIndex < 0) rainIndex = 0;
        if (tempIndex > 63) tempIndex = 63;
        if (rainIndex > 63) rainIndex = 63;

        return LOOKUP[tempIndex + rainIndex * 64];
    }
}
