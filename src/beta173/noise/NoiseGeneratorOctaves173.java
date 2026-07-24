package beta173.noise;

import java.util.Random;

public class NoiseGeneratorOctaves173 extends NoiseGenerator173 {

    private final NoiseGeneratorPerlin173[] noiseGenerators;
    private final int totalNoiseGenerators;
    private final NoiseGeneratorPerlin173.AxisCache axisCache = new NoiseGeneratorPerlin173.AxisCache();

    public NoiseGeneratorOctaves173(Random var1, int var2) {
        this.totalNoiseGenerators = var2;
        this.noiseGenerators = new NoiseGeneratorPerlin173[var2];

        for (int var3 = 0; var3 < var2; ++var3) {
            this.noiseGenerators[var3] = new NoiseGeneratorPerlin173(var1);
        }
    }

    /** Rebuilds every octave in place, preserving constructor order and Random consumption. */
    public void reseed(Random random) {
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].reseed(random);
        }
    }

    /** Consumes the exact Random sequence for N Perlin constructors without allocating them. */
    public static void consumeConstructorRandom(Random random, int octaves) {
        for (int octave = 0; octave < octaves; octave++) {
            NoiseGeneratorPerlin173.consumeConstructorRandom(random);
        }
    }

    public double generateNoiseForCoordinate(double var1, double var3) {
        double var5 = 0.0D;
        double var7 = 1.0D;

        for (int var9 = 0; var9 < this.totalNoiseGenerators; ++var9) {
            var5 += this.noiseGenerators[var9].a(var1 * var7, var3 * var7) / var7;
            var7 /= 2.0D;
        }

        return var5;
    }

    public double[] generateNoise(double[] var1, double var2, double var4, double var6,
                                  int var8, int var9, int var10,
                                  double var11, double var13, double var15) {
        int requiredLength = var8 * var9 * var10;

        // Important fix:
        // Old Beta code only checked for null. In this standalone searcher we reuse
        // the same noise buffers for different-sized calls, so a previous tiny call
        // can leave an array that is too small for the next larger call.
        if (var1 == null || var1.length < requiredLength) {
            var1 = new double[requiredLength];
        } else {
            for (int var17 = 0; var17 < var1.length; ++var17) {
                var1[var17] = 0.0D;
            }
        }

        double var20 = 1.0D;

        for (int var19 = 0; var19 < this.totalNoiseGenerators; ++var19) {
            this.noiseGenerators[var19].a(
                    var1,
                    var2,
                    var4,
                    var6,
                    var8,
                    var9,
                    var10,
                    var11 * var20,
                    var13 * var20,
                    var15 * var20,
                    var20
            );
            var20 /= 2.0D;
        }

        return var1;
    }

    public double[] generateNoise(double[] var1, int var2, int var3,
                                  int var4, int var5,
                                  double var6, double var8, double var10) {
        return this.generateNoise(var1, (double) var2, 10.0D, (double) var3,
                var4, 1, var5, var6, 1.0D, var8);
    }
    public double[] generateNoiseCachedAxes(double[] out, double fromX, double fromY, double fromZ,
                                             int xLen, int yLen, int zLen,
                                             double scaleX, double scaleY, double scaleZ) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) {
            out = new double[requiredLength];
        }

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxes(
                    out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache
            );
            amplitude /= 2.0D;
        }
        return out;
    }


    public double[] generateNoiseCachedAxesStrided(double[] out,
                                                    double fromX, double fromY, double fromZ,
                                                    int xLen, int yLen, int zLen,
                                                    double stepX, double stepY, double stepZ,
                                                    double scaleX, double scaleY, double scaleZ) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) {
            out = new double[requiredLength];
        }

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxesStrided(
                    out, fromX, fromY, fromZ,
                    xLen, yLen, zLen,
                    stepX, stepY, stepZ,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache
            );
            amplitude /= 2.0D;
        }
        return out;
    }

    public double[] generateNoise2DStridedCachedAxes(double[] out,
                                                      double fromX, double fromZ,
                                                      int xLen, int zLen,
                                                      double stepX, double stepZ,
                                                      double scaleX, double scaleZ) {
        int requiredLength = xLen * zLen;
        if (out == null || out.length < requiredLength) {
            out = new double[requiredLength];
        }

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoise2DCachedAxesStrided(
                    out, fromX, fromZ, xLen, zLen, stepX, stepZ,
                    scaleX * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache
            );
            amplitude /= 2.0D;
        }
        return out;
    }


    public double[] generateNoiseCachedAxesMasked(double[] out,
                                                   double fromX, double fromY, double fromZ,
                                                   int xLen, int yLen, int zLen,
                                                   double scaleX, double scaleY, double scaleZ,
                                                   byte[] pointMask, int requiredBit) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) out = new double[requiredLength];
        if (pointMask == null || pointMask.length < requiredLength) {
            throw new IllegalArgumentException("pointMask too small");
        }

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxesMasked(
                    out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache, pointMask, requiredBit
            );
            amplitude /= 2.0D;
        }
        return out;
    }

    public double[] generateNoiseCachedAxesStridedMasked(double[] out,
                                                          double fromX, double fromY, double fromZ,
                                                          int xLen, int yLen, int zLen,
                                                          double stepX, double stepY, double stepZ,
                                                          double scaleX, double scaleY, double scaleZ,
                                                          byte[] pointMask, int requiredBit) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) out = new double[requiredLength];
        if (pointMask == null || pointMask.length < requiredLength) {
            throw new IllegalArgumentException("pointMask too small");
        }

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxesStridedMasked(
                    out, fromX, fromY, fromZ,
                    xLen, yLen, zLen,
                    stepX, stepY, stepZ,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache, pointMask, requiredBit
            );
            amplitude /= 2.0D;
        }
        return out;
    }


    public double[] generateNoiseCachedAxesActiveY(double[] out,
                                                    double fromX, double fromY, double fromZ,
                                                    int xLen, int yLen, int zLen,
                                                    double scaleX, double scaleY, double scaleZ,
                                                    int[] activeYByColumn) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) out = new double[requiredLength];

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxesActiveY(
                    out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache, activeYByColumn
            );
            amplitude /= 2.0D;
        }
        return out;
    }

    public double[] generateNoiseCachedAxesStridedActiveY(double[] out,
                                                           double fromX, double fromY, double fromZ,
                                                           int xLen, int yLen, int zLen,
                                                           double stepX, double stepY, double stepZ,
                                                           double scaleX, double scaleY, double scaleZ,
                                                           int[] activeYByColumn) {
        int requiredLength = xLen * yLen * zLen;
        if (out == null || out.length < requiredLength) out = new double[requiredLength];

        double amplitude = 1.0D;
        for (int octave = 0; octave < this.totalNoiseGenerators; octave++) {
            this.noiseGenerators[octave].addNoiseCachedAxesStridedActiveY(
                    out, fromX, fromY, fromZ, xLen, yLen, zLen,
                    stepX, stepY, stepZ,
                    scaleX * amplitude, scaleY * amplitude, scaleZ * amplitude,
                    amplitude, octave == 0, this.axisCache, activeYByColumn
            );
            amplitude /= 2.0D;
        }
        return out;
    }

}
