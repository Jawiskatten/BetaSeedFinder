enum GpuBackendKind {
    AUTO("Automatic"),
    AMD("AMD HIP"),
    NVIDIA("NVIDIA CUDA/HIP"),
    LEGACY("Legacy worker"),
    NONE("None");

    private final String displayName;

    GpuBackendKind(String displayName) {
        this.displayName = displayName;
    }

    String displayName() {
        return displayName;
    }

    static GpuBackendKind parse(String value) {
        if (value == null) return AUTO;
        String normalized = value.trim().toLowerCase();
        return switch (normalized) {
            case "amd", "hip" -> AMD;
            case "nvidia", "cuda" -> NVIDIA;
            case "legacy" -> LEGACY;
            case "auto", "" -> AUTO;
            default -> AUTO;
        };
    }
}
