import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Offline regression for the live P23 Mega filter contract. */
public final class P23MegaRegression {
    private static final Path OBSERVED_30K = Path.of("testdata", "p23_observed_30k_survivors.csv");

    public static void main(String[] args) throws Exception {
        SearchSettings general = new SearchSettings();
        general.hunterMode = true;
        general.megaMode = false;

        SearchSettings mega = new SearchSettings();
        mega.hunterMode = true;
        mega.megaMode = true;

        require(!MegaFilter.rejects(general, 4.50D, false, 0, 1),
                "General mode must never apply the Mega topology reject");
        require(MegaFilter.rejects(mega, 4.50D, false, 0, 10),
                "Mega must reject score<5 with FullY112==0");
        require(MegaFilter.rejects(mega, 4.50D, false, 3, 3),
                "Mega must reject score<5 with Y96 largest<4");
        require(!MegaFilter.rejects(mega, 5.00D, false, 0, 0),
                "Score 5.00 must survive the exclusive score<5 rule");
        require(!MegaFilter.rejects(mega, 4.00D, true, 0, 0),
                "Extreme topology bypass must always survive");
        require(MegaFilter.upperMin(mega) == 8, "Mega Upper gate must default to 8");
        require(MegaFilter.highMin(mega) == 6, "Mega High gate must default to 6");

        List<String> lines = Files.readAllLines(OBSERVED_30K);
        require(lines.size() > 1, "Missing observed 30k regression rows: " + OBSERVED_30K);

        String[] header = lines.get(0).split(",", -1);
        Map<String, Integer> col = new HashMap<>();
        for (int i = 0; i < header.length; i++) col.put(header[i], i);

        int total = 0;
        int independent = 0;
        int killed = 0;
        int upperKilled = 0;
        int highKilled = 0;
        int topologyKilled = 0;
        int p19Killed = 0;
        int coarseKilled = 0;
        int maxBlocks = 0;
        long maxSeed = 0L;

        for (int lineIndex = 1; lineIndex < lines.size(); lineIndex++) {
            String line = lines.get(lineIndex).trim();
            if (line.isEmpty()) continue;
            String[] v = line.split(",", -1);

            String source = value(v, col, "sourceRun");
            long seed = Long.parseLong(value(v, col, "seed"));
            int blocks = (int) Math.round(Double.parseDouble(value(v, col, "blocks")));
            int p20 = intValue(v, col, "nativeP20");
            int upper = intValue(v, col, "nativeUpper");
            int high = intValue(v, col, "nativeHigh");
            double p19Score = Double.parseDouble(value(v, col, "nativeP19Score"));
            boolean extreme = Boolean.parseBoolean(value(v, col, "nativeP19Extreme"));
            int fullY112 = intValue(v, col, "nativeFullY112");
            int y96Largest = intValue(v, col, "nativeY96Largest");
            int coarse = intValue(v, col, "nativeCoarse");

            total++;
            if (source.contains("independent")) independent++;
            if (blocks > maxBlocks) {
                maxBlocks = blocks;
                maxSeed = seed;
            }

            boolean survive = true;
            if (p20 <= 0) survive = false;
            if (upper < MegaFilter.upperMin(mega)) {
                survive = false;
                upperKilled++;
            }
            if (high < MegaFilter.highMin(mega)) {
                survive = false;
                highKilled++;
            }
            if (!(extreme || p19Score >= P19MonsterGate.THRESHOLD)) {
                survive = false;
                p19Killed++;
            }
            if (MegaFilter.rejects(mega, p19Score, extreme, fullY112, y96Largest)) {
                survive = false;
                topologyKilled++;
            }
            if (coarse < 85) {
                survive = false;
                coarseKilled++;
            }
            if (!survive) {
                killed++;
                System.out.println("KILLED observed 30k seed=" + seed + " blocks=" + blocks
                        + " upper=" + upper + " high=" + high + " p19=" + p19Score
                        + " extreme=" + extreme + " fullY112=" + fullY112
                        + " y96Largest=" + y96Largest + " coarse=" + coarse);
            }
        }

        System.out.println("P23 MEGA REGRESSION");
        System.out.println("Observed 30k+ rows:       " + total);
        System.out.println("Independent rows:         " + independent);
        System.out.println("Upper 8 kills:            " + upperKilled);
        System.out.println("High 6 kills:             " + highKilled);
        System.out.println("P19 current-gate kills:   " + p19Killed);
        System.out.println("Topology union kills:     " + topologyKilled);
        System.out.println("Coarse 85 kills:          " + coarseKilled);
        System.out.println("Total Mega kills:         " + killed);
        System.out.println("Largest regression seed:  " + maxSeed + " | " + maxBlocks + " blocks");

        require(total == 257, "Expected 257 observed 30k+ islands, got " + total);
        require(independent == 10, "Expected 10 independent-run 30k+ islands, got " + independent);
        require(killed == 0, "P23 Mega killed observed 30k+ islands");
        System.out.println("P23 MEGA REGRESSION PASSED");
    }

    private static int intValue(String[] values, Map<String, Integer> columns, String name) {
        return (int) Math.round(Double.parseDouble(value(values, columns, name)));
    }

    private static String value(String[] values, Map<String, Integer> columns, String name) {
        Integer index = columns.get(name);
        if (index == null || index >= values.length) throw new IllegalArgumentException("Missing column: " + name);
        return values[index];
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
}
