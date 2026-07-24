public final class P37GpuProtocolCheck {
    private P37GpuProtocolCheck() { }

    public static void main(String[] args) throws Exception {
        check("general", true, "GENERAL research");
        check("mega", false, "MEGA lean");
        check("record60", false, "RECORD60 lean");
        check("record80", false, "RECORD80 lean");
        System.out.println("P37 GPU PROTOCOL CHECK PASSED");
    }

    private static void check(String profile, boolean researchTelemetry, String name) throws Exception {
        long[] seeds = {123456789L, -987654321L};
        try (GpuStage0Scout scout = new GpuStage0Scout(2, profile, researchTelemetry, System.out::println)) {
            if (scout.capacity() != 2 || scout.centerCount() != 8) {
                throw new IllegalStateException(name + " worker returned wrong capacity/center count");
            }
            GpuStage0Scout.BatchResult result = scout.filter(seeds, seeds.length);
            int expected = seeds.length * scout.centerCount();
            if (result.coarseScores.length < expected || result.p20Counts.length < expected) {
                throw new IllegalStateException(name + " worker returned a truncated multi-center response");
            }
            System.out.println(name + " ST0R3708 startup + " + expected + "-region response: PASS");
        }
    }
}
