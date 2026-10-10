// Makes the map icons: dist/Textures/DetailedMiniMap/icons/<style>/<name>.dds (128 x 128 BGRA, full mip chain), two
// styles: Default (a dark round badge, a coloured rim) and Vanilla (a parchment-white silhouette in a dark outline, as
// the game's map markers), each with a Font Awesome 6 Free Solid glyph (the font SKSE Menu Framework ships; icons
// CC BY 4.0, fontawesome.com) or, for the weapon, a drawn sword.
//   java tools/MakeIcons.java "<path to fa-solid-900.ttf>" dist [<folder for PNG previews>]
// Any of the files can be replaced by an own DDS of any size (square, transparent background); a new folder next to
// these is a new style the menu offers.
import java.awt.*;
import java.awt.font.GlyphVector;
import java.awt.geom.*;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.*;
import java.nio.file.*;

public class MakeIcons {
    record Icon(String name, int glyph, int r, int g, int b) {}

    static final Icon[] ICONS = {
        new Icon("enemy", 0xF54C, 235, 70, 55),
        new Icon("guard", 0xF505, 90, 150, 235),
        new Icon("npc", 0xF007, 215, 215, 215),
        new Icon("follower", 0xF007, 90, 205, 120),
        new Icon("creature", 0xF1B0, 190, 165, 130),
        new Icon("door", 0xF52B, 240, 150, 50),
        new Icon("food", 0xF6D7, 230, 190, 90),
        new Icon("potion", 0xF0C3, 215, 95, 225),
        new Icon("container", 0xF187, 195, 135, 75),
        new Icon("weapon", 0, 200, 210, 225),
        new Icon("armor", 0xF3ED, 140, 180, 220),
        new Icon("loot", 0xF51E, 245, 210, 90),
        new Icon("quest", 0x21, 255, 200, 60),
        new Icon("body", 0xF714, 175, 170, 160),
        new Icon("flora", 0xF06C, 120, 200, 90),
        new Icon("ore", 0xF3A5, 165, 180, 205),
        new Icon("chest", 0xF552, 230, 175, 60),
        new Icon("clutter", 0xE4CF, 150, 145, 135),
        new Icon("player", 0xF007, 255, 210, 90),  // the character (drawn upright; the map adds a pointer on its rim)
    };
    static final int S = 128;

    public static void main(String[] a) throws Exception {
        Font fa = Font.createFont(Font.TRUETYPE_FONT, new File(a[0]));
        Path icons = Paths.get(a[1], "Textures", "DetailedMiniMap", "icons");
        for (String style : new String[] { "Default", "Vanilla" }) {
            Path out = icons.resolve(style);
            Files.createDirectories(out);
            for (Icon icon : ICONS) {
                BufferedImage img = new BufferedImage(S, S, BufferedImage.TYPE_INT_ARGB);
                Graphics2D g = img.createGraphics();
                g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
                g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE);
                Shape shape = icon.glyph != 0 ?
                    fa.deriveFont(100f).createGlyphVector(g.getFontRenderContext(), new String(Character.toChars(icon.glyph))).getOutline() :
                    sword();
                if (style.equals("Default")) {
                    badge(g, icon, shape);
                } else {
                    vanilla(g, icon, shape);
                }
                g.dispose();
                writeDds(img, out.resolve(icon.name + ".dds"));
                if (a.length > 2) {  // previews
                    Files.createDirectories(Paths.get(a[2], style));
                    javax.imageio.ImageIO.write(img, "png", Paths.get(a[2], style, icon.name + ".png").toFile());
                }
                System.out.println(out.resolve(icon.name + ".dds"));
            }
            BufferedImage arrow = pointer(style.equals("Vanilla"));
            writeDds(arrow, out.resolve("pointer.dds"));
            if (a.length > 2) {
                javax.imageio.ImageIO.write(arrow, "png", Paths.get(a[2], style, "pointer.png").toFile());
            }
            System.out.println(out.resolve("pointer.dds"));
        }
    }

    // the character's pointer: an arrowhead pointing up, turned by the map where the character faces. The texture's
    // middle is the character; it reaches 2.1 badge radii either side (Icons.cpp kPointerSpan), the badge drawn over
    // its middle - the tip stands out above it, the wings either side of it
    static BufferedImage pointer(boolean vanilla) {
        BufferedImage img = new BufferedImage(S, S, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE);
        double c = S / 2.0, r = c / 2.1;   // the badge's radius in this texture (room left round the arrow for its shadow)
        Path2D head = new Path2D.Double();
        head.moveTo(c, c - r * 1.70);                // the tip
        head.lineTo(c + r * 0.62, c - r * 0.92);     // the right wing (clear of the badge)
        head.lineTo(c, c - r * 1.14);                // the notch
        head.lineTo(c - r * 0.62, c - r * 0.92);     // the left wing
        head.closePath();
        BasicStroke edge = new BasicStroke(vanilla ? 9f : 7f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND);
        // a soft shadow down right
        Shape shadow = AffineTransform.getTranslateInstance(3, 4).createTransformedShape(head);
        g.setColor(new Color(0, 0, 0, 90));
        g.fill(edge.createStrokedShape(shadow));
        g.fill(shadow);
        // the outline, then the arrowhead lit from the top left
        g.setColor(vanilla ? new Color(24, 20, 14, 240) : new Color(18, 13, 6, 245));
        g.fill(edge.createStrokedShape(head));
        Rectangle2D b = head.getBounds2D();
        g.setPaint(vanilla ?
            new GradientPaint((float) b.getMinX(), (float) b.getMinY(), new Color(248, 242, 226), (float) b.getMaxX(), (float) b.getMaxY(), new Color(196, 184, 156)) :
            new GradientPaint((float) b.getMinX(), (float) b.getMinY(), new Color(255, 228, 130), (float) b.getMaxX(), (float) b.getMaxY(), new Color(205, 140, 36)));
        g.fill(head);
        // a ridge down its middle: the lit half and the shaded half of a faceted head
        Path2D shade = new Path2D.Double();
        shade.moveTo(c, c - r * 1.70);
        shade.lineTo(c + r * 0.62, c - r * 0.92);
        shade.lineTo(c, c - r * 1.14);
        shade.closePath();
        g.setColor(new Color(0, 0, 0, vanilla ? 40 : 55));
        g.fill(shade);
        g.dispose();
        return img;
    }

    // the glyph centred optically: between the outline's box and its ink's centre of mass (a paw's heavy pad, a
    // flask's wide bottom pull the eye), scaled to a_box px and to reach no farther than a_reach from the middle
    static Shape place(Shape a_shape, double a_box, double a_reach) {
        Rectangle2D b = a_shape.getBounds2D();
        double[] mass = centroid(a_shape);
        double cx = b.getCenterX() * 0.55 + mass[0] * 0.45, cy = b.getCenterY() * 0.55 + mass[1] * 0.45;
        double k = Math.min(a_box / Math.max(b.getWidth(), b.getHeight()), a_reach / farthest(a_shape, cx, cy));
        AffineTransform t = new AffineTransform();
        t.translate(S / 2.0, S / 2.0);
        t.scale(k, k);
        t.translate(-cx, -cy);
        return t.createTransformedShape(a_shape);
    }

    // Default: a dark round badge, a coloured rim, the glyph in the kind's colour
    static void badge(Graphics2D g, Icon icon, Shape shape) {
        Color col = new Color(icon.r, icon.g, icon.b);
        double ring = 9, r = S / 2.0 - ring / 2 - 1;
        g.setColor(new Color(10, 12, 18, 215));
        g.fill(new Ellipse2D.Double(S / 2.0 - r, S / 2.0 - r, 2 * r, 2 * r));
        g.setColor(col);
        g.setStroke(new BasicStroke((float) ring));
        g.draw(new Ellipse2D.Double(S / 2.0 - r, S / 2.0 - r, 2 * r, 2 * r));
        g.setColor(col);
        g.fill(place(shape, 64, (r - ring / 2) * 0.80));
    }

    // Vanilla: as the game's own map markers - no badge, a parchment-white silhouette (the kind's colour only a
    // hint of it), a dark ink outline and a soft shadow under it
    static void vanilla(Graphics2D g, Icon icon, Shape shape) {
        Shape s = place(shape, 92, S / 2.0 - 12);
        double mix = icon.name.equals("enemy") || icon.name.equals("quest") ? 0.8 : 0.6;  // the kind's colour, plain to see (the ones to find at a glance the most)
        Color col = new Color((int) Math.round(232 + (icon.r - 232) * mix), (int) Math.round(226 + (icon.g - 226) * mix), (int) Math.round(208 + (icon.b - 208) * mix));
        BasicStroke edge = new BasicStroke(11, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND);
        Shape shadow = AffineTransform.getTranslateInstance(3, 4).createTransformedShape(s);
        g.setColor(new Color(0, 0, 0, 110));
        g.fill(edge.createStrokedShape(shadow));
        g.fill(shadow);
        g.setColor(new Color(24, 20, 14, 240));
        g.fill(edge.createStrokedShape(s));
        g.setColor(col);
        g.fill(s);
    }

    // a sword along the diagonal, point up right, as one outline (centred like a glyph)
    static Shape sword() {
        Area a = new Area();
        double w = 11;
        // blade with its point
        Path2D blade = new Path2D.Double();
        double h = w / 2 / Math.sqrt(2);
        blade.moveTo(-8 - h, 8 - h);
        blade.lineTo(30 - h, -30 - h);
        blade.lineTo(40, -40);
        blade.lineTo(30 + h, -30 + h);
        blade.lineTo(-8 + h, 8 + h);
        blade.closePath();
        a.add(new Area(blade));
        BasicStroke round = new BasicStroke(9, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND);
        a.add(new Area(round.createStrokedShape(new Line2D.Double(-24, -6, 6, 24))));                        // guard
        a.add(new Area(new BasicStroke(8, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(new Line2D.Double(-8, 8, -24, 24))));  // grip
        a.add(new Area(new Ellipse2D.Double(-37, 23, 14, 14)));                                             // pommel
        return a;
    }

    // the ink's centre of mass, from a fine raster of the shape
    static double[] centroid(Shape s) {
        Rectangle2D b = s.getBounds2D();
        int n = 400;
        double k = n / Math.max(b.getWidth(), b.getHeight());
        BufferedImage m = new BufferedImage(n + 2, n + 2, BufferedImage.TYPE_BYTE_GRAY);
        Graphics2D g = m.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setColor(Color.WHITE);
        AffineTransform t = new AffineTransform();
        t.scale(k, k);
        t.translate(-b.getX(), -b.getY());
        g.fill(t.createTransformedShape(s));
        g.dispose();
        double sum = 0, sx = 0, sy = 0;
        for (int y = 0; y < m.getHeight(); y++) {
            for (int x = 0; x < m.getWidth(); x++) {
                double v = m.getRaster().getSample(x, y, 0) / 255.0;
                sum += v;
                sx += (x + 0.5) * v;
                sy += (y + 0.5) * v;
            }
        }
        return new double[] { b.getX() + sx / sum / k, b.getY() + sy / sum / k };
    }

    // how far the outline reaches from (cx, cy)
    static double farthest(Shape s, double cx, double cy) {
        double best = 0;
        double[] p = new double[6];
        for (PathIterator it = s.getPathIterator(null, 0.25); !it.isDone(); it.next()) {
            if (it.currentSegment(p) != PathIterator.SEG_CLOSE) {
                best = Math.max(best, Math.hypot(p[0] - cx, p[1] - cy));
            }
        }
        return best;
    }

    // uncompressed 32-bit BGRA DDS with every mip level (box filtered, straight alpha)
    static void writeDds(BufferedImage top, Path path) throws IOException {
        java.util.List<int[]> levels = new java.util.ArrayList<>();
        int w = top.getWidth();
        int[] px = top.getRGB(0, 0, w, w, null, 0, w);
        levels.add(px);
        while (w > 1) {
            int n = w / 2;
            int[] next = new int[n * n];
            for (int y = 0; y < n; y++) {
                for (int x = 0; x < n; x++) {
                    double sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int j = 0; j < 2; j++) {
                        for (int i = 0; i < 2; i++) {
                            int p = px[(y * 2 + j) * w + x * 2 + i];
                            double al = (p >>> 24) / 255.0;
                            sa += al;
                            sr += ((p >> 16) & 255) * al;
                            sg += ((p >> 8) & 255) * al;
                            sb += (p & 255) * al;
                        }
                    }
                    int A = (int) Math.round(sa / 4 * 255);
                    int R = sa > 0 ? (int) Math.round(sr / sa) : 0, G = sa > 0 ? (int) Math.round(sg / sa) : 0, B = sa > 0 ? (int) Math.round(sb / sa) : 0;
                    next[y * n + x] = (A << 24) | (R << 16) | (G << 8) | B;
                }
            }
            px = next;
            w = n;
            levels.add(px);
        }
        int size = top.getWidth();
        ByteBuffer h = ByteBuffer.allocate(128).order(ByteOrder.LITTLE_ENDIAN);
        h.putInt(0x20534444);                      // "DDS "
        h.putInt(124);                             // header size
        h.putInt(0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000);  // caps, height, width, pitch, pixel format, mip count
        h.putInt(size).putInt(size).putInt(size * 4).putInt(0).putInt(levels.size());
        for (int i = 0; i < 11; i++) h.putInt(0);
        h.putInt(32).putInt(0x41).putInt(0).putInt(32);  // pixel format: RGB + alpha, 32 bit
        h.putInt(0x00FF0000).putInt(0x0000FF00).putInt(0x000000FF).putInt(0xFF000000);
        h.putInt(0x1000 | 0x8 | 0x400000);         // texture, complex, mipmap
        h.putInt(0).putInt(0).putInt(0).putInt(0);
        try (OutputStream o = new BufferedOutputStream(Files.newOutputStream(path))) {
            o.write(h.array());
            for (int[] level : levels) {
                ByteBuffer b = ByteBuffer.allocate(level.length * 4).order(ByteOrder.LITTLE_ENDIAN);
                for (int p : level) b.putInt(p);  // ARGB int little endian = B G R A bytes
                o.write(b.array());
            }
        }
    }
}
