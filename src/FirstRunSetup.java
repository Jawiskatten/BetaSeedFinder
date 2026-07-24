import javax.swing.*;
import java.awt.*;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

public final class FirstRunSetup {
    private FirstRunSetup() {
    }

    public static boolean runIfNeeded() {
        Properties properties = AppPaths.loadGuiProperties();
        if (Boolean.parseBoolean(properties.getProperty("setupCompleted", "false"))) {
            return true;
        }

        JTextField outputField = new JTextField(AppPaths.outputRoot().toString(), 42);
        JButton browseButton = new JButton("Browse...");
        JPanel outputRow = new JPanel(new BorderLayout(8, 0));
        outputRow.add(outputField, BorderLayout.CENTER);
        outputRow.add(browseButton, BorderLayout.EAST);

        browseButton.addActionListener(e -> {
            JFileChooser chooser = new JFileChooser(outputField.getText().trim());
            chooser.setDialogTitle("Choose BetaSeedFinder output folder");
            chooser.setFileSelectionMode(JFileChooser.DIRECTORIES_ONLY);
            chooser.setAcceptAllFileFilterUsed(false);
            if (chooser.showOpenDialog(null) == JFileChooser.APPROVE_OPTION) {
                outputField.setText(chooser.getSelectedFile().toPath().toAbsolutePath().normalize().toString());
            }
        });

        JTextArea checks = new JTextArea(startupSummary(AppPaths.outputRoot()));
        checks.setEditable(false);
        checks.setRows(7);
        checks.setColumns(58);
        checks.setBackground(UIManager.getColor("Panel.background"));
        checks.setBorder(BorderFactory.createEmptyBorder(4, 4, 4, 4));

        JPanel panel = new JPanel();
        panel.setLayout(new BoxLayout(panel, BoxLayout.Y_AXIS));
        JLabel heading = new JLabel("BetaSeedFinder first-run setup");
        heading.setFont(heading.getFont().deriveFont(Font.BOLD, 16f));
        heading.setAlignmentX(Component.LEFT_ALIGNMENT);
        JLabel credit = new JLabel("Minecraft Beta 1.7.3 floating-island finder by Jawiskatten");
        credit.setAlignmentX(Component.LEFT_ALIGNMENT);
        JLabel outputLabel = new JLabel("Output folder");
        outputLabel.setAlignmentX(Component.LEFT_ALIGNMENT);
        outputRow.setAlignmentX(Component.LEFT_ALIGNMENT);
        JScrollPane checksScroll = new JScrollPane(checks);
        checksScroll.setAlignmentX(Component.LEFT_ALIGNMENT);

        panel.add(heading);
        panel.add(Box.createVerticalStrut(4));
        panel.add(credit);
        panel.add(Box.createVerticalStrut(14));
        panel.add(outputLabel);
        panel.add(Box.createVerticalStrut(4));
        panel.add(outputRow);
        panel.add(Box.createVerticalStrut(12));
        panel.add(checksScroll);

        while (true) {
            int choice = JOptionPane.showConfirmDialog(
                    null,
                    panel,
                    "BetaSeedFinder Setup",
                    JOptionPane.OK_CANCEL_OPTION,
                    JOptionPane.PLAIN_MESSAGE
            );
            if (choice != JOptionPane.OK_OPTION) {
                return false;
            }

            try {
                Path selected = AppPaths.normalizeOutputPath(Path.of(outputField.getText().trim()));
                Files.createDirectories(selected.resolve("runs"));
                Path writeTest = selected.resolve(".write_test");
                Files.writeString(writeTest, "ok");
                Files.deleteIfExists(writeTest);

                properties.setProperty("outputDirectory", selected.toString());
                properties.setProperty("setupCompleted", "true");
                AppPaths.storeGuiProperties(properties);
                AppPaths.setOutputRootForCurrentProcess(selected);
                return true;
            } catch (Exception ex) {
                JOptionPane.showMessageDialog(
                        null,
                        "That output folder could not be used:\n" + ex.getMessage(),
                        "Output folder error",
                        JOptionPane.ERROR_MESSAGE
                );
            }
        }
    }

    public static String startupSummary(Path outputRoot) {
        Path gpuExe = AppPaths.appRoot().resolve("gpu_p20_benchmark").resolve("build").resolve("gpu_p20_benchmark.exe");
        Path marker = AppPaths.appRoot().resolve("gpu_p20_benchmark").resolve("build").resolve("P38_RELIABLE_RECORD_WORKER_OK.txt");
        Path font = AppPaths.appRoot().resolve("assets").resolve("fonts").resolve("minecraft.ttf");
        long free = outputRoot.toFile().getUsableSpace();
        return "Java: " + System.getProperty("java.version") + "\n"
                + "Application folder: " + AppPaths.appRoot() + "\n"
                + "Output folder: " + outputRoot + "\n"
                + "Free disk space: " + String.format("%.1f GB", free / (1024.0 * 1024.0 * 1024.0)) + "\n"
                + "GPU worker: " + (Files.isRegularFile(gpuExe) ? "Found" : "Will be built on launch") + "\n"
                + "GPU validation marker: " + (Files.isRegularFile(marker) ? "Found" : "Not created yet") + "\n"
                + "Minecraft font: " + (Files.isRegularFile(font) ? "Found" : "Fallback font will be used");
    }
}
