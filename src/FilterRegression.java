import java.util.ArrayList;
import java.util.List;

public class FilterRegression {
    private record KnownSeed(long seed, int expectedBlocks, String note) {
    }

    public static void main(String[] args) {
        SearchSettings settings = currentHunterSettings();
        IslandSearchEngine engine = new IslandSearchEngine();

        List<KnownSeed> seeds = new ArrayList<>();
        seeds.add(new KnownSeed(-1731876557010487504L, 56404, "56k king"));
        seeds.add(new KnownSeed(4146589239694833506L, 44627, "44.6k dense"));
        seeds.add(new KnownSeed(-6883130948309166189L, 44516, "44.5k dense"));
        seeds.add(new KnownSeed(102348863809768193L, 42263, "139x47 giant"));
        seeds.add(new KnownSeed(-6737435262697316038L, 39379, "39.4k"));
        seeds.add(new KnownSeed(-1762684864290574874L, 39059, "39.1k thick"));
        seeds.add(new KnownSeed(-8213011358039210236L, 38938, "38.9k"));
        seeds.add(new KnownSeed(830086261255456863L, 38649, "90x89 continent"));
        seeds.add(new KnownSeed(7294127151031299012L, 38649, "same blocks, dense mountain"));
        seeds.add(new KnownSeed(-5802382150946236502L, 38055, "38.1k"));
        seeds.add(new KnownSeed(5805130109399864829L, 28883, "lowish coarse monster"));
        seeds.add(new KnownSeed(-1004569639183103174L, 23561, "coarse-92 outlier"));

        System.out.println("FilterRegression");
        System.out.println("Current gate: Y72>=2, Y88>=5, coarse>=85");
        System.out.println("Experimental shadow rule: Y96 largest cluster >= 3");
        System.out.println();
        System.out.printf("%-21s %7s %7s %5s %5s %5s %6s %8s %8s %10s %s%n",
                "seed", "blocks", "expect", "Y72", "Y88", "Y96", "Y96cl", "coarse", "current", "Y96cl>=3", "note");

        int changed = 0;
        int currentKilled = 0;
        int experimentalKilled = 0;

        for (KnownSeed known : seeds) {
            IslandSearchEngine.SeedDiagnostic d = engine.inspectSeed(known.seed, settings);

            boolean exactOk = d.blocks == known.expectedBlocks;
            if (!exactOk) {
                changed++;
            }
            if (!d.currentPipelinePass) {
                currentKilled++;
            }
            if (!d.experimentalY96Cluster3Pass) {
                experimentalKilled++;
            }

            System.out.printf("%-21d %7d %7d %5d %5d %5d %6d %8d %8s %10s %s%s%n",
                    d.seed,
                    d.blocks,
                    known.expectedBlocks,
                    d.stage0FullY72,
                    d.stage0FullY88,
                    d.stage0FullY96,
                    d.stage0Y96LargestCluster,
                    d.coarse,
                    d.currentPipelinePass ? "PASS" : "KILLED",
                    d.experimentalY96Cluster3Pass ? "PASS" : "KILLED",
                    known.note,
                    exactOk ? "" : "  <-- EXACT CHANGED");
        }

        System.out.println();
        System.out.println("Summary:");
        System.out.println("  Exact results changed: " + changed);
        System.out.println("  Current pipeline killed known seeds: " + currentKilled);
        System.out.println("  Experimental Y96 cluster>=3 killed known seeds: " + experimentalKilled);

        if (changed > 0 || currentKilled > 0) {
            System.err.println("REGRESSION FAILED. Do not trust the patch until the changed rows are explained.");
            System.exit(2);
        }

        System.out.println("REGRESSION PASSED for the current pipeline.");
        if (experimentalKilled > 0) {
            System.out.println("Y96 cluster>=3 is NOT safe on the known-seed set yet.");
        } else {
            System.out.println("Y96 cluster>=3 survived this known-seed set. Still needs shadow/audit validation.");
        }
    }

    private static SearchSettings currentHunterSettings() {
        SearchSettings settings = new SearchSettings();
        settings.chunkRadius = 7;
        settings.threads = 1;
        settings.minBlocks = 1000;
        settings.minColumns = 100;
        settings.minYForMatch = 60;
        settings.minWidth = 10;
        settings.minDepth = 10;
        settings.minAvgThickness = 2.0;
        settings.savePreviews = false;

        settings.hunterMode = true;
        settings.hunterCoarseMinCells = 85;
        settings.hunterStage0Enabled = true;
        settings.hunterStage0Step = 4;
        settings.hunterStage0MinReentrySamples = 2;
        settings.hunterStage0MinUpperYIndex = 9;
        settings.hunterStage0HighEnabled = true;
        settings.hunterStage0HighMinUpperYIndex = 11;
        settings.hunterStage0HighMinReentrySamples = 5;
        settings.performanceProfilerEnabled = false;
        return settings;
    }
}
