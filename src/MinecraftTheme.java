import javax.swing.*;
import javax.swing.border.Border;
import javax.swing.border.CompoundBorder;
import javax.swing.border.EmptyBorder;
import javax.swing.border.LineBorder;
import javax.swing.border.TitledBorder;
import javax.swing.plaf.basic.BasicButtonUI;
import javax.swing.plaf.basic.BasicComboBoxUI;
import javax.swing.plaf.basic.BasicScrollBarUI;
import javax.swing.table.DefaultTableCellRenderer;
import javax.swing.table.JTableHeader;
import java.awt.*;
import java.io.File;

public class MinecraftTheme {
    public static final Color BG = new Color(18, 18, 19);
    public static final Color BG_DARK = new Color(10, 10, 11);

    public static final Color DIRT = new Color(34, 31, 25);
    public static final Color DIRT_DARK = new Color(24, 22, 18);

    public static final Color STONE = new Color(38, 36, 32);
    public static final Color STONE_DARK = new Color(27, 26, 23);
    public static final Color STONE_LIGHT = new Color(134, 118, 70);

    public static final Color PANEL_DARK = new Color(25, 24, 22);
    public static final Color PANEL_INNER = new Color(11, 11, 12);
    public static final Color PANEL_INNER_ALT = new Color(18, 18, 19);

    public static final Color SETTING_CELL = new Color(29, 28, 24);
    public static final Color SETTING_CELL_BORDER = new Color(110, 88, 30);

    public static final Color TEXT = new Color(236, 223, 178);
    public static final Color TEXT_DIM = new Color(176, 160, 116);
    public static final Color TEXT_DISABLED = new Color(105, 97, 78);

    public static final Color BORDER_LIGHT = new Color(151, 121, 37);
    public static final Color BORDER_MID = new Color(78, 64, 28);
    public static final Color BORDER_DARK = new Color(5, 5, 6);

    public static final Color SELECTED = new Color(98, 69, 13);
    public static final Color BLUE_HIT = new Color(245, 171, 28);
    public static final Color RED_BEST = new Color(231, 76, 42);
    public static final Color GREEN = new Color(103, 181, 55);

    private static final Font BASE_FONT = loadBaseFont();

    public static final Font TITLE_FONT = BASE_FONT.deriveFont(Font.PLAIN, 27f);
    public static final Font HEADER_FONT = BASE_FONT.deriveFont(Font.PLAIN, 15f);
    public static final Font UI_FONT = BASE_FONT.deriveFont(Font.PLAIN, 14f);
    public static final Font UI_BOLD_FONT = BASE_FONT.deriveFont(Font.PLAIN, 14f);
    public static final Font SMALL_FONT = BASE_FONT.deriveFont(Font.PLAIN, 12f);

    private static Font loadBaseFont() {
        String[] paths = {
                "assets/fonts/minecraft.ttf",
                "assets/fonts/Minecraft.ttf",
                "assets/fonts/Minecraftia.ttf",
                "assets/fonts/minecraftia.ttf",
                "assets/minecraft.ttf",
                "assets/Minecraft.ttf",
                "assets/Minecraftia.ttf",
                "assets/minecraftia.ttf"
        };

        for (String path : paths) {
            try {
                File file = AppPaths.resolve(path).toFile();
                if (file.exists()) {
                    return Font.createFont(Font.TRUETYPE_FONT, file);
                }
            } catch (Exception ignored) {
            }
        }

        return new Font("Consolas", Font.PLAIN, 14);
    }

    public static void apply(JFrame frame) {
        frame.getContentPane().setBackground(BG);
        applyRecursive(frame.getContentPane());
    }

    private static void applyRecursive(Component component) {
        style(component);
        if (component instanceof Container container) {
            for (Component child : container.getComponents()) {
                applyRecursive(child);
            }
        }
    }

    private static void style(Component component) {
        if (component instanceof JPanel panel) {
            stylePanel(panel);
        } else if (component instanceof JLabel label) {
            styleLabel(label);
        } else if (component instanceof JButton button) {
            styleButton(button);
        } else if (component instanceof JTextField field) {
            styleTextField(field);
        } else if (component instanceof JTextArea area) {
            styleTextArea(area);
        } else if (component instanceof JCheckBox box) {
            styleCheckBox(box);
        } else if (component instanceof JComboBox<?> comboBox) {
            styleComboBox(comboBox);
        } else if (component instanceof JTable table) {
            styleTable(table);
        } else if (component instanceof JScrollPane scrollPane) {
            styleScrollPane(scrollPane);
        } else {
            component.setFont(UI_FONT);
        }
    }

    private static void stylePanel(JPanel panel) {
        if ("settingCell".equals(panel.getName())) {
            panel.setOpaque(true);
            panel.setBackground(SETTING_CELL);
            panel.setBorder(settingCellBorder());
            return;
        }

        Border border = panel.getBorder();
        String title = null;

        if (border instanceof MinecraftTitleBorder minecraftTitleBorder) {
            title = minecraftTitleBorder.getTitle();
        } else if (border instanceof TitledBorder titledBorder) {
            title = titledBorder.getTitle();
        }

        if (title != null) {
            String lower = title.toLowerCase();
            Color bg = STONE_DARK;

            if (lower.contains("settings") || lower.contains("general") || lower.contains("advanced")) {
                bg = DIRT;
            } else if (lower.contains("preview") || lower.contains("selected") || lower.contains("details") || lower.contains("island")) {
                bg = STONE;
            } else if (lower.contains("graph") || lower.contains("statistics")) {
                bg = PANEL_DARK;
            }

            panel.setOpaque(true);
            panel.setBackground(bg);
            panel.setBorder(titled(title));
        } else {
            panel.setOpaque(true);
            if (!(panel instanceof TexturePanel) && !(panel instanceof BlockPanel)) {
                panel.setBackground(BG);
            }
        }
    }

    public static Border titled(String title) {
        return new MinecraftTitleBorder(title);
    }

    public static Border boxBorder() {
        return new CompoundBorder(
                new LineBorder(BORDER_DARK, 2),
                new CompoundBorder(
                        new LineBorder(BORDER_MID, 1),
                        new EmptyBorder(10, 12, 10, 12)
                )
        );
    }

    public static Border settingCellBorder() {
        return new CompoundBorder(
                new LineBorder(BORDER_DARK, 2),
                new CompoundBorder(
                        new LineBorder(SETTING_CELL_BORDER, 1),
                        new EmptyBorder(8, 10, 8, 10)
                )
        );
    }

    public static Border insetBorder() {
        return new CompoundBorder(
                new LineBorder(BORDER_DARK, 2),
                new CompoundBorder(
                        new LineBorder(BORDER_MID, 1),
                        new EmptyBorder(6, 8, 6, 8)
                )
        );
    }

    private static void styleLabel(JLabel label) {
        if ("appTitle".equals(label.getName())) {
            label.setForeground(BLUE_HIT);
            label.setFont(TITLE_FONT);
        } else if ("accentValue".equals(label.getName())) {
            label.setForeground(BLUE_HIT);
            label.setFont(HEADER_FONT);
        } else if ("settingLabel".equals(label.getName())) {
            label.setForeground(TEXT_DIM);
            label.setFont(SMALL_FONT);
        } else {
            label.setForeground(TEXT);
            label.setFont(UI_FONT);
        }
    }

    private static void styleButton(JButton button) {
        if ("comboArrow".equals(button.getName())) {
            button.setUI(new BasicButtonUI());
            button.setFont(SMALL_FONT);
            button.setForeground(TEXT_DIM);
            button.setBackground(STONE_DARK);
            button.setFocusPainted(false);
            button.setContentAreaFilled(true);
            button.setOpaque(true);
            button.setBorder(new CompoundBorder(
                    new LineBorder(BORDER_DARK, 1),
                    new EmptyBorder(0, 6, 0, 6)
            ));
            button.setPreferredSize(new Dimension(34, 30));
            return;
        }

        button.setUI(new MinecraftButtonUI());
        button.setFont(UI_FONT);
        button.setForeground(button.isEnabled() ? TEXT : TEXT_DISABLED);
        button.setFocusPainted(false);
        button.setContentAreaFilled(false);
        button.setOpaque(false);
        boolean compact = Boolean.TRUE.equals(button.getClientProperty("compactButton"));
        button.setBorder(compact ? new EmptyBorder(7, 11, 7, 11) : new EmptyBorder(9, 14, 9, 14));
        button.setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
    }

    private static void styleTextField(JTextField field) {
        field.setFont(UI_FONT);
        field.setForeground(TEXT);
        field.setCaretColor(TEXT);
        field.setBackground(new Color(16, 16, 17));
        field.setBorder(insetBorder());
        field.setSelectionColor(SELECTED);
        field.setSelectedTextColor(Color.WHITE);
        field.setDisabledTextColor(TEXT_DISABLED);
        field.setOpaque(true);
        field.setMargin(new Insets(0, 0, 0, 0));
    }

    private static void styleTextArea(JTextArea area) {
        area.setFont(UI_FONT);
        area.setForeground(TEXT);
        area.setCaretColor(TEXT);
        if (area.isOpaque()) {
            area.setBackground(PANEL_INNER);
        }
        area.setBorder(BorderFactory.createEmptyBorder(8, 10, 8, 10));
        area.setSelectionColor(SELECTED);
        area.setSelectedTextColor(Color.WHITE);
        area.setDisabledTextColor(TEXT_DISABLED);
    }

    private static void styleCheckBox(JCheckBox box) {
        box.setFont(UI_FONT);
        box.setForeground(TEXT);
        box.setOpaque(false);
        box.setFocusPainted(false);
        box.setIconTextGap(7);

        Icon empty = new PixelCheckIcon(false, false);
        Icon checked = new PixelCheckIcon(true, false);
        Icon emptyDisabled = new PixelCheckIcon(false, true);
        Icon checkedDisabled = new PixelCheckIcon(true, true);

        box.setIcon(empty);
        box.setSelectedIcon(checked);
        box.setDisabledIcon(emptyDisabled);
        box.setDisabledSelectedIcon(checkedDisabled);
    }

    private static void styleScrollPane(JScrollPane scrollPane) {
        scrollPane.setBorder(new LineBorder(BORDER_DARK, 2));
        if (scrollPane.getViewport().isOpaque()) {
            scrollPane.getViewport().setBackground(PANEL_INNER);
        }
        styleScrollBar(scrollPane.getVerticalScrollBar());
        styleScrollBar(scrollPane.getHorizontalScrollBar());
    }

    private static void styleScrollBar(JScrollBar bar) {
        bar.setBackground(PANEL_INNER);
        bar.setForeground(STONE_LIGHT);
        bar.setUI(new BasicScrollBarUI() {
            @Override
            protected void configureScrollBarColors() {
                this.thumbColor = new Color(112, 92, 40);
                this.trackColor = PANEL_INNER;
                this.thumbDarkShadowColor = BORDER_DARK;
                this.thumbHighlightColor = BORDER_LIGHT;
                this.thumbLightShadowColor = BORDER_MID;
            }

            @Override
            protected JButton createDecreaseButton(int orientation) { return createZeroButton(); }

            @Override
            protected JButton createIncreaseButton(int orientation) { return createZeroButton(); }

            @Override
            protected void paintThumb(Graphics g, JComponent c, Rectangle thumbBounds) {
                if (thumbBounds.isEmpty() || !scrollbar.isEnabled()) return;
                Graphics2D g2 = (Graphics2D) g.create();
                g2.setColor(thumbColor);
                g2.fillRect(thumbBounds.x, thumbBounds.y, thumbBounds.width, thumbBounds.height);
                g2.setColor(BORDER_DARK);
                g2.drawRect(thumbBounds.x, thumbBounds.y, thumbBounds.width - 1, thumbBounds.height - 1);
                g2.dispose();
            }

            @Override
            protected void paintTrack(Graphics g, JComponent c, Rectangle trackBounds) {
                g.setColor(PANEL_INNER);
                g.fillRect(trackBounds.x, trackBounds.y, trackBounds.width, trackBounds.height);
            }
        });
        bar.setPreferredSize(new Dimension(12, 12));
    }

    private static JButton createZeroButton() {
        JButton button = new JButton();
        button.setPreferredSize(new Dimension(0, 0));
        button.setMinimumSize(new Dimension(0, 0));
        button.setMaximumSize(new Dimension(0, 0));
        return button;
    }

    private static void styleComboBox(JComboBox<?> comboBox) {
        comboBox.setUI(new TerminalComboBoxUI());
        comboBox.setFont(UI_FONT);
        comboBox.setForeground(TEXT);
        comboBox.setBackground(PANEL_INNER);
        comboBox.setBorder(insetBorder());
        comboBox.setFocusable(false);
        comboBox.setOpaque(true);
        comboBox.setRenderer(new DefaultListCellRenderer() {
            @Override
            public Component getListCellRendererComponent(JList<?> list, Object value, int index, boolean isSelected, boolean cellHasFocus) {
                JLabel label = (JLabel) super.getListCellRendererComponent(list, value, index, isSelected, cellHasFocus);
                label.setOpaque(true);
                label.setFont(UI_FONT);
                label.setForeground(TEXT);
                label.setBackground(isSelected ? SELECTED : PANEL_INNER);
                label.setBorder(new EmptyBorder(7, 9, 7, 9));
                return label;
            }
        });
    }

    private static class TerminalComboBoxUI extends BasicComboBoxUI {
        @Override
        protected JButton createArrowButton() {
            JButton button = new JButton("v");
            button.setName("comboArrow");
            button.setFocusable(false);
            return button;
        }

        @Override
        public void paintCurrentValueBackground(Graphics g, Rectangle bounds, boolean hasFocus) {
            g.setColor(PANEL_INNER);
            g.fillRect(bounds.x, bounds.y, bounds.width, bounds.height);
        }

        @Override
        public void paintCurrentValue(Graphics g, Rectangle bounds, boolean hasFocus) {
            Object selected = comboBox.getSelectedItem();
            String text = selected == null ? "" : selected.toString();

            Graphics2D g2 = (Graphics2D) g.create();
            g2.setClip(bounds.x, bounds.y, bounds.width, bounds.height);
            g2.setFont(UI_FONT);
            g2.setColor(comboBox.isEnabled() ? TEXT : TEXT_DISABLED);
            FontMetrics metrics = g2.getFontMetrics();
            int x = bounds.x + 9;
            int y = bounds.y + Math.max(metrics.getAscent(), (bounds.height - metrics.getHeight()) / 2 + metrics.getAscent());
            g2.drawString(text, x, y);
            g2.dispose();
        }
    }

    private static void styleTable(JTable table) {
        table.setFont(UI_FONT);
        table.setRowHeight(30);
        table.setForeground(TEXT);
        table.setBackground(PANEL_INNER);
        table.setGridColor(new Color(49, 43, 27));
        table.setSelectionBackground(SELECTED);
        table.setSelectionForeground(Color.WHITE);
        table.setShowGrid(true);
        table.setIntercellSpacing(new Dimension(1, 1));
        table.setFocusable(false);

        JTableHeader header = table.getTableHeader();
        header.setFont(UI_FONT);
        header.setForeground(TEXT);
        header.setBackground(STONE);
        header.setBorder(new LineBorder(BORDER_DARK, 2));
        header.setReorderingAllowed(false);

        DefaultTableCellRenderer headerRenderer = new DefaultTableCellRenderer();
        headerRenderer.setHorizontalAlignment(SwingConstants.CENTER);
        headerRenderer.setFont(UI_FONT);
        headerRenderer.setForeground(TEXT);
        headerRenderer.setBackground(STONE);
        headerRenderer.setBorder(new CompoundBorder(new LineBorder(BORDER_DARK, 1), BorderFactory.createEmptyBorder(5, 6, 5, 6)));

        for (int i = 0; i < table.getColumnCount(); i++) {
            table.getColumnModel().getColumn(i).setHeaderRenderer(headerRenderer);
        }

        DefaultTableCellRenderer cellRenderer = new DefaultTableCellRenderer() {
            @Override
            public Component getTableCellRendererComponent(JTable table, Object value, boolean isSelected, boolean hasFocus, int row, int column) {
                Component c = super.getTableCellRendererComponent(table, value, isSelected, hasFocus, row, column);
                setHorizontalAlignment(SwingConstants.CENTER);
                setFont(UI_FONT);
                setBorder(BorderFactory.createEmptyBorder(0, 8, 0, 8));
                if (isSelected) {
                    c.setBackground(SELECTED);
                    c.setForeground(new Color(255, 186, 37));
                } else {
                    c.setBackground(row % 2 == 0 ? PANEL_INNER : PANEL_INNER_ALT);
                    c.setForeground(TEXT);
                }
                return c;
            }
        };

        for (int i = 0; i < table.getColumnCount(); i++) {
            table.getColumnModel().getColumn(i).setCellRenderer(cellRenderer);
        }
    }

    private static class MinecraftButtonUI extends BasicButtonUI {
        @Override
        public void paint(Graphics g, JComponent c) {
            AbstractButton button = (AbstractButton) c;
            ButtonModel model = button.getModel();
            Graphics2D g2 = (Graphics2D) g.create();
            g2.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);

            int w = c.getWidth();
            int h = c.getHeight();
            boolean enabled = button.isEnabled();
            boolean pressed = enabled && model.isPressed();
            boolean hover = enabled && model.isRollover();

            boolean navButton = Boolean.TRUE.equals(button.getClientProperty("navButton"));
            boolean selectedNav = Boolean.TRUE.equals(button.getClientProperty("selectedNav"));
            boolean accentButton = Boolean.TRUE.equals(button.getClientProperty("accentButton"));

            Color top = enabled ? new Color(44, 41, 34) : new Color(29, 28, 24);
            Color bottom = enabled ? new Color(28, 27, 24) : new Color(21, 20, 18);
            if (selectedNav || accentButton) {
                top = new Color(96, 71, 18);
                bottom = new Color(56, 42, 13);
            }
            if (hover) {
                top = new Color(110, 82, 22);
                bottom = new Color(63, 48, 16);
            }
            if (pressed) {
                top = new Color(43, 34, 12);
                bottom = new Color(82, 59, 14);
            }

            g2.setColor(BORDER_DARK);
            g2.fillRect(0, 0, w, h);
            for (int y = 2; y < h - 2; y++) {
                float t = (y - 2) / Math.max(1f, h - 4f);
                int r = (int) (top.getRed() * (1 - t) + bottom.getRed() * t);
                int gr = (int) (top.getGreen() * (1 - t) + bottom.getGreen() * t);
                int b = (int) (top.getBlue() * (1 - t) + bottom.getBlue() * t);
                g2.setColor(new Color(r, gr, b));
                g2.drawLine(2, y, w - 3, y);
            }
            g2.setColor(selectedNav || accentButton || hover
                    ? new Color(245, 163, 18, enabled ? 180 : 55)
                    : new Color(218, 195, 112, enabled ? 46 : 18));
            g2.drawLine(2, 2, w - 3, 2);
            g2.drawLine(2, 2, 2, h - 3);
            g2.setColor(new Color(0, 0, 0, 90));
            g2.drawLine(2, h - 3, w - 3, h - 3);
            g2.drawLine(w - 3, 2, w - 3, h - 3);
            if (hover && !pressed) {
                g2.setColor(new Color(255, 255, 255, 16));
                g2.fillRect(3, 3, w - 6, h - 6);
            }

            String text = button.getText();
            g2.setFont(button.getFont());
            FontMetrics fm = g2.getFontMetrics();
            int textW = fm.stringWidth(text);
            int textX = (w - textW) / 2;
            int textY = (h - fm.getHeight()) / 2 + fm.getAscent();
            if (pressed) {
                textX += 1;
                textY += 1;
            }
            g2.setColor(new Color(0, 0, 0, 180));
            g2.drawString(text, textX + 2, textY + 2);
            g2.setColor(selectedNav || accentButton ? new Color(255, 186, 37) : (enabled ? TEXT : TEXT_DISABLED));
            g2.drawString(text, textX, textY);
            g2.dispose();
        }

        @Override
        public Dimension getPreferredSize(JComponent c) {
            Dimension d = super.getPreferredSize(c);
            boolean navButton = c instanceof AbstractButton && Boolean.TRUE.equals(((AbstractButton) c).getClientProperty("navButton"));
            boolean compact = c instanceof AbstractButton && Boolean.TRUE.equals(((AbstractButton) c).getClientProperty("compactButton"));
            if (navButton) {
                d.width += 18;
                d.height = Math.max(d.height + 8, 36);
            } else if (compact) {
                d.width += 12;
                d.height = Math.max(d.height + 5, 31);
            } else {
                d.width += 18;
                d.height = Math.max(d.height + 8, 34);
            }
            return d;
        }
    }

    private static class PixelCheckIcon implements Icon {
        private static final int SIZE = 18;
        private final boolean checked;
        private final boolean disabled;

        PixelCheckIcon(boolean checked, boolean disabled) {
            this.checked = checked;
            this.disabled = disabled;
        }

        @Override public int getIconWidth() { return SIZE; }
        @Override public int getIconHeight() { return SIZE; }

        @Override
        public void paintIcon(Component c, Graphics g, int x, int y) {
            Graphics2D g2 = (Graphics2D) g.create();
            Color face = disabled ? new Color(38, 42, 52) : new Color(26, 30, 38);
            Color edge = disabled ? new Color(65, 70, 82) : BORDER_MID;
            g2.setColor(BORDER_DARK);
            g2.fillRect(x, y, SIZE, SIZE);
            g2.setColor(face);
            g2.fillRect(x + 2, y + 2, SIZE - 4, SIZE - 4);
            g2.setColor(edge);
            g2.drawRect(x + 1, y + 1, SIZE - 3, SIZE - 3);
            if (checked) {
                Color mark = disabled ? new Color(102, 116, 138) : BLUE_HIT;
                g2.setColor(mark);
                g2.fillRect(x + 4, y + 8, 3, 3);
                g2.fillRect(x + 7, y + 11, 3, 3);
                g2.fillRect(x + 10, y + 8, 3, 3);
                g2.fillRect(x + 13, y + 5, 3, 3);
                g2.fillRect(x + 9, y + 9, 3, 3);
                g2.fillRect(x + 6, y + 10, 3, 3);
            }
            g2.dispose();
        }
    }
}
