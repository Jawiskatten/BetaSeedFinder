import javax.swing.border.AbstractBorder;
import java.awt.*;

public class MinecraftTitleBorder extends AbstractBorder {
    private static final int TOP_HEIGHT = 26;
    private static final int SIDE = 8;
    private static final int BOTTOM = 8;

    private final String title;

    public MinecraftTitleBorder(String title) {
        this.title = title;
    }

    public String getTitle() {
        return title;
    }

    @Override
    public Insets getBorderInsets(Component c) {
        return new Insets(TOP_HEIGHT + 10, SIDE, BOTTOM, SIDE);
    }

    @Override
    public Insets getBorderInsets(Component c, Insets insets) {
        insets.top = TOP_HEIGHT + 10;
        insets.left = SIDE;
        insets.bottom = BOTTOM;
        insets.right = SIDE;
        return insets;
    }

    @Override
    public void paintBorder(Component c, Graphics g, int x, int y, int width, int height) {
        Graphics2D g2 = (Graphics2D) g.create();
        g2.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);

        int w = width - 1;
        int h = height - 1;
        Color bg = c.getBackground() != null ? c.getBackground() : MinecraftTheme.STONE_DARK;
        Color strip = shift(bg, -4);
        Color accent = MinecraftTheme.BORDER_LIGHT;

        g2.setColor(MinecraftTheme.BORDER_DARK);
        g2.drawRect(x, y, w, h);
        g2.setColor(MinecraftTheme.BORDER_MID);
        g2.drawRect(x + 1, y + 1, w - 2, h - 2);

        g2.setColor(strip);
        g2.fillRect(x + 2, y + 2, w - 3, TOP_HEIGHT);
        g2.setColor(new Color(255, 255, 255, 8));
        g2.drawLine(x + 2, y + 2, x + w - 2, y + 2);
        g2.setColor(accent);
        g2.drawLine(x + 2, y + TOP_HEIGHT + 2, x + w - 2, y + TOP_HEIGHT + 2);

        g2.setFont(MinecraftTheme.HEADER_FONT);
        int textX = x + 12;
        int textY = y + 19;
        g2.setColor(new Color(0, 0, 0, 160));
        g2.drawString(title, textX + 2, textY + 2);
        g2.setColor(MinecraftTheme.TEXT);
        g2.drawString(title, textX, textY);

        g2.dispose();
    }

    private static Color shift(Color c, int amount) {
        return new Color(clamp(c.getRed() + amount), clamp(c.getGreen() + amount), clamp(c.getBlue() + amount));
    }

    private static int clamp(int value) {
        return Math.max(0, Math.min(255, value));
    }
}
