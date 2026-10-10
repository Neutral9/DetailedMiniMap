// Makes the minimap frames: dist/Textures/DetailedMiniMap/frames/<Style>/round.dds and square.dds (512 x 512 BGRA,
// full mip chain). Each is rendered as relief: a height field (the band's bevelled profile, raised and carved
// ornaments, grain), lit by a light from the top left (diffuse and specular), with glows of its own, a shadow cast
// outwards and a shadow falling inwards over the map's edge. The map's picture fills the hole: its edge is at HOLE
// of the texture's half size. Drawn at twice the size, then halved.
//   java tools/MakeFrames.java dist [<folder for PNG previews>]
// Any file can be replaced by an own DDS (the hole at the same place); a new folder is a new frame the menu offers.
import java.awt.*;
import java.awt.geom.*;
import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.*;
import java.nio.file.*;
import java.util.*;
import java.util.List;

public class MakeFrames {
    static final int OUT_SIZE = 512;
    static final int W = OUT_SIZE * 2;      // worked at twice the size
    static final double C = W / 2.0;
    static final double HOLE = 0.80 * C;    // the picture's edge
    static final double RIM = C - 26;       // the band's outer edge (room beyond for spikes, glow, shadow)
    static final double CORNER = 0.18;      // the square's corners, of its half size

    // ---- the canvas: per pixel height, colour, specular, glow, coverage
    static final class Relief {
        final double[] h = new double[W * W];
        final double[] r = new double[W * W], g = new double[W * W], b = new double[W * W];
        final double[] ks = new double[W * W];      // specular strength
        final double[] shin = new double[W * W];    // shininess
        final double[] er = new double[W * W], eg = new double[W * W], eb = new double[W * W];  // glow (added after the light)
        final double[] a = new double[W * W];       // coverage
        final boolean round;

        Relief(boolean round) {
            this.round = round;
        }
    }

    interface Style {
        void draw(Relief rel, Random rnd);
    }

    public static void main(String[] args) throws Exception {
        Path out = Paths.get(args[0], "Textures", "DetailedMiniMap", "frames");
        Map<String, Style> styles = new LinkedHashMap<>();
        styles.put("Runestone", MakeFrames::runestone);
        styles.put("Frost", MakeFrames::frost);
        styles.put("GoldRope", MakeFrames::goldRope);
        styles.put("Ironwork", MakeFrames::ironwork);
        styles.put("Embers", MakeFrames::embers);
        styles.put("Compass", MakeFrames::compass);
        styles.put("NordicKnot", MakeFrames::nordicKnot);
        styles.put("Leather", MakeFrames::leather);
        styles.put("Oakwood", MakeFrames::oakwood);
        styles.put("Dwemer", MakeFrames::dwemer);
        styles.put("Parchment", MakeFrames::parchment);
        for (var e : styles.entrySet()) {
            for (boolean round : new boolean[] { true, false }) {
                Relief rel = new Relief(round);
                e.getValue().draw(rel, new Random(e.getKey().hashCode() * 31L + (round ? 1 : 2)));
                BufferedImage img = halve(render(rel));
                Path dir = out.resolve(e.getKey());
                Files.createDirectories(dir);
                String name = round ? "round" : "square";
                writeDds(img, dir.resolve(name + ".dds"));
                if (args.length > 1) {
                    Files.createDirectories(Paths.get(args[1]));
                    javax.imageio.ImageIO.write(img, "png", Paths.get(args[1], e.getKey() + "_" + name + ".png").toFile());
                }
                System.out.println(dir.resolve(name + ".dds"));
            }
        }
    }

    // ---- geometry

    static Shape outline(boolean round, double r) {
        return round ? new Ellipse2D.Double(C - r, C - r, 2 * r, 2 * r) : new RoundRectangle2D.Double(C - r, C - r, 2 * r, 2 * r, 2 * r * CORNER, 2 * r * CORNER);
    }

    // the "radius" of a pixel: the r of the outline it lies on
    static double rho(boolean round, double x, double y) {
        double dx = Math.abs(x - C), dy = Math.abs(y - C);
        if (round) {
            return Math.hypot(dx, dy);
        }
        double lo = 0, hi = C * 1.5;
        for (int i = 0; i < 26; i++) {
            double r = (lo + hi) / 2, k = r * CORNER;
            double qx = dx - (r - k), qy = dy - (r - k);
            double sd = Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - k;
            if (sd > 0) {
                lo = r;
            } else {
                hi = r;
            }
        }
        return (lo + hi) / 2;
    }

    // a point along the outline at r (t in [0, 1) round it, from the top, clockwise) and the outward normal there
    static double[] along(boolean round, double r, double t) {
        List<double[]> pts = new ArrayList<>();
        double[] p = new double[6];
        for (PathIterator it = outline(round, r).getPathIterator(null, 0.25); !it.isDone(); it.next()) {
            if (it.currentSegment(p) != PathIterator.SEG_CLOSE) {
                // long straight sides cut into short steps: the top middle is a point of every outline (it is where
                // t starts; a side's corner instead put the start a quarter round on some outlines and not others)
                if (!pts.isEmpty()) {
                    double[] q = pts.get(pts.size() - 1);
                    int n = (int) Math.ceil(Math.hypot(p[0] - q[0], p[1] - q[1]) / 2.0);
                    for (int k = 1; k < n; k++) {
                        pts.add(new double[] { q[0] + (p[0] - q[0]) * k / n, q[1] + (p[1] - q[1]) * k / n });
                    }
                }
                pts.add(new double[] { p[0], p[1] });
            }
        }
        // start at the top middle
        int start = 0;
        double best = Double.MAX_VALUE;
        for (int i = 0; i < pts.size(); i++) {
            double d = Math.abs(pts.get(i)[0] - C) + (pts.get(i)[1] > C ? 1e9 : 0);
            if (d < best) {
                best = d;
                start = i;
            }
        }
        Collections.rotate(pts, -start);
        pts.add(pts.get(0));
        double total = 0;
        double[] len = new double[pts.size()];
        for (int i = 1; i < pts.size(); i++) {
            total += Math.hypot(pts.get(i)[0] - pts.get(i - 1)[0], pts.get(i)[1] - pts.get(i - 1)[1]);
            len[i] = total;
        }
        double want = (((t % 1) + 1) % 1) * total;
        for (int i = 1; i < pts.size(); i++) {
            if (len[i] >= want) {
                double seg = len[i] - len[i - 1], u = seg > 0 ? (want - len[i - 1]) / seg : 0;
                double x = pts.get(i - 1)[0] + (pts.get(i)[0] - pts.get(i - 1)[0]) * u, y = pts.get(i - 1)[1] + (pts.get(i)[1] - pts.get(i - 1)[1]) * u;
                double tx = pts.get(i)[0] - pts.get(i - 1)[0], ty = pts.get(i)[1] - pts.get(i - 1)[1], tl = Math.hypot(tx, ty);
                double nx = tl > 0 ? ty / tl : 0, ny = tl > 0 ? -tx / tl : 0;
                if ((x - C) * nx + (y - C) * ny < 0) {
                    nx = -nx;
                    ny = -ny;
                }
                return new double[] { x, y, nx, ny };
            }
        }
        return new double[] { C, C - r, 0, -1 };
    }

    // a shape (x along the outline, y outwards) placed at a point of it
    static Shape placed(Shape s, double[] at) {
        AffineTransform t = new AffineTransform();
        t.translate(at[0], at[1]);
        t.rotate(Math.atan2(at[3], at[2]) - Math.PI / 2);
        return t.createTransformedShape(s);
    }

    // ---- noise

    static double hash(int x, int y, int seed) {
        long h = x * 374761393L + y * 668265263L + seed * 2147483647L;
        h = (h ^ (h >>> 13)) * 1274126177L;
        h ^= h >>> 16;
        return (h & 0xFFFFFF) / (double) 0xFFFFFF;
    }

    static double noise(double x, double y, int seed) {
        int xi = (int) Math.floor(x), yi = (int) Math.floor(y);
        double fx = x - xi, fy = y - yi;
        double u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
        double a = hash(xi, yi, seed), b = hash(xi + 1, yi, seed), c = hash(xi, yi + 1, seed), d = hash(xi + 1, yi + 1, seed);
        return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
    }

    static double fbm(double x, double y, int seed, int octaves) {
        double s = 0, amp = 0.5, f = 1;
        for (int i = 0; i < octaves; i++) {
            s += noise(x * f, y * f, seed + i * 17) * amp;
            amp *= 0.5;
            f *= 2.03;
        }
        return s;
    }

    static double smooth(double e0, double e1, double x) {
        double t = Math.max(0, Math.min(1, (x - e0) / (e1 - e0)));
        return t * t * (3 - 2 * t);
    }

    // ---- painting

    interface Profile {
        // u: across the band, 0 at the hole, 1 at the rim; px, py the pixel; fills h, the colour, ks, shin
        void at(double u, double px, double py, double[] out);
    }

    // the band: every pixel between the hole and the rim, by its place across the band
    static void band(Relief rel, double from, double to, Profile p) {
        double[] o = new double[6];
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                double rr = rho(rel.round, x + 0.5, y + 0.5);
                if (rr < from - 1 || rr > to + 1) {
                    continue;
                }
                double cover = Math.min(smooth(from - 1, from + 1, rr), 1 - smooth(to - 1, to + 1, rr));
                if (cover <= 0) {
                    continue;
                }
                p.at((rr - from) / (to - from), x, y, o);
                int i = y * W + x;
                rel.h[i] = rel.h[i] * (1 - cover) + o[0] * cover;
                rel.r[i] = rel.r[i] * (1 - cover) + o[1] * cover;
                rel.g[i] = rel.g[i] * (1 - cover) + o[2] * cover;
                rel.b[i] = rel.b[i] * (1 - cover) + o[3] * cover;
                rel.ks[i] = rel.ks[i] * (1 - cover) + o[4] * cover;
                rel.shin[i] = rel.shin[i] * (1 - cover) + o[5] * cover;
                rel.a[i] = Math.max(rel.a[i], cover);
            }
        }
    }

    static float[] mask(Shape s) {
        BufferedImage m = new BufferedImage(W, W, BufferedImage.TYPE_INT_ARGB);
        Graphics2D g = m.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setRenderingHint(RenderingHints.KEY_STROKE_CONTROL, RenderingHints.VALUE_STROKE_PURE);
        g.setColor(Color.WHITE);
        g.fill(s);
        g.dispose();
        float[] out = new float[W * W];
        int[] px = m.getRGB(0, 0, W, W, null, 0, W);
        for (int i = 0; i < out.length; i++) {
            out[i] = (px[i] >>> 24) / 255f;
        }
        return out;
    }

    static float[] blur(float[] src, int radius) {
        if (radius <= 0) {
            return src.clone();
        }
        float[] tmp = new float[src.length], out = new float[src.length];
        for (int pass = 0; pass < 2; pass++) {
            float[] in = pass == 0 ? src : out;
            // horizontal
            for (int y = 0; y < W; y++) {
                double sum = 0;
                for (int x = -radius; x <= radius; x++) {
                    sum += in[y * W + Math.max(0, Math.min(W - 1, x))];
                }
                for (int x = 0; x < W; x++) {
                    tmp[y * W + x] = (float) (sum / (2 * radius + 1));
                    sum += in[y * W + Math.min(W - 1, x + radius + 1)] - in[y * W + Math.max(0, x - radius)];
                }
            }
            // vertical
            for (int x = 0; x < W; x++) {
                double sum = 0;
                for (int y = -radius; y <= radius; y++) {
                    sum += tmp[Math.max(0, Math.min(W - 1, y)) * W + x];
                }
                for (int y = 0; y < W; y++) {
                    out[y * W + x] = (float) (sum / (2 * radius + 1));
                    sum += tmp[Math.min(W - 1, y + radius + 1) * W + x] - tmp[Math.max(0, y - radius) * W + x];
                }
            }
        }
        return out;
    }

    // a raised (height > 0) or carved (< 0) ornament: rounded off over bevel px at its edges, its colour where it is
    // a shape cut to the band, a_margin px short of its rims
    static Shape clip(boolean round, Shape s, double a_margin) {
        Area out = new Area(s);
        Area band = new Area(outline(round, RIM - a_margin));
        band.subtract(new Area(outline(round, HOLE + a_margin)));
        out.intersect(band);
        return out;
    }

    static void emboss(Relief rel, Shape s, double height, int bevel, Color col, double ks, double shin) {
        float[] m = mask(s);
        float[] soft = blur(m, bevel);
        for (int i = 0; i < m.length; i++) {
            if (soft[i] <= 0 && m[i] <= 0) {
                continue;
            }
            double lift = Math.min(1, soft[i] * 2);
            rel.h[i] += height * (height > 0 ? lift * m[i] : Math.max(lift, m[i]));
            double c = m[i];
            if (col != null) {
                rel.r[i] += (col.getRed() - rel.r[i]) * c;
                rel.g[i] += (col.getGreen() - rel.g[i]) * c;
                rel.b[i] += (col.getBlue() - rel.b[i]) * c;
                rel.ks[i] += (ks - rel.ks[i]) * c;
                rel.shin[i] += (shin - rel.shin[i]) * c;
            }
            rel.a[i] = Math.max(rel.a[i], c);
        }
    }

    // light of its own (runes, lava, frost), spread over glow px
    static void glow(Relief rel, Shape s, Color col, double strength, int spread) {
        float[] m = mask(s);
        float[] soft = blur(m, spread);
        for (int i = 0; i < m.length; i++) {
            double v = Math.max(m[i], soft[i] * 1.6) * strength;
            if (v <= 0) {
                continue;
            }
            rel.er[i] += col.getRed() * v;
            rel.eg[i] += col.getGreen() * v;
            rel.eb[i] += col.getBlue() * v;
        }
    }

    // ---- lighting and the shadows

    static BufferedImage render(Relief rel) {
        double lx = -0.55, ly = -0.68, lz = 0.48, ll = Math.sqrt(lx * lx + ly * ly + lz * lz);
        lx /= ll;
        ly /= ll;
        lz /= ll;
        double hx = lx, hy = ly, hz = lz + 1, hl = Math.sqrt(hx * hx + hy * hy + hz * hz);
        hx /= hl;
        hy /= hl;
        hz /= hl;
        float[] cover = new float[W * W];
        for (int i = 0; i < cover.length; i++) {
            cover[i] = (float) rel.a[i];
        }
        // the shadow it casts outwards (down right, soft), and the light it gives off spreading past it
        float[] shadow = blur(cover, 9);
        float[] glowR = new float[W * W], glowG = new float[W * W], glowB = new float[W * W];
        for (int i = 0; i < cover.length; i++) {
            glowR[i] = (float) rel.er[i];
            glowG[i] = (float) rel.eg[i];
            glowB[i] = (float) rel.eb[i];
        }
        float[] haloR = blur(glowR, 8), haloG = blur(glowG, 8), haloB = blur(glowB, 8);
        BufferedImage img = new BufferedImage(W, W, BufferedImage.TYPE_INT_ARGB);
        int sx = 5, sy = 8;
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                double rr = rho(rel.round, x + 0.5, y + 0.5);
                double R = 0, G = 0, B = 0, A = 0;
                // inside the hole: the shadow falling over the map's edge
                if (rr < HOLE + 1) {
                    double t = smooth(HOLE - 0.17 * C, HOLE, rr);
                    A = 0.82 * Math.pow(t, 1.6);
                }
                // outside: the cast shadow
                int si = Math.max(0, Math.min(W - 1, y - sy)) * W + Math.max(0, Math.min(W - 1, x - sx));
                if (rr > HOLE) {
                    double sh = shadow[si] * 0.7;
                    A = A + sh * (1 - A);
                }
                // the band itself, lit
                double c = rel.a[i];
                if (c > 0) {
                    int xm = Math.max(0, x - 1), xp = Math.min(W - 1, x + 1), ym = Math.max(0, y - 1), yp = Math.min(W - 1, y + 1);
                    double dhx = (rel.h[y * W + xp] - rel.h[y * W + xm]) / 2, dhy = (rel.h[yp * W + x] - rel.h[ym * W + x]) / 2;
                    double nx = -dhx, ny = -dhy, nz = 1, nl = Math.sqrt(nx * nx + ny * ny + nz * nz);
                    nx /= nl;
                    ny /= nl;
                    nz /= nl;
                    double dif = Math.max(0, nx * lx + ny * ly + nz * lz);
                    double spec = Math.pow(Math.max(0, nx * hx + ny * hy + nz * hz), Math.max(1, rel.shin[i])) * rel.ks[i];
                    double lit = 0.32 + dif * 0.95;
                    double cr = rel.r[i] * lit + 255 * spec + rel.er[i];
                    double cg = rel.g[i] * lit + 245 * spec + rel.eg[i];
                    double cb = rel.b[i] * lit + 225 * spec + rel.eb[i];
                    // over the shadow
                    R = cr * c;
                    G = cg * c;
                    B = cb * c;
                    A = c + A * (1 - c);
                    R /= Math.max(A, 1e-6);
                    G /= Math.max(A, 1e-6);
                    B /= Math.max(A, 1e-6);
                }
                // the halo of its glows, past the band (added light: as colour with its own alpha)
                // off the band its own glows show crisp (flames, sparks), the halo round them soft
                final double own = rel.a[i] <= 0 ? 1 : 0;
                double hr = Math.max(haloR[i], rel.er[i] * own), hg = Math.max(haloG[i], rel.eg[i] * own), hb = Math.max(haloB[i], rel.eb[i] * own), hm = Math.max(hr, Math.max(hg, hb));
                if (hm > 1 && rr > HOLE) {
                    double ha = Math.min(1, hm / 255.0) * 0.9;
                    double outA = ha + A * (1 - ha);
                    R = (hr / hm * 255 * ha + R * A * (1 - ha)) / Math.max(outA, 1e-6);
                    G = (hg / hm * 255 * ha + G * A * (1 - ha)) / Math.max(outA, 1e-6);
                    B = (hb / hm * 255 * ha + B * A * (1 - ha)) / Math.max(outA, 1e-6);
                    A = outA;
                }
                int ai = (int) Math.round(Math.max(0, Math.min(1, A)) * 255);
                img.setRGB(x, y, (ai << 24) | (clamp(R) << 16) | (clamp(G) << 8) | clamp(B));
            }
        }
        return img;
    }

    static int clamp(double v) {
        return (int) Math.max(0, Math.min(255, Math.round(v)));
    }

    // 2 x 2 pixels into one (premultiplied)
    static BufferedImage halve(BufferedImage src) {
        int n = src.getWidth() / 2;
        BufferedImage out = new BufferedImage(n, n, BufferedImage.TYPE_INT_ARGB);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                double sa = 0, sr = 0, sg = 0, sb = 0;
                for (int j = 0; j < 2; j++) {
                    for (int i = 0; i < 2; i++) {
                        int p = src.getRGB(x * 2 + i, y * 2 + j);
                        double a = (p >>> 24) / 255.0;
                        sa += a;
                        sr += ((p >> 16) & 255) * a;
                        sg += ((p >> 8) & 255) * a;
                        sb += (p & 255) * a;
                    }
                }
                int A = clamp(sa / 4 * 255);
                out.setRGB(x, y, (A << 24) | (sa > 0 ? clamp(sr / sa) << 16 | clamp(sg / sa) << 8 | clamp(sb / sa) : 0));
            }
        }
        return out;
    }

    static void set(double[] o, double h, Color c, double ks, double shin) {
        o[0] = h;
        o[1] = c.getRed();
        o[2] = c.getGreen();
        o[3] = c.getBlue();
        o[4] = ks;
        o[5] = shin;
    }

    static Color mix(Color a, Color b, double t) {
        t = Math.max(0, Math.min(1, t));
        return new Color((int) Math.round(a.getRed() + (b.getRed() - a.getRed()) * t), (int) Math.round(a.getGreen() + (b.getGreen() - a.getGreen()) * t),
            (int) Math.round(a.getBlue() + (b.getBlue() - a.getBlue()) * t));
    }

    // a band profile: rounded rims at both edges (lip px wide, raised by lip height), a body between
    static double rims(double u, double width, double lip, double lipH, double body) {
        double d0 = u * width, d1 = (1 - u) * width;
        double inner = d0 < lip ? Math.sin(Math.PI * d0 / lip) * lipH : 0;
        double outer = d1 < lip ? Math.sin(Math.PI * d1 / lip) * lipH : 0;
        double bodyH = smooth(0, lip * 0.6, d0) * smooth(0, lip * 0.6, d1) * body;
        return Math.max(inner, Math.max(outer, bodyH));
    }

    // ---- the styles

    // weathered dark stone: a raised lip each side, runes cut deep, a faint blue light in them
    static void runestone(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double grain = fbm(px / 26.0, py / 26.0, 3, 5);
            double cracks = Math.pow(Math.abs(fbm(px / 60.0, py / 60.0, 9, 4) - 0.5) * 2, 0.25);
            double h = rims(u, width, 22, 22, 14) + grain * 9 - (1 - cracks) * 6;
            Color c = mix(new Color(44, 46, 54), new Color(96, 96, 104), grain * 1.2 - 0.2);
            set(o, h, mix(c, new Color(20, 20, 24), (1 - cracks) * 0.6), 0.10, 12);
        });
        int count = rel.round ? 24 : 28;
        double mid = (HOLE + RIM) / 2;
        for (int i = 0; i < count; i++) {
            double[] at = along(rel.round, mid, (i + 0.5) / count);
            Path2D rune = new Path2D.Double();
            int strokes = 2 + rnd.nextInt(3);
            for (int s = 0; s < strokes; s++) {
                double x0 = (rnd.nextInt(3) - 1) * 11, y0 = (rnd.nextInt(3) - 1) * 19, x1 = (rnd.nextInt(3) - 1) * 11, y1 = (rnd.nextInt(3) - 1) * 19;
                if (x0 == x1 && y0 == y1) {
                    y1 = -y0 + 19;
                }
                rune.moveTo(x0, y0);
                rune.lineTo(x1, y1);
            }
            Shape cut = new BasicStroke(9f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(placed(rune, at));
            // inside the band only, short of its rims (a rune at a corner of the square leaned out of it)
            emboss(rel, clip(rel.round, cut, 12), -18, 2, new Color(20, 24, 34), 0.05, 8);
            glow(rel, clip(rel.round, new BasicStroke(4.5f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(placed(rune, at)), 14), new Color(120, 200, 255), 1.0, 4);
        }
    }

    // ice: glassy teal, bright facets, crystals standing out of it, a cold glow round it
    static void frost(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM - 6, (u, px, py, o) -> {
            double facet = fbm(px / 18.0, py / 18.0, 21, 3);
            double h = rims(u, width - 6, 18, 18, 12) + Math.floor(facet * 6) / 6 * 10;
            Color c = mix(new Color(30, 120, 160), new Color(150, 230, 250), facet * 1.3 - 0.15);
            set(o, h, c, 0.9, 60);
        });
        int count = rel.round ? 26 : 30;
        for (int i = 0; i < count; i++) {
            double[] at = along(rel.round, RIM - 18, (i + rnd.nextDouble() * 0.4) / count);
            double h = 26 + rnd.nextInt(26), w = 8 + rnd.nextInt(6);
            Path2D shard = new Path2D.Double();
            shard.moveTo(-w, 0);
            shard.lineTo(-w * 0.3, h * 0.7);
            shard.lineTo(0, h);
            shard.lineTo(w * 0.4, h * 0.65);
            shard.lineTo(w, 0);
            shard.closePath();
            Shape s = placed(shard, at);
            emboss(rel, s, 30, 3, new Color(200, 245, 255), 1.0, 80);
            glow(rel, s, new Color(70, 200, 255), 0.18, 10);
        }
        for (int i = 0; i < 90; i++) {  // sparkles
            double[] at = along(rel.round, HOLE + 8 + rnd.nextDouble() * (width - 24), rnd.nextDouble());
            double r = 1.5 + rnd.nextDouble() * 2.5;
            glow(rel, new Ellipse2D.Double(at[0] - r, at[1] - r, 2 * r, 2 * r), new Color(220, 250, 255), 0.9, 2);
        }
    }

    // a twisted gold rope between two engraved silver rims
    static void goldRope(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        double silver = 16;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double d0 = u * width, d1 = (1 - u) * width;
            boolean rim = d0 < silver || d1 < silver;
            double h = rim ? Math.sin(Math.PI * Math.min(d0, d1) / silver) * 16 : 2;
            double grain = fbm(px / 8.0, py / 8.0, 5, 3) * 3;
            set(o, h + (rim ? grain * 0.4 : 0), rim ? new Color(196, 198, 206) : new Color(40, 28, 10), rim ? 0.85 : 0.1, rim ? 50 : 8);
        });
        double mid = (HOLE + RIM) / 2, rope = width - 2 * silver;
        int count = rel.round ? 70 : 80;
        for (int i = 0; i < count; i++) {
            double[] at = along(rel.round, mid, (double) i / count);
            Shape strand = AffineTransform.getRotateInstance(Math.toRadians(40)).createTransformedShape(
                new Ellipse2D.Double(-rope * 0.22, -rope * 0.62, rope * 0.44, rope * 1.24));
            emboss(rel, placed(strand, at), 24, 5, new Color(222, 172, 70), 0.95, 40);
        }
        // engraved dots on the silver
        int dots = rel.round ? 90 : 104;
        for (int i = 0; i < dots; i++) {
            for (double rr : new double[] { HOLE + silver / 2, RIM - silver / 2 }) {
                double[] at = along(rel.round, rr, (i + 0.5) / dots);
                emboss(rel, new Ellipse2D.Double(at[0] - 2.2, at[1] - 2.2, 4.4, 4.4), -5, 1, null, 0, 0);
            }
        }
    }

    // forged iron plates, each bevelled, riveted, hammer-marked, a little rust
    static void ironwork(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> set(o, 0, new Color(22, 22, 24), 0.2, 10));
        int count = rel.round ? 16 : 20;
        double inner = HOLE + 4, outer = RIM - 4;
        for (int i = 0; i < count; i++) {
            double t0 = (i + 0.04) / count, t1 = (i + 0.96) / count;
            Path2D plate = new Path2D.Double();
            int steps = 16;
            for (int k = 0; k <= steps; k++) {
                double[] p = along(rel.round, outer, t0 + (t1 - t0) * k / steps);
                if (k == 0) {
                    plate.moveTo(p[0], p[1]);
                } else {
                    plate.lineTo(p[0], p[1]);
                }
            }
            for (int k = steps; k >= 0; k--) {
                double[] p = along(rel.round, inner, t0 + (t1 - t0) * k / steps);
                plate.lineTo(p[0], p[1]);
            }
            plate.closePath();
            double rust = rnd.nextDouble();
            emboss(rel, plate, 26, 7, mix(new Color(92, 92, 98), new Color(110, 72, 44), rust * 0.45), 0.55, 24);
            for (double side : new double[] { 0.16, 0.84 }) {
                for (double rr : new double[] { inner + width * 0.27, outer - width * 0.27 }) {
                    double[] r = along(rel.round, rr, t0 + (t1 - t0) * side);
                    emboss(rel, new Ellipse2D.Double(r[0] - 6.5, r[1] - 6.5, 13, 13), 12, 4, new Color(150, 148, 144), 0.9, 40);
                }
            }
        }
        // hammer marks and scratches in the height
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                if (rel.a[i] > 0) {
                    rel.h[i] += (fbm(x / 14.0, y / 14.0, 41, 3) - 0.5) * 7;
                }
            }
        }
    }

    // charred obsidian with lava in its cracks, flames licking out
    static void embers(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        int count = rel.round ? 34 : 40;
        for (int i = 0; i < count; i++) {  // flames first: under the band
            // rising from under the band's rim, the tips short of the texture's edge (cut off there otherwise)
            double[] at = along(rel.round, RIM - 16, (i + rnd.nextDouble() * 0.6) / count);
            double h = 16 + rnd.nextDouble() * 20, w = 9 + rnd.nextDouble() * 6, lean = rnd.nextDouble() * 8 - 4;
            Path2D f = new Path2D.Double();
            f.moveTo(-w, 0);
            f.quadTo(-w * 0.7, h * 0.55, lean, h);
            f.quadTo(w * 0.7, h * 0.55, w, 0);
            f.closePath();
            Shape s = placed(f, at);
            glow(rel, s, new Color(255, 96, 24), 0.95, 2);
            Shape core = placed(AffineTransform.getScaleInstance(0.5, 0.6).createTransformedShape(f), at);
            glow(rel, core, new Color(255, 214, 110), 0.9, 2);
        }
        band(rel, HOLE, RIM - 10, (u, px, py, o) -> {
            double n = fbm(px / 34.0, py / 34.0, 13, 5);
            double crack = Math.abs(n - 0.5);
            double h = rims(u, width - 10, 16, 16, 12) + n * 10 - (crack < 0.05 ? 10 : 0);
            set(o, h, crack < 0.05 ? new Color(60, 14, 6) : mix(new Color(16, 12, 12), new Color(60, 40, 36), n), crack < 0.05 ? 0.0 : 0.35, 30);
        });
        // lava along the cracks
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                if (rel.a[i] <= 0) {
                    continue;
                }
                double crack = Math.abs(fbm(x / 34.0, y / 34.0, 13, 5) - 0.5);
                if (crack < 0.06) {
                    double v = (1 - crack / 0.06) * rel.a[i];
                    rel.er[i] += 255 * v;
                    rel.eg[i] += 110 * v;
                    rel.eb[i] += 25 * v;
                }
            }
        }
        for (int i = 0; i < 70; i++) {
            double[] at = along(rel.round, RIM + 2 + rnd.nextDouble() * 12, rnd.nextDouble());
            double r = 1.2 + rnd.nextDouble() * 2;
            glow(rel, new Ellipse2D.Double(at[0] - r, at[1] - r, 2 * r, 2 * r), new Color(255, 190, 80), 0.9, 2);
        }
    }

    // polished brass: an engraved scale, a raised outer rim, eight gems set in it
    static void compass(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double d1 = (1 - u) * width;
            double outerRim = d1 < 22 ? Math.sin(Math.PI * d1 / 22) * 18 : 0;
            double h = Math.max(rims(u, width, 12, 10, 6), outerRim);
            double brush = fbm(px / 3.0, py / 60.0, 31, 2) * 2;
            set(o, h + brush, mix(new Color(150, 108, 40), new Color(226, 184, 102), 0.55 + brush * 0.2), 0.9, 36);
        });
        int ticks = rel.round ? 120 : 136;
        for (int i = 0; i < ticks; i++) {
            boolean big = i % 15 == 0;
            double[] at = along(rel.round, HOLE + 8, (double) i / ticks);
            double len = big ? 24 : (i % 5 == 0 ? 16 : 10);
            Shape tick = new BasicStroke(big ? 4.2f : 2.4f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(
                new Line2D.Double(at[0], at[1], at[0] + at[2] * len, at[1] + at[3] * len));
            emboss(rel, tick, -7, 1, new Color(90, 60, 20), 0.2, 10);
        }
    }

    // ---- styles for the vanilla map (sepia, Nordic)

    // dark iron with Nordic knotwork: two silver ribbons weaving over and under each other all round
    static void nordicKnot(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double grain = fbm(px / 16.0, py / 16.0, 51, 3);
            set(o, rims(u, width, 16, 16, 4) + grain * 4, mix(new Color(34, 34, 38), new Color(64, 64, 70), grain), 0.35, 20);
        });
        double mid = (HOLE + RIM) / 2, amp = width * 0.24;
        int waves = rel.round ? 18 : 22, steps = waves * 24;
        Path2D over = new Path2D.Double(), under = new Path2D.Double();
        for (int strand = 0; strand < 2; strand++) {
            for (int k = 0; k < steps; k++) {
                double t0 = (double) k / steps, t1 = (double) (k + 1) / steps;
                double s0 = Math.sin(t0 * waves * 2 * Math.PI) * (strand == 0 ? 1 : -1), s1 = Math.sin(t1 * waves * 2 * Math.PI) * (strand == 0 ? 1 : -1);
                double[] a = along(rel.round, mid + s0 * amp, t0), b = along(rel.round, mid + s1 * amp, t1);
                // the ribbons cross every half wave: which one is on top changes at each crossing
                int half = (int) Math.floor((t0 + 0.25 / waves) * waves * 2);
                ((half + strand) % 2 == 0 ? over : under).append(new Line2D.Double(a[0], a[1], b[0], b[1]), false);
            }
        }
        BasicStroke ribbon = new BasicStroke(9f, BasicStroke.CAP_BUTT, BasicStroke.JOIN_ROUND);
        emboss(rel, clip(rel.round, ribbon.createStrokedShape(under), 4), 10, 3, new Color(150, 152, 158), 0.8, 40);
        // the ribbon on top: a dark gap cut round it first, then raised over the other
        emboss(rel, clip(rel.round, new BasicStroke(15f, BasicStroke.CAP_BUTT, BasicStroke.JOIN_ROUND).createStrokedShape(over), 4), -8, 1, new Color(20, 20, 22), 0.1, 8);
        emboss(rel, clip(rel.round, ribbon.createStrokedShape(over), 4), 20, 3, new Color(176, 178, 184), 0.85, 44);
    }

    // dark leather, creased, stitched along both edges
    static void leather(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double crease = fbm(px / 22.0, py / 22.0, 61, 5);
            double pores = fbm(px / 3.0, py / 3.0, 67, 2);
            double h = rims(u, width, 26, 12, 10) + crease * 8 + pores * 1.5;
            set(o, h, mix(new Color(52, 32, 20), new Color(104, 68, 40), crease * 1.3 - 0.2), 0.25, 14);
        });
        Path2D stitches = new Path2D.Double();
        int count = rel.round ? 120 : 136;
        for (double rr : new double[] { HOLE + 11, RIM - 11 }) {
            for (int i = 0; i < count; i++) {
                double[] a = along(rel.round, rr, (i + 0.15) / count), b = along(rel.round, rr, (i + 0.65) / count);
                stitches.append(new Line2D.Double(a[0], a[1], b[0], b[1]), false);
            }
        }
        emboss(rel, new BasicStroke(7f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(stitches), -4, 1, new Color(30, 18, 10), 0.05, 8);
        emboss(rel, new BasicStroke(3.4f, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND).createStrokedShape(stitches), 6, 1, new Color(196, 170, 120), 0.3, 16);
    }

    // carved oak, its grain running round, bound with iron straps every eighth
    static void oakwood(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double ang = Math.atan2(py - C, px - C);
            double rr = rho(rel.round, px, py);
            double warp = fbm(ang * 9 + 50, rr / 40.0, 71, 3) * 18;
            double grain = 0.5 + 0.5 * Math.sin((rr + warp) * 0.55);
            double knots = fbm(px / 50.0, py / 50.0, 73, 3);
            double h = rims(u, width, 18, 14, 10) + grain * 3 + knots * 4;
            set(o, h, mix(new Color(64, 40, 22), new Color(122, 84, 48), grain * 0.6 + knots * 0.5 - 0.1), 0.15, 12);
        });
        Path2D straps = new Path2D.Double(), rivets = new Path2D.Double();
        for (int i = 0; i < 8; i++) {
            double t = (i + 0.5) / 8, span = rel.round ? 0.022 : 0.018;
            Path2D strap = new Path2D.Double();
            int steps = 8;
            for (int k = 0; k <= steps; k++) {
                double[] p = along(rel.round, RIM + 2, t - span + 2 * span * k / steps);
                if (k == 0) {
                    strap.moveTo(p[0], p[1]);
                } else {
                    strap.lineTo(p[0], p[1]);
                }
            }
            for (int k = steps; k >= 0; k--) {
                double[] p = along(rel.round, HOLE - 2, t - span + 2 * span * k / steps);
                strap.lineTo(p[0], p[1]);
            }
            strap.closePath();
            straps.append(strap, false);
            for (double rr : new double[] { HOLE + width * 0.25, RIM - width * 0.25 }) {
                double[] p = along(rel.round, rr, t);
                rivets.append(new Ellipse2D.Double(p[0] - 5, p[1] - 5, 10, 10), false);
            }
        }
        emboss(rel, clip(rel.round, straps, 0), 16, 4, new Color(58, 58, 62), 0.55, 26);
        emboss(rel, rivets, 10, 3, new Color(120, 118, 114), 0.8, 36);
    }

    // Dwemer bronze, a green patina in its hollows, a stepped rim and angular blocks all round
    static void dwemer(Relief rel, Random rnd) {
        double width = RIM - HOLE;
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double step = u < 0.22 ? 6 : u > 0.78 ? 10 : 2;
            double patina = Math.max(0, fbm(px / 30.0, py / 30.0, 81, 4) - 0.55) * 2.6;
            Color bronze = mix(new Color(150, 92, 46), new Color(208, 150, 86), fbm(px / 9.0, py / 9.0, 83, 2));
            set(o, step + fbm(px / 7.0, py / 7.0, 85, 2) * 1.5, mix(bronze, new Color(80, 130, 104), patina), 0.85 - patina * 0.5, 34);
        });
        int count = rel.round ? 24 : 28;
        Path2D blocks = new Path2D.Double(), cuts = new Path2D.Double();
        for (int i = 0; i < count; i++) {
            double t = (i + 0.5) / count, span = 0.42 / count;
            double[] a = along(rel.round, HOLE + width * 0.3, t - span * 0.6), b = along(rel.round, RIM - width * 0.32, t - span);
            double[] c = along(rel.round, RIM - width * 0.32, t + span), d = along(rel.round, HOLE + width * 0.3, t + span * 0.6);
            Path2D block = new Path2D.Double();
            block.moveTo(a[0], a[1]);
            block.lineTo(b[0], b[1]);
            block.lineTo(c[0], c[1]);
            block.lineTo(d[0], d[1]);
            block.closePath();
            blocks.append(block, false);
            double[] m0 = along(rel.round, HOLE + width * 0.36, t), m1 = along(rel.round, RIM - width * 0.38, t);
            cuts.append(new Line2D.Double(m0[0], m0[1], m1[0], m1[1]), false);
        }
        emboss(rel, blocks, 12, 3, new Color(196, 138, 76), 0.9, 40);
        emboss(rel, new BasicStroke(3f).createStrokedShape(cuts), -8, 1, new Color(90, 60, 30), 0.2, 10);  // a line cut down each block
    }

    // the map's own parchment carried on past its edge: aged, a fine ink border, the outer edge torn and burnt
    static void parchment(Relief rel, Random rnd) {
        band(rel, HOLE, RIM, (u, px, py, o) -> {
            double fibres = fbm(px / 5.0, py / 5.0, 91, 3), stains = fbm(px / 40.0, py / 40.0, 93, 4);
            set(o, fibres * 3 + stains * 4, mix(new Color(214, 192, 148), new Color(160, 128, 84), stains * 1.1 - 0.15), 0.05, 6);
        });
        // a double ink line round the inner edge
        Area ink = new Area(new BasicStroke(3.5f).createStrokedShape(outline(rel.round, HOLE + 10)));
        ink.add(new Area(new BasicStroke(1.6f).createStrokedShape(outline(rel.round, HOLE + 17))));
        emboss(rel, ink, -1.5, 1, new Color(70, 52, 32), 0.05, 6);
        // the outer edge: torn where the noise says, charred brown to black towards it, a few embers still glowing
        for (int y = 0; y < W; y++) {
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                if (rel.a[i] <= 0) {
                    continue;
                }
                double rr = rho(rel.round, x + 0.5, y + 0.5);
                double ang = Math.atan2(y - C, x - C);
                double edge = RIM - 4 - fbm(ang * 30 + 100, 0.5, 95, 4) * 40 - fbm(x / 9.0, y / 9.0, 97, 2) * 8;
                if (rr > edge + 1.5) {
                    rel.a[i] = 0;
                    continue;
                }
                double burn = smooth(edge - 26, edge, rr);
                double k = 1 - burn * 0.92;
                rel.r[i] *= k;
                rel.g[i] *= Math.max(0, k - burn * 0.08);
                rel.b[i] *= Math.max(0, k - burn * 0.12);
                rel.h[i] -= burn * 4;
                rel.a[i] *= smooth(edge + 1.5, edge - 1.5, rr);
                if (burn > 0.85 && fbm(x / 3.0, y / 3.0, 99, 2) > 0.62) {
                    rel.er[i] += 120 * (burn - 0.85) / 0.15;
                    rel.eg[i] += 40 * (burn - 0.85) / 0.15;
                }
            }
        }
    }

    // ---- DDS: uncompressed 32-bit BGRA with every mip level (box filtered, straight alpha)
    static void writeDds(BufferedImage top, Path path) throws IOException {
        List<int[]> levels = new ArrayList<>();
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
        h.putInt(0x20534444);
        h.putInt(124);
        h.putInt(0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000);
        h.putInt(size).putInt(size).putInt(size * 4).putInt(0).putInt(levels.size());
        for (int i = 0; i < 11; i++) h.putInt(0);
        h.putInt(32).putInt(0x41).putInt(0).putInt(32);
        h.putInt(0x00FF0000).putInt(0x0000FF00).putInt(0x000000FF).putInt(0xFF000000);
        h.putInt(0x1000 | 0x8 | 0x400000);
        h.putInt(0).putInt(0).putInt(0).putInt(0);
        try (OutputStream o = new BufferedOutputStream(Files.newOutputStream(path))) {
            o.write(h.array());
            for (int[] level : levels) {
                ByteBuffer b = ByteBuffer.allocate(level.length * 4).order(ByteOrder.LITTLE_ENDIAN);
                for (int p : level) b.putInt(p);
                o.write(b.array());
            }
        }
    }
}
