public final class P24GpuModeExactness {
    private static final int DEFAULT_COUNT = 8192;
    private static final long DEFAULT_SEQUENCE = 123456789L;

    private P24GpuModeExactness() { }

    public static void main(String[] args) throws Exception {
        int count = args.length >= 1 ? Integer.parseInt(args[0]) : DEFAULT_COUNT;
        long sequence = args.length >= 2 ? Long.parseLong(args[1]) : DEFAULT_SEQUENCE;
        if (count < 1) throw new IllegalArgumentException("count must be >= 1");

        long[] seeds = new long[count];
        for (int i = 0; i < count; i++) seeds[i] = deterministicSeed(sequence, i);

        GpuStage0Scout.BatchResult general;
        GpuStage0Scout.BatchResult mega;
        GpuStage0Scout.BatchResult megaLean;
        try (GpuStage0Scout scout = new GpuStage0Scout(count, false, true, System.out::println)) {
            general = copy(scout.filter(seeds, count), count);
        }
        try (GpuStage0Scout scout = new GpuStage0Scout(count, true, true, System.out::println)) {
            mega = copy(scout.filter(seeds, count), count);
        }
        try (GpuStage0Scout scout = new GpuStage0Scout(count, true, false, System.out::println)) {
            megaLean = copy(scout.filter(seeds, count), count);
        }

        int comparedP19 = 0;
        int expectedMegaRejects = 0;
        int expectedMegaCoarse = 0;
        int megaFinalPasses = 0;

        for (int i = 0; i < count; i++) {
            int gp20 = general.p20Counts[i] & 0xFF;
            int gUpper = general.fullUpperCounts[i] & 0xFF;
            int gHigh = general.highReentryCounts[i] & 0xFF;
            int mp20 = mega.p20Counts[i] & 0xFF;
            int mUpper = mega.fullUpperCounts[i] & 0xFF;
            int mHigh = mega.highReentryCounts[i] & 0xFF;

            require(mp20 == (megaLean.p20Counts[i] & 0xFF), i, seeds[i],
                    "Research/lean P20 mismatch", mp20, megaLean.p20Counts[i] & 0xFF);
            require(mUpper == (megaLean.fullUpperCounts[i] & 0xFF), i, seeds[i],
                    "Research/lean Upper mismatch", mUpper, megaLean.fullUpperCounts[i] & 0xFF);
            require(mHigh == (megaLean.highReentryCounts[i] & 0xFF), i, seeds[i],
                    "Research/lean High mismatch", mHigh, megaLean.highReentryCounts[i] & 0xFF);
            require((mega.p19Pass[i] & 0xFF) == (megaLean.p19Pass[i] & 0xFF), i, seeds[i],
                    "Research/lean P19 mismatch", mega.p19Pass[i] & 0xFF, megaLean.p19Pass[i] & 0xFF);
            require((mega.p19ExtremeBypass[i] & 0xFF) == (megaLean.p19ExtremeBypass[i] & 0xFF), i, seeds[i],
                    "Research/lean extreme mismatch", mega.p19ExtremeBypass[i] & 0xFF, megaLean.p19ExtremeBypass[i] & 0xFF);
            require((mega.megaTopologyRejected[i] & 0xFF) == (megaLean.megaTopologyRejected[i] & 0xFF), i, seeds[i],
                    "Research/lean Mega reject mismatch", mega.megaTopologyRejected[i] & 0xFF, megaLean.megaTopologyRejected[i] & 0xFF);
            require(mega.coarseScores[i] == megaLean.coarseScores[i], i, seeds[i],
                    "Research/lean coarse mismatch", mega.coarseScores[i], megaLean.coarseScores[i]);

            require(gp20 == mp20, i, seeds[i], "P20 mismatch", gp20, mp20);
            require(gUpper == mUpper, i, seeds[i], "Upper mismatch", gUpper, mUpper);
            require((general.megaTopologyRejected[i] & 0xFF) == 0,
                    i, seeds[i], "General worker set Mega reject flag", 0,
                    general.megaTopologyRejected[i] & 0xFF);

            boolean reachesMegaP19 = gp20 > 0 && gUpper >= 8 && gHigh >= 6;
            if (!reachesMegaP19) {
                require((mega.p19Pass[i] & 0xFF) == 0, i, seeds[i],
                        "Mega evaluated P19 for an early reject", 0, mega.p19Pass[i] & 0xFF);
                require((mega.megaTopologyRejected[i] & 0xFF) == 0, i, seeds[i],
                        "Mega topology flag set for an early reject", 0, mega.megaTopologyRejected[i] & 0xFF);
                require(mega.coarseScores[i] == 0, i, seeds[i],
                        "Mega coarse score set for an early reject", 0, mega.coarseScores[i]);
                continue;
            }

            comparedP19++;
            require(gHigh == mHigh, i, seeds[i], "High mismatch after Upper>=8", gHigh, mHigh);
            require((general.p19Pass[i] & 0xFF) == (mega.p19Pass[i] & 0xFF),
                    i, seeds[i], "P19 pass mismatch", general.p19Pass[i] & 0xFF, mega.p19Pass[i] & 0xFF);
            compareP19Telemetry(i, seeds[i], general, mega);

            boolean p19Pass = (general.p19Pass[i] & 0xFF) != 0;
            boolean extreme = (general.p19ExtremeBypass[i] & 0xFF) != 0;
            double score = general.p19Scores[i];
            boolean expectedReject = p19Pass
                    && !extreme
                    && score < 5.0D
                    && (general.p19FullY112[i] == 0 || general.p19Y96LargestCluster[i] < 4);
            int actualReject = mega.megaTopologyRejected[i] & 0xFF;
            require(actualReject == (expectedReject ? 1 : 0), i, seeds[i],
                    "Mega topology decision mismatch", expectedReject ? 1 : 0, actualReject);

            if (expectedReject) {
                expectedMegaRejects++;
                require(mega.coarseScores[i] == 0, i, seeds[i],
                        "Rejected Mega seed still received coarse work", 0, mega.coarseScores[i]);
            } else if (p19Pass) {
                expectedMegaCoarse++;
                require(mega.coarseScores[i] == general.coarseScores[i], i, seeds[i],
                        "Mega/general coarse mismatch", general.coarseScores[i], mega.coarseScores[i]);
                if (mega.coarseScores[i] >= 85) megaFinalPasses++;
            } else {
                require(mega.coarseScores[i] == 0, i, seeds[i],
                        "P19 reject received Mega coarse work", 0, mega.coarseScores[i]);
            }
        }

        if (comparedP19 == 0) throw new IllegalStateException("No seeds reached Mega P19 in exactness sample");
        if (expectedMegaRejects == 0) throw new IllegalStateException("No Mega topology rejects observed; increase sample size");

        System.out.println("P24 GPU MODE EXACTNESS");
        System.out.println("Seeds compared:           " + count);
        System.out.println("Reached Mega P19:         " + comparedP19);
        System.out.println("Mega topology rejects:    " + expectedMegaRejects);
        System.out.println("Mega coarse inputs:       " + expectedMegaCoarse);
        System.out.println("Mega coarse>=85 passes:   " + megaFinalPasses);
        System.out.println("Lean/research outputs:    identical for live decision fields");
        System.out.println("P24 GPU MODE EXACTNESS PASSED");
    }

    private static void compareP19Telemetry(
            int i,
            long seed,
            GpuStage0Scout.BatchResult a,
            GpuStage0Scout.BatchResult b
    ) {
        require((a.p19ExtremeBypass[i] & 0xFF) == (b.p19ExtremeBypass[i] & 0xFF),
                i, seed, "P19 extreme mismatch", a.p19ExtremeBypass[i] & 0xFF, b.p19ExtremeBypass[i] & 0xFF);
        long aBits = Double.doubleToRawLongBits(a.p19Scores[i]);
        long bBits = Double.doubleToRawLongBits(b.p19Scores[i]);
        if (aBits != bBits) {
            throw fail(i, seed, "P19 score bits mismatch", Long.toUnsignedString(aBits), Long.toUnsignedString(bBits));
        }
        require(a.p19FullY88[i] == b.p19FullY88[i], i, seed, "FullY88 mismatch", a.p19FullY88[i], b.p19FullY88[i]);
        require(a.p19FullY96[i] == b.p19FullY96[i], i, seed, "FullY96 mismatch", a.p19FullY96[i], b.p19FullY96[i]);
        require(a.p19FullY104[i] == b.p19FullY104[i], i, seed, "FullY104 mismatch", a.p19FullY104[i], b.p19FullY104[i]);
        require(a.p19FullY112[i] == b.p19FullY112[i], i, seed, "FullY112 mismatch", a.p19FullY112[i], b.p19FullY112[i]);
        require(a.p19Y96LargestCluster[i] == b.p19Y96LargestCluster[i], i, seed,
                "Y96 largest-cluster mismatch", a.p19Y96LargestCluster[i], b.p19Y96LargestCluster[i]);
    }

    private static GpuStage0Scout.BatchResult copy(GpuStage0Scout.BatchResult r, int n) {
        return new GpuStage0Scout.BatchResult(
                java.util.Arrays.copyOf(r.p20Counts, n),
                java.util.Arrays.copyOf(r.fullUpperCounts, n),
                java.util.Arrays.copyOf(r.highReentryCounts, n),
                java.util.Arrays.copyOf(r.p19Pass, n),
                java.util.Arrays.copyOf(r.megaTopologyRejected, n),
                java.util.Arrays.copyOf(r.coarseScores, n),
                java.util.Arrays.copyOf(r.p19ExtremeBypass, n),
                java.util.Arrays.copyOf(r.p19Scores, n),
                java.util.Arrays.copyOf(r.p19FullY88, n),
                java.util.Arrays.copyOf(r.p19FullY96, n),
                java.util.Arrays.copyOf(r.p19FullY104, n),
                java.util.Arrays.copyOf(r.p19FullY112, n),
                java.util.Arrays.copyOf(r.p19Y88LargestCluster, n),
                java.util.Arrays.copyOf(r.p19Y88Width, n),
                java.util.Arrays.copyOf(r.p19Y88Depth, n),
                java.util.Arrays.copyOf(r.p19Y88TouchesBorder, n),
                java.util.Arrays.copyOf(r.p19Y96LargestCluster, n),
                java.util.Arrays.copyOf(r.p19Y96Width, n),
                java.util.Arrays.copyOf(r.p19Y96Depth, n),
                java.util.Arrays.copyOf(r.p19Y96TouchesBorder, n)
        );
    }

    private static void require(boolean ok, int index, long seed, String message, int expected, int actual) {
        if (!ok) throw fail(index, seed, message, Integer.toString(expected), Integer.toString(actual));
    }

    private static IllegalStateException fail(int index, long seed, String message, String expected, String actual) {
        return new IllegalStateException(message + " | index=" + index + " seed=" + seed
                + " expected=" + expected + " actual=" + actual);
    }

    private static long deterministicSeed(long sequenceSeed, long attempt) {
        long z = sequenceSeed + attempt * 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }
}
