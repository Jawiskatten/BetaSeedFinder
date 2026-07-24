import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public final class AppPaths {
    private static final Path APP_ROOT = resolveApplicationRoot();
    private static final Path CONFIG_ROOT = APP_ROOT.resolve("config");
    private static final Path GUI_CONFIG = CONFIG_ROOT.resolve("gui.properties");
    private static volatile Path outputRoot = loadConfiguredOutputRoot();

    private AppPaths() {
    }

    public static Path appRoot() {
        return APP_ROOT;
    }

    public static Path resolve(String first, String... more) {
        return APP_ROOT.resolve(Path.of(first, more)).normalize();
    }

    public static Path configRoot() {
        return CONFIG_ROOT;
    }

    public static Path guiConfigPath() {
        return GUI_CONFIG;
    }

    public static Path defaultOutputRoot() {
        return APP_ROOT.resolve("out").normalize();
    }

    public static Path outputRoot() {
        return outputRoot;
    }

    public static Path runsRoot() {
        return outputRoot().resolve("runs");
    }

    public static Path favoritesPath() {
        return outputRoot().resolve("favorites.txt");
    }

    public static Path latestRunPointer() {
        return outputRoot().resolve("latest_run.txt");
    }

    public static void setOutputRootForCurrentProcess(Path path) {
        outputRoot = normalizeOutputPath(path);
    }

    public static Path normalizeOutputPath(Path path) {
        Path value = path == null ? defaultOutputRoot() : path;
        if (!value.isAbsolute()) {
            value = APP_ROOT.resolve(value);
        }
        return value.toAbsolutePath().normalize();
    }

    public static Properties loadGuiProperties() {
        Properties properties = new Properties();
        if (!Files.isRegularFile(GUI_CONFIG)) {
            return properties;
        }
        try (var input = Files.newInputStream(GUI_CONFIG)) {
            properties.load(input);
        } catch (IOException ignored) {
        }
        return properties;
    }

    public static void storeGuiProperties(Properties properties) throws IOException {
        Files.createDirectories(CONFIG_ROOT);
        try (var output = Files.newOutputStream(GUI_CONFIG)) {
            properties.store(output, "BetaSeedFinder GUI settings");
        }
    }

    private static Path resolveApplicationRoot() {
        String explicit = System.getProperty("betaseedfinder.appRoot", "").trim();
        if (explicit.isEmpty()) {
            explicit = System.getenv().getOrDefault("BSF_APP_ROOT", "").trim();
        }
        if (!explicit.isEmpty()) {
            try {
                Path resolved = Path.of(explicit).toAbsolutePath().normalize();
                // jpackage's $APPDIR points at the internal app directory. The
                // writable application/output root is its parent directory.
                if (resolved.getFileName() != null
                        && resolved.getFileName().toString().equalsIgnoreCase("app")
                        && resolved.getParent() != null
                        && Files.isDirectory(resolved.getParent().resolve("runtime"))) {
                    return resolved.getParent();
                }
                return resolved;
            } catch (Exception ignored) {
            }
        }

        // Source/development launches continue to use the current project folder.
        return Path.of("").toAbsolutePath().normalize();
    }

    private static Path loadConfiguredOutputRoot() {
        Properties properties = loadGuiProperties();
        String configured = properties.getProperty("outputDirectory", "").trim();
        if (configured.isEmpty()) {
            return defaultOutputRoot();
        }
        try {
            return normalizeOutputPath(Path.of(configured));
        } catch (Exception ignored) {
            return defaultOutputRoot();
        }
    }
}
