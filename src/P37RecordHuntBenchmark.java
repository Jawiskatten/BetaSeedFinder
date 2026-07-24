import java.util.Locale;

/** Times the actual persistent P37 production stream for Mega and aggressive record profiles. */
public final class P37RecordHuntBenchmark {
    private static final String[] PROFILES = {"mega", "record60", "record80"};
    private static final String[] NAMES = {"Mega 30k+", "Record 60k+", "World Record 80k+"};
    private static final int[] P20_MIN = {1, 3, 7};
    private static final int[] UPPER_MIN = {8, 19, 34};
    private static final int[] HIGH_MIN = {6, 12, 22};
    private static final int[] COARSE_MIN = {85, 95, 700};

    private P37RecordHuntBenchmark() { }

    public static void main(String[] args) throws Exception {
        int worlds = args.length > 0 ? Integer.parseInt(args[0]) : 32768;
        int iterations = args.length > 1 ? Integer.parseInt(args[1]) : 5;
        long sequenceSeed = args.length > 2 ? Long.parseLong(args[2]) : 123456789L;
        if (worlds < 1 || iterations < 1) throw new IllegalArgumentException("worlds/iterations must be positive");

        long[] seeds = new long[worlds];
        for (int i = 0; i < worlds; i++) seeds[i] = deterministicSeedForAttempt(sequenceSeed, i);
        double[] rates = new double[PROFILES.length];

        System.out.println("P37 production record-hunt benchmark");
        System.out.println("  deterministic worlds: " + worlds);
        System.out.println("  regions/world:        " + GpuStage0Scout.CENTER_COUNT);
        System.out.println("  iterations:           " + iterations);
        System.out.println("  telemetry:            lean 8B/region");
        System.out.println();
        System.out.printf("  %-23s %12s %12s %11s %11s %11s %11s%n",
                "profile", "regions/s", "gain", "P20 pass", "Upper pass", "High pass", "coarse pass");

        for (int profileIndex = 0; profileIndex < PROFILES.length; profileIndex++) {
            long nanos = 0L;
            long p20 = 0L, upper = 0L, high = 0L, coarse = 0L;
            try (GpuStage0Scout scout = new GpuStage0Scout(worlds, PROFILES[profileIndex], false, ignored -> { })) {
                scout.filter(seeds, worlds); // warmup
                GpuStage0Scout.BatchResult last = null;
                for (int iteration = 0; iteration < iterations; iteration++) {
                    long start = System.nanoTime();
                    last = scout.filter(seeds, worlds);
                    nanos += System.nanoTime() - start;
                }
                int regions = worlds * scout.centerCount();
                for (int i = 0; i < regions; i++) {
                    int a = last.p20Counts[i] & 0xFF;
                    int b = last.fullUpperCounts[i] & 0xFF;
                    int c = last.highReentryCounts[i] & 0xFF;
                    int d = last.coarseScores[i];
                    if (a >= P20_MIN[profileIndex]) p20++;
                    if (a >= P20_MIN[profileIndex] && b >= UPPER_MIN[profileIndex]) upper++;
                    if (a >= P20_MIN[profileIndex] && b >= UPPER_MIN[profileIndex] && c >= HIGH_MIN[profileIndex]) high++;
                    if (last.p19Pass[i] != 0 && last.megaTopologyRejected[i] == 0 && d >= COARSE_MIN[profileIndex]) coarse++;
                }
                rates[profileIndex] = (double) regions * iterations * 1_000_000_000.0 / nanos;
            }
            double gain = profileIndex == 0 ? 0.0 : (rates[profileIndex] / rates[0] - 1.0) * 100.0;
            System.out.printf(Locale.US, "  %-23s %12.1f %+11.1f%% %11d %11d %11d %11d%n",
                    NAMES[profileIndex], rates[profileIndex], gain, p20, upper, high, coarse);
        }
        System.out.println();
        System.out.println("Record modes intentionally trade completeness below their target range for speed.");
        System.out.println("Run P37RecordHuntValidation to see the bundled historical evidence contract.");
    }

    private static long deterministicSeedForAttempt(long sequenceSeed, long attempt) {
        long z = sequenceSeed + attempt * 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }
}
