public final class P30GpuProtocolCheck {
    private P30GpuProtocolCheck() { }

    public static void main(String[] args) throws Exception {
        check(false, true, "GENERAL research");
        check(true, true, "MEGA research");
        check(true, false, "MEGA lean");
        System.out.println("P30 GPU PROTOCOL CHECK PASSED");
    }

    private static void check(boolean megaMode, boolean researchTelemetry, String name) throws Exception {
        long[] seeds = {123456789L, -987654321L};
        try (GpuStage0Scout scout = new GpuStage0Scout(2, megaMode, researchTelemetry, System.out::println)) {
            if (scout.capacity() != 2 || scout.centerCount() != 8) {
                throw new IllegalStateException(name + " worker returned wrong capacity/center count");
            }
            GpuStage0Scout.BatchResult result = scout.filter(seeds, seeds.length);
            int expected = seeds.length * scout.centerCount();
            if (result.coarseScores.length < expected || result.p20Counts.length < expected) {
                throw new IllegalStateException(name + " worker returned a truncated multi-center response");
            }
            for (int center = 0; center < scout.centerCount(); center++) {
                int index = scout.resultIndex(1, center);
                if (index != scout.centerCount() + center) {
                    throw new IllegalStateException(name + " result indexing mismatch");
                }
            }
            System.out.println(name + " ST0R3008 startup + 16-region response: PASS");
        }
    }
}
