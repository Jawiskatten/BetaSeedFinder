import javax.swing.*;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import javax.swing.table.AbstractTableModel;
import javax.swing.table.TableRowSorter;
import java.awt.*;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class HistoryTab extends JPanel {
    public interface NavigationListener {
        void viewStatistics(RunRecord run);
        void viewIslands(RunRecord run);
    }

    private final RunRepository repository;
    private final NavigationListener navigationListener;
    private final RunTableModel tableModel = new RunTableModel();
    private final JTable table = new JTable(tableModel);
    private final TableRowSorter<RunTableModel> sorter = new TableRowSorter<>(tableModel);
    private final JTextArea detailsArea = new JTextArea();
    private final JLabel summaryLabel = new JLabel();
    private final JTextField searchField = new JTextField(22);
    private final JComboBox<String> statusCombo = new JComboBox<>(new String[] {
            "All statuses", "Completed", "Stopped", "Interrupted", "Imported", "Errors"
    });
    private final JComboBox<String> profileCombo = new JComboBox<>();
    private final JButton includeButton = new JButton("Include in Stats");

    private List<RunRecord> allRuns = List.of();

    public HistoryTab(RunRepository repository, NavigationListener navigationListener) {
        super(new BorderLayout(10, 10));
        this.repository = repository;
        this.navigationListener = navigationListener;
        setOpaque(true);
        setBackground(MinecraftTheme.BG);
        setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));

        add(createToolbar(), BorderLayout.NORTH);
        add(createMainPanel(), BorderLayout.CENTER);
        add(createFooter(), BorderLayout.SOUTH);

        statusCombo.addActionListener(e -> applyFilter());
        profileCombo.addActionListener(e -> applyFilter());
        addDocumentListener(searchField, this::applyFilter);
        table.getSelectionModel().addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting()) updateSelectedRunDetails();
        });
    }

    private JPanel createToolbar() {
        JPanel panel = new TexturePanel(new GridBagLayout(), "", Color.BLACK, 24);
        panel.setBorder(MinecraftTheme.boxBorder());
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.gridy = 0;
        gbc.fill = GridBagConstraints.HORIZONTAL;
        gbc.insets = new Insets(0, 0, 0, 10);

        searchField.setToolTipText("Search runs by date, profile, status, build, or folder.");
        gbc.gridx = 0;
        gbc.weightx = 1.0;
        panel.add(toolbarGroup("SEARCH RUNS", searchField), gbc);

        gbc.weightx = 0;
        gbc.gridx++;
        panel.add(toolbarGroup("STATUS", statusCombo), gbc);

        gbc.gridx++;
        panel.add(toolbarGroup("PROFILE", profileCombo), gbc);

        JButton refreshButton = new JButton("Refresh");
        refreshButton.addActionListener(e -> refreshData());
        gbc.gridx++;
        gbc.insets = new Insets(0, 0, 0, 0);
        panel.add(refreshButton, gbc);
        return panel;
    }

    private JPanel toolbarGroup(String label, JComponent component) {
        JPanel group = new JPanel(new BorderLayout(3, 2));
        group.setOpaque(false);
        JLabel caption = new JLabel(label);
        caption.setFont(MinecraftTheme.SMALL_FONT);
        caption.setForeground(MinecraftTheme.TEXT_DIM);
        group.add(caption, BorderLayout.NORTH);
        group.add(component, BorderLayout.CENTER);
        return group;
    }

    private JComponent createMainPanel() {
        table.setRowHeight(31);
        table.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        table.setRowSorter(sorter);
        table.setFillsViewportHeight(true);
        JScrollPane tableScroll = new JScrollPane(table);

        sorter.setComparator(3, java.util.Comparator.comparingLong(v -> (Long) v));
        sorter.setComparator(4, java.util.Comparator.comparingDouble(v -> (Double) v));
        sorter.setComparator(5, java.util.Comparator.comparingLong(v -> (Long) v));
        sorter.setComparator(6, java.util.Comparator.comparingLong(v -> (Long) v));
        sorter.setComparator(7, java.util.Comparator.comparingLong(v -> (Long) v));

        JPanel tablePanel = new TexturePanel(new BorderLayout(), "", Color.BLACK, 68);
        tablePanel.setBorder(MinecraftTheme.titled("RUN HISTORY"));
        tablePanel.add(tableScroll, BorderLayout.CENTER);

        JPanel detailsPanel = new TexturePanel(new BorderLayout(8, 8), "", Color.BLACK, 52);
        detailsPanel.setBorder(MinecraftTheme.titled("SELECTED RUN"));
        detailsPanel.setPreferredSize(new Dimension(455, 0));
        detailsPanel.setMinimumSize(new Dimension(390, 0));

        detailsArea.setEditable(false);
        detailsArea.setLineWrap(true);
        detailsArea.setWrapStyleWord(false);
        detailsArea.setText("Select a run.");
        detailsArea.setBorder(BorderFactory.createEmptyBorder(10, 12, 10, 12));
        JScrollPane detailsScroll = new JScrollPane(detailsArea);
        detailsPanel.add(detailsScroll, BorderLayout.CENTER);

        JPanel buttons = new JPanel(new GridLayout(2, 2, 8, 8));
        buttons.setOpaque(false);
        JButton islandsButton = new JButton("View Results");
        JButton statsButton = new JButton("Detailed Statistics");
        JButton folderButton = new JButton("Open Run Folder");
        islandsButton.addActionListener(e -> {
            RunRecord run = selectedRun();
            if (run != null) navigationListener.viewIslands(run);
        });
        statsButton.addActionListener(e -> {
            RunRecord run = selectedRun();
            if (run != null) navigationListener.viewStatistics(run);
        });
        folderButton.addActionListener(e -> {
            RunRecord run = selectedRun();
            if (run != null) openFolder(run.folder.toFile());
        });
        includeButton.addActionListener(e -> toggleIncluded());
        buttons.add(islandsButton);
        buttons.add(folderButton);
        buttons.add(statsButton);
        buttons.add(includeButton);
        detailsPanel.add(buttons, BorderLayout.SOUTH);

        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, tablePanel, detailsPanel);
        split.setResizeWeight(0.70);
        split.setDividerSize(10);
        split.setBorder(null);
        return split;
    }

    private JPanel createFooter() {
        JPanel footer = new TexturePanel(new BorderLayout(), "", Color.BLACK, 18);
        footer.setBorder(MinecraftTheme.boxBorder());
        JLabel left = new JLabel("ALL-TIME SUMMARY");
        left.setForeground(MinecraftTheme.BLUE_HIT);
        summaryLabel.setForeground(MinecraftTheme.TEXT_DIM);
        footer.add(left, BorderLayout.WEST);
        footer.add(summaryLabel, BorderLayout.EAST);
        return footer;
    }

    public void refreshData() {
        RunRecord selected = selectedRun();
        allRuns = repository.refresh();
        for (RunRecord run : allRuns) run.clearIslandCache();
        rebuildProfileFilter();
        applyFilter();
        if (selected != null) selectRun(selected.folder.toString());
        if (table.getSelectedRow() < 0 && table.getRowCount() > 0) {
            table.setRowSelectionInterval(0, 0);
        }
        updateSummary();
    }

    private void rebuildProfileFilter() {
        Object previous = profileCombo.getSelectedItem();
        profileCombo.removeAllItems();
        profileCombo.addItem("All profiles");
        Set<String> profiles = new LinkedHashSet<>();
        for (RunRecord run : allRuns) profiles.add(run.profileName());
        for (String profile : profiles) profileCombo.addItem(profile);
        if (previous != null) profileCombo.setSelectedItem(previous);
        if (profileCombo.getSelectedIndex() < 0) profileCombo.setSelectedIndex(0);
    }

    private void applyFilter() {
        if (profileCombo.getItemCount() == 0) return;
        String query = searchField.getText().trim().toLowerCase();
        String status = String.valueOf(statusCombo.getSelectedItem());
        String profile = String.valueOf(profileCombo.getSelectedItem());
        List<RunRecord> filtered = new ArrayList<>();
        for (RunRecord run : allRuns) {
            String friendly = run.friendlyStatus();
            String lowerStatus = friendly.toLowerCase();
            if (!"All statuses".equals(status)) {
                boolean match = switch (status) {
                    case "Completed" -> lowerStatus.contains("completed");
                    case "Stopped" -> lowerStatus.contains("stopped");
                    case "Interrupted" -> lowerStatus.contains("interrupted");
                    case "Imported" -> lowerStatus.contains("imported");
                    case "Errors" -> lowerStatus.contains("error") || lowerStatus.contains("failed");
                    default -> true;
                };
                if (!match) continue;
            }
            if (!"All profiles".equals(profile) && !run.profileName().equals(profile)) continue;
            if (!query.isEmpty()) {
                String haystack = (run.dateText() + " " + run.profileName() + " " + friendly + " " +
                        run.build + " " + run.folder).toLowerCase();
                if (!haystack.contains(query)) continue;
            }
            filtered.add(run);
        }
        tableModel.setRuns(filtered);
        sorter.setSortKeys(List.of(new RowSorter.SortKey(0, SortOrder.DESCENDING)));
        sorter.sort();
        if (table.getRowCount() > 0) table.setRowSelectionInterval(0, 0);
        else detailsArea.setText("No runs match these filters.");
        updateSummary();
    }

    private void updateSummary() {
        long totalRegions = 0;
        double totalSeconds = 0;
        long included = 0;
        for (RunRecord run : allRuns) {
            if (!run.includedInTotals()) continue;
            included++;
            totalRegions += run.checked;
            totalSeconds += run.elapsedSeconds;
        }
        summaryLabel.setText("Production runs: " + included + "   |   Runtime: " +
                UiFormat.duration(totalSeconds) + "   |   Regions: " + String.format("%,d", totalRegions));
    }

    private void updateSelectedRunDetails() {
        RunRecord run = selectedRun();
        detailsArea.setText(run == null ? "Select a run." : runDetailsText(run));
        detailsArea.setCaretPosition(0);
        if (run != null) {
            includeButton.setText(run.includedInTotals() ? "Exclude from Stats" : "Include in Stats");
        }
    }

    private String runDetailsText(RunRecord run) {
        int best = run.bestBlocks(repository);
        long c20 = run.countAtLeast(repository, 20_000);
        long c40 = run.countAtLeast(repository, 40_000);
        long c60 = run.countAtLeast(repository, 60_000);
        long c80 = run.countAtLeast(repository, 80_000);
        long c100 = run.countAtLeast(repository, 100_000);
        String checkedUnit = run.regionsAreCheckedUnit() ? "REGIONS" : "SEEDS";
        String rateUnit = run.regionsAreCheckedUnit() ? "regions/s" : "seeds/s";
        String gpu = run.manifest.getOrDefault("gpuName", run.manifest.getOrDefault("gpu", "Unknown"));

        return "DATE               " + run.dateText() + "\n" +
                "PROFILE            " + run.profileName() + "\n" +
                "STATUS             " + run.friendlyStatus() + "\n" +
                "RUNTIME            " + UiFormat.duration(run.elapsedSeconds) + "\n" +
                checkedUnit + spacesFor(checkedUnit) + String.format("%,d", run.checked) + "\n" +
                "WORLDS PROCESSED   " + String.format("%,d", run.worldsProcessed()) + "\n" +
                "AVERAGE SPEED      " + String.format("%,.1f %s", run.averageSpeed(), rateUnit) + "\n" +
                "BEST RESULT        " + (best > 0 ? String.format("%,d blocks", best) : "-") + "\n\n" +
                "20K+  " + c20 + "     40K+  " + c40 + "     60K+  " + c60 + "\n" +
                "80K+  " + c80 + "     100K+ " + c100 + "\n\n" +
                "APP BUILD          " + run.build + "\n" +
                "GPU                " + gpu + "\n" +
                "IN ALL-TIME STATS  " + (run.includedInTotals() ? "YES" : "NO") + "\n" +
                "OUTPUT FOLDER      " + run.folder;
    }

    private String spacesFor(String checkedUnit) {
        return " ".repeat(Math.max(1, 19 - checkedUnit.length()));
    }

    private void toggleIncluded() {
        RunRecord run = selectedRun();
        if (run == null) return;
        try {
            repository.setIncludedInTotals(run, !run.includedInTotals());
            tableModel.fireTableDataChanged();
            updateSelectedRunDetails();
            updateSummary();
        } catch (IOException e) {
            JOptionPane.showMessageDialog(this, e.getMessage(), "Could not save setting", JOptionPane.ERROR_MESSAGE);
        }
    }

    private RunRecord selectedRun() {
        int viewRow = table.getSelectedRow();
        if (viewRow < 0) return null;
        int modelRow = table.convertRowIndexToModel(viewRow);
        return tableModel.getRun(modelRow);
    }

    private void selectRun(String folder) {
        for (int modelRow = 0; modelRow < tableModel.getRowCount(); modelRow++) {
            RunRecord run = tableModel.getRun(modelRow);
            if (run.folder.toString().equals(folder)) {
                int viewRow = table.convertRowIndexToView(modelRow);
                if (viewRow >= 0) table.setRowSelectionInterval(viewRow, viewRow);
                return;
            }
        }
    }

    private void openFolder(java.io.File folder) {
        try {
            Desktop.getDesktop().open(folder);
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, e.getMessage(), "Could not open folder", JOptionPane.ERROR_MESSAGE);
        }
    }

    private static void addDocumentListener(JTextField field, Runnable action) {
        field.getDocument().addDocumentListener(new DocumentListener() {
            @Override public void insertUpdate(DocumentEvent e) { action.run(); }
            @Override public void removeUpdate(DocumentEvent e) { action.run(); }
            @Override public void changedUpdate(DocumentEvent e) { action.run(); }
        });
    }

    private class RunTableModel extends AbstractTableModel {
        private final String[] columns = {
                "Date", "Profile", "Runtime", "Regions / Seeds", "Avg Speed", "Best", "60k+", "80k+", "Status"
        };
        private List<RunRecord> runs = List.of();

        public void setRuns(List<RunRecord> runs) {
            this.runs = List.copyOf(runs);
            fireTableDataChanged();
        }

        public RunRecord getRun(int row) {
            return row >= 0 && row < runs.size() ? runs.get(row) : null;
        }

        @Override public int getRowCount() { return runs.size(); }
        @Override public int getColumnCount() { return columns.length; }
        @Override public String getColumnName(int column) { return columns[column]; }
        @Override public Class<?> getColumnClass(int column) {
            return switch (column) {
                case 3, 5, 6, 7 -> Long.class;
                case 4 -> Double.class;
                default -> String.class;
            };
        }

        @Override
        public Object getValueAt(int rowIndex, int columnIndex) {
            RunRecord run = runs.get(rowIndex);
            return switch (columnIndex) {
                case 0 -> run.dateText();
                case 1 -> run.profileName();
                case 2 -> UiFormat.duration(run.elapsedSeconds);
                case 3 -> run.checked;
                case 4 -> Math.round(run.averageSpeed() * 10.0) / 10.0;
                case 5 -> (long) run.bestBlocks(repository);
                case 6 -> run.countAtLeast(repository, 60_000);
                case 7 -> run.countAtLeast(repository, 80_000);
                case 8 -> run.friendlyStatus();
                default -> "";
            };
        }
    }
}
