public final class UiFormat {
    private UiFormat() {}

    public static String compact(long value) {
        if (Math.abs(value) >= 1_000_000_000L) return String.format("%.2fB", value / 1_000_000_000.0);
        if (Math.abs(value) >= 1_000_000L) return String.format("%.2fM", value / 1_000_000.0);
        if (Math.abs(value) >= 1_000L) return String.format("%.1fk", value / 1_000.0);
        return Long.toString(value);
    }

    public static String compact(double value) {
        if (Math.abs(value) >= 1_000_000_000.0) return String.format("%.2fB", value / 1_000_000_000.0);
        if (Math.abs(value) >= 1_000_000.0) return String.format("%.2fM", value / 1_000_000.0);
        if (Math.abs(value) >= 1_000.0) return String.format("%.1fk", value / 1_000.0);
        return String.format("%.1f", value);
    }

    public static String duration(double seconds) {
        long total = Math.max(0L, Math.round(seconds));
        long hours = total / 3600;
        long minutes = (total % 3600) / 60;
        long secs = total % 60;
        if (hours > 0) return String.format("%dh %02dm", hours, minutes);
        if (minutes > 0) return String.format("%dm %02ds", minutes, secs);
        return secs + "s";
    }
}
