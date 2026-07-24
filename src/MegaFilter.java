/**
 * P23 live Mega-mode filter contract.
 *
 * Keep the native translation in gpu_p20_benchmark.cpp exactly aligned with
 * these branch conditions. General mode never calls the live reject.
 */
final class MegaFilter {
    static final int DEFAULT_UPPER_MIN = 8;
    static final int DEFAULT_HIGH_MIN = 6;
    static final double DEFAULT_P19_SCORE_CEILING = 5.0D;
    static final int DEFAULT_Y96_MIN_LARGEST_CLUSTER = 4;

    private MegaFilter() { }

    static int upperMin(SearchSettings settings) {
        return settings.megaUpperMinPositiveColumns > 0
                ? settings.megaUpperMinPositiveColumns
                : DEFAULT_UPPER_MIN;
    }

    static int highMin(SearchSettings settings) {
        return settings.megaHighMinReentryColumns > 0
                ? settings.megaHighMinReentryColumns
                : DEFAULT_HIGH_MIN;
    }

    static boolean rejects(
            SearchSettings settings,
            double p19Score,
            boolean extremeBypass,
            int fullY112,
            int y96LargestCluster
    ) {
        if (!settings.megaMode || extremeBypass) return false;

        double ceiling = settings.megaTopologyMaxP19ScoreExclusive > 0.0D
                ? settings.megaTopologyMaxP19ScoreExclusive
                : DEFAULT_P19_SCORE_CEILING;
        if (!(p19Score < ceiling)) return false;

        int y96Min = settings.megaTopologyMinY96LargestCluster > 0
                ? settings.megaTopologyMinY96LargestCluster
                : DEFAULT_Y96_MIN_LARGEST_CLUSTER;

        // Union rule. Short-circuit the cheap FullY112 test before the cluster comparison.
        if (settings.megaTopologyRejectWhenFullY112Zero && fullY112 == 0) return true;
        return y96LargestCluster < y96Min;
    }
}
