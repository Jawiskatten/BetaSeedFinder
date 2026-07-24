import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.Locale;
import java.util.concurrent.TimeUnit;

final class GpuBackendLocator {
    private static final Path CONFIG_PATH = Path.of("config", "gpu_backend.properties");
    private static final Path AMD_EXE = Path.of("backend", "amd", "gpu_p20_benchmark.exe");
    private static final Path NVIDIA_EXE = Path.of("backend", "nvidia", "gpu_p20_benchmark.exe");
    private static final Path LEGACY_EXE = Path.of("gpu_p20_benchmark", "build", "gpu_p20_benchmark.exe");

    private GpuBackendLocator() {
    }

    static ResolvedBackend resolve() throws IOException {
        GpuBackendKind requested = requestedBackend();
        List<Path> tried = new ArrayList<>();

        if (requested == GpuBackendKind.AMD) {
            ResolvedBackend amd = tryExisting(GpuBackendKind.AMD, AMD_EXE, tried);
            if (amd != null) return amd;

            // Existing BetaSeedFinder installs keep the validated AMD worker in
            // gpu_p20_benchmark\build. Treat that as the AMD fallback so the
            // forced AMD launcher remains compatible after installing P57/P58.
            ResolvedBackend legacyAmd = tryExisting(GpuBackendKind.AMD, LEGACY_EXE, tried);
            if (legacyAmd != null) return legacyAmd;

            throw missingRequestedBackend(GpuBackendKind.AMD, tried);
        }
        if (requested == GpuBackendKind.NVIDIA) {
            return resolveSingle(GpuBackendKind.NVIDIA, NVIDIA_EXE, tried);
        }
        if (requested == GpuBackendKind.LEGACY) {
            return resolveSingle(GpuBackendKind.LEGACY, LEGACY_EXE, tried);
        }

        GpuBackendKind detected = detectInstalledVendor();
        ResolvedBackend auto;
        if (detected == GpuBackendKind.NVIDIA) {
            auto = tryExisting(GpuBackendKind.NVIDIA, NVIDIA_EXE, tried);
            if (auto != null) return auto;
            auto = tryExisting(GpuBackendKind.AMD, AMD_EXE, tried);
            if (auto != null) return auto;
        } else {
            auto = tryExisting(GpuBackendKind.AMD, AMD_EXE, tried);
            if (auto != null) return auto;
            auto = tryExisting(GpuBackendKind.NVIDIA, NVIDIA_EXE, tried);
            if (auto != null) return auto;
        }
        auto = tryExisting(GpuBackendKind.LEGACY, LEGACY_EXE, tried);
        if (auto != null) return auto;

        StringBuilder message = new StringBuilder();
        message.append("No supported GPU worker executable was found.\n");
        message.append("Checked:\n");
        for (Path path : tried) {
            message.append("  - ").append(path.toAbsolutePath().normalize()).append('\n');
        }
        message.append("\nChoose a backend by either:\n");
        message.append("  • placing an AMD worker at backend\\amd\\gpu_p20_benchmark.exe\n");
        message.append("  • placing an NVIDIA worker at backend\\nvidia\\gpu_p20_benchmark.exe\n");
        message.append("  • or keeping the legacy worker at gpu_p20_benchmark\\build\\gpu_p20_benchmark.exe\n");
        throw new IOException(message.toString().trim());
    }


    static GpuBackendKind detectInstalledVendor() {
        String forcedName = System.getenv("BSF_GPU_NAME");
        if (forcedName != null && !forcedName.isBlank()) {
            return classifyAdapterName(forcedName);
        }

        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        if (!os.contains("win")) return GpuBackendKind.NONE;

        Process process = null;
        try {
            process = new ProcessBuilder(
                    "powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                    "(Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name) -join [Environment]::NewLine"
            ).redirectErrorStream(true).start();
            if (!process.waitFor(6, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                return GpuBackendKind.NONE;
            }
            String names = new String(process.getInputStream().readAllBytes());
            return classifyAdapterName(names);
        } catch (Exception ignored) {
            return GpuBackendKind.NONE;
        } finally {
            if (process != null && process.isAlive()) process.destroyForcibly();
        }
    }

    private static GpuBackendKind classifyAdapterName(String names) {
        String lower = names == null ? "" : names.toLowerCase(Locale.ROOT);
        if (lower.contains("nvidia") || lower.contains("geforce") || lower.contains("quadro")) {
            return GpuBackendKind.NVIDIA;
        }
        if (lower.contains("amd") || lower.contains("radeon")) {
            return GpuBackendKind.AMD;
        }
        return GpuBackendKind.NONE;
    }

    static boolean executableExists() {
        try {
            return Files.isRegularFile(resolve().path());
        } catch (IOException ignored) {
            return false;
        }
    }

    static GpuBackendKind requestedBackend() {
        String env = System.getenv("BSF_BACKEND");
        if (env != null && !env.isBlank()) {
            return GpuBackendKind.parse(env);
        }

        Properties properties = new Properties();
        if (Files.isRegularFile(CONFIG_PATH)) {
            try (InputStream in = Files.newInputStream(CONFIG_PATH)) {
                properties.load(in);
            } catch (IOException ignored) {
            }
        }
        return GpuBackendKind.parse(properties.getProperty("backend", "auto"));
    }

    static void ensureDefaultConfig() {
        try {
            if (Files.exists(CONFIG_PATH)) return;
            Files.createDirectories(CONFIG_PATH.getParent());
            Files.writeString(CONFIG_PATH,
                    "# BetaSeedFinder GPU backend selection\n" +
                    "# backend=auto | amd | nvidia | legacy\n" +
                    "backend=auto\n");
        } catch (IOException ignored) {
        }
    }

    private static IOException missingRequestedBackend(GpuBackendKind kind, List<Path> tried) {
        StringBuilder message = new StringBuilder();
        message.append("Requested GPU backend ").append(kind.displayName()).append(" was not found.\nChecked:\n");
        for (Path path : tried) {
            message.append("  - ").append(path).append('\n');
        }
        return new IOException(message.toString().trim());
    }

    private static ResolvedBackend resolveSingle(GpuBackendKind kind, Path path, List<Path> tried) throws IOException {
        ResolvedBackend found = tryExisting(kind, path, tried);
        if (found != null) {
            return found;
        }
        throw new IOException("Requested GPU backend " + kind.displayName() + " was not found at "
                + path.toAbsolutePath().normalize());
    }

    private static ResolvedBackend tryExisting(GpuBackendKind kind, Path path, List<Path> tried) {
        Path absolute = path.toAbsolutePath().normalize();
        tried.add(absolute);
        if (Files.isRegularFile(absolute)) {
            return new ResolvedBackend(kind, absolute);
        }
        return null;
    }

    record ResolvedBackend(GpuBackendKind kind, Path path) {
        String displayName() {
            return kind.displayName();
        }
    }
}
