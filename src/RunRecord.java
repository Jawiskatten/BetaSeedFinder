import java.nio.file.Path;
import java.time.Instant;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.time.format.DateTimeFormatter;
import java.util.Collections;
import java.util.List;
import java.util.Map;

public class RunRecord {
    private static final DateTimeFormatter DATE_FORMAT = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm");

    public final Path folder;
    public final Map<String, String> manifest;
    public final String build;
    public final String status;
    public final String runLabel;
    public final long startEpochMs;
    public final long endEpochMs;
    public final double elapsedSeconds;
    public final long checked;
    public final int matches;
    public final int topUpdates;
    public final boolean deterministic;

    private volatile List<IslandRecord> islands;
    private Boolean includeOverride;

    public RunRecord(Path folder, Map<String, String> manifest, Boolean includeOverride) {
        this.folder = folder;
        this.manifest = Collections.unmodifiableMap(manifest);
        this.build = manifest.getOrDefault("build", "Unknown build");
        this.status = manifest.getOrDefault("status", "UNKNOWN");
        this.runLabel = manifest.getOrDefault("runLabel", folder.getFileName().toString());
        this.startEpochMs = parseLong(manifest.get("startEpochMs"), 0L);
        this.endEpochMs = parseLong(manifest.get("endEpochMs"), 0L);
        this.elapsedSeconds = parseDouble(manifest.get("elapsedSeconds"), 0.0);
        this.checked = parseLong(manifest.get("checked"), 0L);
        this.matches = (int) parseLong(manifest.get("matches"), 0L);
        this.topUpdates = (int) parseLong(manifest.get("topUpdates"), 0L);
        this.deterministic = Boolean.parseBoolean(manifest.getOrDefault("deterministicSeedMode", "false"));
        this.includeOverride = includeOverride;
    }

    public synchronized List<IslandRecord> islands(RunRepository repository) {
        if (islands == null) {
            islands = repository.loadIslands(this);
        }
        return islands;
    }

    public synchronized void clearIslandCache() {
        islands = null;
    }

    public boolean includedInTotals() {
        if (includeOverride != null) {
            return includeOverride;
        }
        return defaultIncludedInTotals();
    }

    public void setIncludeOverride(Boolean includeOverride) {
        this.includeOverride = includeOverride;
    }

    public boolean defaultIncludedInTotals() {
        String label = runLabel.toLowerCase();
        String folderName = folder.getFileName().toString().toLowerCase();
        boolean looksLikeBenchmark = label.contains("benchmark") || folderName.contains("benchmark")
                || label.contains("test") || label.contains("dev");
        boolean looksLikeHunt = label.contains("overnight") || label.contains("gui") || label.contains("hunt")
                || folderName.contains("overnight") || folderName.contains("hunt");

        return !deterministic && !looksLikeBenchmark && looksLikeHunt && checked >= 1_000_000L;
    }

    public double averageSpeed() {
        return elapsedSeconds > 0.0 ? checked / elapsedSeconds : 0.0;
    }

    public String shortBuild() {
        int p = build.indexOf("P");
        if (p >= 0) {
            int end = p + 1;
            while (end < build.length() && Character.isDigit(build.charAt(end))) {
                end++;
            }
            if (end > p + 1) {
                return build.substring(p, end);
            }
        }
        return build;
    }

    public String dateText() {
        if (startEpochMs <= 0L) {
            return folder.getFileName().toString();
        }
        LocalDateTime dateTime = LocalDateTime.ofInstant(Instant.ofEpochMilli(startEpochMs), ZoneId.systemDefault());
        return DATE_FORMAT.format(dateTime);
    }

    public int bestBlocks(RunRepository repository) {
        int best = 0;
        for (IslandRecord island : islands(repository)) {
            best = Math.max(best, island.blocks);
        }
        return best;
    }

    public long countAtLeast(RunRepository repository, int threshold) {
        return islands(repository).stream().filter(i -> i.blocks >= threshold).count();
    }

    public String friendlyStatus() {
        String value = status == null ? "" : status.trim().toUpperCase();
        return switch (value) {
            case "FINISHED", "COMPLETE", "COMPLETED" -> "Completed";
            case "STOPPED" -> "Stopped by user";
            case "RUNNING" -> "Interrupted / unfinished";
            case "ERROR", "FAILED" -> "Error / interrupted";
            default -> value.isEmpty() ? "Unknown" : status;
        };
    }

    public String profileName() {
        String saved = manifest.getOrDefault("huntProfileName", "").trim();
        if (!saved.isEmpty()) {
            String lower = saved.toLowerCase();
            if (lower.contains("world")) return "World Record";
            if (lower.contains("record")) return "Record Hunt";
            if (lower.contains("mega")) return "Mega";
            if (lower.contains("general")) return "General";
            return saved;
        }
        String lower = (runLabel + " " + build).toLowerCase();
        if (lower.contains("world") || lower.contains("80k")) return "World Record";
        if (lower.contains("record") || lower.contains("60k")) return "Record Hunt";
        if (lower.contains("mega")) return "Mega";
        return "General / legacy";
    }

    public boolean regionsAreCheckedUnit() {
        return "radius7Regions".equals(manifest.get("checkedUnit"));
    }

    public long worldsProcessed() {
        if (!regionsAreCheckedUnit()) return checked;
        int centers = (int) parseLong(manifest.get("gpuCoverageCentersPerWorld"), 8L);
        return centers > 0 ? checked / centers : checked;
    }

    private static long parseLong(String text, long fallback) {
        if (text == null) return fallback;
        try {
            return Long.parseLong(text.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static double parseDouble(String text, double fallback) {
        if (text == null) return fallback;
        try {
            return Double.parseDouble(text.trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }
}
