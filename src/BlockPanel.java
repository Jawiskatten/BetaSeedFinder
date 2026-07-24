import javax.swing.*;
import java.awt.*;
import java.util.Random;

public class BlockPanel extends JPanel {
    private final Color base;
    private final Color dark;
    private final Color light;
    private final int blockSize;

    public BlockPanel(LayoutManager layout, Color base) {
        this(layout, base, 8);
    }

    public BlockPanel(LayoutManager layout, Color base, int blockSize) {
        super(layout);
        this.base = base;
        this.blockSize = blockSize;

        this.dark = shift(base, -18);
        this.light = shift(base, 16);

        setOpaque(true);
        setBackground(base);
    }

    public BlockPanel(Color base) {
        this(new BorderLayout(), base, 8);
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);

        Graphics2D g2 = (Graphics2D) g.create();

        int fullW = getWidth();
        int fullH = getHeight();

        // Flat clean base everywhere, including title/border area.
        g2.setColor(base);
        g2.fillRect(0, 0, fullW, fullH);

        Insets insets = getInsets();

        int x0 = insets.left;
        int y0 = insets.top;
        int w = Math.max(0, fullW - insets.left - insets.right);
        int h = Math.max(0, fullH - insets.top - insets.bottom);

        // Texture only inside the actual content area.
        Shape oldClip = g2.getClip();
        g2.setClip(x0, y0, w, h);

        Random random = new Random(1337);

        for (int y = y0; y < y0 + h; y += blockSize) {
            for (int x = x0; x < x0 + w; x += blockSize) {
                int r = random.nextInt(100);

                if (r < 34) {
                    g2.setColor(dark);
                } else if (r < 58) {
                    g2.setColor(light);
                } else {
                    continue;
                }

                g2.fillRect(x, y, blockSize, blockSize);
            }
        }

        g2.setColor(new Color(0, 0, 0, 45));
        g2.fillRect(x0, y0, w, h);

        g2.setClip(oldClip);
        g2.dispose();
    }

    private static Color shift(Color c, int amount) {
        return new Color(
                clamp(c.getRed() + amount),
                clamp(c.getGreen() + amount),
                clamp(c.getBlue() + amount)
        );
    }

    private static int clamp(int v) {
        return Math.max(0, Math.min(255, v));
    }
}