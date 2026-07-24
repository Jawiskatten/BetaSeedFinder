package beta173.noise;

import java.util.Random;

public class NoiseGeneratorPerlin173 {
    private final int[] d;
    public double a;
    public double b;
    public double c;

    public static final class AxisCache {
        int[] xPerm0 = new int[0];
        int[] xPerm1 = new int[0];
        double[] xFrac = new double[0];
        double[] xFracMinus1 = new double[0];
        double[] xFade = new double[0];

        int[] yIndex = new int[0];
        int[] yCellStart = new int[0];
        double[] yFrac = new double[0];
        double[] yFracMinus1 = new double[0];
        double[] yFade = new double[0];

        int[] zIndex = new int[0];
        double[] zFrac = new double[0];
        double[] zFracMinus1 = new double[0];
        double[] zFade = new double[0];

        void ensure(int xLen, int yLen, int zLen) {
            if (xPerm0.length < xLen) {
                xPerm0 = new int[xLen];
                xPerm1 = new int[xLen];
                xFrac = new double[xLen];
                xFracMinus1 = new double[xLen];
                xFade = new double[xLen];
            }
            if (yIndex.length < yLen) {
                yIndex = new int[yLen];
                yCellStart = new int[yLen];
                yFrac = new double[yLen];
                yFracMinus1 = new double[yLen];
                yFade = new double[yLen];
            }
            if (zIndex.length < zLen) {
                zIndex = new int[zLen];
                zFrac = new double[zLen];
                zFracMinus1 = new double[zLen];
                zFade = new double[zLen];
            }
        }
    }

    public NoiseGeneratorPerlin173() {
        this(new Random());
    }

    public NoiseGeneratorPerlin173(Random var1) {
        this.d = new int[512];
        reseed(var1);
    }

    /** Rebuilds this generator in place with the exact constructor Random sequence. */
    public void reseed(Random var1) {
        this.a = var1.nextDouble() * 256.0D;
        this.b = var1.nextDouble() * 256.0D;
        this.c = var1.nextDouble() * 256.0D;

        int var2;
        for (var2 = 0; var2 < 256; this.d[var2] = var2++) {}

        for (var2 = 0; var2 < 256; ++var2) {
            int var3 = var1.nextInt(256 - var2) + var2;
            int var4 = this.d[var2];
            this.d[var2] = this.d[var3];
            this.d[var3] = var4;
            this.d[var2 + 256] = this.d[var2];
        }
    }

    /** Advances Random exactly like one Perlin constructor, without allocating a permutation table. */
    public static void consumeConstructorRandom(Random random) {
        random.nextDouble();
        random.nextDouble();
        random.nextDouble();
        for (int i = 0; i < 256; i++) {
            random.nextInt(256 - i);
        }
    }

    public double a(double var1, double var3, double var5) {
        double var7 = var1 + this.a;
        double var9 = var3 + this.b;
        double var11 = var5 + this.c;
        int var13 = (int)var7;
        int var14 = (int)var9;
        int var15 = (int)var11;
        if (var7 < (double)var13) --var13;
        if (var9 < (double)var14) --var14;
        if (var11 < (double)var15) --var15;
        int var16 = var13 & 255;
        int var17 = var14 & 255;
        int var18 = var15 & 255;
        var7 -= (double)var13;
        var9 -= (double)var14;
        var11 -= (double)var15;
        double var19 = fade(var7);
        double var21 = fade(var9);
        double var23 = fade(var11);
        int var25 = this.d[var16] + var17;
        int var26 = this.d[var25] + var18;
        int var27 = this.d[var25 + 1] + var18;
        int var28 = this.d[var16 + 1] + var17;
        int var29 = this.d[var28] + var18;
        int var30 = this.d[var28 + 1] + var18;
        return this.b(var23, this.b(var21, this.b(var19, this.a(this.d[var26], var7, var9, var11), this.a(this.d[var29], var7 - 1.0D, var9, var11)), this.b(var19, this.a(this.d[var27], var7, var9 - 1.0D, var11), this.a(this.d[var30], var7 - 1.0D, var9 - 1.0D, var11))), this.b(var21, this.b(var19, this.a(this.d[var26 + 1], var7, var9, var11 - 1.0D), this.a(this.d[var29 + 1], var7 - 1.0D, var9, var11 - 1.0D)), this.b(var19, this.a(this.d[var27 + 1], var7, var9 - 1.0D, var11 - 1.0D), this.a(this.d[var30 + 1], var7 - 1.0D, var9 - 1.0D, var11 - 1.0D))));
    }

    private static double fade(double v) {
        return v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
    }

    public final double b(double var1, double var3, double var5) {
        return var3 + var1 * (var5 - var3);
    }

    public final double a(int var1, double var2, double var4) {
        double zeroX = 0.0D * var2;
        switch (var1 & 15) {
            case 0: return var2 + 0.0D;
            case 1: return -var2 + 0.0D;
            case 2: return var2 + -0.0D;
            case 3: return -var2 + -0.0D;
            case 4: return var2 + var4;
            case 5: return -var2 + var4;
            case 6: return var2 + -var4;
            case 7: return -var2 + -var4;
            case 8: return zeroX + var4;
            case 9: return -zeroX + var4;
            case 10: return zeroX + -var4;
            case 11: return -zeroX + -var4;
            case 12: return zeroX + var2;
            case 13: return -zeroX + var4;
            case 14: return zeroX + -var2;
            default: return -zeroX + -var4;
        }
    }

    public final double a(int var1, double var2, double var4, double var6) {
        switch (var1 & 15) {
            case 0: return var2 + var4;
            case 1: return -var2 + var4;
            case 2: return var2 + -var4;
            case 3: return -var2 + -var4;
            case 4: return var2 + var6;
            case 5: return -var2 + var6;
            case 6: return var2 + -var6;
            case 7: return -var2 + -var6;
            case 8: return var4 + var6;
            case 9: return -var4 + var6;
            case 10: return var4 + -var6;
            case 11: return -var4 + -var6;
            case 12: return var4 + var2;
            case 13: return -var4 + var6;
            case 14: return var4 + -var2;
            default: return -var4 + -var6;
        }
    }

    public double a(double var1, double var3) { return this.a(var1, var3, 0.0D); }

    // Legacy exact method
    public void a(double[] var1, double var2, double var4, double var6, int var8, int var9, int var10, double var11, double var13, double var15, double var17) {
        int var19;
        int var20;
        double var21;
        double var23;
        double var25;
        int var27;
        double var28;
        int var30;
        int var31;
        int var32;
        int var33;
        boolean var36;
        boolean var37;
        double var42;
        int var46;
        if (var9 == 1) {
            boolean var34 = false;
            boolean var35 = false;
            var36 = false;
            var37 = false;
            double var38 = 0.0D;
            double var40 = 0.0D;
            var33 = 0;
            var42 = 1.0D / var17;

            for(int var44 = 0; var44 < var8; ++var44) {
                var21 = (var2 + (double)var44) * var11 + this.a;
                int var45 = (int)var21;
                if (var21 < (double)var45) --var45;
                var46 = var45 & 255;
                var21 -= (double)var45;
                var23 = var21 * var21 * var21 * (var21 * (var21 * 6.0D - 15.0D) + 10.0D);

                for(var27 = 0; var27 < var10; ++var27) {
                    var25 = (var6 + (double)var27) * var15 + this.c;
                    var30 = (int)var25;
                    if (var25 < (double)var30) --var30;
                    var31 = var30 & 255;
                    var25 -= (double)var30;
                    var28 = var25 * var25 * var25 * (var25 * (var25 * 6.0D - 15.0D) + 10.0D);
                    var19 = this.d[var46] + 0;
                    int var47 = this.d[var19] + var31;
                    int var48 = this.d[var46 + 1] + 0;
                    var20 = this.d[var48] + var31;
                    var38 = this.b(var23, this.a(this.d[var47], var21, var25), this.a(this.d[var20], var21 - 1.0D, 0.0D, var25));
                    var40 = this.b(var23, this.a(this.d[var47 + 1], var21, 0.0D, var25 - 1.0D), this.a(this.d[var20 + 1], var21 - 1.0D, 0.0D, var25 - 1.0D));
                    double var49 = this.b(var28, var38, var40);
                    var32 = var33++;
                    var1[var32] += var49 * var42;
                }
            }
        } else {
            var19 = 0;
            double var66 = 1.0D / var17;
            var20 = -1;
            var36 = false;
            var37 = false;
            boolean var67 = false;
            boolean var39 = false;
            boolean var68 = false;
            boolean var41 = false;
            var42 = 0.0D;
            var21 = 0.0D;
            double var69 = 0.0D;
            var23 = 0.0D;

            for(var27 = 0; var27 < var8; ++var27) {
                var25 = (var2 + (double)var27) * var11 + this.a;
                var30 = (int)var25;
                if (var25 < (double)var30) --var30;
                var31 = var30 & 255;
                var25 -= (double)var30;
                var28 = var25 * var25 * var25 * (var25 * (var25 * 6.0D - 15.0D) + 10.0D);

                for(var46 = 0; var46 < var10; ++var46) {
                    double var70 = (var6 + (double)var46) * var15 + this.c;
                    int var71 = (int)var70;
                    if (var70 < (double)var71) --var71;
                    int var50 = var71 & 255;
                    var70 -= (double)var71;
                    double var51 = var70 * var70 * var70 * (var70 * (var70 * 6.0D - 15.0D) + 10.0D);

                    for(int var53 = 0; var53 < var9; ++var53) {
                        double var54 = (var4 + (double)var53) * var13 + this.b;
                        int var56 = (int)var54;
                        if (var54 < (double)var56) --var56;
                        int var57 = var56 & 255;
                        var54 -= (double)var56;
                        double var58 = var54 * var54 * var54 * (var54 * (var54 * 6.0D - 15.0D) + 10.0D);
                        if (var53 == 0 || var57 != var20) {
                            var20 = var57;
                            int var60 = this.d[var31] + var57;
                            int var61 = this.d[var60] + var50;
                            int var62 = this.d[var60 + 1] + var50;
                            int var63 = this.d[var31 + 1] + var57;
                            var33 = this.d[var63] + var50;
                            int var64 = this.d[var63 + 1] + var50;
                            var42 = this.b(var28, this.a(this.d[var61], var25, var54, var70), this.a(this.d[var33], var25 - 1.0D, var54, var70));
                            var21 = this.b(var28, this.a(this.d[var62], var25, var54 - 1.0D, var70), this.a(this.d[var64], var25 - 1.0D, var54 - 1.0D, var70));
                            var69 = this.b(var28, this.a(this.d[var61 + 1], var25, var54, var70 - 1.0D), this.a(this.d[var33 + 1], var25 - 1.0D, var54, var70 - 1.0D));
                            var23 = this.b(var28, this.a(this.d[var62 + 1], var25, var54 - 1.0D, var70 - 1.0D), this.a(this.d[var64 + 1], var25 - 1.0D, var54 - 1.0D, var70 - 1.0D));
                        }
                        double var72 = this.b(var58, var42, var21);
                        double var73 = this.b(var58, var69, var23);
                        double var74 = this.b(var51, var72, var73);
                        var32 = var19++;
                        var1[var32] += var74 * var66;
                    }
                }
            }
        }
    }

    // Cached-axis exact 3D path. Preserves the legacy Y-cell corner cache.
    public void addNoiseCachedAxes(double[] out, double fromX, double fromY, double fromZ,
                                   int xLen, int yLen, int zLen,
                                   double scaleX, double scaleY, double scaleZ,
                                   double amplitude, boolean firstOctave, AxisCache cache) {
        if (yLen == 1) {
            a(out, fromX, fromY, fromZ, xLen, yLen, zLen, scaleX, scaleY, scaleZ, amplitude);
            return;
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.yIndex[y] = floor & 255;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int outIndex = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int previousYi = -1;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                for (int y = 0; y < yLen; y++) {
                    int yi = cache.yIndex[y];
                    double yf = cache.yFrac[y];
                    double yfm1 = cache.yFracMinus1[y];
                    double yfd = cache.yFade[y];

                    if (y == 0 || yi != previousYi) {
                        previousYi = yi;
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                    }

                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    if (firstOctave) {
                        out[outIndex] = 0.0D + value;
                    } else {
                        out[outIndex] += value;
                    }
                    outIndex++;
                }
            }
        }
    }


    /**
     * Exact cached-axis 3D path for regularly strided sample coordinates.
     * A step of 1.0 on every axis is equivalent to addNoiseCachedAxes(...).
     */
    public void addNoiseCachedAxesStrided(double[] out, double fromX, double fromY, double fromZ,
                                          int xLen, int yLen, int zLen,
                                          double stepX, double stepY, double stepZ,
                                          double scaleX, double scaleY, double scaleZ,
                                          double amplitude, boolean firstOctave, AxisCache cache) {
        if (yLen == 1) {
            addNoise2DCachedAxesStrided(
                    out, fromX, fromZ, xLen, zLen, stepX, stepZ,
                    scaleX, scaleZ, amplitude, firstOctave, cache
            );
            return;
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x * stepX) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z * stepZ) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y * stepY) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.yIndex[y] = floor & 255;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int outIndex = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int previousYi = -1;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                for (int y = 0; y < yLen; y++) {
                    int yi = cache.yIndex[y];
                    double yf = cache.yFrac[y];
                    double yfm1 = cache.yFracMinus1[y];
                    double yfd = cache.yFade[y];

                    if (y == 0 || yi != previousYi) {
                        previousYi = yi;
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                    }

                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    if (firstOctave) out[outIndex] = 0.0D + value;
                    else out[outIndex] += value;
                    outIndex++;
                }
            }
        }
    }


    /**
     * Exact masked version of addNoiseCachedAxes(). Only points whose mask contains
     * requiredBit are evaluated and accumulated. Needed points are bit-identical to
     * the full path. When a Y cell is entered through a skipped point, the cached
     * corner gradients are still evaluated from that cell's first Y sample, matching
     * the legacy Beta Y-cell cache exactly.
     */
    public void addNoiseCachedAxesMasked(double[] out, double fromX, double fromY, double fromZ,
                                         int xLen, int yLen, int zLen,
                                         double scaleX, double scaleY, double scaleZ,
                                         double amplitude, boolean firstOctave, AxisCache cache,
                                         byte[] pointMask, int requiredBit) {
        if (yLen == 1) {
            addNoiseCachedAxes(out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    scaleX, scaleY, scaleZ, amplitude, firstOctave, cache);
            return;
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.yIndex[y] = floor & 255;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int outIndex = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int previousYi = -1;
                int cellStartY = 0;
                boolean cornersValid = false;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                for (int y = 0; y < yLen; y++) {
                    int yi = cache.yIndex[y];
                    if (y == 0 || yi != previousYi) {
                        previousYi = yi;
                        cellStartY = y;
                        cornersValid = false;
                    }

                    int index = outIndex++;
                    if ((pointMask[index] & requiredBit) == 0) {
                        continue;
                    }

                    if (!cornersValid) {
                        double yf = cache.yFrac[cellStartY];
                        double yfm1 = cache.yFracMinus1[cellStartY];
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                        cornersValid = true;
                    }

                    double yfd = cache.yFade[y];
                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    if (firstOctave) out[index] = 0.0D + value;
                    else out[index] += value;
                }
            }
        }
    }

    /** Exact masked version of addNoiseCachedAxesStrided(). */
    public void addNoiseCachedAxesStridedMasked(double[] out, double fromX, double fromY, double fromZ,
                                                int xLen, int yLen, int zLen,
                                                double stepX, double stepY, double stepZ,
                                                double scaleX, double scaleY, double scaleZ,
                                                double amplitude, boolean firstOctave, AxisCache cache,
                                                byte[] pointMask, int requiredBit) {
        if (yLen == 1) {
            addNoise2DCachedAxesStrided(out, fromX, fromZ, xLen, zLen, stepX, stepZ,
                    scaleX, scaleZ, amplitude, firstOctave, cache);
            return;
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x * stepX) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z * stepZ) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y * stepY) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.yIndex[y] = floor & 255;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int outIndex = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int previousYi = -1;
                int cellStartY = 0;
                boolean cornersValid = false;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                for (int y = 0; y < yLen; y++) {
                    int yi = cache.yIndex[y];
                    if (y == 0 || yi != previousYi) {
                        previousYi = yi;
                        cellStartY = y;
                        cornersValid = false;
                    }

                    int index = outIndex++;
                    if ((pointMask[index] & requiredBit) == 0) {
                        continue;
                    }

                    if (!cornersValid) {
                        double yf = cache.yFrac[cellStartY];
                        double yfm1 = cache.yFracMinus1[cellStartY];
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                        cornersValid = true;
                    }

                    double yfd = cache.yFade[y];
                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    if (firstOctave) out[index] = 0.0D + value;
                    else out[index] += value;
                }
            }
        }
    }


    /**
     * Patch 12 exact active-Y version of addNoiseCachedAxesMasked().
     * Each X/Z column carries a bit mask of the Y samples that can be consumed by
     * the final terrain blend, so skipped points are never walked in the octave hot loop.
     */
    public void addNoiseCachedAxesActiveY(double[] out, double fromX, double fromY, double fromZ,
                                          int xLen, int yLen, int zLen,
                                          double scaleX, double scaleY, double scaleZ,
                                          double amplitude, boolean firstOctave, AxisCache cache,
                                          int[] activeYByColumn) {
        if (yLen == 1) {
            addNoiseCachedAxes(out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    scaleX, scaleY, scaleZ, amplitude, firstOctave, cache);
            return;
        }
        if (yLen > 32) throw new IllegalArgumentException("active-Y mask supports at most 32 Y samples");
        if (activeYByColumn == null || activeYByColumn.length < xLen * zLen) {
            throw new IllegalArgumentException("activeYByColumn too small");
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        int previousYi = Integer.MIN_VALUE;
        int cellStart = 0;
        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int yi = floor & 255;
            if (y == 0 || yi != previousYi) {
                cellStart = y;
                previousYi = yi;
            }
            cache.yIndex[y] = yi;
            cache.yCellStart[y] = cellStart;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int columnIndex = 0;
        int outBase = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int active = activeYByColumn[columnIndex++];
                if (active == 0) {
                    outBase += yLen;
                    continue;
                }

                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int activeCellStart = -1;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                while (active != 0) {
                    int y = Integer.numberOfTrailingZeros(active);
                    active &= active - 1;
                    int yi = cache.yIndex[y];
                    int firstY = cache.yCellStart[y];

                    if (firstY != activeCellStart) {
                        activeCellStart = firstY;
                        double yf = cache.yFrac[firstY];
                        double yfm1 = cache.yFracMinus1[firstY];
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                    }

                    double yfd = cache.yFade[y];
                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    int index = outBase + y;
                    if (firstOctave) out[index] = 0.0D + value;
                    else out[index] += value;
                }
                outBase += yLen;
            }
        }
    }

    /** Exact strided active-Y equivalent used by the Stage0 sparse batch. */
    public void addNoiseCachedAxesStridedActiveY(double[] out, double fromX, double fromY, double fromZ,
                                                 int xLen, int yLen, int zLen,
                                                 double stepX, double stepY, double stepZ,
                                                 double scaleX, double scaleY, double scaleZ,
                                                 double amplitude, boolean firstOctave, AxisCache cache,
                                                 int[] activeYByColumn) {
        if (yLen == 1) {
            addNoise2DCachedAxesStrided(out, fromX, fromZ, xLen, zLen, stepX, stepZ,
                    scaleX, scaleZ, amplitude, firstOctave, cache);
            return;
        }
        if (yLen > 32) throw new IllegalArgumentException("active-Y mask supports at most 32 Y samples");
        if (activeYByColumn == null || activeYByColumn.length < xLen * zLen) {
            throw new IllegalArgumentException("activeYByColumn too small");
        }

        cache.ensure(xLen, yLen, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x * stepX) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z * stepZ) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        int previousYi = Integer.MIN_VALUE;
        int cellStart = 0;
        for (int y = 0; y < yLen; y++) {
            double v = (fromY + (double) y * stepY) * scaleY + this.b;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int yi = floor & 255;
            if (y == 0 || yi != previousYi) {
                cellStart = y;
                previousYi = yi;
            }
            cache.yIndex[y] = yi;
            cache.yCellStart[y] = cellStart;
            v -= (double) floor;
            cache.yFrac[y] = v;
            cache.yFracMinus1[y] = v - 1.0D;
            cache.yFade[y] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int columnIndex = 0;
        int outBase = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int active = activeYByColumn[columnIndex++];
                if (active == 0) {
                    outBase += yLen;
                    continue;
                }

                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];
                int activeCellStart = -1;
                double q00 = 0.0D, q01 = 0.0D, q10 = 0.0D, q11 = 0.0D;

                while (active != 0) {
                    int y = Integer.numberOfTrailingZeros(active);
                    active &= active - 1;
                    int yi = cache.yIndex[y];
                    int firstY = cache.yCellStart[y];

                    if (firstY != activeCellStart) {
                        activeCellStart = firstY;
                        double yf = cache.yFrac[firstY];
                        double yfm1 = cache.yFracMinus1[firstY];
                        int p0 = xp0 + yi;
                        int p00 = this.d[p0] + zi;
                        int p01 = this.d[p0 + 1] + zi;
                        int p1 = xp1 + yi;
                        int p10 = this.d[p1] + zi;
                        int p11 = this.d[p1 + 1] + zi;

                        q00 = this.b(xfd,
                                this.a(this.d[p00], xf, yf, zf),
                                this.a(this.d[p10], xfm1, yf, zf));
                        q01 = this.b(xfd,
                                this.a(this.d[p01], xf, yfm1, zf),
                                this.a(this.d[p11], xfm1, yfm1, zf));
                        q10 = this.b(xfd,
                                this.a(this.d[p00 + 1], xf, yf, zfm1),
                                this.a(this.d[p10 + 1], xfm1, yf, zfm1));
                        q11 = this.b(xfd,
                                this.a(this.d[p01 + 1], xf, yfm1, zfm1),
                                this.a(this.d[p11 + 1], xfm1, yfm1, zfm1));
                    }

                    double yfd = cache.yFade[y];
                    double r0 = this.b(yfd, q00, q01);
                    double r1 = this.b(yfd, q10, q11);
                    double value = this.b(zfd, r0, r1) * weight;
                    int index = outBase + y;
                    if (firstOctave) out[index] = 0.0D + value;
                    else out[index] += value;
                }
                outBase += yLen;
            }
        }
    }

    /**
     * Exact strided equivalent of the legacy yLen==1 Perlin bulk path.
     * This keeps the old mixed 2D/3D gradient calls byte-for-byte in the same order.
     */
    public void addNoise2DCachedAxesStrided(double[] out, double fromX, double fromZ,
                                            int xLen, int zLen,
                                            double stepX, double stepZ,
                                            double scaleX, double scaleZ,
                                            double amplitude, boolean firstOctave, AxisCache cache) {
        cache.ensure(xLen, 1, zLen);

        for (int x = 0; x < xLen; x++) {
            double v = (fromX + (double) x * stepX) * scaleX + this.a;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            int xi = floor & 255;
            v -= (double) floor;
            cache.xPerm0[x] = this.d[xi];
            cache.xPerm1[x] = this.d[xi + 1];
            cache.xFrac[x] = v;
            cache.xFracMinus1[x] = v - 1.0D;
            cache.xFade[x] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        for (int z = 0; z < zLen; z++) {
            double v = (fromZ + (double) z * stepZ) * scaleZ + this.c;
            int floor = (int) v;
            if (v < (double) floor) --floor;
            cache.zIndex[z] = floor & 255;
            v -= (double) floor;
            cache.zFrac[z] = v;
            cache.zFracMinus1[z] = v - 1.0D;
            cache.zFade[z] = v * v * v * (v * (v * 6.0D - 15.0D) + 10.0D);
        }

        double weight = 1.0D / amplitude;
        int outIndex = 0;
        for (int x = 0; x < xLen; x++) {
            int xp0 = cache.xPerm0[x];
            int xp1 = cache.xPerm1[x];
            double xf = cache.xFrac[x];
            double xfm1 = cache.xFracMinus1[x];
            double xfd = cache.xFade[x];

            for (int z = 0; z < zLen; z++) {
                int zi = cache.zIndex[z];
                double zf = cache.zFrac[z];
                double zfm1 = cache.zFracMinus1[z];
                double zfd = cache.zFade[z];

                int p00 = this.d[xp0] + zi;
                int p10 = this.d[xp1] + zi;

                double q0 = this.b(xfd,
                        this.a(this.d[p00], xf, zf),
                        this.a(this.d[p10], xfm1, 0.0D, zf));
                double q1 = this.b(xfd,
                        this.a(this.d[p00 + 1], xf, 0.0D, zfm1),
                        this.a(this.d[p10 + 1], xfm1, 0.0D, zfm1));
                double value = this.b(zfd, q0, q1) * weight;

                if (firstOctave) out[outIndex] = 0.0D + value;
                else out[outIndex] += value;
                outIndex++;
            }
        }
    }

}
