import javax.swing.*;
import java.awt.*;
import java.util.ArrayList;
import java.util.List;

public class HitGraphPanel extends JPanel {
    private static class Hit {
        final long checked;
        final int blocks;

        Hit(long checked, int blocks) {
            this.checked = checked;
            this.blocks = blocks;
        }
    }

    private static class PixelBucket {
        int count = 0;
        int maxBlocks = 0;
    }

    private final List<Hit> hits = new ArrayList<>();

    private long currentChecked = 0;
    private int maxBlocks = 1;
    private int bestBlocks = 0;
    private long bestChecked = 0;

    public HitGraphPanel() {
        setPreferredSize(new Dimension(0, 125));
        setMinimumSize(new Dimension(0, 100));
        setOpaque(true);
        setBackground(MinecraftTheme.PANEL_DARK);
        setBorder(MinecraftTheme.titled("Hit Graph"));
    }

    public synchronized void reset() {
        hits.clear();
        currentChecked = 0;
        maxBlocks = 1;
        bestBlocks = 0;
        bestChecked = 0;
        repaint();
    }

    public synchronized void setCurrentChecked(long checked) {
        currentChecked = Math.max(currentChecked, checked);
        repaint();
    }

    public synchronized void addHit(long checked, int blocks) {
        hits.add(new Hit(checked, blocks));

        currentChecked = Math.max(currentChecked, checked);
        maxBlocks = Math.max(maxBlocks, blocks);

        if (blocks > bestBlocks) {
            bestBlocks = blocks;
            bestChecked = checked;
        }

        repaint();
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);

        List<Hit> copy;
        long latestChecked;
        int localMaxBlocks;
        int localBestBlocks;
        long localBestChecked;

        synchronized (this) {
            copy = new ArrayList<>(hits);
            latestChecked = Math.max(1, currentChecked);
            localMaxBlocks = Math.max(1, maxBlocks);
            localBestBlocks = bestBlocks;
            localBestChecked = bestChecked;
        }

        int graphMax = niceCeil(localMaxBlocks);

        Graphics2D g2 = (Graphics2D) g.create();

        try {
            g2.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);

            int w = getWidth();
            int h = getHeight();

            int plotX = 50;
            int plotY = 34;
            int plotW = Math.max(1, w - 72);
            int plotH = Math.max(1, h - 58);
            int bottom = plotY + plotH;

            g2.setColor(MinecraftTheme.PANEL_INNER);
            g2.fillRect(plotX, plotY, plotW, plotH);

            g2.setColor(new Color(62, 62, 62));
            for (int i = 1; i <= 4; i++) {
                int y = plotY + (plotH * i / 5);
                g2.drawLine(plotX, y, plotX + plotW, y);
            }

            for (int i = 1; i <= 12; i++) {
                int x = plotX + (plotW * i / 12);
                g2.drawLine(x, plotY, x, bottom);
            }

            g2.setColor(new Color(95, 95, 95));
            g2.drawRect(plotX, plotY, plotW, plotH);

            g2.setFont(MinecraftTheme.UI_BOLD_FONT);
            g2.setColor(MinecraftTheme.TEXT);

            String infoText =
                    "Hits: " + copy.size()
                            + "   Checked: " + compact(latestChecked)
                            + "   Best: " + compact(localBestBlocks);

            int infoWidth = g2.getFontMetrics().stringWidth(infoText);
            g2.drawString(infoText, Math.max(plotX, plotX + plotW - infoWidth), 22);

            drawYAxisLabels(g2, graphMax, plotY, plotH, bottom);

            if (copy.isEmpty()) {
                g2.setFont(MinecraftTheme.UI_FONT);
                g2.setColor(MinecraftTheme.TEXT_DIM);

                String text = "No hits yet";
                int textW = g2.getFontMetrics().stringWidth(text);

                g2.drawString(
                        text,
                        plotX + (plotW - textW) / 2,
                        plotY + plotH / 2 + 4
                );

                return;
            }

            PixelBucket[] buckets = new PixelBucket[plotW + 1];

            for (int i = 0; i < buckets.length; i++) {
                buckets[i] = new PixelBucket();
            }

            for (Hit hit : copy) {
                int xIndex = (int) Math.round((hit.checked / (double) latestChecked) * plotW);

                if (xIndex < 0) {
                    xIndex = 0;
                }

                if (xIndex > plotW) {
                    xIndex = plotW;
                }

                buckets[xIndex].count++;
                buckets[xIndex].maxBlocks = Math.max(buckets[xIndex].maxBlocks, hit.blocks);
            }

            for (int xIndex = 0; xIndex < buckets.length; xIndex++) {
                PixelBucket bucket = buckets[xIndex];

                if (bucket.maxBlocks <= 0) {
                    continue;
                }

                double heightRatio = bucket.maxBlocks / (double) graphMax;
                int barHeight = Math.max(2, (int) Math.round(heightRatio * plotH));

                int x = plotX + xIndex;
                int y = bottom - barHeight;

                int alpha = Math.min(255, 120 + bucket.count * 20);

                g2.setStroke(new BasicStroke(2f));
                g2.setColor(new Color(
                        MinecraftTheme.BLUE_HIT.getRed(),
                        MinecraftTheme.BLUE_HIT.getGreen(),
                        MinecraftTheme.BLUE_HIT.getBlue(),
                        alpha
                ));
                g2.drawLine(x, bottom - 1, x, y);
            }

            if (localBestBlocks > 0 && localBestChecked > 0) {
                int bestXIndex = (int) Math.round((localBestChecked / (double) latestChecked) * plotW);

                if (bestXIndex < 0) {
                    bestXIndex = 0;
                }

                if (bestXIndex > plotW) {
                    bestXIndex = plotW;
                }

                int bestHeight = Math.max(
                        2,
                        (int) Math.round((localBestBlocks / (double) graphMax) * plotH)
                );

                int x = plotX + bestXIndex;
                int y = bottom - bestHeight;

                g2.setStroke(new BasicStroke(3f));
                g2.setColor(MinecraftTheme.RED_BEST);
                g2.drawLine(x, bottom - 1, x, y);
            }
        } finally {
            g2.dispose();
        }
    }

    private void drawYAxisLabels(Graphics2D g2, int graphMax, int plotY, int plotH, int bottom) {
        g2.setFont(MinecraftTheme.SMALL_FONT);
        g2.setColor(MinecraftTheme.TEXT_DIM);

        for (int i = 0; i <= 4; i++) {
            int value = graphMax - (graphMax * i / 4);
            int y = plotY + (plotH * i / 4) + 4;

            if (i == 4) {
                value = 0;
                y = bottom;
            }

            g2.drawString(compact(value), 18, y);
        }
    }

    private int niceCeil(int value) {
        if (value <= 1000) return 1000;
        if (value <= 2000) return 2000;
        if (value <= 5000) return 5000;
        if (value <= 10000) return 10000;
        if (value <= 15000) return 15000;
        if (value <= 20000) return 20000;
        if (value <= 30000) return 30000;
        if (value <= 50000) return 50000;

        int step = 25000;
        return ((value + step - 1) / step) * step;
    }

    private String compact(long value) {
        if (value >= 1_000_000_000L) {
            return String.format("%.1fB", value / 1_000_000_000.0);
        }

        if (value >= 1_000_000L) {
            return String.format("%.1fM", value / 1_000_000.0);
        }

        if (value >= 1_000L) {
            return String.format("%.1fk", value / 1_000.0);
        }

        return String.valueOf(value);
    }
}