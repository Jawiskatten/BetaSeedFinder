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
 * Writes exact Java P20 -> Stage-1 upper scout -> Stage0/0.5 re-entry records
 * for differential testing against the HIP implementation.
 *
 * This class is validation/benchmark tooling only. It does not change hunter behavior.
 */
public final class GpuStage0ReferenceDump {
    private static final int COLUMNS = 16 * 16;
    private static final int Y_COUNT = 17;
    private static final int DENSITY_COUNT = COLUMNS * Y_COUNT;
    private static final int P20_MIN = 1;
    private static final int STAGE1_MIN = 5;
    private static final byte[] MAGIC = {'S', 'T', '0', 'R', 'E', 'F', '0', '1'};

    private GpuStage0ReferenceDump() {}

    public static void main(String[] args) throws Exception {
        if (args.length > 0 && "bench".equalsIgnoreCase(args[0])) {
            runBenchmark(args);
            return;
        }

        Path output = Path.of(args.length > 0 ? args[0] : "gpu_stage0_reference.bin");
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 1000;
        long sequenceSeed = args.length > 2 ? Long.parseLong(args[2]) : 123456789L;
        if (count < 1) throw new IllegalArgumentException("count must be >= 1");

        Field densityField = BetaTerrain173.class.getDeclaredField("stage0ScoutDensity");
        densityField.setAccessible(true);
        Field requestedField = BetaTerrain173.class.getDeclaredField("stage0RequestedActiveY");
        requestedField.setAccessible(true);

        Files.createDirectories(output.toAbsolutePath().getParent());
        BetaTerrain173 terrain = new BetaTerrain173(deterministicSeedForAttempt(sequenceSeed, 0));

        // seed + four counts + 256 lower masks + every 16x16x17 density bit pattern
        ByteBuffer record = ByteBuffer.allocate(8 + 16 + COLUMNS * 4 + DENSITY_COUNT * 8)
                .order(ByteOrder.LITTLE_ENDIAN);

        long started = System.nanoTime();
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(output.toFile()), 1 << 20))) {
            out.write(MAGIC);
            writeLeInt(out, 1);
            writeLeInt(out, count);
            writeLeInt(out, DENSITY_COUNT);
            writeLeInt(out, COLUMNS);

            for (int attempt = 0; attempt < count; attempt++) {
                long seed = deterministicSeedForAttempt(sequenceSeed, attempt);
                terrain.reseed(seed);

                BetaTerrain173.ProgressiveStage0TierFeatures p20 =
                        terrain.prepareStage0UpperPositiveScoutTier64AroundZero(7, 4, 11);

                // Always complete the full upper slice so raw upper density can be
                // checked even for records production P20 would reject.
                int fullUpper = terrain.completeStage0UpperPositiveScoutAfterProgressiveTiers(7, 4, 11);
                int highReentry = 0;
                int candidateColumns = 0;
                if (p20.upperPositiveColumns >= P20_MIN && fullUpper >= STAGE1_MIN) {
                    terrain.completeHighSparseReentryAfterUpperScoutCandidateColumns(7, 4, 9, 2, 11, 5);
                    highReentry = terrain.countPreparedHighSparseReentryColumns(11);
                    candidateColumns = terrain.getLastStage0LowerCandidateColumns();
                }

                double[] density = (double[]) densityField.get(terrain);
                int[] requested = (int[]) requestedField.get(terrain);

                record.clear();
                record.putLong(seed);
                record.putInt(p20.upperPositiveColumns);
                record.putInt(fullUpper);
                record.putInt(highReentry);
                record.putInt(candidateColumns);
                for (int column = 0; column < COLUMNS; column++) {
                    int lowerMask = candidateColumns > 0 ? requested[column] : 0;
                    record.putInt(lowerMask);
                }
                for (int i = 0; i < DENSITY_COUNT; i++) {
                    record.putLong(Double.doubleToRawLongBits(density[i]));
                }
                out.write(record.array(), 0, record.position());
            }
        }

        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        System.out.printf(Locale.ROOT,
                "Wrote %,d exact Java Stage0 records to %s in %.3fs (%.1f seeds/s)%n",
                count, output.toAbsolutePath(), seconds, count / seconds);
    }

    private static void runBenchmark(String[] args) {
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 100_000;
        long sequenceSeed = args.length > 2 ? Long.parseLong(args[2]) : 123456789L;
        if (count < 1) throw new IllegalArgumentException("count must be >= 1");

        BetaTerrain173 terrain = new BetaTerrain173(deterministicSeedForAttempt(sequenceSeed, 0));
        int warmup = Math.min(count, 5000);
        for (int i = 0; i < warmup; i++) {
            runExactChain(terrain, deterministicSeedForAttempt(sequenceSeed, i));
        }

        long checksum = 0L;
        long started = System.nanoTime();
        for (int i = 0; i < count; i++) {
            checksum += runExactChain(terrain, deterministicSeedForAttempt(sequenceSeed, i));
        }
        double seconds = (System.nanoTime() - started) / 1_000_000_000.0;
        System.out.printf(Locale.ROOT,
                "Java exact P20+Stage1+Stage0 chain | %,d seeds | %.3fs | %.1f seeds/s | checksum=%d%n",
                count, seconds, count / seconds, checksum);
    }

    private static long runExactChain(BetaTerrain173 terrain, long seed) {
        terrain.reseed(seed);
        int p20 = terrain.prepareStage0UpperPositiveScoutTier64AroundZero(7, 4, 11).upperPositiveColumns;
        if (p20 == 0) return p20;

        int fullUpper = terrain.completeStage0UpperPositiveScoutAfterProgressiveTiers(7, 4, 11);
        if (fullUpper < 5) return p20 + 1000L * fullUpper;

        terrain.completeHighSparseReentryAfterUpperScoutCandidateColumns(7, 4, 9, 2, 11, 5);
        int high = terrain.countPreparedHighSparseReentryColumns(11);
        return p20 + 1000L * fullUpper + 1_000_000L * high;
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
