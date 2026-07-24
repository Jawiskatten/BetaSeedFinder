import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.ToIntFunction;

public class StatisticsCalculator {
    public enum DistributionMetric {
        BLOCKS(
                "Blocks",
                "block",
                50_000,
                new int[] {20_000, 25_000, 30_000},
                new int[] {5_000, 10_000, 15_000, 20_000, 25_000, 30_000, 35_000, 40_000, 50_000},
                new String[] {"<5k", "5-10k", "10-15k", "15-20k", "20-25k", "25-30k", "30-35k", "35-40k", "40-50k", "50k+"},
                island -> island.blocks
        ),
        RAW_FOOTPRINT(
                "Raw footprint",
                "raw footprint area",
                9_000,
                new int[] {4_000, 5_000, 7_000},
                new int[] {1_000, 1_500, 2_000, 2_500, 3_000, 4_000, 5_000, 7_000, 9_000},
                new String[] {"<1k", "1-1.5k", "1.5-2k", "2-2.5k", "2.5-3k", "3-4k", "4-5k", "5-7k", "7-9k", "9k+"},
                island -> island.footprintArea
        ),
        COLUMNS(
                "Occupied columns",
                "occupied columns",
                3_500,
                new int[] {1_500, 2_000, 2_500},
                new int[] {500, 750, 1_000, 1_250, 1_500, 2_000, 2_500, 3_000, 3_500},
                new String[] {"<500", "500-750", "750-1k", "1-1.25k", "1.25-1.5k", "1.5-2k", "2-2.5k", "2.5-3k", "3-3.5k", "3.5k+"},
                island -> island.columns
        );

        public final String label;
        public final String chartLabel;
        public final int defaultTarget;
        private final int[] tailThresholds;
        private final int[] bounds;
        private final String[] bucketLabels;
        private final ToIntFunction<IslandRecord> accessor;

        DistributionMetric(
                String label,
                String chartLabel,
                int defaultTarget,
                int[] tailThresholds,
                int[] bounds,
                String[] bucketLabels,
                ToIntFunction<IslandRecord> accessor
        ) {
            this.label = label;
            this.chartLabel = chartLabel;
            this.defaultTarget = defaultTarget;
            this.tailThresholds = tailThresholds;
            this.bounds = bounds;
            this.bucketLabels = bucketLabels;
            this.accessor = accessor;
        }

        public int value(IslandRecord island) {
            return accessor.applyAsInt(island);
        }

        int bucketIndex(int value) {
            for (int i = 0; i < bounds.length; i++) {
                if (value < bounds[i]) return i;
            }
            return bucketLabels.length - 1;
        }

        String[] bucketLabels() {
            return bucketLabels;
        }

        public int[] tailThresholds() {
            return tailThresholds.clone();
        }

        @Override
        public String toString() {
            return label;
        }
    }

    public enum DistributionMode {
        RAW_COUNT("Raw count"),
        PER_MILLION("Per million seeds"),
        PERCENT_OF_MATCHES("Percentage of matches");

        public final String label;

        DistributionMode(String label) {
            this.label = label;
        }

        @Override
        public String toString() {
            return label;
        }
    }

    public static class Summary {
        public long seeds;
        public double runtimeSeconds;
        public long islands;
        public int best;
        public long count30k;
        public long count40k;
        public long count50k;

        public double averageSpeed() {
            return runtimeSeconds > 0 ? seeds / runtimeSeconds : 0.0;
        }
    }

    public static class MetricSummary {
        public int count;
        public double mean;
        public double stdDev;
        public double median;
        public double p90;
        public double p99;
        public double max;
    }

    public static class OddsResult {
        public int observations;
        public int exceedances;
        public double oneInMatches = Double.NaN;
        public double oneInSeeds = Double.NaN;
        public double hitsPer100M = Double.NaN;
        public double zScore = Double.NaN;
        public double futureChancePercent = Double.NaN;
    }

    public static class TailModelEstimate {
        public final String name;
        public final double oneInSeeds;

        public TailModelEstimate(String name, double oneInSeeds) {
            this.name = name;
            this.oneInSeeds = oneInSeeds;
        }
    }

    public static class TheoreticalOddsResult {
        public boolean valid;
        public String message = "";
        public int observations;
        public int observedExceedances;
        public int tailThreshold;
        public int tailObservations;
        public double target;
        public double maxObserved;
        public double observedOneInSeeds = Double.NaN;
        public double consensusOneInSeeds = Double.NaN;
        public double lowOneInSeeds = Double.NaN;
        public double highOneInSeeds = Double.NaN;
        public double futureChancePercent = Double.NaN;
        public double modelSpreadFactor = Double.NaN;
        public String confidence = "Unknown";
        public List<TailModelEstimate> models = List.of();
    }

    public static class RarityPoint {
        public final double target;
        public final double consensusOneInSeeds;
        public final double lowOneInSeeds;
        public final double highOneInSeeds;

        public RarityPoint(double target, double consensusOneInSeeds, double lowOneInSeeds, double highOneInSeeds) {
            this.target = target;
            this.consensusOneInSeeds = consensusOneInSeeds;
            this.lowOneInSeeds = lowOneInSeeds;
            this.highOneInSeeds = highOneInSeeds;
        }
    }

    public static class Point {
        public final double x;
        public final double y;
        public final String label;

        public Point(double x, double y, String label) {
            this.x = x;
            this.y = y;
            this.label = label;
        }
    }

    public Summary summarizeRuns(List<RunRecord> runs, RunRepository repository) {
        Summary summary = new Summary();
        for (RunRecord run : runs) {
            summary.seeds += run.checked;
            summary.runtimeSeconds += run.elapsedSeconds;
            for (IslandRecord island : run.islands(repository)) {
                summary.islands++;
                summary.best = Math.max(summary.best, island.blocks);
                if (island.blocks >= 30_000) summary.count30k++;
                if (island.blocks >= 40_000) summary.count40k++;
                if (island.blocks >= 50_000) summary.count50k++;
            }
        }
        return summary;
    }

    public Summary summarizeRun(RunRecord run, RunRepository repository) {
        return run == null ? new Summary() : summarizeRuns(List.of(run), repository);
    }

    public Map<String, Double> distribution(
            List<IslandRecord> islands,
            DistributionMetric metric,
            DistributionMode mode,
            long seeds
    ) {
        List<Integer> values = new ArrayList<>(islands.size());
        for (IslandRecord island : islands) values.add(metric.value(island));
        return distributionValues(values, metric, mode, seeds);
    }

    public Map<String, Double> distributionValues(
            List<Integer> values,
            DistributionMetric metric,
            DistributionMode mode,
            long seeds
    ) {
        String[] labels = metric.bucketLabels();
        long[] counts = new long[labels.length];
        for (int value : values) counts[metric.bucketIndex(value)]++;

        LinkedHashMap<String, Double> result = new LinkedHashMap<>();
        for (int i = 0; i < counts.length; i++) {
            double plotted;
            if (mode == DistributionMode.PER_MILLION) {
                plotted = seeds > 0 ? counts[i] * 1_000_000.0 / seeds : 0.0;
            } else if (mode == DistributionMode.PERCENT_OF_MATCHES) {
                plotted = values.isEmpty() ? 0.0 : counts[i] * 100.0 / values.size();
            } else {
                plotted = counts[i];
            }
            result.put(labels[i], plotted);
        }
        return result;
    }

    public List<Integer> metricValues(List<IslandRecord> islands, DistributionMetric metric) {
        List<Integer> values = new ArrayList<>(islands.size());
        for (IslandRecord island : islands) values.add(metric.value(island));
        return values;
    }

    public MetricSummary summarizeMetric(List<Integer> values) {
        MetricSummary summary = new MetricSummary();
        if (values == null || values.isEmpty()) return summary;

        List<Integer> sorted = new ArrayList<>(values);
        sorted.sort(Integer::compareTo);
        summary.count = sorted.size();

        double sum = 0.0;
        for (int value : sorted) sum += value;
        summary.mean = sum / sorted.size();

        double squared = 0.0;
        for (int value : sorted) {
            double d = value - summary.mean;
            squared += d * d;
        }
        summary.stdDev = Math.sqrt(squared / sorted.size());
        summary.median = percentile(sorted, 0.50);
        summary.p90 = percentile(sorted, 0.90);
        summary.p99 = percentile(sorted, 0.99);
        summary.max = sorted.get(sorted.size() - 1);
        return summary;
    }

    public OddsResult calculateOdds(
            List<Integer> values,
            long searchedSeeds,
            double target,
            long futureSeeds
    ) {
        OddsResult result = new OddsResult();
        if (values == null) return result;
        result.observations = values.size();

        MetricSummary summary = summarizeMetric(values);
        for (int value : values) {
            if (value >= target) result.exceedances++;
        }

        if (summary.stdDev > 0.0) {
            result.zScore = (target - summary.mean) / summary.stdDev;
        }

        if (result.exceedances > 0) {
            result.oneInMatches = values.size() / (double) result.exceedances;
            if (searchedSeeds > 0) {
                result.oneInSeeds = searchedSeeds / (double) result.exceedances;
                result.hitsPer100M = result.exceedances * 100_000_000.0 / searchedSeeds;
                if (futureSeeds > 0) {
                    double lambda = result.exceedances * (futureSeeds / (double) searchedSeeds);
                    result.futureChancePercent = -Math.expm1(-lambda) * 100.0;
                }
            }
        }
        return result;
    }

    public TheoreticalOddsResult calculateTheoreticalOdds(
            List<Integer> values,
            long searchedSeeds,
            DistributionMetric metric,
            double target,
            long futureSeeds,
            int requestedThreshold
    ) {
        TheoreticalOddsResult result = new TheoreticalOddsResult();
        result.target = target;
        if (values == null || values.isEmpty()) {
            result.message = "No islands available";
            return result;
        }
        if (searchedSeeds <= 0L) {
            result.message = "No searched-seed exposure available";
            return result;
        }
        if (!Double.isFinite(target) || target < 0.0) {
            result.message = "Invalid target";
            return result;
        }

        result.observations = values.size();
        result.maxObserved = values.stream().mapToInt(Integer::intValue).max().orElse(0);
        for (int value : values) {
            if (value >= target) result.observedExceedances++;
        }
        if (result.observedExceedances > 0) {
            result.observedOneInSeeds = searchedSeeds / (double) result.observedExceedances;
        }

        int threshold = resolveTailThreshold(values, metric, target, requestedThreshold);
        if (threshold <= 0) {
            result.message = "Target is below the available tail thresholds";
            return result;
        }
        result.tailThreshold = threshold;
        result.tailObservations = countAtLeast(values, threshold);

        if (target < threshold) {
            result.message = "Target must be at or above the tail threshold";
            return result;
        }
        if (result.tailObservations < 20) {
            result.message = "Too few tail observations (need at least 20)";
            return result;
        }

        List<FittedTailModel> fitted = fitTailModels(values, threshold);
        if (fitted.size() < 2) {
            result.message = "Could not fit enough independent tail models";
            return result;
        }

        double baseRatePerSeed = result.tailObservations / (double) searchedSeeds;
        double excess = Math.max(0.0, target - threshold);
        List<Double> finitePredictions = new ArrayList<>();
        List<TailModelEstimate> estimates = new ArrayList<>();
        for (FittedTailModel model : fitted) {
            double survival = model.survival(excess);
            double oneInSeeds = oneInSeeds(baseRatePerSeed, survival);
            estimates.add(new TailModelEstimate(model.name, oneInSeeds));
            if (Double.isFinite(oneInSeeds) && oneInSeeds > 0.0) {
                finitePredictions.add(oneInSeeds);
            }
        }
        result.models = List.copyOf(estimates);

        if (finitePredictions.size() < 2) {
            result.message = "Tail models diverged beyond a usable range";
            return result;
        }

        finitePredictions.sort(Double::compareTo);
        result.lowOneInSeeds = finitePredictions.get(0);
        result.highOneInSeeds = finitePredictions.get(finitePredictions.size() - 1);
        result.consensusOneInSeeds = geometricMedian(finitePredictions);
        result.modelSpreadFactor = result.highOneInSeeds / result.lowOneInSeeds;

        if (futureSeeds > 0L && result.consensusOneInSeeds > 0.0) {
            double lambda = futureSeeds / result.consensusOneInSeeds;
            result.futureChancePercent = -Math.expm1(-lambda) * 100.0;
        }

        result.confidence = confidenceLabel(
                result.tailObservations,
                result.modelSpreadFactor,
                target,
                result.maxObserved
        );
        result.valid = true;
        result.message = "Three-model extreme-tail estimate";
        return result;
    }

    public List<RarityPoint> rarityCurve(
            List<Integer> values,
            long searchedSeeds,
            DistributionMetric metric,
            double requestedTarget,
            int requestedThreshold
    ) {
        if (values == null || values.isEmpty() || searchedSeeds <= 0L) return List.of();
        int threshold = resolveTailThreshold(values, metric, requestedTarget, requestedThreshold);
        if (threshold <= 0 || countAtLeast(values, threshold) < 20) return List.of();

        List<FittedTailModel> models = fitTailModels(values, threshold);
        if (models.size() < 2) return List.of();

        int tailObservations = countAtLeast(values, threshold);
        double baseRatePerSeed = tailObservations / (double) searchedSeeds;
        double maxObserved = values.stream().mapToInt(Integer::intValue).max().orElse(threshold);
        double naturalEnd = Math.max(maxObserved * 1.25, threshold * 1.75);
        double targetEnd = Math.min(Math.max(requestedTarget, threshold) * 1.08, maxObserved * 3.0);
        double end = Math.max(naturalEnd, targetEnd);
        if (end <= threshold) end = threshold + 1.0;

        List<RarityPoint> points = new ArrayList<>();
        final int pointCount = 80;
        for (int i = 0; i < pointCount; i++) {
            double fraction = i / (double) (pointCount - 1);
            double target = threshold + (end - threshold) * fraction;
            double excess = target - threshold;

            List<Double> predictions = new ArrayList<>();
            for (FittedTailModel model : models) {
                double oneIn = oneInSeeds(baseRatePerSeed, model.survival(excess));
                if (Double.isFinite(oneIn) && oneIn > 0.0) predictions.add(oneIn);
            }
            if (predictions.size() < 2) continue;
            predictions.sort(Double::compareTo);
            points.add(new RarityPoint(
                    target,
                    geometricMedian(predictions),
                    predictions.get(0),
                    predictions.get(predictions.size() - 1)
            ));
        }
        return points;
    }

    private static int resolveTailThreshold(
            List<Integer> values,
            DistributionMetric metric,
            double target,
            int requestedThreshold
    ) {
        if (requestedThreshold > 0) return requestedThreshold;

        int selected = 0;
        for (int threshold : metric.tailThresholds()) {
            if (threshold <= target && countAtLeast(values, threshold) >= 40) {
                selected = threshold;
            }
        }
        if (selected > 0) return selected;

        for (int threshold : metric.tailThresholds()) {
            if (threshold <= target && countAtLeast(values, threshold) >= 20) {
                selected = threshold;
            }
        }
        return selected;
    }

    private static int countAtLeast(List<Integer> values, double threshold) {
        int count = 0;
        for (int value : values) {
            if (value >= threshold) count++;
        }
        return count;
    }

    private static List<FittedTailModel> fitTailModels(List<Integer> values, int threshold) {
        List<Double> excesses = new ArrayList<>();
        for (int value : values) {
            if (value >= threshold) excesses.add((double) value - threshold);
        }
        if (excesses.size() < 20) return List.of();

        List<FittedTailModel> models = new ArrayList<>();

        double mean = 0.0;
        for (double value : excesses) mean += value;
        mean /= excesses.size();
        if (mean > 0.0 && Double.isFinite(mean)) {
            models.add(FittedTailModel.exponential(mean));
        }

        double variance = 0.0;
        for (double value : excesses) {
            double d = value - mean;
            variance += d * d;
        }
        variance /= excesses.size();
        if (mean > 0.0 && variance > 0.0 && Double.isFinite(variance)) {
            double xi = 0.5 * (1.0 - (mean * mean / variance));
            xi = Math.max(-0.45, Math.min(0.95, xi));
            double beta = mean * (1.0 - xi);
            if (beta > 0.0 && Double.isFinite(beta)) {
                models.add(FittedTailModel.gpd(xi, beta));
            }
        }

        FittedTailModel weibull = fitWeibull(excesses);
        if (weibull != null) models.add(weibull);

        return models;
    }

    private static FittedTailModel fitWeibull(List<Double> excesses) {
        List<Double> sorted = new ArrayList<>(excesses);
        sorted.sort(Double::compareTo);
        int n = sorted.size();

        double sumX = 0.0;
        double sumY = 0.0;
        double sumXX = 0.0;
        double sumXY = 0.0;
        int count = 0;

        for (int i = 0; i < n; i++) {
            double excess = sorted.get(i);
            if (excess <= 0.0) continue;
            double survival = (n - i - 0.5) / n;
            if (survival <= 0.0 || survival >= 1.0) continue;

            double x = Math.log(excess);
            double y = Math.log(-Math.log(survival));
            if (!Double.isFinite(x) || !Double.isFinite(y)) continue;
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
            count++;
        }

        if (count < 10) return null;
        double denominator = count * sumXX - sumX * sumX;
        if (Math.abs(denominator) < 1e-12) return null;

        double shape = (count * sumXY - sumX * sumY) / denominator;
        double intercept = (sumY - shape * sumX) / count;
        if (!(shape > 0.05) || !Double.isFinite(shape)) return null;

        double scale = Math.exp(-intercept / shape);
        if (!(scale > 0.0) || !Double.isFinite(scale)) return null;
        return FittedTailModel.weibull(shape, scale);
    }

    private static double oneInSeeds(double baseRatePerSeed, double conditionalSurvival) {
        if (!(baseRatePerSeed > 0.0) || !(conditionalSurvival > 0.0)) {
            return Double.POSITIVE_INFINITY;
        }
        double probability = baseRatePerSeed * Math.min(1.0, conditionalSurvival);
        if (!(probability > 0.0) || !Double.isFinite(probability)) {
            return Double.POSITIVE_INFINITY;
        }
        return Math.max(1.0, 1.0 / probability);
    }

    private static double geometricMedian(List<Double> sortedValues) {
        if (sortedValues.isEmpty()) return Double.NaN;
        int middle = sortedValues.size() / 2;
        if ((sortedValues.size() & 1) == 1) return sortedValues.get(middle);
        double a = sortedValues.get(middle - 1);
        double b = sortedValues.get(middle);
        return Math.exp((Math.log(a) + Math.log(b)) * 0.5);
    }

    private static String confidenceLabel(int tailObservations, double spreadFactor, double target, double maxObserved) {
        double extrapolation = maxObserved > 0.0 ? target / maxObserved : Double.POSITIVE_INFINITY;
        if (spreadFactor > 50.0 || extrapolation > 1.75 || tailObservations < 30) return "Very low";
        if (tailObservations >= 100 && spreadFactor <= 4.0 && extrapolation <= 1.15) return "Good";
        if (tailObservations >= 50 && spreadFactor <= 10.0 && extrapolation <= 1.35) return "Moderate";
        return "Low";
    }

    private static class FittedTailModel {
        private static final int EXPONENTIAL = 0;
        private static final int GPD = 1;
        private static final int WEIBULL = 2;

        final String name;
        final int type;
        final double a;
        final double b;

        private FittedTailModel(String name, int type, double a, double b) {
            this.name = name;
            this.type = type;
            this.a = a;
            this.b = b;
        }

        static FittedTailModel exponential(double scale) {
            return new FittedTailModel("Exponential", EXPONENTIAL, scale, 0.0);
        }

        static FittedTailModel gpd(double xi, double beta) {
            return new FittedTailModel("GPD", GPD, xi, beta);
        }

        static FittedTailModel weibull(double shape, double scale) {
            return new FittedTailModel("Weibull", WEIBULL, shape, scale);
        }

        double survival(double excess) {
            if (excess <= 0.0) return 1.0;
            double value;
            if (type == EXPONENTIAL) {
                value = Math.exp(-excess / a);
            } else if (type == GPD) {
                double xi = a;
                double beta = b;
                if (Math.abs(xi) < 1e-9) {
                    value = Math.exp(-excess / beta);
                } else {
                    double z = 1.0 + xi * excess / beta;
                    if (z <= 0.0) return 0.0;
                    value = Math.pow(z, -1.0 / xi);
                }
            } else {
                double shape = a;
                double scale = b;
                value = Math.exp(-Math.pow(excess / scale, shape));
            }
            if (!Double.isFinite(value)) return value > 0.0 ? 1.0 : 0.0;
            return Math.max(0.0, Math.min(1.0, value));
        }
    }

    public List<Point> bestProgression(RunRecord run, RunRepository repository) {
        if (run == null) return List.of();
        List<IslandRecord> sorted = new ArrayList<>(run.islands(repository));
        sorted.sort(Comparator.comparingLong(i -> i.attempt));

        List<Point> points = new ArrayList<>();
        int best = 0;
        for (IslandRecord island : sorted) {
            if (island.blocks > best) {
                best = island.blocks;
                points.add(new Point(island.attempt, best, Long.toString(island.seed)));
            }
        }
        return points;
    }

    public List<Point> allTimeRecordProgression(List<RunRecord> runs, RunRepository repository) {
        List<RunRecord> chronological = new ArrayList<>(runs);
        chronological.sort(Comparator.comparingLong(r -> r.startEpochMs));

        List<Point> points = new ArrayList<>();
        int best = 0;
        int runIndex = 0;
        for (RunRecord run : chronological) {
            runIndex++;
            int runBest = run.bestBlocks(repository);
            if (runBest > best) {
                best = runBest;
                points.add(new Point(runIndex, best, run.dateText()));
            }
        }
        return points;
    }

    public List<Point> speedByRun(List<RunRecord> runs) {
        List<RunRecord> chronological = new ArrayList<>(runs);
        chronological.sort(Comparator.comparingLong(r -> r.startEpochMs));
        List<Point> points = new ArrayList<>();
        int index = 0;
        for (RunRecord run : chronological) {
            index++;
            points.add(new Point(index, run.averageSpeed(), run.shortBuild() + " | " + run.dateText()));
        }
        return points;
    }

    private static double percentile(List<Integer> sorted, double p) {
        if (sorted.isEmpty()) return 0.0;
        if (sorted.size() == 1) return sorted.get(0);
        double index = p * (sorted.size() - 1);
        int low = (int) Math.floor(index);
        int high = (int) Math.ceil(index);
        if (low == high) return sorted.get(low);
        double fraction = index - low;
        return sorted.get(low) + (sorted.get(high) - sorted.get(low)) * fraction;
    }
}
