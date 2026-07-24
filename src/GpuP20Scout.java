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

/**
 * Persistent Java <-> HIP bridge for the exact P20 64-column scout.
 *
 * The native process stays alive for the whole search. Java sends batches of
 * Minecraft seeds and receives one unsigned byte per seed: the exact number of
 * P20-positive columns (0 means reject, >0 means continue through the trusted
 * Java hunter pipeline).
 */
final class GpuP20Scout implements AutoCloseable {
    private static final byte[] MAGIC = "P20STR01".getBytes(StandardCharsets.US_ASCII);

    private final Process process;
    private final BufferedInputStream input;
    private final BufferedOutputStream output;
    private final Thread stderrThread;
    private final AtomicBoolean closed = new AtomicBoolean(false);
    private final int capacity;

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

    GpuP20Scout(int capacity, Consumer<String> log) throws IOException {
        if (capacity < 1) {
            throw new IllegalArgumentException("GPU P20 batch size must be at least 1");
        }
        this.capacity = capacity;

        GpuBackendLocator.ensureDefaultConfig();
        GpuBackendLocator.ResolvedBackend resolvedBackend = GpuBackendLocator.resolve();
        Path exe = resolvedBackend.path();
        if (!Files.isRegularFile(exe)) {
            throw new IOException("GPU P20 executable not found: " + exe);
        }

        ProcessBuilder builder = new ProcessBuilder(
                exe.toString(),
                "stream",
                Integer.toString(capacity)
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
                "gpu-p20-stderr"
        );
        this.stderrThread.setDaemon(true);
        this.stderrThread.start();

        byte[] actualMagic = new byte[MAGIC.length];
        try {
            readFully(input, actualMagic, 0, actualMagic.length);
        } catch (IOException e) {
            destroyProcess();
            throw new IOException("GPU P20 stream failed during startup", e);
        }
        if (!Arrays.equals(MAGIC, actualMagic)) {
            destroyProcess();
            throw new IOException(
                    "GPU P20 stream protocol mismatch. Rebuild BetaSeedFinderWorker.exe with the integrated source."
            );
        }
    }

    int capacity() {
        return capacity;
    }

    /**
     * Filters one seed batch. The returned array has {@code count} entries.
     * Each unsigned byte is the native exact P20 positive-column count.
     */
    synchronized byte[] filter(long[] seeds, int count) throws IOException {
        if (closed.get()) {
            throw new IOException("GPU P20 scout is closed");
        }
        if (count < 0 || count > capacity || count > seeds.length) {
            throw new IllegalArgumentException("Invalid GPU P20 batch count: " + count);
        }
        if (!process.isAlive()) {
            throw new IOException("GPU P20 process exited with code " + safeExitCode());
        }

        writeLe32(output, count);
        for (int i = 0; i < count; i++) {
            writeLe64(output, seeds[i]);
        }
        output.flush();

        int responseCount = readLe32(input);
        if (responseCount != count) {
            throw new IOException(
                    "GPU P20 response size mismatch: expected " + count + ", got " + responseCount
            );
        }

        byte[] positiveCounts = new byte[count];
        readFully(input, positiveCounts, 0, count);
        return positiveCounts;
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }

        try {
            if (process.isAlive()) {
                writeLe32(output, 0);
                output.flush();
            }
        } catch (IOException ignored) {
        }

        try {
            output.close();
        } catch (IOException ignored) {
        }
        try {
            input.close();
        } catch (IOException ignored) {
        }

        try {
            if (!process.waitFor(2, TimeUnit.SECONDS)) {
                destroyProcess();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            destroyProcess();
        }
    }

    private void destroyProcess() {
        process.destroy();
        try {
            if (!process.waitFor(500, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            process.destroyForcibly();
        }
    }

    private int safeExitCode() {
        try {
            return process.exitValue();
        } catch (IllegalThreadStateException e) {
            return Integer.MIN_VALUE;
        }
    }

    private static void pumpStderr(InputStream errorStream, Consumer<String> log) {
        try (InputStream in = errorStream) {
            StringBuilder line = new StringBuilder();
            int value;
            while ((value = in.read()) >= 0) {
                if (value == '\n') {
                    emitLine(line, log);
                } else if (value != '\r') {
                    line.append((char) value);
                }
            }
            emitLine(line, log);
        } catch (IOException ignored) {
            // Process shutdown normally closes this stream.
        }
    }

    private static void emitLine(StringBuilder line, Consumer<String> log) {
        if (line.length() == 0) {
            return;
        }
        log.accept("GPU P20 | " + line);
        line.setLength(0);
    }

    private static void writeLe32(OutputStream out, int value) throws IOException {
        out.write(value);
        out.write(value >>> 8);
        out.write(value >>> 16);
        out.write(value >>> 24);
    }

    private static void writeLe64(OutputStream out, long value) throws IOException {
        out.write((int) value);
        out.write((int) (value >>> 8));
        out.write((int) (value >>> 16));
        out.write((int) (value >>> 24));
        out.write((int) (value >>> 32));
        out.write((int) (value >>> 40));
        out.write((int) (value >>> 48));
        out.write((int) (value >>> 56));
    }

    private static int readLe32(InputStream in) throws IOException {
        int b0 = in.read();
        int b1 = in.read();
        int b2 = in.read();
        int b3 = in.read();
        if ((b0 | b1 | b2 | b3) < 0) {
            throw new EOFException("GPU P20 process closed the stream");
        }
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    private static void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        int total = 0;
        while (total < length) {
            int read = in.read(buffer, offset + total, length - total);
            if (read < 0) {
                throw new EOFException("GPU P20 process closed the stream");
            }
            total += read;
        }
    }
}
