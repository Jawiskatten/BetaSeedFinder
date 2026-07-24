import javax.swing.*;
import java.awt.*;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class StatisticsChartPanel extends JPanel {
    private enum Mode { BAR, LINE, RARITY }

    private Mode mode = Mode.BAR;
    private String title = "No data";
    private String yLabel = "Count";
    private final List<String> categories = new ArrayList<>();
    private final List<Double> seriesA = new ArrayList<>();
    private final List<Double> seriesB = new ArrayList<>();
    private String seriesAName = "Current";
    private String seriesBName = "All time";
    private final List<StatisticsCalculator.Point> linePoints = new ArrayList<>();
    private final List<StatisticsCalculator.RarityPoint> rarityPoints = new ArrayList<>();
    private double rarityTarget = Double.NaN;

    public StatisticsChartPanel() {
        setOpaque(true);
        setBackground(MinecraftTheme.PANEL_DARK);
        setBorder(MinecraftTheme.titled("Chart"));
        setPreferredSize(new Dimension(0, 360));
    }

    public void showBars(String title, String yLabel, Map<String, Double> values) {
        showBars(title, yLabel, values, null, "Current", "All time");
    }

    public void showBars(
            String title,
            String yLabel,
            Map<String, Double> valuesA,
            Map<String, Double> valuesB,
            String seriesAName,
            String seriesBName
    ) {
        this.mode = Mode.BAR;
        this.title = title;
        this.yLabel = yLabel;
        this.categories.clear();
        this.seriesA.clear();
        this.seriesB.clear();
        this.linePoints.clear();
        this.seriesAName = seriesAName;
        this.seriesBName = seriesBName;
        this.rarityPoints.clear();
        this.rarityTarget = Double.NaN;

        for (Map.Entry<String, Double> entry : valuesA.entrySet()) {
            categories.add(entry.getKey());
            seriesA.add(entry.getValue());
            seriesB.add(valuesB == null ? Double.NaN : valuesB.getOrDefault(entry.getKey(), 0.0));
        }
        repaint();
    }

    public void showLine(String title, String yLabel, List<StatisticsCalculator.Point> points) {
        this.mode = Mode.LINE;
        this.title = title;
        this.yLabel = yLabel;
        this.linePoints.clear();
        this.linePoints.addAll(points);
        this.categories.clear();
        this.seriesA.clear();
        this.seriesB.clear();
        this.rarityPoints.clear();
        this.rarityTarget = Double.NaN;
        repaint();
    }

    public void showRarityCurve(
            String title,
            List<StatisticsCalculator.RarityPoint> points,
            double target
    ) {
        this.mode = Mode.RARITY;
        this.title = title;
        this.yLabel = "One in X seeds";
        this.rarityPoints.clear();
        this.rarityPoints.addAll(points);
        this.rarityTarget = target;
        this.categories.clear();
        this.seriesA.clear();
        this.seriesB.clear();
        this.linePoints.clear();
        repaint();
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        Graphics2D g2 = (Graphics2D) g.create();
        try {
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
            g2.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);

            int w = getWidth();
            int h = getHeight();
            int left = mode == Mode.RARITY ? 118 : 72;
            int right = 24;
            int top = 62;
            int bottom = 55;
            int plotW = Math.max(1, w - left - right);
            int plotH = Math.max(1, h - top - bottom);

            g2.setColor(MinecraftTheme.TEXT);
            g2.setFont(MinecraftTheme.HEADER_FONT);
            g2.drawString(title, left, 43);

            g2.setColor(MinecraftTheme.PANEL_INNER);
            g2.fillRect(left, top, plotW, plotH);

            if (mode == Mode.RARITY) {
                drawRarity(g2, left, top, plotW, plotH);
                return;
            }

            double maxValue = maxValue();
            if (maxValue <= 0) maxValue = 1;
            double niceMax = niceCeil(maxValue);

            drawGrid(g2, left, top, plotW, plotH, niceMax);

            if (mode == Mode.BAR) {
                drawBars(g2, left, top, plotW, plotH, niceMax);
            } else {
                drawLine(g2, left, top, plotW, plotH, niceMax);
            }
        } finally {
            g2.dispose();
        }
    }

    private void drawGrid(Graphics2D g2, int left, int top, int plotW, int plotH, double maxValue) {
        DecimalFormat format = new DecimalFormat("0.##");
        g2.setFont(MinecraftTheme.SMALL_FONT);
        for (int i = 0; i <= 5; i++) {
            int y = top + plotH - (plotH * i / 5);
            g2.setColor(new Color(58, 60, 60));
            g2.drawLine(left, y, left + plotW, y);
            g2.setColor(MinecraftTheme.TEXT_DIM);
            String label = compact(maxValue * i / 5.0, format);
            int tw = g2.getFontMetrics().stringWidth(label);
            g2.drawString(label, left - tw - 8, y + 4);
        }
        g2.setColor(MinecraftTheme.BORDER_MID);
        g2.drawRect(left, top, plotW, plotH);

        g2.setColor(MinecraftTheme.TEXT_DIM);
        g2.drawString(yLabel, 8, top + 12);
    }

    private void drawBars(Graphics2D g2, int left, int top, int plotW, int plotH, double maxValue) {
        if (categories.isEmpty()) {
            drawEmpty(g2, left, top, plotW, plotH);
            return;
        }

        int groupW = Math.max(1, plotW / categories.size());
        boolean dual = seriesB.stream().anyMatch(v -> !Double.isNaN(v));
        int barW = dual ? Math.max(3, Math.min(24, groupW / 3)) : Math.max(4, Math.min(38, groupW / 2));

        for (int i = 0; i < categories.size(); i++) {
            int centerX = left + i * groupW + groupW / 2;
            double a = seriesA.get(i);
            int hA = (int) Math.round((a / maxValue) * plotH);
            g2.setColor(MinecraftTheme.BLUE_HIT);
            g2.fillRect(centerX - (dual ? barW : barW / 2), top + plotH - hA, barW, hA);

            if (dual) {
                double b = seriesB.get(i);
                int hB = (int) Math.round((b / maxValue) * plotH);
                g2.setColor(MinecraftTheme.GREEN);
                g2.fillRect(centerX + 1, top + plotH - hB, barW, hB);
            }

            g2.setColor(MinecraftTheme.TEXT_DIM);
            g2.setFont(MinecraftTheme.SMALL_FONT);
            String label = categories.get(i);
            int tw = g2.getFontMetrics().stringWidth(label);
            g2.drawString(label, centerX - tw / 2, top + plotH + 20);
        }

        if (dual) {
            int x = left + plotW - 210;
            int y = 43;
            g2.setColor(MinecraftTheme.BLUE_HIT);
            g2.fillRect(x, y - 8, 10, 10);
            g2.setColor(MinecraftTheme.TEXT);
            g2.drawString(seriesAName, x + 16, y);
            x += 100;
            g2.setColor(MinecraftTheme.GREEN);
            g2.fillRect(x, y - 8, 10, 10);
            g2.setColor(MinecraftTheme.TEXT);
            g2.drawString(seriesBName, x + 16, y);
        }
    }

    private void drawLine(Graphics2D g2, int left, int top, int plotW, int plotH, double maxValue) {
        if (linePoints.isEmpty()) {
            drawEmpty(g2, left, top, plotW, plotH);
            return;
        }

        double minX = linePoints.get(0).x;
        double maxX = linePoints.get(linePoints.size() - 1).x;
        if (maxX <= minX) maxX = minX + 1;

        g2.setColor(MinecraftTheme.BLUE_HIT);
        g2.setStroke(new BasicStroke(2f));

        int lastX = -1;
        int lastY = -1;
        for (StatisticsCalculator.Point point : linePoints) {
            int x = left + (int) Math.round((point.x - minX) / (maxX - minX) * plotW);
            int y = top + plotH - (int) Math.round(point.y / maxValue * plotH);
            if (lastX >= 0) {
                g2.drawLine(lastX, lastY, x, y);
            }
            g2.fillRect(x - 2, y - 2, 5, 5);
            lastX = x;
            lastY = y;
        }

        g2.setColor(MinecraftTheme.TEXT_DIM);
        g2.setFont(MinecraftTheme.SMALL_FONT);
        String first = linePoints.get(0).label;
        String last = linePoints.get(linePoints.size() - 1).label;
        g2.drawString(first == null ? "Start" : first, left, top + plotH + 20);
        int tw = g2.getFontMetrics().stringWidth(last == null ? "End" : last);
        g2.drawString(last == null ? "End" : last, left + plotW - tw, top + plotH + 20);
    }

    private void drawRarity(Graphics2D g2, int left, int top, int plotW, int plotH) {
        List<StatisticsCalculator.RarityPoint> valid = new ArrayList<>();
        for (StatisticsCalculator.RarityPoint point : rarityPoints) {
            if (isPositiveFinite(point.consensusOneInSeeds)
                    && isPositiveFinite(point.lowOneInSeeds)
                    && isPositiveFinite(point.highOneInSeeds)) {
                valid.add(point);
            }
        }
        if (valid.size() < 2) {
            drawEmpty(g2, left, top, plotW, plotH);
            return;
        }

        double minX = valid.get(0).target;
        double maxX = valid.get(valid.size() - 1).target;
        if (maxX <= minX) maxX = minX + 1.0;

        double minOneIn = Double.POSITIVE_INFINITY;
        double maxOneIn = 0.0;
        for (StatisticsCalculator.RarityPoint point : valid) {
            minOneIn = Math.min(minOneIn, point.lowOneInSeeds);
            maxOneIn = Math.max(maxOneIn, point.highOneInSeeds);
        }
        double minLog = Math.floor(Math.log10(minOneIn));
        double maxLog = Math.ceil(Math.log10(maxOneIn));
        if (maxLog - minLog < 2.0) maxLog = minLog + 2.0;

        drawRarityGrid(g2, left, top, plotW, plotH, minLog, maxLog, minX, maxX);

        Polygon band = new Polygon();
        for (StatisticsCalculator.RarityPoint point : valid) {
            band.addPoint(
                    xFor(point.target, minX, maxX, left, plotW),
                    yForLog(point.highOneInSeeds, minLog, maxLog, top, plotH)
            );
        }
        for (int i = valid.size() - 1; i >= 0; i--) {
            StatisticsCalculator.RarityPoint point = valid.get(i);
            band.addPoint(
                    xFor(point.target, minX, maxX, left, plotW),
                    yForLog(point.lowOneInSeeds, minLog, maxLog, top, plotH)
            );
        }
        Color blue = MinecraftTheme.BLUE_HIT;
        g2.setColor(new Color(blue.getRed(), blue.getGreen(), blue.getBlue(), 42));
        g2.fillPolygon(band);

        drawRarityBoundary(g2, valid, true, left, top, plotW, plotH, minX, maxX, minLog, maxLog);
        drawRarityBoundary(g2, valid, false, left, top, plotW, plotH, minX, maxX, minLog, maxLog);

        g2.setColor(MinecraftTheme.BLUE_HIT);
        g2.setStroke(new BasicStroke(2.5f));
        int lastX = -1;
        int lastY = -1;
        for (StatisticsCalculator.RarityPoint point : valid) {
            int x = xFor(point.target, minX, maxX, left, plotW);
            int y = yForLog(point.consensusOneInSeeds, minLog, maxLog, top, plotH);
            if (lastX >= 0) g2.drawLine(lastX, lastY, x, y);
            lastX = x;
            lastY = y;
        }

        if (Double.isFinite(rarityTarget) && rarityTarget >= minX && rarityTarget <= maxX) {
            StatisticsCalculator.RarityPoint nearest = valid.get(0);
            for (StatisticsCalculator.RarityPoint point : valid) {
                if (Math.abs(point.target - rarityTarget) < Math.abs(nearest.target - rarityTarget)) nearest = point;
            }
            int x = xFor(rarityTarget, minX, maxX, left, plotW);
            int y = yForLog(nearest.consensusOneInSeeds, minLog, maxLog, top, plotH);
            Stroke old = g2.getStroke();
            g2.setStroke(new BasicStroke(1f, BasicStroke.CAP_BUTT, BasicStroke.JOIN_MITER, 10f, new float[] {5f, 4f}, 0f));
            g2.setColor(MinecraftTheme.TEXT_DIM);
            g2.drawLine(x, top, x, top + plotH);
            g2.setStroke(old);
            g2.setColor(MinecraftTheme.TEXT);
            g2.fillRect(x - 3, y - 3, 7, 7);
            String label = "Target " + compact(rarityTarget, new DecimalFormat("0.##"));
            int tw = g2.getFontMetrics().stringWidth(label);
            int labelX = Math.min(left + plotW - tw - 4, Math.max(left + 4, x - tw / 2));
            g2.drawString(label, labelX, top + 16);
        }
    }

    private void drawRarityGrid(
            Graphics2D g2,
            int left,
            int top,
            int plotW,
            int plotH,
            double minLog,
            double maxLog,
            double minX,
            double maxX
    ) {
        int logRange = Math.max(1, (int) Math.ceil(maxLog - minLog));
        int step = Math.max(1, (int) Math.ceil(logRange / 6.0));
        g2.setFont(MinecraftTheme.SMALL_FONT);
        for (int power = (int) Math.ceil(minLog); power <= (int) Math.floor(maxLog); power += step) {
            double value = Math.pow(10.0, power);
            int y = yForLog(value, minLog, maxLog, top, plotH);
            g2.setColor(new Color(58, 60, 60));
            g2.drawLine(left, y, left + plotW, y);
            g2.setColor(MinecraftTheme.TEXT_DIM);
            String label = "1 in " + compact(value, new DecimalFormat("0.##"));
            int tw = g2.getFontMetrics().stringWidth(label);
            g2.drawString(label, left - tw - 8, y + 4);
        }

        for (int i = 0; i <= 4; i++) {
            double fraction = i / 4.0;
            int x = left + (int) Math.round(plotW * fraction);
            double value = minX + (maxX - minX) * fraction;
            g2.setColor(new Color(58, 60, 60));
            g2.drawLine(x, top, x, top + plotH);
            g2.setColor(MinecraftTheme.TEXT_DIM);
            String label = compact(value, new DecimalFormat("0.##"));
            int tw = g2.getFontMetrics().stringWidth(label);
            g2.drawString(label, x - tw / 2, top + plotH + 20);
        }

        g2.setColor(MinecraftTheme.BORDER_MID);
        g2.drawRect(left, top, plotW, plotH);
        g2.setColor(MinecraftTheme.TEXT_DIM);
        g2.drawString("One in X seeds", 8, top + 12);
    }

    private void drawRarityBoundary(
            Graphics2D g2,
            List<StatisticsCalculator.RarityPoint> points,
            boolean high,
            int left,
            int top,
            int plotW,
            int plotH,
            double minX,
            double maxX,
            double minLog,
            double maxLog
    ) {
        g2.setColor(MinecraftTheme.BORDER_LIGHT);
        g2.setStroke(new BasicStroke(1f));
        int lastX = -1;
        int lastY = -1;
        for (StatisticsCalculator.RarityPoint point : points) {
            double value = high ? point.highOneInSeeds : point.lowOneInSeeds;
            int x = xFor(point.target, minX, maxX, left, plotW);
            int y = yForLog(value, minLog, maxLog, top, plotH);
            if (lastX >= 0) g2.drawLine(lastX, lastY, x, y);
            lastX = x;
            lastY = y;
        }
    }

    private static int xFor(double value, double min, double max, int left, int width) {
        return left + (int) Math.round((value - min) / (max - min) * width);
    }

    private static int yForLog(double value, double minLog, double maxLog, int top, int height) {
        double log = Math.log10(value);
        double fraction = (log - minLog) / (maxLog - minLog);
        return top + height - (int) Math.round(fraction * height);
    }

    private static boolean isPositiveFinite(double value) {
        return value > 0.0 && Double.isFinite(value);
    }

    private void drawEmpty(Graphics2D g2, int left, int top, int plotW, int plotH) {
        g2.setColor(MinecraftTheme.TEXT_DIM);
        g2.setFont(MinecraftTheme.UI_FONT);
        String text = "No data for this view";
        int tw = g2.getFontMetrics().stringWidth(text);
        g2.drawString(text, left + (plotW - tw) / 2, top + plotH / 2);
    }

    private double maxValue() {
        double max = 0;
        if (mode == Mode.BAR) {
            for (double v : seriesA) max = Math.max(max, v);
            for (double v : seriesB) if (!Double.isNaN(v)) max = Math.max(max, v);
        } else {
            for (StatisticsCalculator.Point point : linePoints) max = Math.max(max, point.y);
        }
        return max;
    }

    private static double niceCeil(double value) {
        if (value <= 1) return 1;
        double power = Math.pow(10, Math.floor(Math.log10(value)));
        double normalized = value / power;
        double nice;
        if (normalized <= 1) nice = 1;
        else if (normalized <= 2) nice = 2;
        else if (normalized <= 5) nice = 5;
        else nice = 10;
        return nice * power;
    }

    private static String compact(double value, DecimalFormat format) {
        if (Math.abs(value) >= 1_000_000) return format.format(value / 1_000_000.0) + "M";
        if (Math.abs(value) >= 1_000) return format.format(value / 1_000.0) + "k";
        return format.format(value);
    }
}
