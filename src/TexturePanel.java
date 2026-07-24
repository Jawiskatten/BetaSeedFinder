import javax.swing.*;
import java.awt.*;

public class TexturePanel extends JPanel {
    @SuppressWarnings("unused")
    private final String texturePath;
    private final Color overlayColor;
    private final int overlayAlpha;

    public TexturePanel(LayoutManager layout, String texturePath, Color overlayColor, int overlayAlpha) {
        super(layout);
        this.texturePath = texturePath;
        this.overlayColor = overlayColor;
        this.overlayAlpha = overlayAlpha;
        setOpaque(true);
        setBackground(MinecraftTheme.STONE_DARK);
    }

    public TexturePanel(String texturePath, Color overlayColor, int overlayAlpha) {
        this(new BorderLayout(), texturePath, overlayColor, overlayAlpha);
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);

        Graphics2D g2 = (Graphics2D) g.create();
        g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);

        int w = getWidth();
        int h = getHeight();

        Color base = getBackground() != null ? getBackground() : MinecraftTheme.STONE_DARK;
        Color top = shift(base, 8);
        Color bottom = shift(base, -6);

        g2.setPaint(new GradientPaint(0, 0, top, 0, h, bottom));
        g2.fillRect(0, 0, w, h);

        g2.setColor(new Color(255, 255, 255, 8));
        g2.fillRect(0, 0, w, Math.min(12, h));

        g2.setColor(new Color(255, 255, 255, 14));
        g2.drawLine(0, 0, Math.max(0, w - 1), 0);

        g2.setColor(new Color(0, 0, 0, 20));
        g2.drawLine(0, Math.max(0, h - 1), Math.max(0, w - 1), Math.max(0, h - 1));

        if (overlayColor != null && overlayAlpha > 0) {
            g2.setColor(new Color(
                    overlayColor.getRed(),
                    overlayColor.getGreen(),
                    overlayColor.getBlue(),
                    Math.max(0, Math.min(255, overlayAlpha))
            ));
            g2.fillRect(0, 0, w, h);
        }

        g2.dispose();
    }

    private static Color shift(Color c, int amount) {
        return new Color(clamp(c.getRed() + amount), clamp(c.getGreen() + amount), clamp(c.getBlue() + amount));
    }

    private static int clamp(int value) {
        return Math.max(0, Math.min(255, value));
    }
}
