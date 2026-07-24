import javax.swing.*;
import java.awt.*;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public class StatisticsTab extends JPanel {
    public enum Scope { CURRENT, ALL_TIME, COMPARE }

    private final RunRepository repository;
    private final LiveRunStatistics liveRun;
    private final StatisticsCalculator calculator = new StatisticsCalculator();
    private final StatisticsChartPanel chartPanel = new StatisticsChartPanel();

    private final JButton currentButton = new JButton("Current Run");
    private final JButton allTimeButton = new JButton("All Time");
    private final JButton compareButton = new JButton("Compare");
    private final JComboBox<String> graphCombo = new JComboBox<>();
    private final JComboBox<StatisticsCalculator.DistributionMetric> metricCombo =
            new JComboBox<>(StatisticsCalculator.DistributionMetric.values());
    private final JComboBox<StatisticsCalculator.DistributionMode> distributionModeCombo =
            new JComboBox<>(StatisticsCalculator.DistributionMode.values());
    private final JComboBox<String> tailThresholdCombo = new JComboBox<>();

    private final JLabel[] statValues = new JLabel[6];
    private final JLabel[] statNames = new JLabel[6];
    private final JLabel[] metricValues = new JLabel[6];
    private final JLabel[] metricNames = new JLabel[6];
    private final JLabel[] oddsValues = new JLabel[6];
    private final JLabel[] oddsNames = new JLabel[6];
    private final JLabel scopeInfoLabel = new JLabel();
    private final JLabel analysisInfoLabel = new JLabel();
    private final JLabel modelDetailsLabel = new JLabel();

    private final JTextField oddsTargetField = new JTextField(8);
    private final JTextField futureSeedsField = new JTextField("100M", 8);
    private final JButton calculateOddsButton = new JButton("Calculate");
    private final JPanel analysisPanel;

    private Scope scope = Scope.CURRENT;
    private RunRecord selectedRun;

    public StatisticsTab(RunRepository repository, LiveRunStatistics liveRun) {
        super(new BorderLayout(8, 8));
        this.repository = repository;
        this.liveRun = liveRun;
        setOpaque(true);
        setBackground(MinecraftTheme.BG);
        setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));

        add(createToolbar(), BorderLayout.NORTH);

        JPanel content = new JPanel(new BorderLayout(8, 8));
        content.setOpaque(false);
        content.add(createSummaryPanel(), BorderLayout.NORTH);
        content.add(chartPanel, BorderLayout.CENTER);
        analysisPanel = createAnalysisPanel();
        content.add(analysisPanel, BorderLayout.SOUTH);
        add(content, BorderLayout.CENTER);

        currentButton.addActionListener(e -> setScope(Scope.CURRENT));
        allTimeButton.addActionListener(e -> setScope(Scope.ALL_TIME));
        compareButton.addActionListener(e -> setScope(Scope.COMPARE));
        graphCombo.addActionListener(e -> refreshView());
        distributionModeCombo.addActionListener(e -> refreshView());
        metricCombo.addActionListener(e -> {
            StatisticsCalculator.DistributionMetric metric = selectedMetric();
            oddsTargetField.setText(Integer.toString(metric.defaultTarget));
            rebuildTailThresholdChoices();
            refreshView();
        });
        tailThresholdCombo.addActionListener(e -> refreshView());
        calculateOddsButton.addActionListener(e -> refreshView());
        oddsTargetField.addActionListener(e -> refreshView());
        futureSeedsField.addActionListener(e -> refreshView());

        oddsTargetField.setText(Integer.toString(selectedMetric().defaultTarget));
        rebuildTailThresholdChoices();

        Timer liveRefreshTimer = new Timer(1500, e -> {
            if (isShowing() && liveRun.active()) refreshView();
        });
        liveRefreshTimer.start();

        setScope(Scope.CURRENT);
    }

    private JPanel createToolbar() {
        JPanel outer = new TexturePanel(new BorderLayout(8, 8), "assets/textures/stone.png", Color.BLACK, 120);
        outer.setBorder(MinecraftTheme.boxBorder());

        JPanel scopeButtons = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 0));
        scopeButtons.setOpaque(false);
        scopeButtons.add(currentButton);
        scopeButtons.add(allTimeButton);
        scopeButtons.add(compareButton);

        JPanel controls = new JPanel(new FlowLayout(FlowLayout.RIGHT, 8, 0));
        controls.setOpaque(false);
        controls.add(new JLabel("Graph"));
        controls.add(graphCombo);
        controls.add(new JLabel("Metric"));
        controls.add(metricCombo);
        controls.add(distributionModeCombo);

        outer.add(scopeButtons, BorderLayout.WEST);
        outer.add(controls, BorderLayout.EAST);
        return outer;
    }

    private JPanel createSummaryPanel() {
        JPanel wrapper = new JPanel(new BorderLayout(8, 5));
        wrapper.setOpaque(false);

        scopeInfoLabel.setForeground(MinecraftTheme.TEXT_DIM);
        scopeInfoLabel.setBorder(BorderFactory.createEmptyBorder(0, 4, 0, 0));
        wrapper.add(scopeInfoLabel, BorderLayout.NORTH);

        JPanel stats = new TexturePanel(new GridLayout(1, 6, 6, 0), "assets/textures/stone.png", Color.BLACK, 160);
        stats.setBorder(MinecraftTheme.boxBorder());

        for (int i = 0; i < statValues.length; i++) {
            JPanel cell = new JPanel(new BorderLayout(2, 2));
            cell.setName("settingCell");
            JLabel value = new JLabel("0", SwingConstants.CENTER);
            JLabel name = new JLabel("Stat", SwingConstants.CENTER);
            value.setFont(MinecraftTheme.HEADER_FONT);
            value.setForeground(MinecraftTheme.TEXT);
            name.setFont(MinecraftTheme.SMALL_FONT);
            name.setForeground(MinecraftTheme.TEXT_DIM);
            statValues[i] = value;
            statNames[i] = name;
            cell.add(value, BorderLayout.CENTER);
            cell.add(name, BorderLayout.SOUTH);
            stats.add(cell);
        }

        wrapper.add(stats, BorderLayout.CENTER);
        return wrapper;
    }

    private JPanel createAnalysisPanel() {
        JPanel wrapper = new JPanel(new BorderLayout(8, 6));
        wrapper.setOpaque(false);

        JPanel metricStats = new TexturePanel(new GridLayout(1, 6, 6, 0), "assets/textures/stone.png", Color.BLACK, 170);
        metricStats.setBorder(MinecraftTheme.titled("Selected metric"));
        String[] names = {"Average", "Std dev", "Median", "P90", "P99", "Maximum"};
        for (int i = 0; i < metricValues.length; i++) {
            JPanel cell = new JPanel(new BorderLayout(2, 2));
            cell.setName("settingCell");
            JLabel value = new JLabel("-", SwingConstants.CENTER);
            JLabel name = new JLabel(names[i], SwingConstants.CENTER);
            value.setFont(MinecraftTheme.UI_FONT);
            value.setForeground(MinecraftTheme.TEXT);
            name.setFont(MinecraftTheme.SMALL_FONT);
            name.setForeground(MinecraftTheme.TEXT_DIM);
            metricValues[i] = value;
            metricNames[i] = name;
            cell.add(value, BorderLayout.CENTER);
            cell.add(name, BorderLayout.SOUTH);
            metricStats.add(cell);
        }

        JPanel oddsPanel = new TexturePanel(new BorderLayout(6, 4), "assets/textures/stone.png", Color.BLACK, 175);
        oddsPanel.setBorder(MinecraftTheme.titled("Rarity estimator"));

        JPanel controls = new JPanel(new FlowLayout(FlowLayout.LEFT, 7, 0));
        controls.setOpaque(false);
        controls.add(new JLabel("Target"));
        controls.add(oddsTargetField);
        controls.add(new JLabel("Future seeds"));
        controls.add(futureSeedsField);
        controls.add(new JLabel("Tail fit"));
        controls.add(tailThresholdCombo);
        controls.add(calculateOddsButton);
        analysisInfoLabel.setFont(MinecraftTheme.SMALL_FONT);
        analysisInfoLabel.setForeground(MinecraftTheme.TEXT_DIM);
        controls.add(analysisInfoLabel);
        oddsPanel.add(controls, BorderLayout.NORTH);

        JPanel oddsResults = new JPanel(new GridLayout(1, 6, 6, 0));
        oddsResults.setOpaque(false);
        String[] oddsLabels = {"Observed", "Observed odds", "Tail estimate", "Model range", "Future chance", "Confidence"};
        for (int i = 0; i < oddsValues.length; i++) {
            JPanel cell = new JPanel(new BorderLayout(2, 1));
            cell.setName("settingCell");
            JLabel value = new JLabel("-", SwingConstants.CENTER);
            JLabel name = new JLabel(oddsLabels[i], SwingConstants.CENTER);
            value.setFont(MinecraftTheme.SMALL_FONT);
            value.setForeground(MinecraftTheme.TEXT);
            name.setFont(MinecraftTheme.SMALL_FONT);
            name.setForeground(MinecraftTheme.TEXT_DIM);
            oddsValues[i] = value;
            oddsNames[i] = name;
            cell.add(value, BorderLayout.CENTER);
            cell.add(name, BorderLayout.SOUTH);
            oddsResults.add(cell);
        }
        oddsPanel.add(oddsResults, BorderLayout.CENTER);

        modelDetailsLabel.setFont(MinecraftTheme.SMALL_FONT);
        modelDetailsLabel.setForeground(MinecraftTheme.TEXT_DIM);
        modelDetailsLabel.setBorder(BorderFactory.createEmptyBorder(2, 5, 1, 5));
        oddsPanel.add(modelDetailsLabel, BorderLayout.SOUTH);

        wrapper.add(metricStats, BorderLayout.NORTH);
        wrapper.add(oddsPanel, BorderLayout.CENTER);
        return wrapper;
    }

    public void setSelectedRun(RunRecord run) {
        this.selectedRun = run;
        setScope(Scope.CURRENT);
    }

    public void refreshData() {
        repository.refresh();
        if (selectedRun == null || !repository.runs().contains(selectedRun)) {
            selectedRun = repository.latestRun();
        }
        refreshView();
    }

    private void setScope(Scope scope) {
        this.scope = scope;
        updateScopeButtonState();
        rebuildGraphChoices();
        refreshView();
    }

    private void updateScopeButtonState() {
        currentButton.setEnabled(scope != Scope.CURRENT);
        allTimeButton.setEnabled(scope != Scope.ALL_TIME);
        compareButton.setEnabled(scope != Scope.COMPARE);
    }

    private void rebuildGraphChoices() {
        Object old = graphCombo.getSelectedItem();
        graphCombo.removeAllItems();
        graphCombo.addItem("Distribution");
        if (scope == Scope.CURRENT) {
            graphCombo.addItem("Tail Rarity");
            graphCombo.addItem("Best Progression");
        } else if (scope == Scope.ALL_TIME) {
            graphCombo.addItem("Tail Rarity");
            graphCombo.addItem("World Record Progression");
            graphCombo.addItem("Average Speed by Run");
        }
        if (old != null) graphCombo.setSelectedItem(old);
        if (graphCombo.getSelectedIndex() < 0) graphCombo.setSelectedIndex(0);
    }

    private void refreshView() {
        if (!isDisplayable() && getParent() == null) return;

        RunRecord current = selectedRun != null ? selectedRun : repository.latestRun();
        List<RunRecord> allRuns = repository.includedRuns();
        String graph = (String) graphCombo.getSelectedItem();
        if (graph == null) return;

        boolean distribution = graph.equals("Distribution");
        boolean rarity = graph.equals("Tail Rarity");
        boolean metricGraph = distribution || rarity;
        metricCombo.setVisible(metricGraph);
        distributionModeCombo.setVisible(distribution);
        analysisPanel.setVisible(metricGraph);

        StatisticsCalculator.DistributionMetric metric = selectedMetric();

        if (scope == Scope.CURRENT && liveRun.active()) {
            StatisticsCalculator.Summary summary = new StatisticsCalculator.Summary();
            summary.seeds = liveRun.checked();
            summary.runtimeSeconds = liveRun.elapsedSeconds();
            summary.islands = liveRun.matches();
            summary.best = liveRun.best();
            for (int blocks : liveRun.blockValues()) {
                if (blocks >= 30_000) summary.count30k++;
                if (blocks >= 40_000) summary.count40k++;
                if (blocks >= 50_000) summary.count50k++;
            }
            updateSummary(summary, true);
            scopeInfoLabel.setText("Live search | " + UiFormat.compact(liveRun.checked()) + " checked | " + UiFormat.compact(liveRun.speed()) + "/s");
            if (graph.equals("Best Progression")) {
                chartPanel.showLine("Live best result progression", "Blocks", liveRun.bestProgression());
            } else if (graph.equals("Tail Rarity")) {
                List<Integer> values = liveRun.values(metric);
                double target = parsedTargetOrDefault();
                chartPanel.showRarityCurve(
                        "Live " + metric.chartLabel + " tail rarity",
                        calculator.rarityCurve(values, liveRun.checked(), metric, target, selectedTailThreshold()),
                        target
                );
                updateAnalysis(values, liveRun.checked(), null, 0L);
            } else {
                List<Integer> values = liveRun.values(metric);
                Map<String, Double> plotted = calculator.distributionValues(values, metric, selectedDistributionMode(), liveRun.checked());
                chartPanel.showBars("Live " + metric.chartLabel + " distribution", distributionYLabel(), plotted);
                updateAnalysis(values, liveRun.checked(), null, 0L);
            }
        } else if (scope == Scope.CURRENT) {
            StatisticsCalculator.Summary summary = calculator.summarizeRun(current, repository);
            updateSummary(summary, true);
            scopeInfoLabel.setText(current == null ? "No run selected" : current.dateText() + " | " + current.shortBuild() + " | " + current.status);
            if (graph.equals("Best Progression")) {
                chartPanel.showLine("Best result progression", "Blocks", calculator.bestProgression(current, repository));
            } else if (graph.equals("Tail Rarity")) {
                List<IslandRecord> islands = current == null ? List.of() : current.islands(repository);
                List<Integer> values = calculator.metricValues(islands, metric);
                long seeds = current == null ? 0L : current.checked;
                double target = parsedTargetOrDefault();
                chartPanel.showRarityCurve(
                        "Current run " + metric.chartLabel + " tail rarity",
                        calculator.rarityCurve(values, seeds, metric, target, selectedTailThreshold()),
                        target
                );
                updateAnalysis(values, seeds, null, 0L);
            } else {
                List<IslandRecord> islands = current == null ? List.of() : current.islands(repository);
                List<Integer> values = calculator.metricValues(islands, metric);
                Map<String, Double> plotted = calculator.distributionValues(values, metric, selectedDistributionMode(), current == null ? 0L : current.checked);
                chartPanel.showBars("Current run " + metric.chartLabel + " distribution", distributionYLabel(), plotted);
                updateAnalysis(values, current == null ? 0L : current.checked, null, 0L);
            }
        } else if (scope == Scope.ALL_TIME) {
            StatisticsCalculator.Summary summary = calculator.summarizeRuns(allRuns, repository);
            updateSummary(summary, false);
            scopeInfoLabel.setText(allRuns.size() + " included hunt runs | benchmarks and short tests excluded by default");
            if (graph.equals("World Record Progression")) {
                chartPanel.showLine("All-time world record progression", "Blocks", calculator.allTimeRecordProgression(allRuns, repository));
            } else if (graph.equals("Average Speed by Run")) {
                chartPanel.showLine("Average search throughput by run", "Search windows/s", calculator.speedByRun(allRuns));
            } else if (graph.equals("Tail Rarity")) {
                List<Integer> values = calculator.metricValues(collectIslands(allRuns), metric);
                double target = parsedTargetOrDefault();
                chartPanel.showRarityCurve(
                        "All-time " + metric.chartLabel + " tail rarity",
                        calculator.rarityCurve(values, summary.seeds, metric, target, selectedTailThreshold()),
                        target
                );
                updateAnalysis(values, summary.seeds, null, 0L);
            } else {
                List<Integer> values = calculator.metricValues(collectIslands(allRuns), metric);
                Map<String, Double> plotted = calculator.distributionValues(values, metric, selectedDistributionMode(), summary.seeds);
                chartPanel.showBars("All-time " + metric.chartLabel + " distribution", distributionYLabel(), plotted);
                updateAnalysis(values, summary.seeds, null, 0L);
            }
        } else {
            StatisticsCalculator.Summary currentSummary = calculator.summarizeRun(current, repository);
            StatisticsCalculator.Summary allSummary = calculator.summarizeRuns(allRuns, repository);
            updateSummary(currentSummary, true);
            scopeInfoLabel.setText("Current run vs all included historical hunts | analysis values show Current / All time");

            List<Integer> currentValues = calculator.metricValues(current == null ? List.of() : current.islands(repository), metric);
            List<Integer> allValues = calculator.metricValues(collectIslands(allRuns), metric);
            Map<String, Double> a = calculator.distributionValues(currentValues, metric, selectedDistributionMode(), current == null ? 0L : current.checked);
            Map<String, Double> b = calculator.distributionValues(allValues, metric, selectedDistributionMode(), allSummary.seeds);
            chartPanel.showBars("Current run vs all time | " + metric.chartLabel, distributionYLabel(), a, b, "Current", "All time");
            updateAnalysis(currentValues, current == null ? 0L : current.checked, allValues, allSummary.seeds);
        }

        revalidate();
        repaint();
    }

    private void updateAnalysis(List<Integer> values, long searchedSeeds, List<Integer> compareValues, long compareSeeds) {
        StatisticsCalculator.MetricSummary a = calculator.summarizeMetric(values);
        StatisticsCalculator.MetricSummary b = compareValues == null ? null : calculator.summarizeMetric(compareValues);

        double[] aValues = {a.mean, a.stdDev, a.median, a.p90, a.p99, a.max};
        double[] bValues = b == null ? null : new double[] {b.mean, b.stdDev, b.median, b.p90, b.p99, b.max};
        for (int i = 0; i < metricValues.length; i++) {
            metricValues[i].setText(bValues == null
                    ? formatMetric(aValues[i])
                    : formatMetric(aValues[i]) + " / " + formatMetric(bValues[i]));
        }

        double target;
        long futureSeeds;
        try {
            target = parseHumanNumber(oddsTargetField.getText());
            futureSeeds = Math.max(0L, Math.round(parseHumanNumber(futureSeedsField.getText())));
            oddsTargetField.setBackground(MinecraftTheme.PANEL_INNER);
            futureSeedsField.setBackground(MinecraftTheme.PANEL_INNER);
        } catch (IllegalArgumentException ex) {
            for (JLabel label : oddsValues) label.setText("-");
            analysisInfoLabel.setText("Invalid target or future-seed value");
            modelDetailsLabel.setText("");
            return;
        }

        StatisticsCalculator.OddsResult observedA = calculator.calculateOdds(values, searchedSeeds, target, futureSeeds);
        StatisticsCalculator.TheoreticalOddsResult theoryA = calculator.calculateTheoreticalOdds(
                values,
                searchedSeeds,
                selectedMetric(),
                target,
                futureSeeds,
                selectedTailThreshold()
        );

        StatisticsCalculator.OddsResult observedB = compareValues == null
                ? null
                : calculator.calculateOdds(compareValues, compareSeeds, target, futureSeeds);
        StatisticsCalculator.TheoreticalOddsResult theoryB = compareValues == null
                ? null
                : calculator.calculateTheoreticalOdds(
                        compareValues,
                        compareSeeds,
                        selectedMetric(),
                        target,
                        futureSeeds,
                        selectedTailThreshold()
                );

        updateOddsLabels(observedA, theoryA, observedB, theoryB);
        updateTailInfo(theoryA, theoryB);
    }

    private void updateOddsLabels(
            StatisticsCalculator.OddsResult observedA,
            StatisticsCalculator.TheoreticalOddsResult theoryA,
            StatisticsCalculator.OddsResult observedB,
            StatisticsCalculator.TheoreticalOddsResult theoryB
    ) {
        String[] first = formatRarity(observedA, theoryA);
        String[] second = observedB == null ? null : formatRarity(observedB, theoryB);
        for (int i = 0; i < oddsValues.length; i++) {
            oddsValues[i].setText(second == null ? first[i] : first[i] + " / " + second[i]);
        }
    }

    private String[] formatRarity(
            StatisticsCalculator.OddsResult observed,
            StatisticsCalculator.TheoreticalOddsResult theory
    ) {
        String observedCount = observed.exceedances + " / " + observed.observations;
        String observedOdds = Double.isFinite(observed.oneInSeeds)
                ? "1 in " + formatRate(observed.oneInSeeds)
                : "No hits";
        String tailEstimate = theory != null && theory.valid
                ? "1 in " + formatRate(theory.consensusOneInSeeds)
                : "Insufficient";
        String modelRange = theory != null && theory.valid
                ? formatRate(theory.lowOneInSeeds) + " - " + formatRate(theory.highOneInSeeds)
                : "-";
        String futureChance = theory != null && theory.valid && Double.isFinite(theory.futureChancePercent)
                ? String.format(Locale.US, "%.1f%%", theory.futureChancePercent)
                : Double.isFinite(observed.futureChancePercent)
                        ? String.format(Locale.US, "%.1f%% obs.", observed.futureChancePercent)
                        : "Unknown";
        String confidence = theory != null && theory.valid ? theory.confidence : "Insufficient";
        return new String[] {observedCount, observedOdds, tailEstimate, modelRange, futureChance, confidence};
    }

    private void updateTailInfo(
            StatisticsCalculator.TheoreticalOddsResult a,
            StatisticsCalculator.TheoreticalOddsResult b
    ) {
        if (b == null) {
            analysisInfoLabel.setText(formatTailSummary(a));
            modelDetailsLabel.setText(formatModelDetails(a));
        } else {
            analysisInfoLabel.setText("Current: " + formatTailSummary(a) + " | All time: " + formatTailSummary(b));
            modelDetailsLabel.setText("Current: " + formatModelDetails(a) + " | All time: " + formatModelDetails(b));
        }
    }

    private String formatTailSummary(StatisticsCalculator.TheoreticalOddsResult result) {
        if (result == null) return "No tail estimate";
        if (!result.valid) return result.message;
        return result.tailObservations
                + " tail islands >= " + formatMetric(result.tailThreshold)
                + " | spread " + String.format(Locale.US, "%.1fx", result.modelSpreadFactor);
    }

    private String formatModelDetails(StatisticsCalculator.TheoreticalOddsResult result) {
        if (result == null || !result.valid || result.models.isEmpty()) return "No model detail";
        List<String> parts = new ArrayList<>();
        for (StatisticsCalculator.TailModelEstimate model : result.models) {
            parts.add(model.name + " 1 in " + formatRate(model.oneInSeeds));
        }
        return String.join(" | ", parts);
    }

    private void updateSummary(StatisticsCalculator.Summary summary, boolean speedInsteadOfRuns) {
        String[] names = speedInsteadOfRuns
                ? new String[] {"Seeds", "Runtime", "Avg speed", "Best", "30k+", "40k+"}
                : new String[] {"Seeds", "Runtime", "Islands", "Best ever", "30k+", "40k+"};
        String[] values = speedInsteadOfRuns
                ? new String[] {
                        UiFormat.compact(summary.seeds),
                        UiFormat.duration(summary.runtimeSeconds),
                        UiFormat.compact(summary.averageSpeed()) + "/s",
                        summary.best <= 0 ? "-" : UiFormat.compact(summary.best),
                        Long.toString(summary.count30k),
                        Long.toString(summary.count40k)
                }
                : new String[] {
                        UiFormat.compact(summary.seeds),
                        UiFormat.duration(summary.runtimeSeconds),
                        UiFormat.compact(summary.islands),
                        summary.best <= 0 ? "-" : UiFormat.compact(summary.best),
                        Long.toString(summary.count30k),
                        Long.toString(summary.count40k)
                };

        for (int i = 0; i < statValues.length; i++) {
            statNames[i].setText(names[i]);
            statValues[i].setText(values[i]);
        }
    }

    private StatisticsCalculator.DistributionMetric selectedMetric() {
        StatisticsCalculator.DistributionMetric metric = (StatisticsCalculator.DistributionMetric) metricCombo.getSelectedItem();
        return metric == null ? StatisticsCalculator.DistributionMetric.BLOCKS : metric;
    }

    private void rebuildTailThresholdChoices() {
        int oldThreshold = selectedTailThreshold();
        tailThresholdCombo.removeAllItems();
        tailThresholdCombo.addItem("Auto");
        int selectedIndex = 0;
        int index = 1;
        for (int threshold : selectedMetric().tailThresholds()) {
            tailThresholdCombo.addItem(formatThreshold(threshold));
            if (threshold == oldThreshold) selectedIndex = index;
            index++;
        }
        tailThresholdCombo.setSelectedIndex(selectedIndex);
    }

    private int selectedTailThreshold() {
        int index = tailThresholdCombo.getSelectedIndex();
        if (index <= 0) return 0;
        int[] thresholds = selectedMetric().tailThresholds();
        int thresholdIndex = index - 1;
        return thresholdIndex >= 0 && thresholdIndex < thresholds.length ? thresholds[thresholdIndex] : 0;
    }

    private double parsedTargetOrDefault() {
        try {
            return parseHumanNumber(oddsTargetField.getText());
        } catch (IllegalArgumentException ex) {
            return selectedMetric().defaultTarget;
        }
    }

    private StatisticsCalculator.DistributionMode selectedDistributionMode() {
        StatisticsCalculator.DistributionMode mode = (StatisticsCalculator.DistributionMode) distributionModeCombo.getSelectedItem();
        return mode == null ? StatisticsCalculator.DistributionMode.RAW_COUNT : mode;
    }

    private String distributionYLabel() {
        return switch (selectedDistributionMode()) {
            case RAW_COUNT -> "Islands";
            case PER_MILLION -> "Per 1M seeds";
            case PERCENT_OF_MATCHES -> "Percent";
        };
    }

    private List<IslandRecord> collectIslands(List<RunRecord> runs) {
        List<IslandRecord> islands = new ArrayList<>();
        for (RunRecord run : runs) islands.addAll(run.islands(repository));
        return islands;
    }

    private static String formatMetric(double value) {
        if (!Double.isFinite(value)) return "-";
        if (Math.abs(value) >= 1000.0) return String.format(Locale.US, "%,.0f", value);
        return String.format(Locale.US, "%.1f", value);
    }

    private static String formatThreshold(int value) {
        if (value >= 1_000 && value % 1_000 == 0) return (value / 1_000) + "k";
        if (value >= 1_000) return String.format(Locale.US, "%.1fk", value / 1_000.0);
        return Integer.toString(value);
    }

    private static String formatRate(double value) {
        if (!Double.isFinite(value) || value <= 0.0) return "-";
        if (value >= 1_000_000_000.0) return String.format(Locale.US, "%.2fB", value / 1_000_000_000.0);
        if (value >= 1_000_000.0) return String.format(Locale.US, "%.2fM", value / 1_000_000.0);
        if (value >= 1_000.0) return String.format(Locale.US, "%.1fk", value / 1_000.0);
        if (value >= 100.0) return String.format(Locale.US, "%.0f", value);
        return String.format(Locale.US, "%.1f", value);
    }

    private static double parseHumanNumber(String text) {
        if (text == null) throw new IllegalArgumentException("missing value");
        String normalized = text.trim().toLowerCase(Locale.ROOT)
                .replace(",", "")
                .replace("_", "")
                .replace(" ", "");
        if (normalized.isEmpty()) throw new IllegalArgumentException("missing value");

        double multiplier = 1.0;
        char last = normalized.charAt(normalized.length() - 1);
        if (last == 'k' || last == 'm' || last == 'b') {
            multiplier = switch (last) {
                case 'k' -> 1_000.0;
                case 'm' -> 1_000_000.0;
                case 'b' -> 1_000_000_000.0;
                default -> 1.0;
            };
            normalized = normalized.substring(0, normalized.length() - 1);
        }

        double value = Double.parseDouble(normalized) * multiplier;
        if (!Double.isFinite(value) || value < 0.0) throw new IllegalArgumentException("invalid value");
        return value;
    }
}
