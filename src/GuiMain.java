import javax.swing.*;
import javax.swing.table.DefaultTableModel;
import javax.swing.table.TableRowSorter;
import javax.swing.border.CompoundBorder;
import javax.swing.border.LineBorder;
import java.awt.*;
import java.awt.datatransfer.StringSelection;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Map;
import java.util.LinkedHashMap;
import java.util.Properties;

public class GuiMain {
    private JFrame frame;

    private final RunRepository runRepository = new RunRepository(AppPaths.runsRoot());
    private final LiveRunStatistics liveRunStatistics = new LiveRunStatistics();
    private final CardLayout tabLayout = new CardLayout();
    private final JPanel tabCards = new JPanel(tabLayout);
    private final Map<String, JButton> navButtons = new LinkedHashMap<>();
    private StatisticsTab statisticsTab;
    private HistoryTab historyTab;
    private IslandsTab islandsTab;
    private JPanel settingsTab;
    private JPanel logContainer;
    private boolean logExpanded = false;
    private JPanel advancedSettingsPanel;

    private JLabel statusLabel;
    private JLabel headerIslandCountLabel;
    private JLabel headerRuntimeLabel;
    private JLabel checkedLabel;
    private JLabel speedLabel;
    private JLabel matchesLabel;
    private JLabel topUpdatesLabel;
    private JLabel bestLabel;
    private JLabel runtimeLabel;
    private JLabel worldsLabel;
    private JLabel allTimeBestLabel;
    private JLabel searchProfileDescriptionLabel;
    private JLabel searchBudgetLabel;
    private JLabel previewLabel;
    private final CardLayout livePreviewCardLayout = new CardLayout();
    private final JPanel livePreviewCards = new JPanel(livePreviewCardLayout);
    private final Island3DPreviewPanel live3DPreviewPanel = new Island3DPreviewPanel();
    private JButton live3DButton;
    private JButton live2DButton;
    private boolean liveShowing3D = true;
    private SwingWorker<Island3DData, Void> livePreviewWorker;
    private int activePreviewRadius = 7;
    private HitGraphPanel hitGraphPanel;

    private JTextArea selectedStatsArea;

    private JButton startButton;
    private JButton stopButton;
    private JButton copySeedButton;
    private JButton openFolderButton;

    private JTable resultTable;
    private DefaultTableModel tableModel;
    private TableRowSorter<DefaultTableModel> tableSorter;
    private JTextArea logArea;

    private JTextField chunkRadiusField;
    private JTextField seedsToCheckField;
    private JTextField threadsField;
    private JTextField minBlocksField;
    private JTextField minColumnsField;
    private JTextField minYField;
    private JTextField minWidthField;
    private JTextField minDepthField;
    private JTextField minThicknessField;
    private JTextField topKeepField;
    private JTextField outputDirectoryField;
    private JCheckBox savePreviewsBox;
    private JCheckBox hunterModeBox;
    private JComboBox<String> huntProfileBox;
    private JCheckBox overnightModeBox;
    private JCheckBox confirmStopBox;
    private JCheckBox showAdvancedLiveBox;

    private IslandSearchEngine engine;
    private Timer runtimeTimer;
    private long lastChecked;
    private IslandRecord allTimeBestIsland;
    private int displayedAllTimeBestBlocks;

    private static final String APP_VERSION = "v0.5.0-alpha.3";
    private static final Path GUI_CONFIG_PATH = AppPaths.guiConfigPath();

    private static final int MEGA_BLOCKS_THRESHOLD = 30_000;
    private static final int MAX_VISIBLE_LOG_CHARS = 250_000;

    private static final String[] HIT_SOUND_PATHS = {
            "assets/sounds/hit.wav",
            "assets/sounds/new_hit.wav",
            "assets/sounds/normal_hit.wav"
    };

    private static final String[] NEW_BEST_SOUND_PATHS = {
            "assets/sounds/new_best.wav",
            "assets/sounds/newbest.wav",
            "assets/sounds/best.wav",
            "assets/sounds/top.wav"
    };

    private static final String[] MEGA_SOUND_PATHS = {
            "assets/sounds/mega.wav",
            "assets/sounds/mega_hit.wav",
            "assets/sounds/monster.wav",
            "assets/sounds/30000.wav",
            "assets/sounds/30k.wav"
    };

    private int bestBlocks = 0;

    private final SoundPlayer soundPlayer = new SoundPlayer();
    private boolean missingHitSoundLogged = false;
    private boolean missingNewBestSoundLogged = false;
    private boolean missingMegaSoundLogged = false;

    private final Map<Long, SearchResult> resultBySeed = new HashMap<>();

    public static void main(String[] args) {
        SwingUtilities.invokeLater(() -> {
            if (!FirstRunSetup.runIfNeeded()) {
                return;
            }
            new GuiMain().createAndShow();
        });
    }

    private void createAndShow() {
        frame = new JFrame("BetaSeedFinder");
        frame.setDefaultCloseOperation(JFrame.DO_NOTHING_ON_CLOSE);
        frame.addWindowListener(new java.awt.event.WindowAdapter() {
            @Override
            public void windowClosing(java.awt.event.WindowEvent e) {
                requestWindowClose();
            }
        });
        frame.setSize(1380, 900);
        frame.setMinimumSize(new Dimension(1280, 780));
        frame.setLocationRelativeTo(null);

        initializeSettingsControls();
        loadGuiPreferences();

        JPanel root = new JPanel(new BorderLayout(8, 8));
        root.setBackground(MinecraftTheme.BG);
        root.setOpaque(true);
        root.setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));

        runRepository.refresh();
        root.add(createAppShellHeader(), BorderLayout.NORTH);
        createTabs();
        root.add(tabCards, BorderLayout.CENTER);

        frame.setContentPane(root);
        MinecraftTheme.apply(frame);
        frame.setVisible(true);

        runRepository.refresh();
        refreshDataTabs();
        refreshAllTimeBest();
        refreshHeaderSummary();
        updateSearchSummary();

        runtimeTimer = new Timer(1000, e -> refreshLiveClock());
        runtimeTimer.start();

        log(APP_VERSION + " opened.");
        log("Ready to search.");
    }

    private JComponent createAppShellHeader() {
        JPanel shell = new TexturePanel(new BorderLayout(0, 0), "", Color.BLACK, 12);
        shell.setBorder(MinecraftTheme.boxBorder());

        JPanel header = new JPanel(new BorderLayout(14, 0));
        header.setOpaque(false);
        header.setBorder(BorderFactory.createEmptyBorder(6, 10, 6, 10));

        JPanel titlePanel = new JPanel();
        titlePanel.setOpaque(false);
        titlePanel.setLayout(new BoxLayout(titlePanel, BoxLayout.Y_AXIS));

        JPanel titleLine = new JPanel(new FlowLayout(FlowLayout.LEFT, 9, 0));
        titleLine.setOpaque(false);
        titleLine.setAlignmentX(Component.LEFT_ALIGNMENT);
        JLabel title = new JLabel("BETA SEED FINDER");
        title.setName("appTitle");
        JLabel version = new JLabel(APP_VERSION);
        version.setFont(MinecraftTheme.SMALL_FONT);
        version.setForeground(MinecraftTheme.TEXT_DIM);
        titleLine.add(title);
        titleLine.add(version);

        JLabel subtitle = new JLabel("Minecraft Beta 1.7.3 floating-island search  |  Created by Jawiskatten");
        subtitle.setAlignmentX(Component.LEFT_ALIGNMENT);
        subtitle.setFont(MinecraftTheme.SMALL_FONT);
        subtitle.setForeground(MinecraftTheme.TEXT_DIM);
        subtitle.setBorder(BorderFactory.createEmptyBorder(2, 3, 0, 0));

        titlePanel.add(titleLine);
        titlePanel.add(subtitle);

        statusLabel = new JLabel("READY", SwingConstants.RIGHT);
        headerIslandCountLabel = new JLabel("0", SwingConstants.RIGHT);
        headerIslandCountLabel.setToolTipText("Verified islands saved in included production runs.");
        headerRuntimeLabel = new JLabel("00:00:00", SwingConstants.RIGHT);

        JPanel headerRight = new JPanel(new FlowLayout(FlowLayout.RIGHT, 7, 0));
        headerRight.setOpaque(false);
        headerRight.add(createHeaderSummary());

        JButton diagnosticsButton = new JButton("DIAGNOSTICS");
        diagnosticsButton.putClientProperty("compactButton", Boolean.TRUE);
        diagnosticsButton.addActionListener(e -> openDiagnosticsDialog());
        headerRight.add(diagnosticsButton);

        header.add(titlePanel, BorderLayout.WEST);
        header.add(headerRight, BorderLayout.EAST);

        JPanel divider = new JPanel();
        divider.setOpaque(true);
        divider.setBackground(MinecraftTheme.BORDER_MID);
        divider.setPreferredSize(new Dimension(1, 1));

        JPanel nav = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 0));
        nav.setOpaque(false);
        nav.setBorder(BorderFactory.createEmptyBorder(6, 10, 5, 10));
        addNavButton(nav, "SEARCH", "SEARCH");
        addNavButton(nav, "ISLANDS", "ISLANDS");
        addNavButton(nav, "RUNS", "RUNS");
        addNavButton(nav, "SETTINGS", "SETTINGS");

        JPanel body = new JPanel();
        body.setOpaque(false);
        body.setLayout(new BoxLayout(body, BoxLayout.Y_AXIS));
        header.setAlignmentX(Component.LEFT_ALIGNMENT);
        divider.setAlignmentX(Component.LEFT_ALIGNMENT);
        nav.setAlignmentX(Component.LEFT_ALIGNMENT);
        body.add(header);
        body.add(divider);
        body.add(nav);

        shell.add(body, BorderLayout.CENTER);
        return shell;
    }

    private JPanel createHeaderSummary() {
        JPanel summary = new JPanel(new GridLayout(1, 3, 0, 0));
        summary.setOpaque(true);
        summary.setBackground(MinecraftTheme.SETTING_CELL);
        summary.setBorder(new CompoundBorder(
                new LineBorder(MinecraftTheme.BORDER_MID, 1),
                BorderFactory.createEmptyBorder(3, 2, 3, 2)
        ));
        summary.setPreferredSize(new Dimension(470, 58));

        summary.add(headerStatItem("STATUS", statusLabel, true));
        summary.add(headerStatItem("SAVED ISLANDS", headerIslandCountLabel, true));
        summary.add(headerStatItem("SESSION TIME", headerRuntimeLabel, false));
        return summary;
    }

    private JPanel headerStatItem(String captionText, JLabel value, boolean divider) {
        JPanel item = new JPanel(new BorderLayout(1, 1));
        item.setOpaque(false);
        item.setBorder(divider
                ? BorderFactory.createCompoundBorder(
                        BorderFactory.createMatteBorder(0, 0, 0, 1, MinecraftTheme.BORDER_MID),
                        BorderFactory.createEmptyBorder(4, 10, 4, 10))
                : BorderFactory.createEmptyBorder(4, 10, 4, 10));

        JLabel caption = new JLabel(captionText, SwingConstants.CENTER);
        caption.setFont(MinecraftTheme.SMALL_FONT);
        caption.setForeground(MinecraftTheme.TEXT_DIM);

        value.setName("accentValue");
        value.setForeground(MinecraftTheme.BLUE_HIT);
        value.setFont(MinecraftTheme.HEADER_FONT);
        value.setHorizontalAlignment(SwingConstants.CENTER);

        item.add(caption, BorderLayout.NORTH);
        item.add(value, BorderLayout.CENTER);
        return item;
    }

    private void addNavButton(JPanel nav, String label, String key) {
        JButton button = new JButton(label);
        button.putClientProperty("navButton", Boolean.TRUE);
        button.addActionListener(e -> showTab(key));
        navButtons.put(key, button);
        nav.add(button);
    }

    private void createTabs() {
        tabCards.setOpaque(true);
        tabCards.setBackground(MinecraftTheme.BG);

        statisticsTab = new StatisticsTab(runRepository, liveRunStatistics);
        islandsTab = new IslandsTab(runRepository);
        historyTab = new HistoryTab(runRepository, new HistoryTab.NavigationListener() {
            @Override
            public void viewStatistics(RunRecord run) {
                statisticsTab.setSelectedRun(run);
                showTab("STATISTICS");
            }

            @Override
            public void viewIslands(RunRecord run) {
                islandsTab.setSelectedRun(run);
                showTab("ISLANDS");
            }
        });
        settingsTab = createSettingsTab();

        tabCards.add(createSearchTab(), "SEARCH");
        tabCards.add(islandsTab, "ISLANDS");
        tabCards.add(historyTab, "RUNS");
        tabCards.add(settingsTab, "SETTINGS");
        tabCards.add(statisticsTab, "STATISTICS");
        showTab("SEARCH");
    }

    private void showTab(String key) {
        tabLayout.show(tabCards, key);
        for (Map.Entry<String, JButton> entry : navButtons.entrySet()) {
            boolean selected = entry.getKey().equals(key);
            entry.getValue().putClientProperty("selectedNav", selected);
            entry.getValue().repaint();
        }
        if ("STATISTICS".equals(key)) statisticsTab.refreshData();
        if ("RUNS".equals(key)) historyTab.refreshData();
        if ("ISLANDS".equals(key)) islandsTab.refreshData();
        if ("SETTINGS".equals(key)) updateSearchSummary();
    }

    private void refreshDataTabs() {
        if (statisticsTab != null) statisticsTab.refreshData();
        if (historyTab != null) historyTab.refreshData();
        if (islandsTab != null) islandsTab.refreshData();
    }

    private JPanel createSearchTab() {
        JPanel panel = new JPanel(new BorderLayout(8, 8));
        panel.setOpaque(true);
        panel.setBackground(MinecraftTheme.BG);
        panel.setBorder(BorderFactory.createEmptyBorder(6, 6, 6, 6));

        panel.add(createTopPanel(), BorderLayout.NORTH);

        JPanel content = new JPanel(new BorderLayout(8, 8));
        content.setOpaque(false);

        JPanel leftColumn = new JPanel(new BorderLayout(8, 8));
        leftColumn.setOpaque(false);
        leftColumn.add(createStatsPanel(), BorderLayout.NORTH);
        leftColumn.add(createCenterPanel(), BorderLayout.CENTER);
        leftColumn.add(createLogPanel(), BorderLayout.SOUTH);

        content.add(leftColumn, BorderLayout.CENTER);
        content.add(createPreviewSidebar(), BorderLayout.EAST);

        panel.add(content, BorderLayout.CENTER);
        return panel;
    }

    private JPanel createTopPanel() {
        JPanel actionPanel = new TexturePanel(new BorderLayout(12, 5), "", Color.BLACK, 14);
        actionPanel.setBorder(MinecraftTheme.boxBorder());

        JPanel mainRow = new JPanel();
        mainRow.setOpaque(false);
        mainRow.setLayout(new BoxLayout(mainRow, BoxLayout.X_AXIS));

        JLabel profileLabel = new JLabel("PROFILE");
        profileLabel.setFont(MinecraftTheme.SMALL_FONT);
        profileLabel.setForeground(MinecraftTheme.TEXT_DIM);
        profileLabel.setBorder(BorderFactory.createEmptyBorder(0, 0, 0, 7));

        huntProfileBox.setPreferredSize(new Dimension(290, 38));
        huntProfileBox.setMinimumSize(new Dimension(290, 38));
        huntProfileBox.setMaximumSize(new Dimension(290, 38));
        huntProfileBox.setPrototypeDisplayValue("World Record (experimental)");

        searchBudgetLabel = new JLabel();
        searchBudgetLabel.setForeground(MinecraftTheme.TEXT_DIM);
        searchBudgetLabel.setFont(MinecraftTheme.SMALL_FONT);
        searchBudgetLabel.setBorder(BorderFactory.createEmptyBorder(0, 10, 0, 0));

        JButton editSettings = new JButton("EDIT SETTINGS");
        editSettings.putClientProperty("compactButton", Boolean.TRUE);
        editSettings.addActionListener(e -> showTab("SETTINGS"));

        startButton = new JButton("START SEARCH");
        startButton.putClientProperty("accentButton", Boolean.TRUE);
        stopButton = new JButton("STOP");
        openFolderButton = new JButton("OPEN RESULTS");
        copySeedButton = new JButton("COPY SEED");
        copySeedButton.putClientProperty("compactButton", Boolean.TRUE);

        stopButton.setEnabled(false);
        copySeedButton.setEnabled(false);
        startButton.addActionListener(e -> startRealSearch());
        stopButton.addActionListener(e -> stopRealSearch());
        copySeedButton.addActionListener(e -> copySelectedSeed());
        openFolderButton.addActionListener(e -> openResultsFolder());

        mainRow.add(profileLabel);
        mainRow.add(huntProfileBox);
        mainRow.add(searchBudgetLabel);
        mainRow.add(Box.createHorizontalStrut(8));
        mainRow.add(editSettings);
        mainRow.add(Box.createHorizontalGlue());
        mainRow.add(startButton);
        mainRow.add(Box.createHorizontalStrut(7));
        mainRow.add(stopButton);
        mainRow.add(Box.createHorizontalStrut(7));
        mainRow.add(openFolderButton);

        searchProfileDescriptionLabel = new JLabel();
        searchProfileDescriptionLabel.setFont(MinecraftTheme.SMALL_FONT);
        searchProfileDescriptionLabel.setForeground(MinecraftTheme.TEXT_DIM);
        searchProfileDescriptionLabel.setBorder(BorderFactory.createEmptyBorder(3, 2, 0, 0));

        actionPanel.add(mainRow, BorderLayout.CENTER);
        actionPanel.add(searchProfileDescriptionLabel, BorderLayout.SOUTH);
        return actionPanel;
    }

    private void initializeSettingsControls() {
        chunkRadiusField = new JTextField("7");
        seedsToCheckField = new JTextField("30000000000");
        threadsField = new JTextField(String.valueOf(Math.max(1, Math.min(10, Runtime.getRuntime().availableProcessors() - 1))));
        minBlocksField = new JTextField("20000");
        minColumnsField = new JTextField("100");
        minYField = new JTextField("60");
        minWidthField = new JTextField("10");
        minDepthField = new JTextField("10");
        minThicknessField = new JTextField("2.0");
        topKeepField = new JTextField("100");
        outputDirectoryField = new JTextField(AppPaths.outputRoot().toString());
        savePreviewsBox = new JCheckBox("Automatically save exceptional previews", true);
        hunterModeBox = new JCheckBox("Use GPU hunter pipeline", true);
        huntProfileBox = new JComboBox<>(new String[] {
                "General",
                "Mega (maximum recall)",
                "Record Hunt (recommended)",
                "World Record (experimental)"
        });
        huntProfileBox.setSelectedIndex(SearchSettings.HUNT_PROFILE_RECORD_60K);
        overnightModeBox = new JCheckBox("Research logging / long-run evidence", false);
        confirmStopBox = new JCheckBox("Confirm before stopping an active search", true);
        showAdvancedLiveBox = new JCheckBox("Show advanced live chart", false);

        huntProfileBox.addActionListener(e -> {
            updateProfileDescription();
            updateSearchSummary();
        });
        overnightModeBox.addActionListener(e -> applyLongRunDefaults());
        savePreviewsBox.setToolTipText("Saves previews only for exceptional candidates, not every match.");
        overnightModeBox.setToolTipText("Enables extra evidence logging. Leave off for normal public searches.");
    }

    private JPanel createSettingsTab() {
        JPanel root = new JPanel(new BorderLayout(10, 10));
        root.setOpaque(true);
        root.setBackground(MinecraftTheme.BG);
        root.setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));

        JPanel leftColumn = new JPanel();
        leftColumn.setOpaque(false);
        leftColumn.setLayout(new BoxLayout(leftColumn, BoxLayout.Y_AXIS));

        JPanel general = settingsSection("GENERAL");
        general.add(settingsToggleRow(
                "Confirm before stopping",
                "Ask before ending an active search.",
                confirmStopBox));
        general.add(settingsToggleRow(
                "Automatic previews",
                "Save previews for exceptional islands.",
                savePreviewsBox));

        JPanel outputControls = new JPanel(new BorderLayout(8, 0));
        outputControls.setOpaque(false);
        outputDirectoryField.setToolTipText("Changes take effect after restarting BetaSeedFinder.");
        JButton chooseOutput = new JButton("Choose...");
        chooseOutput.putClientProperty("compactButton", Boolean.TRUE);
        chooseOutput.addActionListener(e -> chooseOutputDirectory());
        JButton openOutput = new JButton("Open");
        openOutput.putClientProperty("compactButton", Boolean.TRUE);
        openOutput.addActionListener(e -> openResultsFolder());
        JPanel outputButtons = new JPanel(new FlowLayout(FlowLayout.RIGHT, 6, 0));
        outputButtons.setOpaque(false);
        outputButtons.add(chooseOutput);
        outputButtons.add(openOutput);
        outputControls.add(outputDirectoryField, BorderLayout.CENTER);
        outputControls.add(outputButtons, BorderLayout.EAST);
        general.add(settingsRow(
                "Output location",
                "Runs, previews and manifests. A changed folder is used after restart.",
                outputControls));

        JPanel search = settingsSection("SEARCH DEFAULTS");
        search.add(settingsRow("Region budget", "Maximum regions checked in a run.", seedsToCheckField));
        search.add(settingsRow("CPU exact workers", "CPU threads used for exact verification.", threadsField));
        search.add(settingsRow("Chunk radius", "Verification radius around each candidate.", chunkRadiusField));
        search.add(settingsRow("Minimum blocks", "Do not save islands below this size.", minBlocksField));
        search.add(settingsRow("Minimum columns", "Minimum occupied surface columns.", minColumnsField));
        search.add(settingsRow("Minimum Y", "Lowest accepted island elevation.", minYField));

        leftColumn.add(general);
        leftColumn.add(Box.createVerticalStrut(10));
        leftColumn.add(search);

        JPanel system = settingsSection("ABOUT & SYSTEM");
        RunRecord latest = runRepository.latestRun();
        String gpu = latest == null ? "Not recorded" : latest.manifest.getOrDefault("gpuName", latest.manifest.getOrDefault("gpu", "Not recorded"));
        Path output = AppPaths.outputRoot();
        double freeGb = output.toFile().getUsableSpace() / (1024.0 * 1024.0 * 1024.0);
        system.add(settingsInfoRow("Created by", "Jawiskatten"));
        system.add(settingsInfoRow("Version", APP_VERSION));
        system.add(settingsInfoRow("Backend", "Exact GPU production pipeline"));
        system.add(settingsInfoRow("GPU", gpu));
        system.add(settingsInfoRow("Java", System.getProperty("java.version")));
        system.add(settingsInfoRow("Saved islands", String.format("%,d", runRepository.allIslands(true).size())));
        system.add(settingsInfoRow("Free disk", String.format("%.1f GB", freeGb)));
        system.add(settingsInfoRow("Latest run", latest == null ? "None" : latest.dateText() + " | " + latest.friendlyStatus()));



        JPanel main = new JPanel(new GridLayout(1, 2, 10, 0));
        main.setOpaque(false);
        main.add(leftColumn);
        main.add(system);

        JPanel advancedWrap = new TexturePanel(new BorderLayout(8, 8), "", Color.BLACK, 42);
        advancedWrap.setBorder(MinecraftTheme.titled("ADVANCED"));
        JLabel warning = new JLabel("Built-in profiles are validated together. Only change these for research or custom searches.");
        warning.setForeground(MinecraftTheme.TEXT_DIM);
        warning.setFont(MinecraftTheme.SMALL_FONT);
        JButton advancedToggle = new JButton("Show Advanced Settings +");
        advancedToggle.putClientProperty("compactButton", Boolean.TRUE);
        advancedSettingsPanel = new JPanel(new GridLayout(0, 3, 8, 8));
        advancedSettingsPanel.setOpaque(false);
        advancedSettingsPanel.add(labeled("Minimum width", minWidthField));
        advancedSettingsPanel.add(labeled("Minimum depth", minDepthField));
        advancedSettingsPanel.add(labeled("Minimum thickness", minThicknessField));
        advancedSettingsPanel.add(labeled("Top results kept", topKeepField));
        advancedSettingsPanel.add(checkCell(hunterModeBox));
        advancedSettingsPanel.add(checkCell(overnightModeBox));
        advancedSettingsPanel.add(checkCell(showAdvancedLiveBox));
        advancedSettingsPanel.add(labeledInfo("GPU centers per world", "8"));
        advancedSettingsPanel.add(labeledInfo("Backend", "Exact P38/P39 production pipeline"));
        advancedSettingsPanel.setVisible(false);
        advancedToggle.addActionListener(e -> {
            boolean show = !advancedSettingsPanel.isVisible();
            advancedSettingsPanel.setVisible(show);
            advancedToggle.setText(show ? "Hide Advanced Settings -" : "Show Advanced Settings +");
            frame.revalidate();
        });
        JPanel advancedHeader = new JPanel(new BorderLayout(8, 0));
        advancedHeader.setOpaque(false);
        advancedHeader.add(warning, BorderLayout.CENTER);
        advancedHeader.add(advancedToggle, BorderLayout.EAST);
        advancedWrap.add(advancedHeader, BorderLayout.NORTH);
        advancedWrap.add(advancedSettingsPanel, BorderLayout.CENTER);

        JPanel actions = new TexturePanel(new FlowLayout(FlowLayout.RIGHT, 10, 0), "", Color.BLACK, 24);
        actions.setBorder(MinecraftTheme.boxBorder());
        JButton copyDiagnostics = new JButton("Copy Diagnostic Report");
        copyDiagnostics.addActionListener(e -> Toolkit.getDefaultToolkit().getSystemClipboard().setContents(
                new StringSelection(settingsDiagnosticsText()), null));
        JButton defaults = new JButton("Restore Defaults");
        defaults.addActionListener(e -> restoreDefaultPreferences());
        JButton save = new JButton("Save Settings");
        save.putClientProperty("accentButton", Boolean.TRUE);
        save.addActionListener(e -> {
            Path selectedOutput;
            try {
                String rawOutput = outputDirectoryField.getText().trim();
                if (rawOutput.isEmpty()) {
                    throw new IllegalArgumentException("Choose an output folder first.");
                }
                selectedOutput = AppPaths.normalizeOutputPath(Path.of(rawOutput));
                Files.createDirectories(selectedOutput);
            } catch (Exception ex) {
                JOptionPane.showMessageDialog(
                        frame,
                        "The output folder is not valid:\n" + ex.getMessage(),
                        "Settings error",
                        JOptionPane.ERROR_MESSAGE
                );
                return;
            }

            outputDirectoryField.setText(selectedOutput.toString());
            boolean outputChanged = !selectedOutput.equals(AppPaths.outputRoot());
            saveGuiPreferences();
            updateSearchSummary();
            JOptionPane.showMessageDialog(
                    frame,
                    outputChanged ? "Settings saved. Restart BetaSeedFinder to use the new output folder." : "Settings saved.",
                    "BetaSeedFinder",
                    JOptionPane.INFORMATION_MESSAGE
            );
        });
        actions.add(copyDiagnostics);
        actions.add(defaults);
        actions.add(save);

        JPanel content = new JPanel(new BorderLayout(10, 10));
        content.setOpaque(false);
        content.add(main, BorderLayout.CENTER);
        content.add(advancedWrap, BorderLayout.SOUTH);

        JScrollPane scroll = new JScrollPane(content);
        scroll.setBorder(null);
        scroll.getViewport().setOpaque(false);
        scroll.setOpaque(false);
        root.add(scroll, BorderLayout.CENTER);
        root.add(actions, BorderLayout.SOUTH);
        return root;
    }

    private JPanel settingsSection(String title) {
        JPanel section = new TexturePanel("", Color.BLACK, 38);
        section.setLayout(new BoxLayout(section, BoxLayout.Y_AXIS));
        section.setBorder(MinecraftTheme.titled(title));
        return section;
    }

    private JPanel settingsRow(String label, String help, JComponent control) {
        JPanel row = new JPanel(new BorderLayout(14, 0));
        row.setOpaque(false);
        row.setBorder(BorderFactory.createCompoundBorder(
                BorderFactory.createMatteBorder(0, 0, 1, 0, MinecraftTheme.BORDER_MID),
                BorderFactory.createEmptyBorder(6, 10, 6, 10)));
        row.setMaximumSize(new Dimension(Integer.MAX_VALUE, 56));

        JPanel text = new JPanel();
        text.setOpaque(false);
        text.setLayout(new BoxLayout(text, BoxLayout.Y_AXIS));
        JLabel title = new JLabel(label);
        title.setForeground(MinecraftTheme.TEXT);
        title.setFont(MinecraftTheme.UI_FONT);
        JLabel description = new JLabel(help);
        description.setForeground(MinecraftTheme.TEXT_DIM);
        description.setFont(MinecraftTheme.SMALL_FONT);
        text.add(title);
        text.add(Box.createVerticalStrut(3));
        text.add(description);

        JPanel controlWrap = new JPanel(new GridBagLayout());
        controlWrap.setOpaque(false);
        int controlWidth = control instanceof JCheckBox
                ? 30
                : Math.min(300, Math.max(180, control.getPreferredSize().width));
        int controlHeight = control instanceof JCheckBox
                ? Math.max(24, control.getPreferredSize().height)
                : Math.max(34, control.getPreferredSize().height);
        control.setPreferredSize(new Dimension(controlWidth, controlHeight));
        controlWrap.add(control);

        row.add(text, BorderLayout.CENTER);
        row.add(controlWrap, BorderLayout.EAST);
        return row;
    }

    private JPanel settingsToggleRow(String label, String help, JCheckBox toggle) {
        toggle.setText("");
        return settingsRow(label, help, toggle);
    }

    private JPanel settingsInfoRow(String label, String value) {
        JPanel row = new JPanel(new BorderLayout(14, 0));
        row.setOpaque(false);
        row.setBorder(BorderFactory.createCompoundBorder(
                BorderFactory.createMatteBorder(0, 0, 1, 0, MinecraftTheme.BORDER_MID),
                BorderFactory.createEmptyBorder(7, 10, 7, 10)));
        row.setMaximumSize(new Dimension(Integer.MAX_VALUE, 46));

        JLabel labelView = new JLabel(label);
        labelView.setForeground(MinecraftTheme.TEXT_DIM);
        labelView.setPreferredSize(new Dimension(135, 24));

        JLabel valueView = new JLabel(value);
        valueView.setForeground(MinecraftTheme.TEXT);
        valueView.setToolTipText(value);

        row.add(labelView, BorderLayout.WEST);
        row.add(valueView, BorderLayout.CENTER);
        return row;
    }

    private String settingsDiagnosticsText() {
        RunRecord latest = runRepository.latestRun();
        String gpu = latest == null ? "Not recorded" : latest.manifest.getOrDefault("gpuName", latest.manifest.getOrDefault("gpu", "Not recorded"));
        Path output = AppPaths.outputRoot();
        double freeGb = output.toFile().getUsableSpace() / (1024.0 * 1024.0 * 1024.0);
        return "CREATED BY      Jawiskatten\n" +
                "APP VERSION     " + APP_VERSION + "\n" +
                "BACKEND         Exact GPU production pipeline\n" +
                "GPU             " + gpu + "\n" +
                "JAVA            " + System.getProperty("java.version") + "\n" +
                "OUTPUT PATH     " + output + "\n" +
                "SAVED ISLANDS   " + String.format("%,d", runRepository.allIslands(true).size()) + "\n" +
                "FREE DISK       " + String.format("%.1f GB", freeGb) + "\n" +
                "LATEST RUN      " + (latest == null ? "None" : latest.dateText() + " | " + latest.friendlyStatus());
    }

    private void applyLongRunDefaults() {
        if (!overnightModeBox.isSelected()) return;
        hunterModeBox.setSelected(true);
        if ("5".equals(chunkRadiusField.getText().trim())) chunkRadiusField.setText("7");
        try {
            if (Integer.parseInt(topKeepField.getText().trim()) < 100) topKeepField.setText("100");
        } catch (NumberFormatException e) {
            topKeepField.setText("100");
        }
    }

     private JPanel createStatsPanel() {
        JPanel statsPanel = new TexturePanel(new GridLayout(1, 6, 0, 0), "", Color.BLACK, 24);
        statsPanel.setBorder(MinecraftTheme.boxBorder());
        statsPanel.setPreferredSize(new Dimension(0, 70));
        statsPanel.setMinimumSize(new Dimension(0, 70));

        speedLabel = new JLabel("0 /s", SwingConstants.CENTER);
        runtimeLabel = new JLabel("0s", SwingConstants.CENTER);
        checkedLabel = new JLabel("0", SwingConstants.CENTER);
        worldsLabel = new JLabel("0", SwingConstants.CENTER);
        bestLabel = new JLabel("-", SwingConstants.CENTER);
        allTimeBestLabel = new JLabel("-", SwingConstants.CENTER);
        matchesLabel = new JLabel("0");
        topUpdatesLabel = new JLabel("0");

        statsPanel.add(statCell("SPEED", "Regions per second", speedLabel, true));
        statsPanel.add(statCell("RUNTIME", "Current run time", runtimeLabel, true));
        statsPanel.add(statCell("REGIONS", "Regions searched", checkedLabel, true));
        statsPanel.add(statCell("WORLDS", "Worlds processed", worldsLabel, true));
        statsPanel.add(statCell("RUN BEST", "Best island this run", bestLabel, true));
        statsPanel.add(statCell("ALL-TIME", "All-time best island", allTimeBestLabel, false));
        return statsPanel;
    }

    private JPanel statCell(String name, String tooltip, JLabel value, boolean divider) {
        JPanel cell = new JPanel(new BorderLayout(2, 2));
        cell.setOpaque(false);
        cell.setToolTipText(tooltip);
        cell.setBorder(divider
                ? BorderFactory.createCompoundBorder(
                        BorderFactory.createMatteBorder(0, 0, 0, 1, MinecraftTheme.BORDER_MID),
                        BorderFactory.createEmptyBorder(5, 7, 5, 7))
                : BorderFactory.createEmptyBorder(5, 7, 5, 7));

        value.setName("accentValue");
        value.setFont(MinecraftTheme.HEADER_FONT);
        value.setForeground(MinecraftTheme.BLUE_HIT);
        value.setToolTipText(tooltip);

        JLabel caption = new JLabel(name, SwingConstants.CENTER);
        caption.setFont(MinecraftTheme.SMALL_FONT);
        caption.setForeground(MinecraftTheme.TEXT_DIM);
        caption.setToolTipText(tooltip);

        cell.add(caption, BorderLayout.NORTH);
        cell.add(value, BorderLayout.CENTER);
        return cell;
    }

    private JPanel labeled(String label, JTextField field) {
    JPanel panel = new JPanel(new BorderLayout(3, 2));
    panel.setName("settingCell");
    panel.setOpaque(true);
    panel.setBackground(MinecraftTheme.SETTING_CELL);
    panel.setBorder(MinecraftTheme.settingCellBorder());

    JLabel jLabel = new JLabel(label);
    jLabel.setName("settingLabel");
    jLabel.setForeground(MinecraftTheme.TEXT);
    jLabel.setFont(MinecraftTheme.SMALL_FONT);

    panel.add(jLabel, BorderLayout.NORTH);
    panel.add(field, BorderLayout.CENTER);

    return panel;
}

    private JPanel checkCell(JCheckBox box) {
    JPanel panel = new JPanel(new GridBagLayout());
    panel.setName("settingCell");
    panel.setOpaque(true);
    panel.setBackground(MinecraftTheme.SETTING_CELL);
    panel.setBorder(MinecraftTheme.settingCellBorder());
    panel.add(box);
    return panel;
}

    private JPanel checkCell(JButton button) {
        JPanel panel = new JPanel(new GridBagLayout());
        panel.setName("settingCell");
        panel.setOpaque(true);
        panel.setBackground(MinecraftTheme.SETTING_CELL);
        panel.setBorder(MinecraftTheme.settingCellBorder());
        panel.add(button);
        return panel;
    }

    private JPanel labeledInfo(String label, String value) {
    JPanel panel = new JPanel(new BorderLayout(3, 2));
    panel.setName("settingCell");
    panel.setOpaque(true);
    panel.setBackground(MinecraftTheme.SETTING_CELL);
    panel.setBorder(MinecraftTheme.settingCellBorder());

    JLabel jLabel = new JLabel(label);
    jLabel.setName("settingLabel");
    jLabel.setForeground(MinecraftTheme.TEXT);
    jLabel.setFont(MinecraftTheme.SMALL_FONT);

    JLabel valueLabel = new JLabel(value);
    valueLabel.setForeground(MinecraftTheme.TEXT_DIM);
    valueLabel.setFont(MinecraftTheme.SMALL_FONT);

    panel.add(jLabel, BorderLayout.NORTH);
    panel.add(valueLabel, BorderLayout.CENTER);
    return panel;
}

    private JPanel createCenterPanel() {
    JPanel panel = new JPanel(new BorderLayout(8, 8));
    panel.setOpaque(true);
    panel.setBackground(MinecraftTheme.BG);

    tableModel = new DefaultTableModel(
            new Object[] {
                    "Rank",
                    "Seed",
                    "Blocks",
                    "Columns",
                    "Footprint",
                    "Fill %",
                    "Y Range",
                    "Center"
            },
            0
    ) {
        @Override
        public boolean isCellEditable(int row, int column) {
            return false;
        }

        @Override
        public Class<?> getColumnClass(int columnIndex) {
            if (columnIndex == 0) return Integer.class;
            if (columnIndex == 1) return Long.class;
            if (columnIndex == 2 || columnIndex == 3) return Integer.class;
            if (columnIndex == 5) return Double.class;
            return String.class;
        }
    };

    resultTable = new JTable(tableModel);
    resultTable.setFillsViewportHeight(false);
    resultTable.setOpaque(false);
    resultTable.setRowHeight(24);
    resultTable.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);

    tableSorter = new TableRowSorter<>(tableModel);
    resultTable.setRowSorter(tableSorter);
    installResultTableSorters();
    tableSorter.toggleSortOrder(2);
    tableSorter.toggleSortOrder(2);

    resultTable.getSelectionModel().addListSelectionListener(e -> {
        if (!e.getValueIsAdjusting()) {
            onResultRowSelected();
        }
    });

    JScrollPane tableScroll = new JScrollPane(resultTable);
    tableScroll.setOpaque(false);
    tableScroll.getViewport().setOpaque(false);
    tableScroll.getViewport().setBackground(new Color(0, 0, 0, 0));
    tableScroll.setBorder(BorderFactory.createLineBorder(MinecraftTheme.BORDER_DARK, 2));

    hitGraphPanel = new HitGraphPanel();

    JPanel tableAndGraphPanel = new JPanel(new BorderLayout(6, 6));
    tableAndGraphPanel.setOpaque(true);
    tableAndGraphPanel.setBackground(MinecraftTheme.BG);

    JPanel islandFeedPanel = new TexturePanel(
            new BorderLayout(),
            "assets/textures/stone.png",
            Color.BLACK,
            225
    );
    islandFeedPanel.setOpaque(true);
    islandFeedPanel.setBackground(MinecraftTheme.STONE_DARK);
    islandFeedPanel.setBorder(MinecraftTheme.titled("Recent Significant Finds"));
    islandFeedPanel.add(tableScroll, BorderLayout.CENTER);

    tableAndGraphPanel.add(islandFeedPanel, BorderLayout.CENTER);

    JPanel advancedLivePanel = new JPanel(new BorderLayout());
    advancedLivePanel.setOpaque(false);
    advancedLivePanel.add(hitGraphPanel, BorderLayout.CENTER);
    advancedLivePanel.setVisible(showAdvancedLiveBox.isSelected());
    showAdvancedLiveBox.addActionListener(e -> {
        advancedLivePanel.setVisible(showAdvancedLiveBox.isSelected());
        frame.revalidate();
    });
    tableAndGraphPanel.add(advancedLivePanel, BorderLayout.SOUTH);

    panel.add(tableAndGraphPanel, BorderLayout.CENTER);
    return panel;
}

    private JPanel createPreviewSidebar() {
    JPanel rightPanel = new JPanel(new BorderLayout(8, 8));
    rightPanel.setOpaque(true);
    rightPanel.setBackground(MinecraftTheme.BG);
    rightPanel.setPreferredSize(new Dimension(420, 0));
    rightPanel.setMinimumSize(new Dimension(390, 0));

    JPanel previewPanel = new TexturePanel(
            new BorderLayout(),
            "assets/textures/stone.png",
            Color.BLACK,
            105
    );
    previewPanel.setOpaque(true);
    previewPanel.setBackground(MinecraftTheme.STONE);
    previewPanel.setBorder(MinecraftTheme.titled("Preview"));
    previewPanel.setPreferredSize(new Dimension(420, 330));
    previewPanel.setMinimumSize(new Dimension(390, 285));

    JPanel previewToolbar = new JPanel(new FlowLayout(FlowLayout.LEFT, 5, 0));
    previewToolbar.setOpaque(false);
    live3DButton = new JButton("3D");
    live2DButton = new JButton("2D");
    JButton topViewButton = new JButton("Top");
    JButton resetViewButton = new JButton("Reset");
    live3DButton.putClientProperty("accentButton", Boolean.TRUE);
    live3DButton.addActionListener(e -> switchLivePreviewMode(true));
    live2DButton.addActionListener(e -> switchLivePreviewMode(false));
    topViewButton.addActionListener(e -> {
        switchLivePreviewMode(true);
        live3DPreviewPanel.topView();
    });
    resetViewButton.addActionListener(e -> {
        switchLivePreviewMode(true);
        live3DPreviewPanel.resetView();
    });
    previewToolbar.add(live3DButton);
    previewToolbar.add(live2DButton);
    previewToolbar.add(topViewButton);
    previewToolbar.add(resetViewButton);

    previewLabel = new JLabel("Click a result to show preview", SwingConstants.CENTER);
    previewLabel.setForeground(MinecraftTheme.TEXT_DIM);
    livePreviewCards.setOpaque(false);
    livePreviewCards.add(live3DPreviewPanel, "3D");
    livePreviewCards.add(previewLabel, "2D");
    livePreviewCardLayout.show(livePreviewCards, "3D");

    previewPanel.add(previewToolbar, BorderLayout.NORTH);
    previewPanel.add(livePreviewCards, BorderLayout.CENTER);

    JPanel statsPanel = new TexturePanel(
            new GridLayout(1, 1),
            "assets/textures/stone.png",
            Color.BLACK,
            225
    );
    statsPanel.setOpaque(true);
    statsPanel.setBackground(MinecraftTheme.STONE);
    statsPanel.setBorder(MinecraftTheme.titled("Selected Island"));

    selectedStatsArea = new JTextArea();
    selectedStatsArea.setEditable(false);
    selectedStatsArea.setFont(MinecraftTheme.UI_FONT);
    selectedStatsArea.setLineWrap(true);
    selectedStatsArea.setWrapStyleWord(false);
    selectedStatsArea.setText("No island selected.");
    selectedStatsArea.setOpaque(false);
    selectedStatsArea.setBackground(MinecraftTheme.PANEL_INNER);
    selectedStatsArea.setForeground(MinecraftTheme.TEXT);
    selectedStatsArea.setCaretColor(MinecraftTheme.TEXT);
    selectedStatsArea.setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));

    JScrollPane statsScroll = new JScrollPane(selectedStatsArea);
    statsScroll.setHorizontalScrollBarPolicy(ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER);
    statsScroll.setVerticalScrollBarPolicy(ScrollPaneConstants.VERTICAL_SCROLLBAR_AS_NEEDED);
    statsScroll.setBorder(BorderFactory.createLineBorder(MinecraftTheme.BORDER_DARK, 2));
    statsScroll.setOpaque(false);
    statsScroll.getViewport().setOpaque(false);
    statsScroll.getViewport().setBackground(new Color(0, 0, 0, 0));
    statsScroll.setBackground(new Color(0, 0, 0, 0));
    JPanel selectedActions = new JPanel(new FlowLayout(FlowLayout.RIGHT, 6, 0));
    selectedActions.setOpaque(false);
    selectedActions.add(copySeedButton);

    statsPanel.setLayout(new BorderLayout(0, 6));
    statsPanel.add(statsScroll, BorderLayout.CENTER);
    statsPanel.add(selectedActions, BorderLayout.SOUTH);

    rightPanel.add(previewPanel, BorderLayout.NORTH);
    rightPanel.add(statsPanel, BorderLayout.CENTER);

    return rightPanel;
}

    private void installResultTableSorters() {
        tableSorter.setComparator(4, Comparator.comparingInt(GuiMain::parseFootprintArea));
        tableSorter.setComparator(6, Comparator.comparingInt(GuiMain::parseYRangeSpan));
    }

    private static int parseFootprintArea(Object value) {
        if (value == null) {
            return 0;
        }

        String text = value.toString().trim().toLowerCase();
        String[] parts = text.split("x");

        if (parts.length != 2) {
            return 0;
        }

        try {
            int width = Integer.parseInt(parts[0].trim());
            int depth = Integer.parseInt(parts[1].trim());
            return width * depth;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static int parseYRangeSpan(Object value) {
        if (value == null) {
            return 0;
        }

        String text = value.toString().trim();
        int dashIndex = text.indexOf('-', 1);

        if (dashIndex < 0) {
            dashIndex = text.indexOf('-');
        }

        if (dashIndex < 0) {
            return 0;
        }

        try {
            int minY = Integer.parseInt(text.substring(0, dashIndex).trim());
            int maxY = Integer.parseInt(text.substring(dashIndex + 1).trim());
            return maxY - minY;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private JPanel createLogPanel() {
        logContainer = new TexturePanel(new BorderLayout(0, 4), "assets/textures/dirt.png", Color.BLACK, 220);
        logContainer.setBorder(MinecraftTheme.boxBorder());

        JButton toggle = new JButton("EVENT LOG +");
        toggle.setHorizontalAlignment(SwingConstants.LEFT);
        logContainer.add(toggle, BorderLayout.NORTH);

        logArea = new JTextArea();
        logArea.setEditable(false);
        logArea.setLineWrap(true);
        logArea.setWrapStyleWord(false);
        logArea.setOpaque(false);

        JScrollPane scroll = new JScrollPane(logArea);
        scroll.setHorizontalScrollBarPolicy(ScrollPaneConstants.HORIZONTAL_SCROLLBAR_NEVER);
        scroll.setOpaque(false);
        scroll.getViewport().setOpaque(false);
        scroll.setPreferredSize(new Dimension(0, 125));
        scroll.setVisible(false);
        logContainer.add(scroll, BorderLayout.CENTER);

        toggle.addActionListener(e -> {
            logExpanded = !logExpanded;
            scroll.setVisible(logExpanded);
            toggle.setText(logExpanded ? "EVENT LOG -" : "EVENT LOG +");
            frame.revalidate();
        });
        return logContainer;
    }

    private SearchSettings readSettingsFromGui() {
        SearchSettings settings = new SearchSettings();

        settings.chunkRadius = parseIntField(chunkRadiusField, "Chunk radius");
        settings.seedsToCheck = parseLongField(seedsToCheckField, "Regions");
        settings.threads = parseIntField(threadsField, "Threads");

        settings.minBlocks = parseIntField(minBlocksField, "Min blocks");
        settings.minColumns = parseIntField(minColumnsField, "Min columns");
        settings.minYForMatch = parseIntField(minYField, "Min Y");

        settings.minWidth = parseIntField(minWidthField, "Min width");
        settings.minDepth = parseIntField(minDepthField, "Min depth");
        settings.minAvgThickness = parseDoubleField(minThicknessField, "Min thickness");

        settings.topResultsToKeep = parseIntField(topKeepField, "Top keep");
        settings.savePreviews = savePreviewsBox.isSelected();
        settings.hunterMode = hunterModeBox.isSelected();
        settings.huntProfile = settings.hunterMode
                ? huntProfileBox.getSelectedIndex()
                : SearchSettings.HUNT_PROFILE_GENERAL;
        settings.megaMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_MEGA;
        settings.recordHuntMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_RECORD_60K;
        settings.extremeRecordHuntMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_RECORD_80K;
        settings.filterResearchEnabled = overnightModeBox.isSelected();

        if (overnightModeBox.isSelected()) {
            settings.hunterMode = true;
            settings.topResultsToKeep = Math.max(100, settings.topResultsToKeep);

            settings.hunterStage0AuditEnabled = true;
            settings.hunterStage0AuditSampleMask = 4095L;
            settings.featureLoggingEnabled = true;
            settings.performanceProfilerEnabled = false;
            settings.debugLogInterval = 5_000_000L;
            settings.deterministicSeedMode = false;
            settings.runLabel = "overnight_gui_" + settings.seedsToCheck;
        } else {
            // Lean production hunt: keep the live General/Mega decisions, but skip
            // shadow research, forced audits, rich feature logging, and profiling.
            settings.hunterStage0AuditEnabled = false;
            settings.featureLoggingEnabled = false;
            settings.performanceProfilerEnabled = false;
        }

        // Research / long run can force Hunter mode on, so resolve the selected
        // hunt profile after that adjustment.
        settings.huntProfile = settings.hunterMode
                ? huntProfileBox.getSelectedIndex()
                : SearchSettings.HUNT_PROFILE_GENERAL;
        settings.megaMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_MEGA;
        settings.recordHuntMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_RECORD_60K;
        settings.extremeRecordHuntMode = settings.hunterMode
                && settings.huntProfile == SearchSettings.HUNT_PROFILE_RECORD_80K;

        if (settings.recordHuntMode) {
            settings.hunterCoarseMinCells = settings.record60CoarseMinCells;
            settings.hunterStage0AuditEnabled = true;
            settings.hunterStage0AuditSampleMask = 262143L; // ~1 / 262,144 rejects
            settings.debugLogInterval = 5_000_000L;
            // Record modes collect focused exact reject evidence. Disable the old
            // 34-byte/region shadow stream and broad feature CSV to keep long runs lean.
            settings.filterResearchEnabled = false;
            settings.featureLoggingEnabled = false;
        } else if (settings.extremeRecordHuntMode) {
            settings.hunterCoarseMinCells = settings.record80CoarseMinCells;
            settings.hunterStage0AuditEnabled = true;
            settings.hunterStage0AuditSampleMask = 262143L;
            settings.debugLogInterval = 5_000_000L;
            settings.filterResearchEnabled = false;
            settings.featureLoggingEnabled = false;
        }

        if (settings.chunkRadius < 1) {
            throw new IllegalArgumentException("Chunk radius must be at least 1.");
        }

        if (settings.threads < 1) {
            throw new IllegalArgumentException("Threads must be at least 1.");
        }

        if (settings.seedsToCheck < 1) {
            throw new IllegalArgumentException("Regions must be at least 1.");
        }

        if (settings.topResultsToKeep < 1) {
            throw new IllegalArgumentException("Top keep must be at least 1.");
        }

        return settings;
    }

    private int parseIntField(JTextField field, String name) {
        try {
            return Integer.parseInt(field.getText().trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + " must be a whole number.");
        }
    }

    private long parseLongField(JTextField field, String name) {
        try {
            return Long.parseLong(field.getText().trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + " must be a whole number.");
        }
    }

    private double parseDoubleField(JTextField field, String name) {
        try {
            return Double.parseDouble(field.getText().trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + " must be a number.");
        }
    }

    private void startRealSearch() {
        SearchSettings settings;

        try {
            settings = readSettingsFromGui();
        } catch (IllegalArgumentException e) {
            JOptionPane.showMessageDialog(frame, e.getMessage(), "Bad settings", JOptionPane.ERROR_MESSAGE);
            return;
        }

        liveRunStatistics.start();
        bestBlocks = 0;
        resultBySeed.clear();
        tableModel.setRowCount(0);
        hitGraphPanel.reset();

        checkedLabel.setText("0");
        worldsLabel.setText("0");
        runtimeLabel.setText("0s");
        lastChecked = 0L;
        speedLabel.setText("0 /s");
        matchesLabel.setText("0");
        topUpdatesLabel.setText("Top updates: 0");
        bestLabel.setText("-");

        if (livePreviewWorker != null) livePreviewWorker.cancel(true);
        live3DPreviewPanel.clear("Click a result to load the 3D preview");
        previewLabel.setIcon(null);
        previewLabel.setText("Click a result to show preview");
        switchLivePreviewMode(true);
        activePreviewRadius = settings.chunkRadius;
        selectedStatsArea.setText("No island selected.");

        copySeedButton.setEnabled(false);
        startButton.setEnabled(false);
        stopButton.setEnabled(true);
        setSettingsEnabled(false);

        saveGuiPreferences();
        statusLabel.setText("SEARCHING");
        statusLabel.setBackground(null);

        log("Starting real search.");
        log("Chunk radius: " + settings.chunkRadius);
        log("Region budget: " + settings.seedsToCheck);
        log("Threads: " + settings.threads);
        log("Min blocks: " + settings.minBlocks);
        String modeName;
        if (!settings.hunterMode) modeName = "Normal";
        else if (settings.extremeRecordHuntMode) modeName = "Hunter / WORLD RECORD coarse 700 (safer)";
        else if (settings.recordHuntMode) modeName = "Hunter / RECORD 60k+ (aggressive)";
        else if (settings.megaMode) modeName = "Hunter / MEGA 30k+";
        else modeName = "Hunter / General";
        log("Mode: " + modeName);
        if (settings.megaMode) {
            log("Mega filters: Upper>=8 | High>=6 | P19<5 + weak Y96/Y112 topology reject before coarse");
        } else if (settings.recordHuntMode) {
            log("RECORD 60k+ filters: P20>=3 | Upper>=19 | High>=12 | P19>=6.70/extreme | coarse>=95");
            log("Historical evidence: 32 unique observed 60k+ islands across 8 runs, 0 rejected. This is empirical, not a proof.");
        } else if (settings.extremeRecordHuntMode) {
            log("WORLD RECORD filters: P20>=3 | Upper>=19 | High>=20 | P19>=6.70/extreme | coarse>=700");
            log("Rebuilt after the 113,331 compact-thick record exposed the old High>=22 gate. Empirical risk remains.");
        }
        if (settings.filterResearchEnabled) {
            log("Research / long run: ON");
            log("Top keep per board: " + settings.topResultsToKeep);
            log("Sideboards: largest blocks | filled footprint | raw footprint");
            log("Research telemetry: features ON | Stage0 audit ~1/4096 | profiler OFF");
            log("Feature sampling: all coarse >=30 + ~1/256 lower-score rows");
            log("Previews: OFF");
        } else if (settings.recordHuntMode || settings.extremeRecordHuntMode) {
            log("Lean record evidence mode: focused exact reject audit ~1/262144 ON | detailed P20/Upper/High/P19 saved | broad shadow stream OFF");
        } else if (settings.hunterMode) {
            log("Lean production mode: shadow research OFF | forced audits OFF | feature CSV OFF | profiler OFF");
        }

        engine = new IslandSearchEngine();

        engine.start(settings, new SearchListener() {
            @Override
            public void onProgress(long checked, int matches, int topUpdates, double seedsPerSecond) {
                SwingUtilities.invokeLater(() -> {
                    lastChecked = checked;
                    checkedLabel.setText(UiFormat.compact(checked));
                    worldsLabel.setText(UiFormat.compact(checked / 8L));
                    speedLabel.setText(UiFormat.compact(seedsPerSecond) + "/s");
                    matchesLabel.setText(Integer.toString(matches));
                    topUpdatesLabel.setText("Top updates: " + topUpdates);
                    hitGraphPanel.setCurrentChecked(checked);
                    liveRunStatistics.progress(checked, matches, seedsPerSecond);
                });
            }

            @Override
            public void onHit(long checked, SearchResult result) {
                SwingUtilities.invokeLater(() -> {
                    hitGraphPanel.addHit(checked, result.blocks);
                    liveRunStatistics.hit(checked, result);
                    playHitSound(result.blocks);
                });
            }

            @Override
            public void onTopResult(SearchResult result, int rank) {
                SwingUtilities.invokeLater(() -> {
                    boolean wasNewBest = result.blocks > bestBlocks;

                    addTopResult(result, rank);
                    if (result.blocks > displayedAllTimeBestBlocks) {
                        displayedAllTimeBestBlocks = result.blocks;
                        allTimeBestLabel.setText(UiFormat.compact(result.blocks));
                    }
                    showPreview(result.previewPath);
                    loadLive3DPreview(result);
                    switchLivePreviewMode(true);
                    showSelectedStats(result);
                    dopamineFlash();
                    playTopResultSound(result, wasNewBest);
                });
            }

            @Override
            public void onLog(String message) {
                SwingUtilities.invokeLater(() -> log(message));
            }

            @Override
            public void onFinished() {
                SwingUtilities.invokeLater(() -> {
                    startButton.setEnabled(true);
                    stopButton.setEnabled(false);
                    setSettingsEnabled(true);
                    statusLabel.setText("STOPPING".equals(statusLabel.getText()) ? "STOPPED" : "FINISHED");
                    liveRunStatistics.finish();
                    refreshDataTabs();
                    refreshAllTimeBest();
                    refreshHeaderSummary();
                });
            }

            @Override
            public void onError(Throwable error) {
                SwingUtilities.invokeLater(() -> {
                    startButton.setEnabled(true);
                    stopButton.setEnabled(false);
                    setSettingsEnabled(true);
                    statusLabel.setText("ERROR");
                    liveRunStatistics.finish();
                    log("ERROR: " + error.getMessage());
                    error.printStackTrace();
                });
            }
        });
    }

    private void stopRealSearch() {
        if (engine == null || !engine.isRunning()) return;
        if (confirmStopBox.isSelected()) {
            int choice = JOptionPane.showConfirmDialog(
                    frame,
                    "Stop the active search cleanly after the current GPU batch?",
                    "Stop search",
                    JOptionPane.YES_NO_OPTION,
                    JOptionPane.WARNING_MESSAGE
            );
            if (choice != JOptionPane.YES_OPTION) return;
        }
        engine.stop();
        stopButton.setEnabled(false);
        saveGuiPreferences();
        statusLabel.setText("STOPPING");
        log("Stop requested.");
    }

    private void addTopResult(SearchResult result, int rank) {
        resultBySeed.put(result.seed, result);

        if (result.blocks > bestBlocks) {
            bestBlocks = result.blocks;
            bestLabel.setText(UiFormat.compact(bestBlocks));
        }

        int footprintArea = Math.max(1, result.width * result.depth);
        double fillPercent = result.columns * 100.0 / footprintArea;

        tableModel.addRow(new Object[] {
                rank,
                result.seed,
                result.blocks,
                result.columns,
                result.width + "x" + result.depth,
                Math.round(fillPercent * 100.0) / 100.0,
                result.minY + "-" + result.maxY,
                result.centerX + ", " + result.centerZ
        });

        while (tableModel.getRowCount() > 100) {
            int lowestBlocksRow = findLowestBlocksModelRow();

            if (lowestBlocksRow >= 0) {
                Long seed = (Long) tableModel.getValueAt(lowestBlocksRow, 1);
                resultBySeed.remove(seed);
                tableModel.removeRow(lowestBlocksRow);
            } else {
                break;
            }
        }

        selectSeed(result.seed);
        copySeedButton.setEnabled(true);
    }

    private int findLowestBlocksModelRow() {
        int lowestRow = -1;
        int lowestBlocks = Integer.MAX_VALUE;

        for (int row = 0; row < tableModel.getRowCount(); row++) {
            int blocks = (Integer) tableModel.getValueAt(row, 2);

            if (blocks < lowestBlocks) {
                lowestBlocks = blocks;
                lowestRow = row;
            }
        }

        return lowestRow;
    }

    private void selectSeed(long seed) {
        for (int modelRow = 0; modelRow < tableModel.getRowCount(); modelRow++) {
            Long rowSeed = (Long) tableModel.getValueAt(modelRow, 1);

            if (rowSeed == seed) {
                int viewRow = resultTable.convertRowIndexToView(modelRow);

                if (viewRow >= 0) {
                    resultTable.setRowSelectionInterval(viewRow, viewRow);
                    resultTable.scrollRectToVisible(resultTable.getCellRect(viewRow, 0, true));
                }

                return;
            }
        }
    }

    private void onResultRowSelected() {
        SearchResult result = getSelectedResult();

        if (result == null) {
            copySeedButton.setEnabled(false);
            selectedStatsArea.setText("No island selected.");
            return;
        }

        copySeedButton.setEnabled(true);
        showPreview(result.previewPath);
        loadLive3DPreview(result);
        switchLivePreviewMode(true);
        showSelectedStats(result);
    }

    private SearchResult getSelectedResult() {
        int selectedRow = resultTable.getSelectedRow();

        if (selectedRow < 0) {
            return null;
        }

        int modelRow = resultTable.convertRowIndexToModel(selectedRow);

        if (modelRow < 0) {
            return null;
        }

        Object seedObject = tableModel.getValueAt(modelRow, 1);

        if (!(seedObject instanceof Long)) {
            return null;
        }

        long seed = (Long) seedObject;

        return resultBySeed.get(seed);
    }

    private void copySelectedSeed() {
        SearchResult result = getSelectedResult();

        if (result == null) {
            log("No result selected.");
            return;
        }

        StringSelection selection = new StringSelection(String.valueOf(result.seed));
        Toolkit.getDefaultToolkit().getSystemClipboard().setContents(selection, null);

        log("Copied seed: " + result.seed);
    }

    private void openResultsFolder() {
        try {
            File folder = AppPaths.outputRoot().toFile();

            Path latestPointer = AppPaths.latestRunPointer();
            if (Files.isRegularFile(latestPointer)) {
                String latestPath = Files.readString(latestPointer).trim();
                if (!latestPath.isEmpty()) {
                    File latestFolder = new File(latestPath);
                    if (latestFolder.isDirectory()) {
                        folder = latestFolder;
                    }
                }
            }

            if (!folder.exists() && !folder.mkdirs()) {
                throw new IllegalStateException("Could not create " + folder);
            }

            Desktop.getDesktop().open(folder);
            log("Opened results folder: " + folder.getPath());
        } catch (Exception e) {
            log("Could not open results folder: " + e.getMessage());
            JOptionPane.showMessageDialog(frame, e.getMessage(), "Could not open folder", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void chooseOutputDirectory() {
        JFileChooser chooser = new JFileChooser(outputDirectoryField.getText().trim());
        chooser.setDialogTitle("Choose BetaSeedFinder output folder");
        chooser.setFileSelectionMode(JFileChooser.DIRECTORIES_ONLY);
        chooser.setAcceptAllFileFilterUsed(false);
        if (chooser.showOpenDialog(frame) == JFileChooser.APPROVE_OPTION) {
            outputDirectoryField.setText(chooser.getSelectedFile().toPath().toAbsolutePath().normalize().toString());
        }
    }

    private void switchLivePreviewMode(boolean use3D) {
        liveShowing3D = use3D;
        livePreviewCardLayout.show(livePreviewCards, use3D ? "3D" : "2D");
        if (live3DButton != null) {
            live3DButton.putClientProperty("accentButton", use3D);
            live3DButton.repaint();
        }
        if (live2DButton != null) {
            live2DButton.putClientProperty("accentButton", !use3D);
            live2DButton.repaint();
        }
    }

    private String live3DCacheKey(SearchResult result) {
        return result.seed + "|" + result.centerX + "|" + result.centerZ + "|"
                + result.blocks + "|" + activePreviewRadius;
    }

    private void loadLive3DPreview(SearchResult result) {
        if (result == null) return;
        if (livePreviewWorker != null) livePreviewWorker.cancel(true);

        String key = live3DCacheKey(result);
        Island3DData cached = Island3DPreviewPanel.cached(key);
        if (cached != null) {
            live3DPreviewPanel.setData(cached);
            return;
        }

        live3DPreviewPanel.clear("Reconstructing exact 3D surface...");
        livePreviewWorker = new SwingWorker<>() {
            @Override protected Island3DData doInBackground() throws Exception {
                return new IslandSearchEngine().generate3DPreviewForSearchResult(result, activePreviewRadius);
            }

            @Override protected void done() {
                if (isCancelled()) return;
                SearchResult current = getSelectedResult();
                if (current == null || current.seed != result.seed
                        || current.centerX != result.centerX || current.centerZ != result.centerZ) return;
                try {
                    Island3DData data = get();
                    Island3DPreviewPanel.cache(key, data);
                    live3DPreviewPanel.setData(data);
                } catch (Exception e) {
                    Throwable cause = e.getCause() == null ? e : e.getCause();
                    live3DPreviewPanel.clear("3D reconstruction failed: " + cause.getMessage());
                    if (result.previewPath != null && !result.previewPath.isEmpty() && liveShowing3D) {
                        switchLivePreviewMode(false);
                    }
                }
            }
        };
        livePreviewWorker.execute();
    }

    private void showPreview(String path) {
        if (path == null || path.isEmpty()) {
            previewLabel.setText("No preview saved");
            previewLabel.setIcon(null);
            return;
        }

        ImageIcon icon = new ImageIcon(path);

        if (icon.getIconWidth() <= 0) {
            previewLabel.setText("Preview not loaded");
            previewLabel.setIcon(null);
            return;
        }

        int maxW = previewLabel.getWidth() - 12;
        int maxH = previewLabel.getHeight() - 12;

        if (maxW <= 50) {
            maxW = 300;
        }

        if (maxH <= 50) {
            maxH = 145;
        }

        int imgW = icon.getIconWidth();
        int imgH = icon.getIconHeight();

        double scale = Math.min(maxW / (double) imgW, maxH / (double) imgH);

        int newW = Math.max(1, (int) (imgW * scale));
        int newH = Math.max(1, (int) (imgH * scale));

        Image scaled = icon.getImage().getScaledInstance(newW, newH, Image.SCALE_FAST);

        previewLabel.setText("");
        previewLabel.setIcon(new ImageIcon(scaled));
    }

    private void showSelectedStats(SearchResult result) {
        double avgThickness = result.blocks / (double) Math.max(1, result.columns);
        int footprintArea = result.width * result.depth;
        double fillPercent = result.columns * 100.0 / Math.max(1, footprintArea);

        String text =
                "Seed: " + result.seed + "\n" +
                "Blocks: " + result.blocks + "\n" +
                "Columns: " + result.columns + "\n" +
                "Footprint: " + result.width + "x" + result.depth + " = " + footprintArea + "\n" +
                "Fill: " + String.format("%.2f%%", fillPercent) + "\n" +
                "Avg thickness: " + String.format("%.2f", avgThickness) + "\n" +
                "Y range: " + result.minY + " to " + result.maxY + " = " + (result.maxY - result.minY) + "\n" +
                "Center X/Z: " + result.centerX + ", " + result.centerZ + "\n" +
                "Preview: " + result.previewPath;

        selectedStatsArea.setText(text);
        selectedStatsArea.setCaretPosition(0);
    }

    private void playHitSound(int blocks) {
        if (blocks >= MEGA_BLOCKS_THRESHOLD) {
            playMegaSound();
            return;
        }

        if (!soundPlayer.playFirstExisting(HIT_SOUND_PATHS)) {
            logMissingSoundOnce("hit", HIT_SOUND_PATHS);
        }
    }

    private void playTopResultSound(SearchResult result, boolean wasNewBest) {
        if (result.blocks >= MEGA_BLOCKS_THRESHOLD) {
            playMegaSound();
            return;
        }

        if (wasNewBest && !soundPlayer.playProtectedFirstExisting(NEW_BEST_SOUND_PATHS)) {
            logMissingSoundOnce("new_best", NEW_BEST_SOUND_PATHS);
        }
    }

    private void playMegaSound() {
        if (!soundPlayer.playProtectedFirstExisting(MEGA_SOUND_PATHS)) {
            logMissingSoundOnce("mega", MEGA_SOUND_PATHS);
        }
    }

    private void logMissingSoundOnce(String type, String[] paths) {
        if ("hit".equals(type)) {
            if (missingHitSoundLogged) return;
            missingHitSoundLogged = true;
        } else if ("new_best".equals(type)) {
            if (missingNewBestSoundLogged) return;
            missingNewBestSoundLogged = true;
        } else if ("mega".equals(type)) {
            if (missingMegaSoundLogged) return;
            missingMegaSoundLogged = true;
        }

        log("Sound missing for " + type + ". Looked for: " + String.join(", ", paths));
    }

private void dopamineFlash() {
    statusLabel.setText("NEW TOP RESULT");
    statusLabel.setForeground(Color.WHITE);
    statusLabel.setBackground(new Color(80, 130, 80));
    statusLabel.setOpaque(true);

    Timer timer = new Timer(500, e -> {
        statusLabel.setText("SEARCHING");
        statusLabel.setForeground(MinecraftTheme.TEXT);
        statusLabel.setBackground(MinecraftTheme.STONE_DARK);
    });

    timer.setRepeats(false);
    timer.start();
}

    private void setSettingsEnabled(boolean enabled) {
        chunkRadiusField.setEnabled(enabled);
        seedsToCheckField.setEnabled(enabled);
        threadsField.setEnabled(enabled);
        minBlocksField.setEnabled(enabled);
        minColumnsField.setEnabled(enabled);
        minYField.setEnabled(enabled);
        minWidthField.setEnabled(enabled);
        minDepthField.setEnabled(enabled);
        minThicknessField.setEnabled(enabled);
        topKeepField.setEnabled(enabled);
        savePreviewsBox.setEnabled(enabled);
        hunterModeBox.setEnabled(enabled);
        huntProfileBox.setEnabled(enabled);
        overnightModeBox.setEnabled(enabled);
        huntProfileBox.setEnabled(enabled);
    }

    private void updateProfileDescription() {
        if (searchProfileDescriptionLabel == null || huntProfileBox == null) return;
        String text = switch (huntProfileBox.getSelectedIndex()) {
            case SearchSettings.HUNT_PROFILE_GENERAL -> "Broad search. Highest recall, lowest speed.";
            case SearchSettings.HUNT_PROFILE_MEGA -> "Maximum-recall large-island hunt. Good for discovering varied 30k+ shapes.";
            case SearchSettings.HUNT_PROFILE_RECORD_60K -> "Recommended. Validated against the known 60k+ tail and optimized for record-sized islands.";
            case SearchSettings.HUNT_PROFILE_RECORD_80K -> "Experimental. Faster and highly selective; may miss an unusual record shape.";
            default -> "Select a search profile.";
        };
        searchProfileDescriptionLabel.setText(text);
    }

    private void updateSearchSummary() {
        updateProfileDescription();
        if (searchBudgetLabel != null && seedsToCheckField != null) {
            try {
                long regions = Long.parseLong(seedsToCheckField.getText().trim());
                searchBudgetLabel.setText("Budget: " + UiFormat.compact(regions) + " regions");
            } catch (NumberFormatException e) {
                searchBudgetLabel.setText("Budget: invalid");
            }
        }
    }

    private void refreshLiveClock() {
        double elapsed = liveRunStatistics.elapsedSeconds();
        if (runtimeLabel != null && liveRunStatistics.active()) {
            runtimeLabel.setText(UiFormat.duration(elapsed));
            worldsLabel.setText(UiFormat.compact(lastChecked / 8L));
        }
        if (headerRuntimeLabel != null) {
            headerRuntimeLabel.setText(UiFormat.duration(elapsed));
        }
    }

    private void refreshHeaderSummary() {
        if (headerIslandCountLabel == null) return;
        try {
            headerIslandCountLabel.setText(String.format("%,d", runRepository.allIslands(true).size()));
        } catch (Exception e) {
            headerIslandCountLabel.setText("-");
        }
    }

    private void refreshAllTimeBest() {
        if (allTimeBestLabel == null) return;
        IslandRecord best = null;
        for (RunRecord run : runRepository.includedRuns()) {
            for (IslandRecord island : run.islands(runRepository)) {
                if (best == null || island.blocks > best.blocks) best = island;
            }
        }
        allTimeBestIsland = best;
        displayedAllTimeBestBlocks = best == null ? 0 : best.blocks;
        allTimeBestLabel.setText(best == null ? "-" : UiFormat.compact(best.blocks));
        allTimeBestLabel.setToolTipText(best == null ? null : "Seed " + best.seed + " | " + best.run.dateText());
    }

    private void restoreDefaultPreferences() {
        huntProfileBox.setSelectedIndex(SearchSettings.HUNT_PROFILE_RECORD_60K);
        seedsToCheckField.setText("30000000000");
        threadsField.setText(String.valueOf(Math.max(1, Math.min(10, Runtime.getRuntime().availableProcessors() - 1))));
        chunkRadiusField.setText("7");
        minBlocksField.setText("20000");
        minColumnsField.setText("100");
        minYField.setText("60");
        minWidthField.setText("10");
        minDepthField.setText("10");
        minThicknessField.setText("2.0");
        topKeepField.setText("100");
        savePreviewsBox.setSelected(true);
        hunterModeBox.setSelected(true);
        overnightModeBox.setSelected(false);
        confirmStopBox.setSelected(true);
        showAdvancedLiveBox.setSelected(false);
        updateSearchSummary();
    }

    private void loadGuiPreferences() {
        if (!Files.isRegularFile(GUI_CONFIG_PATH)) return;
        Properties properties = new Properties();
        try (var input = Files.newInputStream(GUI_CONFIG_PATH)) {
            properties.load(input);
            huntProfileBox.setSelectedIndex(parsePropertyInt(properties, "profile", huntProfileBox.getSelectedIndex(), 0, 3));
            seedsToCheckField.setText(properties.getProperty("regionBudget", seedsToCheckField.getText()));
            threadsField.setText(properties.getProperty("threads", threadsField.getText()));
            chunkRadiusField.setText(properties.getProperty("chunkRadius", chunkRadiusField.getText()));
            minBlocksField.setText(properties.getProperty("minBlocks", minBlocksField.getText()));
            minColumnsField.setText(properties.getProperty("minColumns", minColumnsField.getText()));
            minYField.setText(properties.getProperty("minY", minYField.getText()));
            minWidthField.setText(properties.getProperty("minWidth", minWidthField.getText()));
            minDepthField.setText(properties.getProperty("minDepth", minDepthField.getText()));
            minThicknessField.setText(properties.getProperty("minThickness", minThicknessField.getText()));
            topKeepField.setText(properties.getProperty("topKeep", topKeepField.getText()));
            savePreviewsBox.setSelected(Boolean.parseBoolean(properties.getProperty("savePreviews", "true")));
            hunterModeBox.setSelected(Boolean.parseBoolean(properties.getProperty("hunterMode", "true")));
            overnightModeBox.setSelected(Boolean.parseBoolean(properties.getProperty("researchLogging", "false")));
            confirmStopBox.setSelected(Boolean.parseBoolean(properties.getProperty("confirmStop", "true")));
            showAdvancedLiveBox.setSelected(Boolean.parseBoolean(properties.getProperty("advancedLive", "false")));
            outputDirectoryField.setText(properties.getProperty("outputDirectory", AppPaths.outputRoot().toString()));
        } catch (Exception e) {
            System.err.println("Could not load GUI settings: " + e.getMessage());
        }
    }

    private int parsePropertyInt(Properties properties, String key, int fallback, int min, int max) {
        try {
            int value = Integer.parseInt(properties.getProperty(key, Integer.toString(fallback)));
            return Math.max(min, Math.min(max, value));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    private void saveGuiPreferences() {
        if (huntProfileBox == null) return;
        Properties properties = new Properties();
        properties.setProperty("profile", Integer.toString(huntProfileBox.getSelectedIndex()));
        properties.setProperty("regionBudget", seedsToCheckField.getText().trim());
        properties.setProperty("threads", threadsField.getText().trim());
        properties.setProperty("chunkRadius", chunkRadiusField.getText().trim());
        properties.setProperty("minBlocks", minBlocksField.getText().trim());
        properties.setProperty("minColumns", minColumnsField.getText().trim());
        properties.setProperty("minY", minYField.getText().trim());
        properties.setProperty("minWidth", minWidthField.getText().trim());
        properties.setProperty("minDepth", minDepthField.getText().trim());
        properties.setProperty("minThickness", minThicknessField.getText().trim());
        properties.setProperty("topKeep", topKeepField.getText().trim());
        properties.setProperty("savePreviews", Boolean.toString(savePreviewsBox.isSelected()));
        properties.setProperty("hunterMode", Boolean.toString(hunterModeBox.isSelected()));
        properties.setProperty("researchLogging", Boolean.toString(overnightModeBox.isSelected()));
        properties.setProperty("confirmStop", Boolean.toString(confirmStopBox.isSelected()));
        properties.setProperty("advancedLive", Boolean.toString(showAdvancedLiveBox.isSelected()));
        String outputPreference;
        try {
            String rawOutput = outputDirectoryField.getText().trim();
            outputPreference = rawOutput.isEmpty()
                    ? AppPaths.outputRoot().toString()
                    : AppPaths.normalizeOutputPath(Path.of(rawOutput)).toString();
        } catch (Exception ignored) {
            outputPreference = AppPaths.outputRoot().toString();
        }
        properties.setProperty("outputDirectory", outputPreference);
        properties.setProperty("setupCompleted", "true");
        try {
            AppPaths.storeGuiProperties(properties);
        } catch (Exception e) {
            System.err.println("Could not save GUI settings: " + e.getMessage());
        }
    }

    private void openDiagnosticsDialog() {
        RunRecord latest = runRepository.latestRun();
        String gpu = latest == null ? "No completed run detected" : latest.manifest.getOrDefault("gpuName", latest.manifest.getOrDefault("gpu", "Unknown"));
        Path output = AppPaths.outputRoot();
        long free = output.toFile().getUsableSpace();
        String text =
                "Application: " + APP_VERSION + "\n" +
                "Java: " + System.getProperty("java.version") + "\n" +
                "Operating system: " + System.getProperty("os.name") + " " + System.getProperty("os.version") + "\n" +
                "CPU workers available: " + Runtime.getRuntime().availableProcessors() + "\n" +
                "GPU from latest run: " + gpu + "\n" +
                "Application directory: " + AppPaths.appRoot() + "\n" +
                "Output directory: " + output + "\n" +
                "Free disk space: " + UiFormat.compact(free) + " bytes\n" +
                "Latest run: " + (latest == null ? "None" : latest.dateText() + " | " + latest.friendlyStatus()) + "\n";

        JTextArea area = new JTextArea(text, 12, 72);
        area.setEditable(false);
        area.setCaretPosition(0);
        JButton copy = new JButton("Copy diagnostic report");
        copy.addActionListener(e -> Toolkit.getDefaultToolkit().getSystemClipboard().setContents(new StringSelection(text), null));
        JPanel panel = new JPanel(new BorderLayout(6, 6));
        panel.add(new JScrollPane(area), BorderLayout.CENTER);
        panel.add(copy, BorderLayout.SOUTH);
        JOptionPane.showMessageDialog(frame, panel, "Diagnostics", JOptionPane.INFORMATION_MESSAGE);
    }

    private void requestWindowClose() {
        IslandSearchEngine activeEngine = engine;
        if (activeEngine == null || !activeEngine.isRunning()) {
            saveGuiPreferences();
            frame.dispose();
            System.exit(0);
            return;
        }

        int choice = JOptionPane.showConfirmDialog(
                frame,
                "A search is still running. Stop it cleanly and save the run before closing?",
                "Stop and close",
                JOptionPane.YES_NO_OPTION,
                JOptionPane.WARNING_MESSAGE
        );
        if (choice != JOptionPane.YES_OPTION) return;

        saveGuiPreferences();
        statusLabel.setText("STOPPING");
        stopButton.setEnabled(false);
        startButton.setEnabled(false);
        frame.setEnabled(false);
        log("Closing safely: stopping backend and flushing run files...");
        activeEngine.stop();

        Thread closer = new Thread(() -> {
            boolean stopped = false;
            try {
                stopped = activeEngine.awaitStopped(60_000L);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            final boolean clean = stopped;
            SwingUtilities.invokeLater(() -> {
                if (!clean) {
                    frame.setEnabled(true);
                    int force = JOptionPane.showConfirmDialog(
                            frame,
                            "The backend did not stop within 60 seconds. Force close now? The latest batch may not be finalized.",
                            "Backend still stopping",
                            JOptionPane.YES_NO_OPTION,
                            JOptionPane.ERROR_MESSAGE
                    );
                    if (force != JOptionPane.YES_OPTION) return;
                }
                frame.dispose();
                System.exit(0);
            });
        }, "safe-gui-close");
        closer.setDaemon(true);
        closer.start();
    }

    private void log(String message) {
        String time = LocalTime.now().format(DateTimeFormatter.ofPattern("HH:mm:ss"));
        logArea.append("[" + time + "] " + message + "\n");
        int length = logArea.getDocument().getLength();
        if (length > MAX_VISIBLE_LOG_CHARS) {
            int remove = length - MAX_VISIBLE_LOG_CHARS;
            try {
                String prefix = logArea.getDocument().getText(0, Math.min(remove + 2048, length));
                int newline = prefix.indexOf('\n', remove);
                logArea.getDocument().remove(0, newline >= 0 ? newline + 1 : remove);
            } catch (javax.swing.text.BadLocationException ignored) {
                // Visible log trimming must never affect the search.
            }
        }
        logArea.setCaretPosition(logArea.getDocument().getLength());
    }
}