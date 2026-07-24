import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class RunRepository {
    private static final String INCLUDE_FILE = "statistics_include.txt";
    private final Path runsRoot;
    private volatile List<RunRecord> cachedRuns = List.of();

    public RunRepository(Path runsRoot) {
        this.runsRoot = runsRoot;
    }

    public synchronized List<RunRecord> refresh() {
        List<RunRecord> runs = new ArrayList<>();
        try {
            Files.createDirectories(runsRoot);
            try (DirectoryStream<Path> stream = Files.newDirectoryStream(runsRoot)) {
                for (Path folder : stream) {
                    if (!Files.isDirectory(folder)) continue;
                    Path manifestPath = folder.resolve("manifest.txt");
                    if (!Files.isRegularFile(manifestPath)) continue;

                    Map<String, String> manifest = parseManifest(manifestPath);
                    Boolean override = readIncludeOverride(folder.resolve(INCLUDE_FILE));
                    runs.add(new RunRecord(folder, manifest, override));
                }
            }
        } catch (IOException e) {
            System.err.println("Could not scan run history: " + e.getMessage());
        }

        runs.sort(Comparator.comparingLong((RunRecord r) -> r.startEpochMs).reversed());
        cachedRuns = List.copyOf(runs);
        return cachedRuns;
    }

    public List<RunRecord> runs() {
        if (cachedRuns.isEmpty()) {
            return refresh();
        }
        return cachedRuns;
    }

    public RunRecord latestRun() {
        List<RunRecord> runs = runs();
        return runs.isEmpty() ? null : runs.get(0);
    }

    public List<RunRecord> includedRuns() {
        List<RunRecord> result = new ArrayList<>();
        for (RunRecord run : runs()) {
            if (run.includedInTotals()) {
                result.add(run);
            }
        }
        return result;
    }

    public synchronized void setIncludedInTotals(RunRecord run, boolean included) throws IOException {
        Path overridePath = run.folder.resolve(INCLUDE_FILE);
        Files.writeString(
                overridePath,
                Boolean.toString(included),
                StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING
        );
        run.setIncludeOverride(included);
    }

    public List<IslandRecord> loadIslands(RunRecord run) {
        Path csv = run.folder.resolve("hunter_candidates.csv");
        if (!Files.isRegularFile(csv)) {
            return List.of();
        }

        List<IslandRecord> islands = new ArrayList<>();
        try (BufferedReader reader = Files.newBufferedReader(csv, StandardCharsets.UTF_8)) {
            String headerLine = reader.readLine();
            if (headerLine == null || headerLine.isBlank()) {
                return List.of();
            }

            String[] headers = splitCsv(headerLine);
            Map<String, Integer> index = new HashMap<>();
            for (int i = 0; i < headers.length; i++) {
                index.put(headers[i].trim(), i);
            }

            String line;
            while ((line = reader.readLine()) != null) {
                if (line.isBlank()) continue;
                String[] row = splitCsv(line);

                boolean matched = getBoolean(row, index, "matched", false);
                if (!matched) continue;

                int blocks = getInt(row, index, "verifiedBlocks", 0);
                if (blocks <= 0) {
                    blocks = getInt(row, index, "rawBlocks", 0);
                }
                if (blocks <= 0) continue;

                int radius = getInt(row, index, "verifiedRadius", 0);
                if (radius <= 0) radius = getInt(row, index, "rawRadius", 0);

                boolean touches = getBoolean(row, index, "verifiedTouchesSide", false);
                String finalSource = getString(row, index, "finalSource", "");

                islands.add(new IslandRecord(
                        run,
                        getLong(row, index, "attempt", 0L),
                        getLong(row, index, "seed", 0L),
                        getInt(row, index, "stage0", 0),
                        getInt(row, index, "stage0Y88", 0),
                        getInt(row, index, "coarse", 0),
                        blocks,
                        getInt(row, index, "columns", 0),
                        getInt(row, index, "width", 0),
                        getInt(row, index, "depth", 0),
                        getInt(row, index, "footprintArea", 0),
                        getDouble(row, index, "fillPercent", 0.0),
                        getDouble(row, index, "avgThickness", 0.0),
                        getInt(row, index, "minY", 0),
                        getInt(row, index, "maxY", 0),
                        getInt(row, index, "centerX", 0),
                        getInt(row, index, "centerZ", 0),
                        getInt(row, index, "searchCenterChunkX", Integer.MIN_VALUE),
                        getInt(row, index, "searchCenterChunkZ", Integer.MIN_VALUE),
                        radius,
                        touches,
                        finalSource
                ));
            }
        } catch (IOException e) {
            System.err.println("Could not read " + csv + ": " + e.getMessage());
        }

        islands.sort(Comparator.comparingInt((IslandRecord i) -> i.blocks).reversed());
        return List.copyOf(islands);
    }

    public List<IslandRecord> allIslands(boolean includedOnly) {
        List<IslandRecord> islands = new ArrayList<>();
        for (RunRecord run : runs()) {
            if (includedOnly && !run.includedInTotals()) continue;
            islands.addAll(run.islands(this));
        }
        return islands;
    }

    private static Map<String, String> parseManifest(Path path) throws IOException {
        Map<String, String> values = new LinkedHashMap<>();
        for (String line : Files.readAllLines(path, StandardCharsets.UTF_8)) {
            int equals = line.indexOf('=');
            if (equals <= 0) continue;
            values.put(line.substring(0, equals).trim(), line.substring(equals + 1).trim());
        }
        return values;
    }

    private static Boolean readIncludeOverride(Path path) {
        if (!Files.isRegularFile(path)) return null;
        try {
            String value = Files.readString(path, StandardCharsets.UTF_8).trim();
            if ("true".equalsIgnoreCase(value)) return Boolean.TRUE;
            if ("false".equalsIgnoreCase(value)) return Boolean.FALSE;
        } catch (IOException ignored) {
        }
        return null;
    }

    private static String[] splitCsv(String line) {
        List<String> values = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        boolean quoted = false;

        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '"') {
                if (quoted && i + 1 < line.length() && line.charAt(i + 1) == '"') {
                    current.append('"');
                    i++;
                } else {
                    quoted = !quoted;
                }
            } else if (c == ',' && !quoted) {
                values.add(current.toString());
                current.setLength(0);
            } else {
                current.append(c);
            }
        }
        values.add(current.toString());
        return values.toArray(new String[0]);
    }

    private static String getString(String[] row, Map<String, Integer> index, String key, String fallback) {
        Integer i = index.get(key);
        if (i == null || i < 0 || i >= row.length) return fallback;
        return row[i].trim();
    }

    private static int getInt(String[] row, Map<String, Integer> index, String key, int fallback) {
        try {
            return Integer.parseInt(getString(row, index, key, Integer.toString(fallback)));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static long getLong(String[] row, Map<String, Integer> index, String key, long fallback) {
        try {
            return Long.parseLong(getString(row, index, key, Long.toString(fallback)));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static double getDouble(String[] row, Map<String, Integer> index, String key, double fallback) {
        try {
            return Double.parseDouble(getString(row, index, key, Double.toString(fallback)));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private static boolean getBoolean(String[] row, Map<String, Integer> index, String key, boolean fallback) {
        String text = getString(row, index, key, Boolean.toString(fallback));
        return Boolean.parseBoolean(text);
    }
}
