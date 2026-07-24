import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Offline evidence check for the intentionally empirical P38 record profiles. */
public final class P38RecordHuntValidation {
    private static final Path EVIDENCE = Path.of("testdata", "P38_record_hunt_evidence.csv");

    private P38RecordHuntValidation() { }

    public static void main(String[] args) throws Exception {
        List<String> lines = Files.readAllLines(EVIDENCE);
        require(lines.size() > 1, "Missing P38 evidence rows: " + EVIDENCE);
        String[] header = lines.get(0).split(",", -1);
        Map<String, Integer> col = new HashMap<>();
        for (int i = 0; i < header.length; i++) col.put(header[i], i);

        int total = 0, over60 = 0, over80 = 0, over100 = 0;
        int record60LostTargets = 0, safeCoarseLostTargets = 0;
        int record60Sacrificed50s = 0, safeCoarseSacrificedBelow80 = 0;
        int largestRecord60Sacrifice = 0;

        for (int row = 1; row < lines.size(); row++) {
            String line = lines.get(row).trim();
            if (line.isEmpty()) continue;
            String[] v = line.split(",", -1);
            int blocks = integer(v, col, "blocks");
            int p20 = integer(v, col, "p20");
            int upper = integer(v, col, "upper");
            int high = integer(v, col, "high");
            double p19 = decimal(v, col, "p19Score");
            int coarse = integer(v, col, "coarse");
            boolean pass60 = p20 >= 3 && upper >= 19 && high >= 12 && p19 >= 6.70 && coarse >= 95;
            boolean passSafeCoarse = p20 >= 3 && upper >= 19 && high >= 20 && p19 >= 6.70 && coarse >= 700;
            total++;
            if (blocks >= 60_000) {
                over60++;
                if (!pass60) record60LostTargets++;
            } else if (!pass60) {
                record60Sacrificed50s++;
                largestRecord60Sacrifice = Math.max(largestRecord60Sacrifice, blocks);
            }
            if (blocks >= 80_000) {
                over80++;
                if (!passSafeCoarse) safeCoarseLostTargets++;
            } else if (!passSafeCoarse) {
                safeCoarseSacrificedBelow80++;
            }
            if (blocks >= 100_000) over100++;
        }

        require(record60LostTargets == 0, "Record60 killed observed 60k+ targets: " + record60LostTargets);
        require(safeCoarseLostTargets == 0, "Safe coarse700 killed observed 80k+ targets: " + safeCoarseLostTargets);
        System.out.println("P38 RECORD-HUNT EVIDENCE VALIDATION PASSED");
        System.out.println("  evidence rows >=50k:             " + total);
        System.out.println("  observed 60k+ preserved:         " + over60 + " / " + over60);
        System.out.println("  observed 80k+ preserved:         " + over80 + " / " + over80);
        System.out.println("  observed 100k+ preserved:        " + over100 + " / " + over100);
        System.out.println("  Record60 lower rows lost:        " + record60Sacrificed50s
                + " (largest=" + largestRecord60Sacrifice + ")");
        System.out.println("  Safe coarse700 sub-80k rows lost: " + safeCoarseSacrificedBelow80);
        System.out.println("  113,331 row uses conservative known lower bounds from its passing Record60 path.");
        System.out.println("  WARNING: historical preservation is evidence, not a mathematical proof.");
    }

    private static int integer(String[] v, Map<String, Integer> col, String name) {
        return (int) Math.round(decimal(v, col, name));
    }

    private static double decimal(String[] v, Map<String, Integer> col, String name) {
        Integer index = col.get(name);
        if (index == null || index >= v.length) throw new IllegalArgumentException("Missing column " + name);
        return Double.parseDouble(v[index]);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
}
