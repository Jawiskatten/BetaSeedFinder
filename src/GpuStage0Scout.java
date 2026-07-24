import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Persistent Java <-> native GPU bridge for exact P20 + Stage-1 + Stage0/0.5 + P19 + coarse filtering. */
final class GpuStage0Scout implements AutoCloseable {
    /** P38 production protocol. Lean responses now include exact P19 score telemetry. */
    private static final byte[] MAGIC = "ST0R3816".getBytes(StandardCharsets.US_ASCII);
    static final int CENTER_COUNT = 8;
    private static final int[] CENTER_CHUNK_X = {0, 15, 0, 15, -15, -15, 0, 15};
    private static final int[] CENTER_CHUNK_Z = {0, 0, 15, 15, 0, 15, -15, -15};
    private static final int RESEARCH_RESPONSE_BYTES_PER_SEED = 34;
    private static final int LEAN_RESPONSE_BYTES_PER_SEED = 16;

    static final class BatchResult {
        final byte[] p20Counts;
        final byte[] fullUpperCounts;
        final byte[] highReentryCounts;
        final byte[] p19Pass;
        final byte[] megaTopologyRejected;
        final int[] coarseScores;

        // Exact native Stage0.75 telemetry. Values are zero/NaN for seeds that do
        // not reach the P19 gate. Mega mode also uses the dedicated topology flag
        // above as a live pre-coarse decision; the remaining fields feed research.
        final byte[] p19ExtremeBypass;
        final double[] p19Scores;
        final int[] p19FullY88;
        final int[] p19FullY96;
        final int[] p19FullY104;
        final int[] p19FullY112;
        final int[] p19Y88LargestCluster;
        final int[] p19Y88Width;
        final int[] p19Y88Depth;
        final byte[] p19Y88TouchesBorder;
        final int[] p19Y96LargestCluster;
        final int[] p19Y96Width;
        final int[] p19Y96Depth;
        final byte[] p19Y96TouchesBorder;

        BatchResult(
                byte[] p20Counts,
                byte[] fullUpperCounts,
                byte[] highReentryCounts,
                byte[] p19Pass,
                byte[] megaTopologyRejected,
                int[] coarseScores,
                byte[] p19ExtremeBypass,
                double[] p19Scores,
                int[] p19FullY88,
                int[] p19FullY96,
                int[] p19FullY104,
                int[] p19FullY112,
                int[] p19Y88LargestCluster,
                int[] p19Y88Width,
                int[] p19Y88Depth,
                byte[] p19Y88TouchesBorder,
                int[] p19Y96LargestCluster,
                int[] p19Y96Width,
                int[] p19Y96Depth,
                byte[] p19Y96TouchesBorder
        ) {
            this.p20Counts = p20Counts;
            this.fullUpperCounts = fullUpperCounts;
            this.highReentryCounts = highReentryCounts;
            this.p19Pass = p19Pass;
            this.megaTopologyRejected = megaTopologyRejected;
            this.coarseScores = coarseScores;
            this.p19ExtremeBypass = p19ExtremeBypass;
            this.p19Scores = p19Scores;
            this.p19FullY88 = p19FullY88;
            this.p19FullY96 = p19FullY96;
            this.p19FullY104 = p19FullY104;
            this.p19FullY112 = p19FullY112;
            this.p19Y88LargestCluster = p19Y88LargestCluster;
            this.p19Y88Width = p19Y88Width;
            this.p19Y88Depth = p19Y88Depth;
            this.p19Y88TouchesBorder = p19Y88TouchesBorder;
            this.p19Y96LargestCluster = p19Y96LargestCluster;
            this.p19Y96Width = p19Y96Width;
            this.p19Y96Depth = p19Y96Depth;
            this.p19Y96TouchesBorder = p19Y96TouchesBorder;
        }
    }

    private final Process process;
    private final BufferedInputStream input;
    private final BufferedOutputStream output;
    private final Thread stderrThread;
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private final int capacity;
    private final boolean researchTelemetry;
    private final int responseBytesPerSeed;
    private final byte[] seedPayload;
    private final byte[] packedResponse;
    private final byte[] reusableP20Counts;
    private final byte[] reusableFullUpperCounts;
    private final byte[] reusableHighReentryCounts;
    private final byte[] reusableP19Pass;
    private final byte[] reusableMegaTopologyRejected;
    private final int[] reusableCoarseScores;
    private final byte[] reusableP19ExtremeBypass;
    private final double[] reusableP19Scores;
    private final int[] reusableP19FullY88;
    private final int[] reusableP19FullY96;
    private final int[] reusableP19FullY104;
    private final int[] reusableP19FullY112;
    private final int[] reusableP19Y88LargestCluster;
    private final int[] reusableP19Y88Width;
    private final int[] reusableP19Y88Depth;
    private final byte[] reusableP19Y88TouchesBorder;
    private final int[] reusableP19Y96LargestCluster;
    private final int[] reusableP19Y96Width;
    private final int[] reusableP19Y96Depth;
    private final byte[] reusableP19Y96TouchesBorder;

    static boolean executableExists() {
        return GpuBackendLocator.executableExists();
    }

    static Path executablePath() {
        try {
            return GpuBackendLocator.resolve().path();
        } catch (IOException ignored) {
            return AppPaths.resolve("backend", "auto", "BetaSeedFinderWorker.exe");
        }
    }

    GpuStage0Scout(int capacity, boolean megaMode, boolean researchTelemetry, Consumer<String> log) throws IOException {
        this(capacity, megaMode ? "mega" : "general", researchTelemetry, log);
    }

    GpuStage0Scout(int capacity, String profile, boolean researchTelemetry, Consumer<String> log) throws IOException {
        if (capacity < 1) throw new IllegalArgumentException("GPU Stage0 batch size must be at least 1");
        if (!("general".equals(profile) || "mega".equals(profile)
                || "record60".equals(profile) || "record80".equals(profile))) {
            throw new IllegalArgumentException("Unknown GPU hunt profile: " + profile);
        }
        this.capacity = capacity;
        this.researchTelemetry = researchTelemetry;
        this.responseBytesPerSeed = researchTelemetry
                ? RESEARCH_RESPONSE_BYTES_PER_SEED
                : LEAN_RESPONSE_BYTES_PER_SEED;
        this.seedPayload = new byte[Math.multiplyExact(capacity, Long.BYTES)];
        int windowCapacity = Math.multiplyExact(capacity, CENTER_COUNT);
        this.packedResponse = new byte[Math.multiplyExact(windowCapacity, responseBytesPerSeed)];
        this.reusableP20Counts = new byte[windowCapacity];
        this.reusableFullUpperCounts = new byte[windowCapacity];
        this.reusableHighReentryCounts = new byte[windowCapacity];
        this.reusableP19Pass = new byte[windowCapacity];
        this.reusableMegaTopologyRejected = new byte[windowCapacity];
        this.reusableCoarseScores = new int[windowCapacity];
        this.reusableP19ExtremeBypass = new byte[windowCapacity];
        this.reusableP19Scores = new double[windowCapacity];
        this.reusableP19FullY88 = new int[windowCapacity];
        this.reusableP19FullY96 = new int[windowCapacity];
        this.reusableP19FullY104 = new int[windowCapacity];
        this.reusableP19FullY112 = new int[windowCapacity];
        this.reusableP19Y88LargestCluster = new int[windowCapacity];
        this.reusableP19Y88Width = new int[windowCapacity];
        this.reusableP19Y88Depth = new int[windowCapacity];
        this.reusableP19Y88TouchesBorder = new byte[windowCapacity];
        this.reusableP19Y96LargestCluster = new int[windowCapacity];
        this.reusableP19Y96Width = new int[windowCapacity];
        this.reusableP19Y96Depth = new int[windowCapacity];
        this.reusableP19Y96TouchesBorder = new byte[windowCapacity];

        GpuBackendLocator.ensureDefaultConfig();
        GpuBackendLocator.ResolvedBackend resolvedBackend = GpuBackendLocator.resolve();
        Path exe = resolvedBackend.path();
        if (!Files.isRegularFile(exe)) throw new IOException("GPU executable not found: " + exe);

        ProcessBuilder builder = new ProcessBuilder(
                exe.toString(),
                "stage0stream",
                Integer.toString(capacity),
                profile,
                researchTelemetry ? "research" : "lean",
                Integer.toString(CENTER_COUNT)
        );
        builder.directory(AppPaths.appRoot().toFile());
        builder.redirectErrorStream(false);

        this.process = builder.start();
        this.input = new BufferedInputStream(process.getInputStream(), 1 << 20);
        this.output = new BufferedOutputStream(process.getOutputStream(), 1 << 20);

        Consumer<String> safeLog = log == null ? ignored -> { } : log;
        safeLog.accept("GPU backend | using " + resolvedBackend.displayName() + " worker at " + exe);

        this.stderrThread = new Thread(
                () -> pumpStderr(process.getErrorStream(), safeLog),
                "gpu-stage0-stderr"
        );
        this.stderrThread.setDaemon(true);
        this.stderrThread.start();

        byte[] actualMagic = new byte[MAGIC.length];
        try {
            readFully(input, actualMagic, 0, actualMagic.length);
        } catch (IOException e) {
            destroyProcess();
            throw new IOException("GPU Stage0 stream failed during startup", e);
        }
        if (!Arrays.equals(MAGIC, actualMagic)) {
            destroyProcess();
            throw new IOException("GPU Stage0 protocol mismatch. Rebuild BetaSeedFinderWorker.exe.");
        }
    }

    int capacity() {
        return capacity;
    }

    int centerCount() {
        return CENTER_COUNT;
    }

    static int productionCenterChunkX(int centerIndex) {
        return CENTER_CHUNK_X[centerIndex];
    }

    static int productionCenterChunkZ(int centerIndex) {
        return CENTER_CHUNK_Z[centerIndex];
    }

    int centerChunkX(int centerIndex) {
        return productionCenterChunkX(centerIndex);
    }

    int centerChunkZ(int centerIndex) {
        return productionCenterChunkZ(centerIndex);
    }

    int resultIndex(int worldIndex, int centerIndex) {
        return worldIndex * CENTER_COUNT + centerIndex;
    }

    synchronized BatchResult filter(long[] seeds, int count) throws IOException {
        if (closed.get()) throw new IOException("GPU Stage0 scout is closed");
        if (count < 0 || count > capacity || count > seeds.length) {
            throw new IllegalArgumentException("Invalid GPU Stage0 batch count: " + count);
        }
        if (!process.isAlive()) throw new IOException("GPU Stage0 process exited with code " + safeExitCode());

        writeLe32(output, count);
        int payloadPos = 0;
        for (int i = 0; i < count; i++) {
            long value = seeds[i];
            seedPayload[payloadPos++] = (byte) value;
            seedPayload[payloadPos++] = (byte) (value >>> 8);
            seedPayload[payloadPos++] = (byte) (value >>> 16);
            seedPayload[payloadPos++] = (byte) (value >>> 24);
            seedPayload[payloadPos++] = (byte) (value >>> 32);
            seedPayload[payloadPos++] = (byte) (value >>> 40);
            seedPayload[payloadPos++] = (byte) (value >>> 48);
            seedPayload[payloadPos++] = (byte) (value >>> 56);
        }
        output.write(seedPayload, 0, payloadPos);
        output.flush();

        int responseCount = readLe32(input);
        if (responseCount != count) {
            throw new IOException("GPU Stage0 response size mismatch: expected " + count + ", got " + responseCount);
        }

        int windowCount = Math.multiplyExact(count, CENTER_COUNT);
        int responseBytes = Math.multiplyExact(windowCount, responseBytesPerSeed);
        readFully(input, packedResponse, 0, responseBytes);
        if (!researchTelemetry) clearResearchTelemetry(windowCount);
        for (int i = 0, p = 0; i < windowCount; i++) {
            reusableP20Counts[i] = packedResponse[p++];
            reusableFullUpperCounts[i] = packedResponse[p++];
            reusableHighReentryCounts[i] = packedResponse[p++];
            reusableP19Pass[i] = packedResponse[p++];
            int coarseLo = packedResponse[p++] & 0xFF;
            int coarseHi = packedResponse[p++] & 0xFF;
            reusableCoarseScores[i] = coarseLo | (coarseHi << 8);
            reusableP19ExtremeBypass[i] = packedResponse[p++];
            reusableMegaTopologyRejected[i] = packedResponse[p++];
            if (researchTelemetry) {
                long scoreBits = readLe64(packedResponse, p);
                p += 8;
                reusableP19Scores[i] = Double.longBitsToDouble(scoreBits);
                reusableP19FullY88[i] = readLe16(packedResponse, p); p += 2;
                reusableP19FullY96[i] = readLe16(packedResponse, p); p += 2;
                reusableP19FullY104[i] = readLe16(packedResponse, p); p += 2;
                reusableP19FullY112[i] = readLe16(packedResponse, p); p += 2;
                reusableP19Y88LargestCluster[i] = readLe16(packedResponse, p); p += 2;
                reusableP19Y88Width[i] = packedResponse[p++] & 0xFF;
                reusableP19Y88Depth[i] = packedResponse[p++] & 0xFF;
                reusableP19Y88TouchesBorder[i] = packedResponse[p++];
                reusableP19Y96LargestCluster[i] = readLe16(packedResponse, p); p += 2;
                reusableP19Y96Width[i] = packedResponse[p++] & 0xFF;
                reusableP19Y96Depth[i] = packedResponse[p++] & 0xFF;
                reusableP19Y96TouchesBorder[i] = packedResponse[p++];
            } else {
                long scoreBits = readLe64(packedResponse, p);
                p += 8;
                reusableP19Scores[i] = Double.longBitsToDouble(scoreBits);
            }
        }
        return new BatchResult(
                reusableP20Counts,
                reusableFullUpperCounts,
                reusableHighReentryCounts,
                reusableP19Pass,
                reusableMegaTopologyRejected,
                reusableCoarseScores,
                reusableP19ExtremeBypass,
                reusableP19Scores,
                reusableP19FullY88,
                reusableP19FullY96,
                reusableP19FullY104,
                reusableP19FullY112,
                reusableP19Y88LargestCluster,
                reusableP19Y88Width,
                reusableP19Y88Depth,
                reusableP19Y88TouchesBorder,
                reusableP19Y96LargestCluster,
                reusableP19Y96Width,
                reusableP19Y96Depth,
                reusableP19Y96TouchesBorder
        );
    }

    private void clearResearchTelemetry(int count) {
        Arrays.fill(reusableP19Scores, 0, count, Double.NaN);
        Arrays.fill(reusableP19FullY88, 0, count, 0);
        Arrays.fill(reusableP19FullY96, 0, count, 0);
        Arrays.fill(reusableP19FullY104, 0, count, 0);
        Arrays.fill(reusableP19FullY112, 0, count, 0);
        Arrays.fill(reusableP19Y88LargestCluster, 0, count, 0);
        Arrays.fill(reusableP19Y88Width, 0, count, 0);
        Arrays.fill(reusableP19Y88Depth, 0, count, 0);
        Arrays.fill(reusableP19Y88TouchesBorder, 0, count, (byte) 0);
        Arrays.fill(reusableP19Y96LargestCluster, 0, count, 0);
        Arrays.fill(reusableP19Y96Width, 0, count, 0);
        Arrays.fill(reusableP19Y96Depth, 0, count, 0);
        Arrays.fill(reusableP19Y96TouchesBorder, 0, count, (byte) 0);
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) return;
        try {
            if (process.isAlive()) {
                writeLe32(output, 0);
                output.flush();
            }
        } catch (IOException ignored) {
        }
        try { output.close(); } catch (IOException ignored) { }
        try { input.close(); } catch (IOException ignored) { }
        try {
            if (!process.waitFor(2, TimeUnit.SECONDS)) destroyProcess();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            destroyProcess();
        }
    }

    private void destroyProcess() {
        process.destroy();
        try {
            if (!process.waitFor(500, TimeUnit.MILLISECONDS)) process.destroyForcibly();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            process.destroyForcibly();
        }
    }

    private int safeExitCode() {
        try { return process.exitValue(); }
        catch (IllegalThreadStateException e) { return Integer.MIN_VALUE; }
    }

    private static void pumpStderr(InputStream errorStream, Consumer<String> log) {
        try (InputStream in = errorStream) {
            StringBuilder line = new StringBuilder();
            int value;
            while ((value = in.read()) >= 0) {
                if (value == '\n') emitLine(line, log);
                else if (value != '\r') line.append((char) value);
            }
            emitLine(line, log);
        } catch (IOException ignored) {
        }
    }

    private static void emitLine(StringBuilder line, Consumer<String> log) {
        if (line.length() == 0) return;
        log.accept("GPU Stage0 | " + line);
        line.setLength(0);
    }

    private static void writeLe32(OutputStream out, int value) throws IOException {
        out.write(value);
        out.write(value >>> 8);
        out.write(value >>> 16);
        out.write(value >>> 24);
    }

    private static int readLe32(InputStream in) throws IOException {
        int b0 = in.read();
        int b1 = in.read();
        int b2 = in.read();
        int b3 = in.read();
        if ((b0 | b1 | b2 | b3) < 0) throw new EOFException("GPU Stage0 process closed the stream");
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    private static int readLe16(byte[] data, int offset) {
        return (data[offset] & 0xFF) | ((data[offset + 1] & 0xFF) << 8);
    }

    private static int readLe32(byte[] data, int offset) {
        return (data[offset] & 0xFF)
                | ((data[offset + 1] & 0xFF) << 8)
                | ((data[offset + 2] & 0xFF) << 16)
                | ((data[offset + 3] & 0xFF) << 24);
    }

    private static long readLe64(byte[] data, int offset) {
        return (data[offset] & 0xFFL)
                | ((data[offset + 1] & 0xFFL) << 8)
                | ((data[offset + 2] & 0xFFL) << 16)
                | ((data[offset + 3] & 0xFFL) << 24)
                | ((data[offset + 4] & 0xFFL) << 32)
                | ((data[offset + 5] & 0xFFL) << 40)
                | ((data[offset + 6] & 0xFFL) << 48)
                | ((data[offset + 7] & 0xFFL) << 56);
    }

    private static void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        int total = 0;
        while (total < length) {
            int read = in.read(buffer, offset + total, length - total);
            if (read < 0) throw new EOFException("GPU Stage0 process closed the stream");
            total += read;
        }
    }
}
