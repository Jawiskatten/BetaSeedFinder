public final class P24GpuProtocolCheck {
    private P24GpuProtocolCheck() { }

    public static void main(String[] args) throws Exception {
        check(false, true, "GENERAL research");
        check(true, true, "MEGA research");
        check(true, false, "MEGA lean");
        System.out.println("P24 GPU PROTOCOL CHECK PASSED");
    }

    private static void check(boolean megaMode, boolean researchTelemetry, String name) throws Exception {
        try (GpuStage0Scout scout = new GpuStage0Scout(1, megaMode, researchTelemetry, System.out::println)) {
            if (scout.capacity() != 1) {
                throw new IllegalStateException(name + " worker returned wrong capacity");
            }
            System.out.println(name + " ST0R2603 startup: PASS");
        }
    }
}
