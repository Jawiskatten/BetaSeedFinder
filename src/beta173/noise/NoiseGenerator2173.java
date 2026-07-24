package beta173.noise;

import java.util.Random;

public class NoiseGenerator2173 {
    private static int[][] d = new int[][] { { 1, 1, 0}, { -1, 1, 0}, { 1, -1, 0}, { -1, -1, 0}, { 1, 0, 1}, { -1, 0, 1}, { 1, 0, -1}, { -1, 0, -1}, { 0, 1, 1}, { 0, -1, 1}, { 0, 1, -1}, { 0, -1, -1}};
    private int[] e;
    private byte[] eMod12;
    private double[] zCoordinateCache = new double[0];
    public double a;
    public double b;
    public double c;
    private static final double f = 0.5D * (Math.sqrt(3.0D) - 1.0D);
    private static final double g = (3.0D - Math.sqrt(3.0D)) / 6.0D;

    public NoiseGenerator2173() {
        this(new Random());
    }

    public NoiseGenerator2173(Random random) {
        this.e = new int[512];
        this.eMod12 = new byte[512];
        reseed(random);
    }

    /** Rebuilds this simplex generator in place with constructor-identical state. */
    public void reseed(Random random) {
        this.a = random.nextDouble() * 256.0D;
        this.b = random.nextDouble() * 256.0D;
        this.c = random.nextDouble() * 256.0D;

        int i;
        for (i = 0; i < 256; this.e[i] = i++) {
            ;
        }

        for (i = 0; i < 256; ++i) {
            int j = random.nextInt(256 - i) + i;
            int k = this.e[i];
            this.e[i] = this.e[j];
            this.e[j] = k;
            this.e[i + 256] = this.e[i];
        }

        for (i = 0; i < 512; ++i) {
            this.eMod12[i] = (byte)(this.e[i] % 12);
        }
    }

    private static int a(double d0) {
        return d0 > 0.0D ? (int) d0 : (int) d0 - 1;
    }

    private static double legacyGradient2(int[] gradient, double x, double z) {
        return (double) gradient[0] * x + (double) gradient[1] * z;
    }

    private static double gradient2(int gradient, double x, double z) {
        switch (gradient) {
            case 0: return 1.0D * x + 1.0D * z;
            case 1: return -1.0D * x + 1.0D * z;
            case 2: return 1.0D * x + -1.0D * z;
            case 3: return -1.0D * x + -1.0D * z;
            case 4:
            case 6: return 1.0D * x + 0.0D * z;
            case 5:
            case 7: return -1.0D * x + 0.0D * z;
            case 8:
            case 10: return 0.0D * x + 1.0D * z;
            default: return 0.0D * x + -1.0D * z;
        }
    }

    public void a(double[] adouble, double d0, double d1, int i, int j, double d2, double d3, double d4) {
        this.aStrided(adouble, d0, d1, i, j, 1, 1, d2, d3, d4);
    }

    /**
     * Exact sparse-grid variant of the legacy 2D simplex sampler.
     *
     * A dense call at start + n is equivalent to a strided call at
     * start + n * stride. Keeping the original start and scale here avoids
     * rescaling coordinates and preserves the same arithmetic path at the
     * sampled locations.
     */
    public void aStrided(
            double[] adouble,
            double d0,
            double d1,
            int i,
            int j,
            int strideX,
            int strideZ,
            double d2,
            double d3,
            double d4
    ) {
        int k = 0;

        if (this.zCoordinateCache.length < j) {
            this.zCoordinateCache = new double[j];
        }
        for (int i1 = 0; i1 < j; ++i1) {
            this.zCoordinateCache[i1] = (d1 + (double) (i1 * strideZ)) * d3 + this.b;
        }

        int[] permutation = this.e;
        byte[] permutationMod12 = this.eMod12;

        for (int l = 0; l < i; ++l) {
            double d5 = (d0 + (double) (l * strideX)) * d2 + this.a;

            for (int i1 = 0; i1 < j; ++i1) {
                double d6 = this.zCoordinateCache[i1];
                double d7 = (d5 + d6) * f;
                int j1 = a(d5 + d7);
                int k1 = a(d6 + d7);
                double d8 = (double) (j1 + k1) * g;
                double d9 = (double) j1 - d8;
                double d10 = (double) k1 - d8;
                double d11 = d5 - d9;
                double d12 = d6 - d10;
                int b0;
                int b1;

                if (d11 > d12) {
                    b0 = 1;
                    b1 = 0;
                } else {
                    b0 = 0;
                    b1 = 1;
                }

                double d13 = d11 - (double) b0 + g;
                double d14 = d12 - (double) b1 + g;
                double d15 = d11 - 1.0D + 2.0D * g;
                double d16 = d12 - 1.0D + 2.0D * g;
                int l1 = j1 & 255;
                int i2 = k1 & 255;
                int j2 = permutationMod12[l1 + permutation[i2]];
                int k2 = permutationMod12[l1 + b0 + permutation[i2 + b1]];
                int l2 = permutationMod12[l1 + 1 + permutation[i2 + 1]];
                double d17 = 0.5D - d11 * d11 - d12 * d12;
                double d18;

                if (d17 < 0.0D) {
                    d18 = 0.0D;
                } else {
                    d17 *= d17;
                    d18 = d17 * d17 * gradient2(j2, d11, d12);
                }

                double d19 = 0.5D - d13 * d13 - d14 * d14;
                double d20;

                if (d19 < 0.0D) {
                    d20 = 0.0D;
                } else {
                    d19 *= d19;
                    d20 = d19 * d19 * gradient2(k2, d13, d14);
                }

                double d21 = 0.5D - d15 * d15 - d16 * d16;
                double d22;

                if (d21 < 0.0D) {
                    d22 = 0.0D;
                } else {
                    d21 *= d21;
                    d22 = d21 * d21 * gradient2(l2, d15, d16);
                }

                adouble[k++] += 70.0D * (d18 + d20 + d22) * d4;
            }
        }
    }

    public void aStridedLegacy(
            double[] adouble,
            double d0,
            double d1,
            int i,
            int j,
            int strideX,
            int strideZ,
            double d2,
            double d3,
            double d4
    ) {
        int k = 0;

        for (int l = 0; l < i; ++l) {
            double d5 = (d0 + (double) (l * strideX)) * d2 + this.a;

            for (int i1 = 0; i1 < j; ++i1) {
                double d6 = (d1 + (double) (i1 * strideZ)) * d3 + this.b;
                double d7 = (d5 + d6) * f;
                int j1 = a(d5 + d7);
                int k1 = a(d6 + d7);
                double d8 = (double) (j1 + k1) * g;
                double d9 = (double) j1 - d8;
                double d10 = (double) k1 - d8;
                double d11 = d5 - d9;
                double d12 = d6 - d10;
                byte b0;
                byte b1;

                if (d11 > d12) {
                    b0 = 1;
                    b1 = 0;
                } else {
                    b0 = 0;
                    b1 = 1;
                }

                double d13 = d11 - (double) b0 + g;
                double d14 = d12 - (double) b1 + g;
                double d15 = d11 - 1.0D + 2.0D * g;
                double d16 = d12 - 1.0D + 2.0D * g;
                int l1 = j1 & 255;
                int i2 = k1 & 255;
                int j2 = this.e[l1 + this.e[i2]] % 12;
                int k2 = this.e[l1 + b0 + this.e[i2 + b1]] % 12;
                int l2 = this.e[l1 + 1 + this.e[i2 + 1]] % 12;
                double d17 = 0.5D - d11 * d11 - d12 * d12;
                double d18;

                if (d17 < 0.0D) {
                    d18 = 0.0D;
                } else {
                    d17 *= d17;
                    d18 = d17 * d17 * legacyGradient2(d[j2], d11, d12);
                }

                double d19 = 0.5D - d13 * d13 - d14 * d14;
                double d20;

                if (d19 < 0.0D) {
                    d20 = 0.0D;
                } else {
                    d19 *= d19;
                    d20 = d19 * d19 * legacyGradient2(d[k2], d13, d14);
                }

                double d21 = 0.5D - d15 * d15 - d16 * d16;
                double d22;

                if (d21 < 0.0D) {
                    d22 = 0.0D;
                } else {
                    d21 *= d21;
                    d22 = d21 * d21 * legacyGradient2(d[l2], d15, d16);
                }

                int i3 = k++;

                adouble[i3] += 70.0D * (d18 + d20 + d22) * d4;
            }
        }
    }
}
