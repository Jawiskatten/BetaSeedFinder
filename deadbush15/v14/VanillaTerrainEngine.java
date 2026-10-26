import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

import net.minecraft.SharedConstants;
import net.minecraft.commands.Commands;
import net.minecraft.core.HolderSet;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.HolderGetter;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.permissions.PermissionSet;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.NoiseColumn;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.FeatureSorter;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.dimension.LevelStem;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.presets.WorldPresets;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;
import net.minecraft.world.level.levelgen.synth.NormalNoise;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.core.registries.BuiltInRegistries;

/**
 * Persistent vanilla 26.3 registry and NoiseBasedChunkGenerator probe.
 * This loads the built-in data registries in-process; it does not start a server.
 * getBaseHeight/getBaseColumn are exact vanilla noise terrain, but exclude
 * structure and feature mutations, so annotations are not bush-placement proof.
 */
public final class VanillaTerrainEngine {
    private static final String TARGET = "minecraft:patch_dead_bush_badlands";

    public static void main(String[] args) throws Exception {
        if (args.length != 3 || !args[0].equals("--annotate")) {
            System.err.println("Usage: VanillaTerrainEngine --annotate input.csv output.csv");
            System.exit(2);
        }
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        var repository = ServerPacksSource.createVanillaTrustedRepository();
        repository.reload();
        var packConfig = new WorldLoader.PackConfig(repository, WorldDataConfiguration.DEFAULT, false, false);
        var init = new WorldLoader.InitConfig(packConfig, Commands.CommandSelection.DEDICATED,
                PermissionSet.ALL_PERMISSIONS);

        WorldLoader.load(init,
                context -> new WorldLoader.DataLoadOutput<>(context.datapackWorldRegistries(), context.datapackDimensions()),
                (resources, serverResources, layers, ignored) -> {
                    try {
                        Engine engine = new Engine(layers.compositeAccess(), ignored);
                        System.out.println("FeatureSorter mapping: " + TARGET + " step=" + engine.featureStep
                                + " index=" + engine.featureIndex + "; vanilla registries loaded; no server started.");
                        annotate(Path.of(args[1]), Path.of(args[2]), engine);
                        return Boolean.TRUE;
                    } catch (Exception error) {
                        throw new RuntimeException("V14 terrain annotation failed", error);
                    } finally {
                        resources.close();
                    }
                }, Runnable::run, Runnable::run).join();
    }

    private static final class Engine {
        final ChunkGenerator generator;
        final LevelHeightAccessor heightAccessor;
        final HolderGetter<NormalNoise> noises;
        final Registry<net.minecraft.world.level.levelgen.placement.PlacedFeature> placedFeatures;
        final int featureStep;
        final int featureIndex;
        long lastSeed = Long.MIN_VALUE;
        RandomState randomState;

        Engine(RegistryAccess access, HolderLookup.Provider worldgenRegistries) {
            this.placedFeatures = access.lookupOrThrow(Registries.PLACED_FEATURE);
            LevelStem overworld = WorldPresets.getNormalOverworld(worldgenRegistries);
            this.generator = overworld.generator();
            this.heightAccessor = LevelHeightAccessor.create(overworld.type().value().minY(),
                    overworld.type().value().height());
            this.noises = worldgenRegistries.lookupOrThrow(Registries.NOISE);

            int foundStep = -1, foundIndex = -1;
            List<Biome> normalOverworldBiomes = ((net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator) generator)
                    .getBiomeSource().possibleBiomes().stream().map(holder -> holder.value()).toList();
            List<FeatureSorter.StepFeatureData> steps = FeatureSorter.buildFeaturesPerStep(
                    normalOverworldBiomes, biome -> biome.getGenerationSettings().features(), false);
            for (int step = 0; step < steps.size(); step++) {
                var data = steps.get(step);
                for (PlacedFeature feature : data.features()) {
                    if (TARGET.equals(String.valueOf(placedFeatures.getKey(feature)))) {
                        foundStep = step;
                        foundIndex = data.indexMapping().applyAsInt(feature);
                    }
                }
            }
            if (foundIndex < 0) throw new IllegalStateException("FeatureSorter omitted " + TARGET);
            this.featureStep = foundStep;
            this.featureIndex = foundIndex;
        }

        String column(long seed, int x, int z) {
            if (seed != lastSeed) {
                NoiseGeneratorSettings settings = ((net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator) generator)
                        .generatorSettings().value();
                randomState = RandomState.create(noises, seed, settings);
                lastSeed = seed;
            }
            int height = generator.getBaseHeight(x, z, Heightmap.Types.WORLD_SURFACE_WG,
                    heightAccessor, randomState);
            NoiseColumn column = generator.getBaseColumn(x, z, heightAccessor, randomState);
            BlockState support = height > heightAccessor.getMinY()
                    ? column.getBlock(height - 1) : net.minecraft.world.level.block.Blocks.AIR.defaultBlockState();
            BlockState atHeight = height < heightAccessor.getMaxY()
                    ? column.getBlock(height) : net.minecraft.world.level.block.Blocks.AIR.defaultBlockState();
            int scanned = heightAccessor.getMaxY() - 1;
            while (scanned >= heightAccessor.getMinY()
                    && !Heightmap.Types.WORLD_SURFACE_WG.isOpaque().test(column.getBlock(scanned))) scanned--;
            int scannedHeight = scanned + 1;
            return height + ":" + blockName(support) + ":" + blockName(atHeight) + ":" + scannedHeight;
        }

        private static String blockName(BlockState state) {
            return String.valueOf(BuiltInRegistries.BLOCK.getKey(state.getBlock()));
        }
    }

    private static void annotate(Path input, Path output, Engine engine) throws Exception {
        try (BufferedReader reader = Files.newBufferedReader(input, StandardCharsets.UTF_8);
             BufferedWriter writer = Files.newBufferedWriter(output, StandardCharsets.UTF_8)) {
            String header = reader.readLine();
            if (header == null) throw new IllegalArgumentException("empty candidate CSV");
            List<String> names = parseCsv(header);
            int seedCol = names.indexOf("seed_signed"), xCol = names.indexOf("start_x");
            int zCol = names.indexOf("start_z"), dxCol = names.indexOf("dx");
            int dzCol = names.indexOf("dz"), runCol = names.indexOf("run");
            if (seedCol < 0 || xCol < 0 || zCol < 0 || dxCol < 0 || dzCol < 0 || runCol < 0)
                throw new IllegalArgumentException("candidate CSV lacks seed/row columns");
            writer.write(header + ",vanilla_base_column_sample_count,vanilla_base_columns,vanilla_noise_scope\n");
            String row;
            while ((row = reader.readLine()) != null) {
                if (row.isBlank()) continue;
                List<String> f = parseCsv(row);
                long seed = Long.parseLong(f.get(seedCol));
                int x = Integer.parseInt(f.get(xCol)), z = Integer.parseInt(f.get(zCol));
                int dx = Integer.parseInt(f.get(dxCol)), dz = Integer.parseInt(f.get(dzCol));
                int count = Integer.parseInt(f.get(runCol));
                int sampleCount = Math.min(count, 15);
                List<String> columns = new ArrayList<>(sampleCount);
                for (int i = 0; i < sampleCount; i++) columns.add(engine.column(seed, x + i * dx, z + i * dz));
                writer.write(row + "," + sampleCount + ",\"" + String.join("|", columns)
                        + "\",VANILLA_NOISE_BASE_COLUMN_EXACT_NOT_POST_FEATURES\n");
            }
        }
    }

    private static List<String> parseCsv(String line) {
        List<String> values = new ArrayList<>();
        StringBuilder value = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (c == '"') {
                if (quoted && i + 1 < line.length() && line.charAt(i + 1) == '"') {
                    value.append('"'); i++;
                } else quoted = !quoted;
            } else if (c == ',' && !quoted) {
                values.add(value.toString()); value.setLength(0);
            } else value.append(c);
        }
        values.add(value.toString());
        return values;
    }
}
