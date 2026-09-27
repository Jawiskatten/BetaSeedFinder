package beta173;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.GZIPInputStream;
import java.util.zip.InflaterInputStream;

/**
 * Reads actual Beta 1.7.3 McRegion (.mcr) chunk data and audits mob-spawner
 * coordinates against DungeonClusterFinder173's isolated-population prediction.
 *
 * This deliberately reads the saved world's Blocks byte arrays instead of
 * reimplementing generation. It is the parity oracle for debugging population
 * order / terrain-state mismatches.
 */
public final class BetaWorldDungeonAudit173 {
    private static final int SPAWNER = 52;
    private static final int BLOCKS_PER_CHUNK = 16 * 16 * 128;

    private BetaWorldDungeonAudit173() {}

    public static void main(String[] args) throws Exception {
        Config config;
        try {
            config = Config.parse(args);
        } catch (IllegalArgumentException ex) {
            System.err.println(ex.getMessage());
            printUsage();
            System.exit(2);
            return;
        }

        if (config.help) {
            printUsage();
            return;
        }

        if (config.world == null) {
            throw new IllegalArgumentException("--world is required");
        }

        Path regionDir = resolveRegionDir(config.world);
        System.out.println("world=" + config.world.toAbsolutePath());
        System.out.println("regionDir=" + regionDir.toAbsolutePath());
        System.out.printf(Locale.ROOT,
                "scan centerChunk=(%d,%d) radius=%d%n",
                config.centerChunkX, config.centerChunkZ, config.chunkRadius);

        List<SpawnerPos> actual = new ArrayList<>();
        int presentChunks = 0;
        int missingChunks = 0;

        for (int chunkX = config.centerChunkX - config.chunkRadius;
             chunkX <= config.centerChunkX + config.chunkRadius; ++chunkX) {
            for (int chunkZ = config.centerChunkZ - config.chunkRadius;
                 chunkZ <= config.centerChunkZ + config.chunkRadius; ++chunkZ) {
                ChunkData chunk = readChunk(regionDir, chunkX, chunkZ);
                if (chunk == null) {
                    ++missingChunks;
                    continue;
                }
                ++presentChunks;
                scanSpawners(chunkX, chunkZ, chunk.blocks, actual);
            }
        }

        actual.sort((a, b) -> {
            int c = Integer.compare(a.x, b.x);
            if (c != 0) return c;
            c = Integer.compare(a.z, b.z);
            if (c != 0) return c;
            return Integer.compare(a.y, b.y);
        });

        System.out.printf(Locale.ROOT,
                "actual chunks[present=%d missing=%d] spawners=%d%n",
                presentChunks, missingChunks, actual.size());

        if (actual.isEmpty()) {
            System.out.println("  ACTUAL: no mob spawners found in generated chunks inside scan radius");
        } else {
            for (SpawnerPos pos : actual) {
                System.out.printf(Locale.ROOT,
                        "  ACTUAL spawner=(%d,%d,%d) chunk=(%d,%d)%n",
                        pos.x, pos.y, pos.z,
                        Math.floorDiv(pos.x, 16), Math.floorDiv(pos.z, 16));
            }
        }

        if (config.seed != null) {
            comparePrediction(config, actual);
        }
    }

    private static void comparePrediction(Config config, List<SpawnerPos> actual) {
        long seed = config.seed.longValue();
        BetaChunk173 generator = new BetaChunk173(seed);
        DungeonClusterFinder173.Analysis analysis = DungeonClusterFinder173.analyze(
                seed,
                config.populationChunkX,
                config.populationChunkZ,
                2,
                generator);

        Set<SpawnerPos> actualSet = new HashSet<>(actual);
        Set<SpawnerPos> predictedSet = new HashSet<>();

        System.out.printf(Locale.ROOT,
                "%nisolated prediction seed=%d populationChunk=(%d,%d) generated=%d surviving=%d%n",
                seed, config.populationChunkX, config.populationChunkZ,
                analysis.generated.size(), analysis.surviving.size());

        for (DungeonClusterFinder173.DungeonAttempt attempt : analysis.attempts) {
            boolean survives = attempt.room != null && analysis.surviving.stream().anyMatch(r ->
                    r.centerX == attempt.centerX
                            && r.centerY == attempt.centerY
                            && r.centerZ == attempt.centerZ);

            String result = attempt.room == null
                    ? "FAIL[" + attempt.failureReason + "]"
                    : (survives ? "SUCCESS_SURVIVES" : "SUCCESS_OVERWRITTEN");

            System.out.printf(Locale.ROOT,
                    "  PRED attempt=%d candidate=(%d,%d,%d) %s actualBlock=%s%n",
                    attempt.attempt,
                    attempt.centerX, attempt.centerY, attempt.centerZ,
                    result,
                    actualSet.contains(new SpawnerPos(
                            attempt.centerX, attempt.centerY, attempt.centerZ))
                            ? "SPAWNER" : "not-spawner");
        }

        for (DungeonClusterFinder173.DungeonRoom room : analysis.surviving) {
            predictedSet.add(new SpawnerPos(room.centerX, room.centerY, room.centerZ));
        }

        Set<SpawnerPos> matched = new HashSet<>(predictedSet);
        matched.retainAll(actualSet);

        Set<SpawnerPos> predictedOnly = new HashSet<>(predictedSet);
        predictedOnly.removeAll(actualSet);

        Set<SpawnerPos> actualOnly = new HashSet<>(actualSet);
        actualOnly.removeAll(predictedSet);

        System.out.printf(Locale.ROOT,
                "%nparity matched=%d predictedOnly=%d actualOnly=%d%n",
                matched.size(), predictedOnly.size(), actualOnly.size());

        for (SpawnerPos p : predictedOnly) {
            System.out.printf(Locale.ROOT,
                    "  MISMATCH predicted-only spawner=(%d,%d,%d)%n",
                    p.x, p.y, p.z);
        }
        for (SpawnerPos p : actualOnly) {
            System.out.printf(Locale.ROOT,
                    "  MISMATCH actual-only spawner=(%d,%d,%d)%n",
                    p.x, p.y, p.z);
        }

        if (predictedOnly.isEmpty() && !predictedSet.isEmpty()) {
            System.out.println("isolated predicted surviving spawners all exist in the saved world.");
        } else if (!predictedOnly.isEmpty()) {
            System.out.println("PARITY FAILURE: at least one isolated predicted spawner is absent from the saved world.");
        }
    }

    private static Path resolveRegionDir(Path world) {
        Path direct = world.resolve("region");
        if (Files.isDirectory(direct)) return direct;

        if (world.getFileName() != null
                && world.getFileName().toString().equalsIgnoreCase("region")
                && Files.isDirectory(world)) {
            return world;
        }

        throw new IllegalArgumentException(
                "Could not find Beta McRegion folder. Expected <world>\\region containing r.*.*.mcr: "
                        + world.toAbsolutePath());
    }

    private static ChunkData readChunk(Path regionDir, int chunkX, int chunkZ) throws IOException {
        int regionX = Math.floorDiv(chunkX, 32);
        int regionZ = Math.floorDiv(chunkZ, 32);
        int localX = Math.floorMod(chunkX, 32);
        int localZ = Math.floorMod(chunkZ, 32);

        Path region = regionDir.resolve("r." + regionX + "." + regionZ + ".mcr");
        if (!Files.isRegularFile(region)) return null;

        int tableIndex = localX + localZ * 32;
        long locationOffset = (long) tableIndex * 4L;

        try (RandomAccessFile raf = new RandomAccessFile(region.toFile(), "r")) {
            if (raf.length() < 4096L) return null;

            raf.seek(locationOffset);
            int b0 = raf.readUnsignedByte();
            int b1 = raf.readUnsignedByte();
            int b2 = raf.readUnsignedByte();
            int sectors = raf.readUnsignedByte();
            int sectorOffset = (b0 << 16) | (b1 << 8) | b2;

            if (sectorOffset == 0 || sectors == 0) return null;

            long chunkOffset = (long) sectorOffset * 4096L;
            if (chunkOffset + 5L > raf.length()) {
                throw new IOException("Invalid region location for chunk "
                        + chunkX + "," + chunkZ + " in " + region);
            }

            raf.seek(chunkOffset);
            int length = raf.readInt();
            if (length <= 1 || length > sectors * 4096) {
                throw new IOException("Invalid compressed chunk length " + length
                        + " for " + chunkX + "," + chunkZ);
            }

            int compression = raf.readUnsignedByte();
            byte[] compressed = new byte[length - 1];
            raf.readFully(compressed);

            InputStream raw = new java.io.ByteArrayInputStream(compressed);
            InputStream decompressed;
            if (compression == 1) {
                decompressed = new GZIPInputStream(raw);
            } else if (compression == 2) {
                decompressed = new InflaterInputStream(raw);
            } else {
                throw new IOException("Unsupported McRegion compression type "
                        + compression + " for chunk " + chunkX + "," + chunkZ);
            }

            try (DataInputStream in = new DataInputStream(new BufferedInputStream(decompressed))) {
                NbtCapture capture = new NbtCapture();
                int rootType;
                try {
                    rootType = in.readUnsignedByte();
                } catch (EOFException ex) {
                    return null;
                }
                if (rootType == 0) return null;
                readString(in); // root name
                readPayload(in, rootType, capture);

                if (capture.blocks == null) {
                    throw new IOException("Chunk " + chunkX + "," + chunkZ
                            + " has no Blocks byte array");
                }
                if (capture.blocks.length != BLOCKS_PER_CHUNK) {
                    throw new IOException("Unexpected Blocks length "
                            + capture.blocks.length + " for chunk " + chunkX + "," + chunkZ);
                }
                return new ChunkData(capture.blocks);
            }
        }
    }

    private static void scanSpawners(
            int chunkX, int chunkZ, byte[] blocks, List<SpawnerPos> out) {
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                int base = (x * 16 + z) * 128;
                for (int y = 0; y < 128; ++y) {
                    if ((blocks[base + y] & 0xFF) == SPAWNER) {
                        out.add(new SpawnerPos(
                                chunkX * 16 + x,
                                y,
                                chunkZ * 16 + z));
                    }
                }
            }
        }
    }

    private static void readPayload(
            DataInputStream in, int type, NbtCapture capture) throws IOException {
        switch (type) {
            case 1:
                in.readByte();
                return;
            case 2:
                in.readShort();
                return;
            case 3:
                in.readInt();
                return;
            case 4:
                in.readLong();
                return;
            case 5:
                in.readFloat();
                return;
            case 6:
                in.readDouble();
                return;
            case 7: {
                int length = in.readInt();
                if (length < 0) throw new IOException("Negative NBT byte-array length");
                byte[] data = new byte[length];
                in.readFully(data);
                return;
            }
            case 8:
                readString(in);
                return;
            case 9: {
                int elementType = in.readUnsignedByte();
                int length = in.readInt();
                if (length < 0) throw new IOException("Negative NBT list length");
                for (int i = 0; i < length; ++i) {
                    readPayload(in, elementType, capture);
                }
                return;
            }
            case 10:
                readCompound(in, capture);
                return;
            case 11: {
                int length = in.readInt();
                if (length < 0) throw new IOException("Negative NBT int-array length");
                for (int i = 0; i < length; ++i) in.readInt();
                return;
            }
            case 12: {
                int length = in.readInt();
                if (length < 0) throw new IOException("Negative NBT long-array length");
                for (int i = 0; i < length; ++i) in.readLong();
                return;
            }
            default:
                throw new IOException("Unknown NBT tag type " + type);
        }
    }

    private static void readCompound(DataInputStream in, NbtCapture capture) throws IOException {
        while (true) {
            int type = in.readUnsignedByte();
            if (type == 0) return;

            String name = readString(in);
            if (type == 7 && name.equals("Blocks")) {
                int length = in.readInt();
                if (length < 0) throw new IOException("Negative Blocks length");
                byte[] data = new byte[length];
                in.readFully(data);
                if (length == BLOCKS_PER_CHUNK) {
                    capture.blocks = data;
                }
                continue;
            }

            readPayload(in, type, capture);
        }
    }

    private static String readString(DataInputStream in) throws IOException {
        return in.readUTF();
    }

    private static void printUsage() {
        System.out.println("Beta 1.7.3 saved-world dungeon auditor");
        System.out.println();
        System.out.println("Scan actual mob spawners from a fresh Beta world:");
        System.out.println("  java -cp build/java/classes beta173.BetaWorldDungeonAudit173 --world \"C:\\path\\to\\World1\" --chunk-radius 3");
        System.out.println();
        System.out.println("Compare saved world against isolated prediction:");
        System.out.println("  java -cp build/java/classes beta173.BetaWorldDungeonAudit173 --world \"C:\\path\\to\\World1\" --seed 501789 --chunk-radius 3");
        System.out.println();
        System.out.println("Options:");
        System.out.println("  --world <path>          Beta world folder (must contain region\\r.*.*.mcr)");
        System.out.println("  --seed <long>           also compare isolated population prediction");
        System.out.println("  --center-chunk-x <int>  actual-world scan center chunk X (default 0)");
        System.out.println("  --center-chunk-z <int>  actual-world scan center chunk Z (default 0)");
        System.out.println("  --chunk-radius <int>    scan radius in chunks (default 3; 0..64)");
        System.out.println("  --pop-chunk-x <int>     predicted population chunk X (default 0)");
        System.out.println("  --pop-chunk-z <int>     predicted population chunk Z (default 0)");
        System.out.println("  --help                  show this text");
    }

    private static final class NbtCapture {
        byte[] blocks;
    }

    private static final class ChunkData {
        final byte[] blocks;

        ChunkData(byte[] blocks) {
            this.blocks = blocks;
        }
    }

    static final class SpawnerPos {
        final int x;
        final int y;
        final int z;

        SpawnerPos(int x, int y, int z) {
            this.x = x;
            this.y = y;
            this.z = z;
        }

        @Override
        public boolean equals(Object obj) {
            if (this == obj) return true;
            if (!(obj instanceof SpawnerPos)) return false;
            SpawnerPos other = (SpawnerPos) obj;
            return x == other.x && y == other.y && z == other.z;
        }

        @Override
        public int hashCode() {
            int h = x;
            h = 31 * h + y;
            h = 31 * h + z;
            return h;
        }
    }

    private static final class Config {
        Path world;
        Long seed;
        int centerChunkX = 0;
        int centerChunkZ = 0;
        int chunkRadius = 3;
        int populationChunkX = 0;
        int populationChunkZ = 0;
        boolean help;

        static Config parse(String[] args) {
            Config c = new Config();
            for (int i = 0; i < args.length; ++i) {
                String arg = args[i];
                switch (arg) {
                    case "--world":
                        c.world = Paths.get(requireValue(args, ++i, arg));
                        break;
                    case "--seed":
                        c.seed = Long.parseLong(requireValue(args, ++i, arg));
                        break;
                    case "--center-chunk-x":
                        c.centerChunkX = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--center-chunk-z":
                        c.centerChunkZ = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--chunk-radius":
                        c.chunkRadius = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--pop-chunk-x":
                        c.populationChunkX = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--pop-chunk-z":
                        c.populationChunkZ = Integer.parseInt(requireValue(args, ++i, arg));
                        break;
                    case "--help":
                    case "-h":
                        c.help = true;
                        break;
                    default:
                        throw new IllegalArgumentException("Unknown option: " + arg);
                }
            }

            if (c.chunkRadius < 0 || c.chunkRadius > 64) {
                throw new IllegalArgumentException("--chunk-radius must be 0..64");
            }
            return c;
        }

        private static String requireValue(String[] args, int index, String option) {
            if (index >= args.length) {
                throw new IllegalArgumentException("Missing value for " + option);
            }
            return args[index];
        }
    }
}
