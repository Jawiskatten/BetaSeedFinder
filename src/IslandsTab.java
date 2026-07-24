import javax.swing.*;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import javax.swing.table.AbstractTableModel;
import javax.swing.table.TableRowSorter;
import java.awt.*;
import java.awt.datatransfer.StringSelection;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class IslandsTab extends JPanel {
    private static final Path FAVORITES_PATH = AppPaths.favoritesPath();

    private final RunRepository repository;
    private final IslandTableModel tableModel = new IslandTableModel();
    private final JTable table = new JTable(tableModel);
    private final TableRowSorter<IslandTableModel> sorter = new TableRowSorter<>(tableModel);

    private final JComboBox<ScopeOption> scopeCombo = new JComboBox<>();
    private final JTextField searchField = new JTextField(22);
    private final JComboBox<String> minimumCombo = new JComboBox<>(new String[] {
            "20,000+", "30,000+", "40,000+", "60,000+", "80,000+", "100,000+", "Custom"
    });
    private final JTextField customMinimumField = new JTextField("20000", 7);
    private final JCheckBox favoritesOnlyBox = new JCheckBox("Favorites only");
    private final JComboBox<String> sortCombo = new JComboBox<>(new String[] {
            "Blocks (high to low)",
            "Footprint (high to low)",
            "Coarse (high to low)",
            "Newest",
            "Thickness",
            "Fill %"
    });
    private final JLabel resultCountLabel = new JLabel("0 islands");
    private final JLabel previewLabel = new JLabel("Select an island to show its preview", SwingConstants.CENTER);
    private final CardLayout previewCardLayout = new CardLayout();
    private final JPanel previewCards = new JPanel(previewCardLayout);
    private final Island3DPreviewPanel preview3DPanel = new Island3DPreviewPanel();
    private final JButton preview3DButton = new JButton("3D");
    private final JButton preview2DButton = new JButton("2D");
    private final JButton previewTopButton = new JButton("Top");
    private final JButton previewSideButton = new JButton("Side");
    private final JButton previewResetButton = new JButton("Reset");
    private final JButton generatePreviewButton = new JButton("Regenerate Preview");
    private final JTextArea detailsArea = new JTextArea();
    private final JButton favoriteButton = new JButton("Add Favorite");
    private final JButton technicalButton = new JButton("Technical details +");

    private final Set<String> favorites = new HashSet<>();
    private List<IslandRecord> unfiltered = List.of();
    private List<IslandRecord> loaded = List.of();
    private RunRecord forcedRun;
    private IslandRecord forcedIsland;
    private boolean technicalDetailsVisible;
    private boolean showing3D = true;
    private SwingWorker<Island3DData, Void> preview3DWorker;

    public IslandsTab(RunRepository repository) {
        super(new BorderLayout(10, 10));
        this.repository = repository;
        setOpaque(true);
        setBackground(MinecraftTheme.BG);
        setBorder(BorderFactory.createEmptyBorder(10, 10, 10, 10));

        loadFavorites();
        add(createToolbar(), BorderLayout.NORTH);
        add(createMainPanel(), BorderLayout.CENTER);

        installSorters();
        scopeCombo.addActionListener(e -> reloadScope());
        minimumCombo.addActionListener(e -> {
            customMinimumField.setVisible("Custom".equals(minimumCombo.getSelectedItem()));
            applyFilters();
            revalidate();
        });
        customMinimumField.addActionListener(e -> applyFilters());
        favoritesOnlyBox.addActionListener(e -> applyFilters());
        sortCombo.addActionListener(e -> applySort());
        addDocumentListener(searchField, this::applyFilters);
        table.getSelectionModel().addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting()) updateSelectedIsland();
        });
    }

    private JPanel createToolbar() {
        JPanel panel = new TexturePanel(new GridBagLayout(), "", Color.BLACK, 24);
        panel.setBorder(MinecraftTheme.boxBorder());
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.gridy = 0;
        gbc.insets = new Insets(0, 0, 0, 10);
        gbc.fill = GridBagConstraints.HORIZONTAL;

        searchField.setToolTipText("Search by seed, coordinates, run date, or profile.");
        gbc.gridx = 0;
        gbc.weightx = 1.0;
        panel.add(toolbarGroup("SEARCH ISLANDS", searchField), gbc);

        gbc.weightx = 0;
        gbc.gridx++;
        panel.add(toolbarGroup("MINIMUM BLOCKS", minimumCombo), gbc);

        gbc.gridx++;
        panel.add(toolbarGroup("FAVORITES", favoritesOnlyBox), gbc);

        gbc.gridx++;
        panel.add(toolbarGroup("SEARCH IN", scopeCombo), gbc);

        gbc.gridx++;
        panel.add(toolbarGroup("SORT BY", sortCombo), gbc);

        gbc.gridx++;
        gbc.insets = new Insets(0, 0, 0, 0);
        resultCountLabel.setForeground(MinecraftTheme.BLUE_HIT);
        resultCountLabel.setHorizontalAlignment(SwingConstants.RIGHT);
        panel.add(toolbarGroup("RESULTS", resultCountLabel), gbc);

        customMinimumField.setVisible(false);
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
        table.getColumnModel().getColumn(0).setPreferredWidth(190);
        table.getColumnModel().getColumn(1).setPreferredWidth(90);
        table.getColumnModel().getColumn(2).setPreferredWidth(80);
        table.getColumnModel().getColumn(3).setPreferredWidth(115);
        table.getColumnModel().getColumn(4).setPreferredWidth(105);
        table.getColumnModel().getColumn(5).setPreferredWidth(90);
        table.getColumnModel().getColumn(6).setPreferredWidth(75);
        table.getColumnModel().getColumn(7).setPreferredWidth(70);
        JScrollPane tableScroll = new JScrollPane(table);

        JPanel tablePanel = new TexturePanel(new BorderLayout(), "", Color.BLACK, 70);
        tablePanel.setBorder(MinecraftTheme.titled("ISLAND LIBRARY"));
        tablePanel.add(tableScroll, BorderLayout.CENTER);

        JPanel right = new JPanel(new BorderLayout(10, 10));
        right.setOpaque(false);
        right.setPreferredSize(new Dimension(560, 0));
        right.setMinimumSize(new Dimension(470, 0));

        JPanel previewPanel = new TexturePanel(new BorderLayout(6, 6), "", Color.BLACK, 42);
        previewPanel.setBorder(MinecraftTheme.titled("PREVIEW"));
        previewPanel.setMinimumSize(new Dimension(460, 240));

        JPanel previewToolbar = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 0));
        previewToolbar.setOpaque(false);
        preview3DButton.putClientProperty("accentButton", Boolean.TRUE);
        preview3DButton.addActionListener(e -> switchPreviewMode(true));
        preview2DButton.addActionListener(e -> switchPreviewMode(false));
        previewTopButton.addActionListener(e -> {
            switchPreviewMode(true);
            preview3DPanel.topView();
        });
        previewSideButton.addActionListener(e -> {
            switchPreviewMode(true);
            preview3DPanel.sideView();
        });
        previewResetButton.addActionListener(e -> {
            switchPreviewMode(true);
            preview3DPanel.resetView();
        });
        previewToolbar.add(preview3DButton);
        previewToolbar.add(preview2DButton);
        previewToolbar.add(previewTopButton);
        previewToolbar.add(previewSideButton);
        previewToolbar.add(previewResetButton);

        previewLabel.setForeground(MinecraftTheme.TEXT_DIM);
        previewLabel.setBorder(BorderFactory.createEmptyBorder(6, 6, 6, 6));
        previewCards.setOpaque(false);
        previewCards.add(preview3DPanel, "3D");
        previewCards.add(previewLabel, "2D");
        previewCardLayout.show(previewCards, "3D");

        generatePreviewButton.setEnabled(false);
        generatePreviewButton.addActionListener(e -> generateSelectedPreview());
        previewPanel.add(previewToolbar, BorderLayout.NORTH);
        previewPanel.add(previewCards, BorderLayout.CENTER);
        previewPanel.add(generatePreviewButton, BorderLayout.SOUTH);

        JPanel detailsPanel = new TexturePanel(new BorderLayout(8, 8), "", Color.BLACK, 58);
        detailsPanel.setBorder(MinecraftTheme.titled("SELECTED ISLAND"));
        detailsArea.setEditable(false);
        detailsArea.setLineWrap(true);
        detailsArea.setWrapStyleWord(true);
        detailsArea.setText("No island selected.");
        detailsArea.setBorder(BorderFactory.createEmptyBorder(10, 12, 10, 12));
        detailsPanel.add(new JScrollPane(detailsArea), BorderLayout.CENTER);

        technicalButton.addActionListener(e -> {
            technicalDetailsVisible = !technicalDetailsVisible;
            technicalButton.setText(technicalDetailsVisible ? "Technical details -" : "Technical details +");
            updateSelectedIsland();
        });

        JPanel actions = new JPanel(new GridLayout(2, 3, 6, 6));
        actions.setOpaque(false);
        JButton copyButton = new JButton("Copy Seed");
        JButton copyLocationButton = new JButton("Copy Seed + Coords");
        JButton folderButton = new JButton("Open Run Folder");
        JButton statsButton = new JButton("Copy Details");
        JButton[] compactActions = { copyButton, copyLocationButton, favoriteButton, folderButton, statsButton, technicalButton };
        for (JButton button : compactActions) button.putClientProperty("compactButton", Boolean.TRUE);
        copyButton.addActionListener(e -> copySelectedSeed());
        copyLocationButton.addActionListener(e -> copySelectedLocation());
        folderButton.addActionListener(e -> openSelectedRunFolder());
        statsButton.addActionListener(e -> copySelectedStats());
        favoriteButton.addActionListener(e -> toggleSelectedFavorite());
        actions.add(copyButton);
        actions.add(copyLocationButton);
        actions.add(favoriteButton);
        actions.add(folderButton);
        actions.add(statsButton);
        actions.add(technicalButton);
        detailsPanel.add(actions, BorderLayout.SOUTH);

        JSplitPane rightSplit = new JSplitPane(JSplitPane.VERTICAL_SPLIT, previewPanel, detailsPanel);
        rightSplit.setResizeWeight(0.46);
        rightSplit.setDividerLocation(0.46);
        rightSplit.setDividerSize(8);
        rightSplit.setBorder(null);
        right.add(rightSplit, BorderLayout.CENTER);

        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, tablePanel, right);
        split.setResizeWeight(0.64);
        split.setDividerSize(10);
        split.setBorder(null);
        return split;
    }

    private void installSorters() {
        sorter.setComparator(0, Comparator.comparingLong(v -> (Long) v));
        sorter.setComparator(1, Comparator.comparingInt(v -> (Integer) v));
        sorter.setComparator(2, Comparator.comparingInt(v -> (Integer) v));
        sorter.setComparator(4, Comparator.comparingLong(v -> ((FootprintValue) v).area));
        sorter.setComparator(5, Comparator.comparingDouble(v -> (Double) v));
        sorter.setComparator(6, Comparator.comparingDouble(v -> (Double) v));
        sorter.setComparator(7, Comparator.comparing(v -> (Boolean) v));
    }

    public void setSelectedRun(RunRecord run) {
        forcedRun = run;
        refreshData();
    }

    public void selectIsland(IslandRecord island) {
        forcedIsland = island;
        forcedRun = island == null ? null : island.run;
        refreshData();
    }

    public void refreshData() {
        repository.refresh();
        ScopeOption previous = (ScopeOption) scopeCombo.getSelectedItem();
        String previousKey = previous == null ? null : previous.key();

        scopeCombo.removeAllItems();
        scopeCombo.addItem(new ScopeOption("Production runs", null, true, false));
        scopeCombo.addItem(new ScopeOption("All runs", null, false, false));
        RunRecord latest = repository.latestRun();
        if (latest != null) scopeCombo.addItem(new ScopeOption("Latest run", latest, false, true));
        for (RunRecord run : repository.runs()) {
            scopeCombo.addItem(new ScopeOption(run.dateText() + " | " + run.profileName(), run, false, false));
        }

        if (forcedRun != null) {
            previousKey = forcedRun.folder.toString();
            forcedRun = null;
        }
        if (previousKey != null) {
            for (int i = 0; i < scopeCombo.getItemCount(); i++) {
                if (scopeCombo.getItemAt(i).key().equals(previousKey)) {
                    scopeCombo.setSelectedIndex(i);
                    reloadScope();
                    selectForcedIsland();
                    return;
                }
            }
        }
        scopeCombo.setSelectedIndex(0);
        reloadScope();
        selectForcedIsland();
    }

    private void reloadScope() {
        ScopeOption option = (ScopeOption) scopeCombo.getSelectedItem();
        if (option == null) return;
        List<IslandRecord> islands = new ArrayList<>();
        if (option.run != null) islands.addAll(option.run.islands(repository));
        else islands.addAll(repository.allIslands(option.includedOnly));
        unfiltered = List.copyOf(islands);
        applyFilters();
    }

    private void applyFilters() {
        int minimum = selectedMinimum();
        String query = searchField.getText().trim().toLowerCase();
        boolean favoritesOnly = favoritesOnlyBox.isSelected();
        List<IslandRecord> filtered = new ArrayList<>();
        for (IslandRecord island : unfiltered) {
            if (island.blocks < minimum) continue;
            if (favoritesOnly && !isFavorite(island)) continue;
            if (!query.isEmpty() && !matchesQuery(island, query)) continue;
            filtered.add(island);
        }
        loaded = List.copyOf(filtered);
        tableModel.setIslands(loaded);
        resultCountLabel.setText(String.format("%,d shown", loaded.size()));
        applySort();
        if (table.getRowCount() > 0) table.setRowSelectionInterval(0, 0);
        else clearSelectionDetails();
    }

    private boolean matchesQuery(IslandRecord island, String query) {
        String haystack = island.seed + " " + island.centerX + " " + island.centerZ + " " +
                island.run.dateText().toLowerCase() + " " + island.run.profileName().toLowerCase();
        return haystack.contains(query);
    }

    private int selectedMinimum() {
        String selected = (String) minimumCombo.getSelectedItem();
        if ("Custom".equals(selected)) {
            try { return Math.max(0, Integer.parseInt(customMinimumField.getText().trim())); }
            catch (NumberFormatException e) { return 0; }
        }
        try { return Integer.parseInt(selected.replace(",", "").replace("+", "")); }
        catch (Exception e) { return 20_000; }
    }

    private void applySort() {
        int column = switch (sortCombo.getSelectedIndex()) {
            case 1 -> 4; // total footprint area
            case 2 -> 2; // coarse
            case 3 -> 3; // newest
            case 4 -> 5; // thickness
            case 5 -> 6; // fill
            default -> 1; // blocks
        };
        sorter.setSortKeys(List.of(new RowSorter.SortKey(column, SortOrder.DESCENDING)));
        sorter.sort();
    }

    private void updateSelectedIsland() {
        IslandRecord island = selectedIsland();
        if (island == null) {
            clearSelectionDetails();
            return;
        }
        detailsArea.setText(islandStatsText(island, technicalDetailsVisible));
        detailsArea.setCaretPosition(0);
        showPreview(island.previewPath());
        load3DPreview(island);
        switchPreviewMode(true);
        updateGeneratePreviewButton(island);
        favoriteButton.setText(isFavorite(island) ? "Remove Favorite" : "Add Favorite");
    }

    private void clearSelectionDetails() {
        detailsArea.setText("No island selected.");
        if (preview3DWorker != null) preview3DWorker.cancel(true);
        preview3DPanel.clear("Select an island to load the 3D preview");
        previewLabel.setIcon(null);
        previewLabel.setText("Select an island to show its preview");
        switchPreviewMode(true);
        generatePreviewButton.setEnabled(false);
        generatePreviewButton.setText("Generate Preview");
        favoriteButton.setText("Add Favorite");
    }

    private IslandRecord selectedIsland() {
        int viewRow = table.getSelectedRow();
        if (viewRow < 0) return null;
        int modelRow = table.convertRowIndexToModel(viewRow);
        return tableModel.getIsland(modelRow);
    }

    private String islandStatsText(IslandRecord i, boolean technical) {
        String verification = i.radius >= 16 ? "Radius 16 verified" : "Radius " + i.radius + " verified";
        if (i.touchesSide) verification += " (touches boundary)";
        String text =
                "SEED        " + i.seed + "\n" +
                "BLOCKS      " + String.format("%,d", i.blocks) + "\n" +
                "CENTER      X " + i.centerX + "   Z " + i.centerZ + "\n" +
                "SIZE        " + i.width + " x " + i.depth + "\n" +
                "COLUMNS     " + String.format("%,d", i.columns) + "\n" +
                "THICKNESS   " + String.format("%.2f", i.avgThickness) + "\n" +
                "FILL        " + String.format("%.2f%%", i.fillPercent) + "\n" +
                "Y RANGE     " + i.minY + "-" + i.maxY + " (" + i.yRange() + ")\n" +
                "COARSE      " + i.coarse + "\n" +
                "VERIFIED    " + verification + "\n" +
                "PROFILE     " + i.run.profileName() + "\n" +
                "FOUND       " + i.run.dateText();
        if (technical) {
            text += "\n\nTECHNICAL DETAILS\n" +
                    "Attempt: " + i.attempt + "\n" +
                    "Search-center chunk: " + i.searchCenterChunkX + ", " + i.searchCenterChunkZ + "\n" +
                    "Stage0: " + i.stage0 + "\n" +
                    "Stage0 Y88: " + i.stage0Y88 + "\n" +
                    "Final source: " + i.finalSource + "\n" +
                    "Build: " + i.run.build + "\n" +
                    "Run folder: " + i.run.folder;
        }
        return text;
    }

    private void copySelectedSeed() {
        IslandRecord island = selectedIsland();
        if (island != null) copyText(Long.toString(island.seed));
    }

    private void copySelectedLocation() {
        IslandRecord island = selectedIsland();
        if (island != null) copyText("Seed: " + island.seed + " | X/Z: " + island.centerX + ", " + island.centerZ);
    }

    private void copySelectedStats() {
        IslandRecord island = selectedIsland();
        if (island != null) copyText(islandStatsText(island, true));
    }

    private void openSelectedRunFolder() {
        IslandRecord island = selectedIsland();
        if (island == null) return;
        try {
            Desktop.getDesktop().open(island.run.folder.toFile());
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, e.getMessage(), "Could not open folder", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void copyText(String text) {
        Toolkit.getDefaultToolkit().getSystemClipboard().setContents(new StringSelection(text), null);
    }

    private void toggleSelectedFavorite() {
        IslandRecord island = selectedIsland();
        if (island == null) return;
        String key = favoriteKey(island);
        if (!favorites.add(key)) favorites.remove(key);
        saveFavorites();
        tableModel.fireTableDataChanged();
        favoriteButton.setText(isFavorite(island) ? "Remove Favorite" : "Add Favorite");
        if (favoritesOnlyBox.isSelected()) applyFilters();
    }

    private boolean isFavorite(IslandRecord island) {
        return favorites.contains(favoriteKey(island));
    }

    private String favoriteKey(IslandRecord island) {
        return island.seed + "|" + island.centerX + "|" + island.centerZ;
    }

    private void loadFavorites() {
        favorites.clear();
        if (!Files.isRegularFile(FAVORITES_PATH)) return;
        try {
            for (String line : Files.readAllLines(FAVORITES_PATH, StandardCharsets.UTF_8)) {
                String value = line.trim();
                if (!value.isEmpty()) favorites.add(value);
            }
        } catch (Exception ignored) {
        }
    }

    private void saveFavorites() {
        try {
            Files.createDirectories(FAVORITES_PATH.getParent());
            List<String> sorted = new ArrayList<>(favorites);
            sorted.sort(String::compareTo);
            Files.write(FAVORITES_PATH, sorted, StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        } catch (Exception e) {
            JOptionPane.showMessageDialog(this, e.getMessage(), "Could not save favorites", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void updateGeneratePreviewButton(IslandRecord island) {
        if (island == null) {
            generatePreviewButton.setEnabled(false);
            generatePreviewButton.setText("Generate Preview");
            return;
        }
        Path path = island.previewPath();
        generatePreviewButton.setEnabled(true);
        generatePreviewButton.setText(path != null && Files.isRegularFile(path)
                ? "Regenerate Preview" : "Generate Preview");
    }

    private void switchPreviewMode(boolean use3D) {
        showing3D = use3D;
        previewCardLayout.show(previewCards, use3D ? "3D" : "2D");
        preview3DButton.putClientProperty("accentButton", use3D);
        preview2DButton.putClientProperty("accentButton", !use3D);
        preview3DButton.repaint();
        preview2DButton.repaint();
    }

    private String preview3DCacheKey(IslandRecord island) {
        if (island == null) return null;
        return island.seed + "|" + island.centerX + "|" + island.centerZ + "|"
                + island.blocks + "|" + island.radius;
    }

    private void load3DPreview(IslandRecord island) {
        if (island == null) return;
        if (preview3DWorker != null) preview3DWorker.cancel(true);

        String key = preview3DCacheKey(island);
        Island3DData cached = Island3DPreviewPanel.cached(key);
        if (cached != null) {
            preview3DPanel.setData(cached);
            return;
        }

        preview3DPanel.clear("Reconstructing exact 3D surface...");
        preview3DWorker = new SwingWorker<>() {
            @Override protected Island3DData doInBackground() throws Exception {
                return new IslandSearchEngine().generate3DPreviewForIsland(island);
            }

            @Override protected void done() {
                if (isCancelled()) return;
                IslandRecord current = selectedIsland();
                if (!sameIsland(current, island)) return;
                try {
                    Island3DData data = get();
                    Island3DPreviewPanel.cache(key, data);
                    preview3DPanel.setData(data);
                } catch (Exception e) {
                    Throwable cause = e.getCause() == null ? e : e.getCause();
                    preview3DPanel.clear("3D reconstruction failed: " + cause.getMessage());
                    Path fallback = island.previewPath();
                    if (fallback != null && Files.isRegularFile(fallback) && showing3D) {
                        switchPreviewMode(false);
                    }
                }
            }
        };
        preview3DWorker.execute();
    }

    private void generateSelectedPreview() {
        IslandRecord island = selectedIsland();
        if (island == null) return;
        generatePreviewButton.setEnabled(false);
        generatePreviewButton.setText("Generating...");
        previewLabel.setIcon(null);
        previewLabel.setText("Generating preview...");

        SwingWorker<Path, Void> worker = new SwingWorker<>() {
            @Override protected Path doInBackground() throws Exception {
                return new IslandSearchEngine().generatePreviewForIsland(island);
            }
            @Override protected void done() {
                IslandRecord current = selectedIsland();
                try {
                    Path path = get();
                    if (sameIsland(current, island)) {
                        showPreview(path);
                        updateGeneratePreviewButton(current);
                    }
                } catch (Exception e) {
                    Throwable cause = e.getCause() == null ? e : e.getCause();
                    if (sameIsland(current, island)) {
                        previewLabel.setIcon(null);
                        previewLabel.setText("Preview generation failed");
                        updateGeneratePreviewButton(current);
                    }
                    JOptionPane.showMessageDialog(IslandsTab.this, cause.getMessage(),
                            "Could not generate preview", JOptionPane.ERROR_MESSAGE);
                }
            }
        };
        worker.execute();
    }

    private boolean sameIsland(IslandRecord a, IslandRecord b) {
        return a != null && b != null && a.seed == b.seed && a.centerX == b.centerX && a.centerZ == b.centerZ
                && a.run.folder.equals(b.run.folder);
    }

    private void showPreview(Path path) {
        if (path == null || !Files.isRegularFile(path)) {
            previewLabel.setIcon(null);
            previewLabel.setText("No preview saved");
            return;
        }
        ImageIcon icon = new ImageIcon(path.toString());
        if (icon.getIconWidth() <= 0) {
            previewLabel.setIcon(null);
            previewLabel.setText("Preview could not be loaded");
            return;
        }
        int maxW = Math.max(300, previewLabel.getWidth() - 16);
        int maxH = Math.max(190, previewLabel.getHeight() - 16);
        double scale = Math.min(maxW / (double) icon.getIconWidth(), maxH / (double) icon.getIconHeight());
        int w = Math.max(1, (int) Math.round(icon.getIconWidth() * scale));
        int h = Math.max(1, (int) Math.round(icon.getIconHeight() * scale));
        previewLabel.setText("");
        previewLabel.setIcon(new ImageIcon(icon.getImage().getScaledInstance(w, h, Image.SCALE_FAST)));
    }

    private void selectForcedIsland() {
        IslandRecord target = forcedIsland;
        forcedIsland = null;
        if (target == null) return;
        for (int modelRow = 0; modelRow < tableModel.getRowCount(); modelRow++) {
            IslandRecord current = tableModel.getIsland(modelRow);
            if (sameIsland(current, target)) {
                int viewRow = table.convertRowIndexToView(modelRow);
                if (viewRow >= 0) {
                    table.setRowSelectionInterval(viewRow, viewRow);
                    table.scrollRectToVisible(table.getCellRect(viewRow, 0, true));
                }
                return;
            }
        }
    }

    private static void addDocumentListener(JTextField field, Runnable action) {
        field.getDocument().addDocumentListener(new DocumentListener() {
            @Override public void insertUpdate(DocumentEvent e) { action.run(); }
            @Override public void removeUpdate(DocumentEvent e) { action.run(); }
            @Override public void changedUpdate(DocumentEvent e) { action.run(); }
        });
    }

    private class IslandTableModel extends AbstractTableModel {
        private final String[] columns = {
                "Seed", "Blocks", "Coarse", "Date", "Footprint", "Thickness", "Fill %", "Favorite"
        };
        private List<IslandRecord> islands = List.of();

        void setIslands(List<IslandRecord> islands) {
            this.islands = List.copyOf(islands);
            fireTableDataChanged();
        }
        IslandRecord getIsland(int row) { return row >= 0 && row < islands.size() ? islands.get(row) : null; }
        @Override public int getRowCount() { return islands.size(); }
        @Override public int getColumnCount() { return columns.length; }
        @Override public String getColumnName(int column) { return columns[column]; }
        @Override public Class<?> getColumnClass(int column) {
            return switch (column) {
                case 0 -> Long.class;
                case 1, 2 -> Integer.class;
                case 4 -> FootprintValue.class;
                case 5, 6 -> Double.class;
                case 7 -> Boolean.class;
                default -> String.class;
            };
        }
        @Override public Object getValueAt(int rowIndex, int columnIndex) {
            IslandRecord i = islands.get(rowIndex);
            return switch (columnIndex) {
                case 0 -> i.seed;
                case 1 -> i.blocks;
                case 2 -> i.coarse;
                case 3 -> i.run.dateText();
                case 4 -> new FootprintValue(i.width, i.depth);
                case 5 -> Math.round(i.avgThickness * 100.0) / 100.0;
                case 6 -> Math.round(i.fillPercent * 100.0) / 100.0;
                case 7 -> isFavorite(i);
                default -> "";
            };
        }
    }

    private static final class FootprintValue {
        final int width;
        final int depth;
        final long area;

        FootprintValue(int width, int depth) {
            this.width = Math.max(0, width);
            this.depth = Math.max(0, depth);
            this.area = (long) this.width * this.depth;
        }

        @Override
        public String toString() {
            return String.format("%,d", area);
        }
    }

    private static class ScopeOption {
        final String label;
        final RunRecord run;
        final boolean includedOnly;
        final boolean latest;
        ScopeOption(String label, RunRecord run, boolean includedOnly, boolean latest) {
            this.label = label;
            this.run = run;
            this.includedOnly = includedOnly;
            this.latest = latest;
        }
        String key() {
            if (latest) return "latest";
            if (run != null) return run.folder.toString();
            return includedOnly ? "included" : "all";
        }
        @Override public String toString() { return label; }
    }
}
