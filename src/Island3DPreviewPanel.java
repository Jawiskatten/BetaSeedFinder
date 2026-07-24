import javax.swing.*;
import java.awt.*;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseWheelEvent;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Lightweight interactive software renderer for exact island column surfaces. */
public final class Island3DPreviewPanel extends JPanel {
    private static final int CACHE_LIMIT = 24;
    private static final Map<String, Island3DData> CACHE = Collections.synchronizedMap(
            new LinkedHashMap<>(32, 0.75f, true) {
                @Override protected boolean removeEldestEntry(Map.Entry<String, Island3DData> eldest) {
                    return size() > CACHE_LIMIT;
                }
            }
    );

    private final List<SurfaceQuad> mesh = new ArrayList<>();
    private Island3DData data;
    private String message = "Select an island to load the 3D preview";
    private double yaw = Math.toRadians(-42.0);
    private double pitch = Math.toRadians(31.0);
    private double zoom = 1.0;
    private int dragX;
    private int dragY;

    public Island3DPreviewPanel() {
        setOpaque(true);
        setBackground(new Color(10, 10, 11));
        setCursor(Cursor.getPredefinedCursor(Cursor.HAND_CURSOR));
        setToolTipText("Drag to rotate. Mouse wheel to zoom.");

        MouseAdapter mouse = new MouseAdapter() {
            @Override public void mousePressed(MouseEvent e) {
                dragX = e.getX();
                dragY = e.getY();
            }

            @Override public void mouseDragged(MouseEvent e) {
                int dx = e.getX() - dragX;
                int dy = e.getY() - dragY;
                dragX = e.getX();
                dragY = e.getY();
                yaw += dx * 0.0105;
                pitch = clamp(pitch + dy * 0.0085, Math.toRadians(-80), Math.toRadians(80));
                repaint();
            }

            @Override public void mouseWheelMoved(MouseWheelEvent e) {
                zoom = clamp(zoom * Math.pow(1.10, -e.getPreciseWheelRotation()), 0.45, 3.25);
                repaint();
            }
        };
        addMouseListener(mouse);
        addMouseMotionListener(mouse);
        addMouseWheelListener(mouse);
    }

    public static Island3DData cached(String key) {
        return key == null ? null : CACHE.get(key);
    }

    public static void cache(String key, Island3DData data) {
        if (key != null && data != null) CACHE.put(key, data);
    }

    public void setData(Island3DData data) {
        this.data = data;
        this.message = null;
        rebuildMesh();
        resetView();
    }

    public void clear(String message) {
        this.data = null;
        this.mesh.clear();
        this.message = message == null ? "No 3D preview available" : message;
        repaint();
    }

    public void setMessage(String message) {
        this.message = message;
        repaint();
    }

    public void resetView() {
        yaw = Math.toRadians(-42.0);
        pitch = Math.toRadians(31.0);
        zoom = 1.0;
        repaint();
    }

    public void topView() {
        yaw = 0.0;
        pitch = Math.toRadians(89.5);
        zoom = 0.95;
        repaint();
    }

    public void sideView() {
        yaw = Math.toRadians(-90.0);
        pitch = Math.toRadians(8.0);
        zoom = 1.0;
        repaint();
    }

    private void rebuildMesh() {
        mesh.clear();
        if (data == null) return;

        for (int x = 0; x < data.width; x++) {
            for (int z = 0; z < data.depth; z++) {
                if (!data.occupied(x, z)) continue;
                int bottom = data.minYAt(x, z);
                int top = data.maxYAt(x, z) + 1;

                mesh.add(SurfaceQuad.top(x, z, top));
                mesh.add(SurfaceQuad.bottom(x, z, bottom));
                addSideSegments(x, z, bottom, top, -1, 0, Face.WEST);
                addSideSegments(x, z, bottom, top, 1, 0, Face.EAST);
                addSideSegments(x, z, bottom, top, 0, -1, Face.NORTH);
                addSideSegments(x, z, bottom, top, 0, 1, Face.SOUTH);
            }
        }
    }

    private void addSideSegments(int x, int z, int bottom, int top, int dx, int dz, Face face) {
        int nx = x + dx;
        int nz = z + dz;
        if (!data.occupied(nx, nz)) {
            mesh.add(SurfaceQuad.side(x, z, bottom, top, face));
            return;
        }

        int neighborBottom = data.minYAt(nx, nz);
        int neighborTop = data.maxYAt(nx, nz) + 1;
        if (top > neighborTop) {
            mesh.add(SurfaceQuad.side(x, z, Math.max(bottom, neighborTop), top, face));
        }
        if (bottom < neighborBottom) {
            mesh.add(SurfaceQuad.side(x, z, bottom, Math.min(top, neighborBottom), face));
        }
    }

    @Override
    protected void paintComponent(Graphics g) {
        super.paintComponent(g);
        Graphics2D g2 = (Graphics2D) g.create();
        try {
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            paintBackground(g2);
            if (data == null || mesh.isEmpty()) {
                paintMessage(g2, message == null ? "No 3D preview available" : message);
                return;
            }
            paintMesh(g2);
            paintOverlay(g2);
        } finally {
            g2.dispose();
        }
    }

    private void paintBackground(Graphics2D g2) {
        int w = getWidth();
        int h = getHeight();
        g2.setPaint(new GradientPaint(0, 0, new Color(22, 22, 24), 0, h, new Color(8, 8, 9)));
        g2.fillRect(0, 0, w, h);
        g2.setColor(new Color(245, 171, 28, 20));
        int spacing = 24;
        for (int x = 0; x < w; x += spacing) g2.drawLine(x, 0, x, h);
        for (int y = 0; y < h; y += spacing) g2.drawLine(0, y, w, y);
    }

    private void paintMessage(Graphics2D g2, String text) {
        g2.setFont(MinecraftTheme.UI_FONT);
        g2.setColor(MinecraftTheme.TEXT_DIM);
        FontMetrics fm = g2.getFontMetrics();
        int x = Math.max(12, (getWidth() - fm.stringWidth(text)) / 2);
        int y = getHeight() / 2;
        g2.drawString(text, x, y);
        g2.setFont(MinecraftTheme.SMALL_FONT);
        String hint = "Drag to rotate  •  Wheel to zoom";
        FontMetrics sm = g2.getFontMetrics();
        g2.drawString(hint, Math.max(12, (getWidth() - sm.stringWidth(hint)) / 2), y + 24);
    }

    private void paintMesh(Graphics2D g2) {
        int w = getWidth();
        int h = getHeight();
        double centerX = data.width * 0.5;
        double centerZ = data.depth * 0.5;
        double centerY = (data.globalMinY + data.globalMaxY + 1) * 0.5;
        double horizontal = Math.max(data.width, data.depth);
        double vertical = Math.max(1.0, data.globalMaxY - data.globalMinY + 1.0);
        double unit = 2.25 / Math.max(horizontal, vertical * 2.0);

        double cosY = Math.cos(yaw);
        double sinY = Math.sin(yaw);
        double cosP = Math.cos(pitch);
        double sinP = Math.sin(pitch);
        double baseScale = Math.min(w, h) * 2.25 * zoom;
        double cameraDistance = 4.4;

        List<RenderQuad> rendered = new ArrayList<>(mesh.size());
        for (SurfaceQuad quad : mesh) {
            int[] px = new int[4];
            int[] py = new int[4];
            double depth = 0.0;
            boolean valid = true;
            for (int i = 0; i < 4; i++) {
                double x = (quad.vertices[i][0] - centerX) * unit;
                double y = (quad.vertices[i][1] - centerY) * unit * 1.55;
                double z = (quad.vertices[i][2] - centerZ) * unit;

                double rx = x * cosY - z * sinY;
                double rz = x * sinY + z * cosY;
                double ry = y * cosP - rz * sinP;
                double rz2 = y * sinP + rz * cosP;
                double denominator = cameraDistance - rz2;
                if (denominator < 0.30) {
                    valid = false;
                    break;
                }
                double perspective = baseScale / denominator;
                px[i] = (int) Math.round(w * 0.5 + rx * perspective);
                py[i] = (int) Math.round(h * 0.53 - ry * perspective);
                depth += rz2;
            }
            if (!valid) continue;

            double[] normal = rotateNormal(quad.face.nx, quad.face.ny, quad.face.nz, cosY, sinY, cosP, sinP);
            double light = 0.54 + 0.46 * Math.max(0.0,
                    normal[0] * -0.38 + normal[1] * 0.78 + normal[2] * 0.50);
            Color color = shaded(baseColor(quad), light);
            rendered.add(new RenderQuad(px, py, depth / 4.0, color));
        }

        rendered.sort(Comparator.comparingDouble(q -> q.depth));
        for (RenderQuad quad : rendered) {
            g2.setColor(quad.color);
            g2.fillPolygon(quad.x, quad.y, 4);
            g2.setColor(new Color(4, 4, 4, 95));
            g2.drawPolygon(quad.x, quad.y, 4);
        }
    }

    private Color baseColor(SurfaceQuad quad) {
        if (quad.face == Face.TOP) return new Color(92, 139, 61);
        if (quad.face == Face.BOTTOM) return new Color(63, 64, 61);
        double middle = (quad.minY + quad.maxY) * 0.5;
        double range = Math.max(1.0, data.globalMaxY - data.globalMinY + 1.0);
        double ratio = (middle - data.globalMinY) / range;
        if (ratio > 0.68) return new Color(116, 78, 43);
        if (ratio > 0.38) return new Color(104, 72, 45);
        return new Color(92, 94, 91);
    }

    private void paintOverlay(Graphics2D g2) {
        g2.setFont(MinecraftTheme.SMALL_FONT);
        g2.setColor(new Color(236, 223, 178, 210));
        String info = data.width + "x" + data.depth + "  •  "
                + String.format("%,d", data.blocks) + " blocks";
        g2.drawString(info, 12, 20);
        g2.setColor(new Color(236, 223, 178, 155));
        g2.drawString("Drag: rotate   Wheel: zoom", 12, getHeight() - 12);
    }

    private static double[] rotateNormal(
            double x, double y, double z,
            double cosY, double sinY, double cosP, double sinP
    ) {
        double rx = x * cosY - z * sinY;
        double rz = x * sinY + z * cosY;
        double ry = y * cosP - rz * sinP;
        double rz2 = y * sinP + rz * cosP;
        return new double[]{rx, ry, rz2};
    }

    private static Color shaded(Color base, double light) {
        return new Color(
                clamp((int) Math.round(base.getRed() * light), 0, 255),
                clamp((int) Math.round(base.getGreen() * light), 0, 255),
                clamp((int) Math.round(base.getBlue() * light), 0, 255)
        );
    }

    private static double clamp(double value, double min, double max) {
        return Math.max(min, Math.min(max, value));
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private enum Face {
        TOP(0, 1, 0), BOTTOM(0, -1, 0),
        WEST(-1, 0, 0), EAST(1, 0, 0),
        NORTH(0, 0, -1), SOUTH(0, 0, 1);

        final double nx;
        final double ny;
        final double nz;
        Face(double nx, double ny, double nz) {
            this.nx = nx;
            this.ny = ny;
            this.nz = nz;
        }
    }

    private static final class SurfaceQuad {
        final double[][] vertices;
        final Face face;
        final int minY;
        final int maxY;

        SurfaceQuad(double[][] vertices, Face face, int minY, int maxY) {
            this.vertices = vertices;
            this.face = face;
            this.minY = minY;
            this.maxY = maxY;
        }

        static SurfaceQuad top(int x, int z, int y) {
            return new SurfaceQuad(new double[][]{
                    {x, y, z}, {x + 1, y, z}, {x + 1, y, z + 1}, {x, y, z + 1}
            }, Face.TOP, y, y);
        }

        static SurfaceQuad bottom(int x, int z, int y) {
            return new SurfaceQuad(new double[][]{
                    {x, y, z + 1}, {x + 1, y, z + 1}, {x + 1, y, z}, {x, y, z}
            }, Face.BOTTOM, y, y);
        }

        static SurfaceQuad side(int x, int z, int y0, int y1, Face face) {
            if (y1 <= y0) throw new IllegalArgumentException("Empty side segment");
            double[][] v = switch (face) {
                case WEST -> new double[][]{{x, y0, z + 1}, {x, y0, z}, {x, y1, z}, {x, y1, z + 1}};
                case EAST -> new double[][]{{x + 1, y0, z}, {x + 1, y0, z + 1}, {x + 1, y1, z + 1}, {x + 1, y1, z}};
                case NORTH -> new double[][]{{x, y0, z}, {x + 1, y0, z}, {x + 1, y1, z}, {x, y1, z}};
                case SOUTH -> new double[][]{{x + 1, y0, z + 1}, {x, y0, z + 1}, {x, y1, z + 1}, {x + 1, y1, z + 1}};
                default -> throw new IllegalArgumentException("Not a side face");
            };
            return new SurfaceQuad(v, face, y0, y1);
        }
    }

    private record RenderQuad(int[] x, int[] y, double depth, Color color) {}
}
