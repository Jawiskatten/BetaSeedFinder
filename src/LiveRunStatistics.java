import java.util.ArrayList;
import java.util.List;

public class LiveRunStatistics {
    public static class Hit {
        public final long checked;
        public final int blocks;
        public final int columns;
        public final int footprintArea;

        Hit(long checked, int blocks, int columns, int footprintArea) {
            this.checked = checked;
            this.blocks = blocks;
            this.columns = columns;
            this.footprintArea = footprintArea;
        }
    }

    private final List<Hit> hits = new ArrayList<>();
    private boolean active;
    private long startNanos;
    private long checked;
    private int matches;
    private double speed;
    private int best;

    public synchronized void start() {
        hits.clear();
        active = true;
        startNanos = System.nanoTime();
        checked = 0L;
        matches = 0;
        speed = 0.0;
        best = 0;
    }

    public synchronized void progress(long checked, int matches, double speed) {
        this.checked = checked;
        this.matches = matches;
        this.speed = speed;
    }

    public synchronized void hit(long checked, int blocks) {
        hits.add(new Hit(checked, blocks, 0, 0));
        best = Math.max(best, blocks);
    }

    public synchronized void hit(long checked, SearchResult result) {
        if (result == null) return;
        hits.add(new Hit(
                checked,
                result.blocks,
                result.columns,
                Math.max(1, result.width * result.depth)
        ));
        best = Math.max(best, result.blocks);
    }

    public synchronized void finish() {
        active = false;
    }

    public synchronized boolean active() { return active; }
    public synchronized long checked() { return checked; }
    public synchronized int matches() { return matches; }
    public synchronized double speed() { return speed; }
    public synchronized int best() { return best; }

    public synchronized double elapsedSeconds() {
        if (startNanos == 0L) return 0.0;
        return Math.max(0.0, (System.nanoTime() - startNanos) / 1_000_000_000.0);
    }

    public synchronized List<Hit> hits() {
        return List.copyOf(hits);
    }

    public synchronized List<Integer> values(StatisticsCalculator.DistributionMetric metric) {
        List<Integer> values = new ArrayList<>(hits.size());
        for (Hit hit : hits) {
            int value = switch (metric) {
                case BLOCKS -> hit.blocks;
                case RAW_FOOTPRINT -> hit.footprintArea;
                case COLUMNS -> hit.columns;
            };
            if (value > 0) values.add(value);
        }
        return values;
    }

    public synchronized List<Integer> blockValues() {
        return values(StatisticsCalculator.DistributionMetric.BLOCKS);
    }

    public synchronized List<StatisticsCalculator.Point> bestProgression() {
        List<StatisticsCalculator.Point> points = new ArrayList<>();
        int runningBest = 0;
        for (Hit hit : hits) {
            if (hit.blocks > runningBest) {
                runningBest = hit.blocks;
                points.add(new StatisticsCalculator.Point(hit.checked, runningBest, UiFormat.compact(hit.checked)));
            }
        }
        return points;
    }
}
