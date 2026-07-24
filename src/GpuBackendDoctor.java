public final class GpuBackendDoctor {
    public static void main(String[] args) {
        GpuBackendLocator.ensureDefaultConfig();
        try {
            GpuBackendLocator.ResolvedBackend backend = GpuBackendLocator.resolve();
            System.out.println("Detected vendor:   " + GpuBackendLocator.detectInstalledVendor().displayName());
            System.out.println("Requested backend: " + GpuBackendLocator.requestedBackend().displayName());
            System.out.println("Resolved backend:  " + backend.displayName());
            System.out.println("Executable:        " + backend.path());
        } catch (Exception e) {
            System.err.println(e.getMessage());
            System.exit(1);
        }
    }
}
