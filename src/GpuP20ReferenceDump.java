import beta173.BetaTerrain173;

import java.io.BufferedOutputStream;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

/**
 * Writes exact Java P20 64-column scout outputs for differential testing against
 * the standalone HIP/C++ benchmark. This class does not change hunter behavior.
 */
public final class GpuP20ReferenceDump {
    private static final int[] AXIS_64 = {0, 2, 5, 7, 8, 10, 13, 15};
    private static final int Y_FROM = 11;
    private static final int Y_TO_EXCLUSIVE = 17;
    private static final int DENSITY_COUNT = 64 * (Y_TO_EXCLUSIVE - Y_FROM);
    private static final byte[] MAGIC = {'P', '2', '0', 'R', 'E', 'F', '0', '1'};

    private GpuP20ReferenceDump() {}

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && "bench".equalsIgnoreCase(args[0])) {
            runBenchmark(args);
            return;
        }

        Path output = Path.of(args.length > 0 ? args[0] : "gpu_p20_reference.bin");
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 1000;
        long sequenceSeed = args.length > 2 ? Long.parseLong(args[2]) : 123456789L;
        if (count < 1) throw new IllegalArgumentException("count must be >= 1");

        Field densityField = BetaTerrain173.class.getDeclaredField("stage0ScoutDensity");
        densityField.setAccessible(true);

        Files.createDirectories(output.toAbsolutePath().getParent());
        BetaTerrain173 terrain = new BetaTerrain173(deterministicSeedForAttempt(sequenceSeed, 0));
        ByteBuffer scratch = ByteBuffer.allocate(8 + 4 + DENSITY_COUNT * 8).order(ByteOrder.LITTLE_ENDIAN);

        long started = System.nanoTime();
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(output.toFile()), 1 << 20))) {
            out.write(MAGIC);
            writeLeInt(out, 1);
            writeLeInt(out, count);
            writeLeInt(out, DENSITY_COUNT);
            writeLeInt(out, AXIS_64.length);

            for (int attempt = 0; attempt < count; attempt++) {
                long seed = deterministicSeedForAttempt(sequenceSeed, attempt);
                terrain.reseed(seed);
                BetaTerrain173.ProgressiveStage0TierFeatures features =
                        terrain.prepareStage0UpperPositiveScoutTier64AroundZero(7, 4, 11);
                double[] density = (double[]) densityField.get(terrain);

                scratch.clear();
                scratch.putLong(seed);
                scratch.putInt(features.upperPositiveColumns);
                for (int gx : AXIS_64) {
                    for (int gz : AXIS_64) {
                        int base = (gx * 16 + gz) * 17;
                        for (int y = Y_FROM; y < Y_TO_EXCLUSIVE; y++) {
                            scratch.putLong(Double.doubleToRawLongBits(density[base + y]));
                        }
                    }
                }
                out.write(scratch.array(), 0, scratch.position());
            }
        }

        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        System.out.printf(Locale.ROOT,
                "Wrote %,d exact Java P20 records to %s in %.3fs (%.1f seeds/s)%n",
                count, output.toAbsolutePath(), seconds, count / seconds);
    }

    private static void runBenchmark(String[] args) {
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 100_000;
        long sequenceSeed = args.length > 2 ? Long.parseLong(args[2]) : 123456789L;
        if (count < 1) throw new IllegalArgumentException("count must be >= 1");

        long firstSeed = deterministicSeedForAttempt(sequenceSeed, 0);
        BetaTerrain173 terrain = new BetaTerrain173(firstSeed);
        long checksum = 0L;

        // Warm up the exact same method before timing HotSpot steady state.
        int warmup = Math.min(count, 10_000);
        for (int i = 0; i < warmup; i++) {
            long seed = deterministicSeedForAttempt(sequenceSeed, i);
            terrain.reseed(seed);
            checksum += terrain.prepareStage0UpperPositiveScoutTier64AroundZero(7, 4, 11).upperPositiveColumns;
        }

        long started = System.nanoTime();
        for (int i = 0; i < count; i++) {
            long seed = deterministicSeedForAttempt(sequenceSeed, i);
            terrain.reseed(seed);
            checksum += terrain.prepareStage0UpperPositiveScoutTier64AroundZero(7, 4, 11).upperPositiveColumns;
        }
        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        System.out.printf(Locale.ROOT,
                "Java exact P20 scout | %,d seeds | %.3fs | %.1f seeds/s | checksum=%d%n",
                count, seconds, count / seconds, checksum);
    }

    private static long deterministicSeedForAttempt(long sequenceSeed, long attempt) {
        long z = sequenceSeed + attempt * 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    private static void writeLeInt(DataOutputStream out, int value) throws Exception {
        out.writeByte(value & 0xFF);
        out.writeByte((value >>> 8) & 0xFF);
        out.writeByte((value >>> 16) & 0xFF);
        out.writeByte((value >>> 24) & 0xFF);
    }
}
