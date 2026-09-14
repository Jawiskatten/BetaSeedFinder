package net.minecraft.src;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Exact Beta 1.7.3 CLIENT-startup verifier for the "ghost floor" hypothesis.
 *
 * The GPU prefilter predicts a water lake from population chunk (-1,-1) whose
 * upper half can carve the sand block that originally made (0,0) a valid spawn.
 * This oracle then uses the actual Beta client World + ChunkProviderLoadOrGenerate
 * classes and reproduces Minecraft.func_6255_d's 17x17 "Building terrain" chunk
 * access order before measuring the player's real preparePlayerToSpawn position.
 */
public final class Beta173GhostFloorOracle {
    private static final int AIR = 0;
    private static final int WATER_MOVING = 8;
    private static final int WATER_STILL = 9;
    private static final int LAVA_MOVING = 10;
    private static final int LAVA_STILL = 11;

    private static final String HEADER =
        "status,seed,sequence_index,pre_gate_y,pre_gate_id,pre_feet_y,pre_obstacle_type,pre_obstacle_y,pre_air_drop," +
        "post_gate_id,gate_destroyed,post_feet_y,post_obstacle_type,post_obstacle_y,post_support_y,post_air_drop," +
        "ghost_floor,freefall_3,freefall_10,freefall_20,lethal_estimate," +
        "water_attempt_x,water_attempt_y,water_attempt_z,predicted_gate_local_y,predicted_mask_bits,dry_carve_predicted,error";

    private static final class NullChunkLoader implements IChunkLoader {
        public Chunk loadChunk(World world, int x, int z) throws IOException { return null; }
        public void saveChunk(World world, Chunk chunk) throws IOException {}
        public void saveExtraChunkData(World world, Chunk chunk) throws IOException {}
        public void chunkTick() {}
        public void saveExtraData() {}
    }

    private static final class MemorySaveHandler implements ISaveHandler {
        private final IChunkLoader loader = new NullChunkLoader();
        public WorldInfo loadWorldInfo() { return null; }
        public void checkSessionLock() {}
        public IChunkLoader getChunkLoader(WorldProvider provider) { return loader; }
        public void saveWorldInfoAndPlayer(WorldInfo info, List players) {}
        public void saveWorldInfo(WorldInfo info) {}
        public File getMapFile(String name) { return null; }
    }

    private static final class Config {
        File input;
        File outputDir;
        int progressEvery = 10;
        int maxSeeds = 0;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        int attemptX, attemptY, attemptZ;
        int predictedGateLocalY;
        int predictedMaskBits;
        int dryCarvePredicted;
    }

    private static final class Obstacle {
        String type = "VOID";
        int y = -1;
        int supportY = -1;
        double topY = 0.0;
        double drop = 999.0;
    }

    private static final class Result {
        String status = "OK";
        String error = "";
        Candidate c;
        int preGateY = -1, preGateId = -1;
        int preFeetY = -1;
        Obstacle pre = new Obstacle();
        int postGateId = -1;
        boolean gateDestroyed;
        int postFeetY = -1;
        Obstacle post = new Obstacle();
        boolean ghostFloor;
        boolean free3, free10, free20, lethal;

        String toCsv() {
            return joinCsv(new String[] {
                status, Long.toString(c.seed), Long.toString(c.sequenceIndex),
                Integer.toString(preGateY), Integer.toString(preGateId), Integer.toString(preFeetY),
                pre.type, Integer.toString(pre.y), f2(pre.drop),
                Integer.toString(postGateId), bit(gateDestroyed), Integer.toString(postFeetY),
                post.type, Integer.toString(post.y), Integer.toString(post.supportY), f2(post.drop),
                bit(ghostFloor), bit(free3), bit(free10), bit(free20), bit(lethal),
                Integer.toString(c.attemptX), Integer.toString(c.attemptY), Integer.toString(c.attemptZ),
                Integer.toString(c.predictedGateLocalY), Integer.toString(c.predictedMaskBits), Integer.toString(c.dryCarvePredicted), error
            });
        }
    }

    private static String bit(boolean b) { return b ? "1" : "0"; }
    private static String f2(double x) { return String.format(java.util.Locale.ROOT, "%.2f", x); }

    private static String joinCsv(String[] values) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < values.length; ++i) {
            if (i != 0) sb.append(',');
            String s = values[i] == null ? "" : values[i];
            if (s.indexOf(',') >= 0 || s.indexOf('"') >= 0 || s.indexOf('\n') >= 0 || s.indexOf('\r') >= 0) {
                sb.append('"').append(s.replace("\"", "\"\"")).append('"');
            } else sb.append(s);
        }
        return sb.toString();
    }

    private static List<String> splitCsv(String line) {
        ArrayList<String> out = new ArrayList<String>();
        StringBuilder cur = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); ++i) {
            char ch = line.charAt(i);
            if (quoted) {
                if (ch == '"') {
                    if (i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); ++i; }
                    else quoted = false;
                } else cur.append(ch);
            } else {
                if (ch == '"') quoted = true;
                else if (ch == ',') { out.add(cur.toString()); cur.setLength(0); }
                else cur.append(ch);
            }
        }
        out.add(cur.toString());
        return out;
    }

    private static Config parseArgs(String[] args) {
        Config c = new Config();
        for (int i = 0; i < args.length; ++i) {
            String a = args[i];
            if ("--input".equals(a)) c.input = new File(args[++i]);
            else if ("--output".equals(a)) c.outputDir = new File(args[++i]);
            else if ("--progress-every".equals(a)) c.progressEvery = Integer.parseInt(args[++i]);
            else if ("--max-seeds".equals(a)) c.maxSeeds = Integer.parseInt(args[++i]);
            else throw new IllegalArgumentException("unknown argument: " + a);
        }
        if (c.input == null) throw new IllegalArgumentException("--input is required");
        if (c.outputDir == null) throw new IllegalArgumentException("--output is required");
        return c;
    }

    private static List<Candidate> readCandidates(File file) throws Exception {
        BufferedReader br = new BufferedReader(new FileReader(file));
        String header = br.readLine();
        if (header == null) throw new IllegalArgumentException("empty input CSV");
        List<String> h = splitCsv(header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < h.size(); ++i) ix.put(h.get(i), Integer.valueOf(i));
        String[] required = {"seed","sequence_index","water_attempt_x","water_attempt_y","water_attempt_z","origin_lake_local_y","mask_bits","dry_carve_predicted"};
        for (String r : required) if (!ix.containsKey(r)) throw new IllegalArgumentException("missing input column: " + r);
        ArrayList<Candidate> out = new ArrayList<Candidate>();
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            try {
                Candidate c = new Candidate();
                c.seed = Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex = Long.parseLong(x.get(ix.get("sequence_index").intValue()));
                c.attemptX = Integer.parseInt(x.get(ix.get("water_attempt_x").intValue()));
                c.attemptY = Integer.parseInt(x.get(ix.get("water_attempt_y").intValue()));
                c.attemptZ = Integer.parseInt(x.get(ix.get("water_attempt_z").intValue()));
                c.predictedGateLocalY = Integer.parseInt(x.get(ix.get("origin_lake_local_y").intValue()));
                c.predictedMaskBits = Integer.parseInt(x.get(ix.get("mask_bits").intValue()));
                c.dryCarvePredicted = Integer.parseInt(x.get(ix.get("dry_carve_predicted").intValue()));
                out.add(c);
            } catch (RuntimeException ignored) {}
        }
        br.close();
        // Verify the most promising dry-carve candidates first.
        Collections.sort(out, new Comparator<Candidate>() {
            public int compare(Candidate a, Candidate b) {
                if (a.dryCarvePredicted != b.dryCarvePredicted) return b.dryCarvePredicted - a.dryCarvePredicted;
                return a.sequenceIndex < b.sequenceIndex ? -1 : (a.sequenceIndex == b.sequenceIndex ? 0 : 1);
            }
        });
        return out;
    }

    private static Set<Long> readProcessed(File master) throws Exception {
        HashSet<Long> out = new HashSet<Long>();
        if (!master.isFile()) return out;
        BufferedReader br = new BufferedReader(new FileReader(master));
        String header = br.readLine();
        if (header == null) { br.close(); return out; }
        List<String> h = splitCsv(header);
        int s = h.indexOf("sequence_index");
        String line;
        while ((line = br.readLine()) != null) {
            List<String> x = splitCsv(line);
            if (s >= 0 && s < x.size()) {
                try { out.add(Long.valueOf(Long.parseLong(x.get(s)))); } catch (RuntimeException ignored) {}
            }
        }
        br.close();
        return out;
    }

    private static int firstUncoveredY(World world, int x, int z) {
        int y = 63;
        while (y + 1 < 128 && !world.isAirBlock(x, y + 1, z)) ++y;
        return y;
    }

    private static boolean collidesAtFeet(World world, int x, int z, int feetY) {
        AxisAlignedBB player = AxisAlignedBB.getBoundingBox(x + 0.2D, (double)feetY, z + 0.2D,
                                                            x + 0.8D, (double)feetY + 1.8D, z + 0.8D);
        for (int y = Math.max(0, feetY - 1); y <= Math.min(127, feetY + 2); ++y) {
            int id = world.getBlockId(x, y, z);
            if (id == AIR) continue;
            Block b = Block.blocksList[id];
            if (b == null) continue;
            AxisAlignedBB box = b.getCollisionBoundingBoxFromPool(world, x, y, z);
            if (box != null && box.intersectsWith(player)) return true;
        }
        return false;
    }

    private static int actualFeetY(World world, int x, int z) {
        int feet = 65;
        while (feet < 127 && collidesAtFeet(world, x, z, feet)) ++feet;
        return feet;
    }

    private static boolean isWater(int id) { return id == WATER_MOVING || id == WATER_STILL; }
    private static boolean isLava(int id) { return id == LAVA_MOVING || id == LAVA_STILL; }

    private static Obstacle firstObstacleBelow(World world, int x, int z, int feetY) {
        Obstacle o = new Obstacle();
        for (int y = Math.min(127, feetY - 1); y >= 0; --y) {
            int id = world.getBlockId(x, y, z);
            if (id == AIR) continue;
            if (isWater(id)) {
                o.type = "WATER"; o.y = y; o.topY = y + 1.0; o.drop = Math.max(0.0, feetY - o.topY); return o;
            }
            if (isLava(id)) {
                o.type = "LAVA"; o.y = y; o.topY = y + 1.0; o.drop = Math.max(0.0, feetY - o.topY); return o;
            }
            Block b = Block.blocksList[id];
            if (b == null) continue;
            AxisAlignedBB box = b.getCollisionBoundingBoxFromPool(world, x, y, z);
            if (box != null && box.maxY <= (double)feetY + 1.0e-9) {
                o.type = "SOLID"; o.y = y; o.supportY = y; o.topY = box.maxY;
                o.drop = Math.max(0.0, feetY - o.topY); return o;
            }
        }
        o.type = "VOID"; o.y = -1; o.supportY = -1; o.topY = 0.0; o.drop = (double)feetY;
        return o;
    }

    private static void buildTerrainExactlyLikeClient(World world, int spawnX, int spawnZ) {
        IChunkProvider provider = world.getIChunkProvider();
        if (provider instanceof ChunkProviderLoadOrGenerate) {
            ((ChunkProviderLoadOrGenerate)provider).setCurrentChunkOver(spawnX >> 4, spawnZ >> 4);
        }
        // Exact access order from Minecraft.func_6255_d: X outer loop, Z inner.
        for (int dx = -128; dx <= 128; dx += 16) {
            for (int dz = -128; dz <= 128; dz += 16) {
                world.getBlockId(spawnX + dx, 64, spawnZ + dz);
            }
        }
        // In Beta this just asks the chunk provider to unload until stable; with
        // our in-memory loader it preserves the same pre-player call boundary.
        world.func_656_j();
    }

    private static Result verify(Candidate c) {
        Result r = new Result();
        r.c = c;
        try {
            World world = new World(new MemorySaveHandler(), "ghost-floor-oracle", c.seed, (WorldProvider)null);
            ChunkCoordinates sp = world.getSpawnPoint();
            if (sp.x != 0 || sp.z != 0) {
                r.status = "SPAWN_MOVED";
                return r;
            }

            r.preGateY = firstUncoveredY(world, 0, 0);
            r.preGateId = world.getBlockId(0, r.preGateY, 0);
            if (r.preGateId != Block.sand.blockID) {
                r.status = "PRE_GATE_NOT_SAND";
                return r;
            }
            r.preFeetY = actualFeetY(world, 0, 0);
            r.pre = firstObstacleBelow(world, 0, 0, r.preFeetY);

            buildTerrainExactlyLikeClient(world, 0, 0);

            r.postGateId = world.getBlockId(0, r.preGateY, 0);
            r.gateDestroyed = r.postGateId != Block.sand.blockID;
            r.postFeetY = actualFeetY(world, 0, 0);
            r.post = firstObstacleBelow(world, 0, 0, r.postFeetY);

            r.ghostFloor = r.gateDestroyed && r.pre.drop <= 1.01 && r.post.drop >= 3.0;
            r.free3 = r.post.drop >= 3.0;
            r.free10 = r.post.drop >= 10.0;
            r.free20 = r.post.drop >= 20.0;
            r.lethal = "SOLID".equals(r.post.type) && r.post.drop >= 23.0;

            if (r.gateDestroyed || r.free3) {
                System.out.println("GHOST CHECK seed=" + c.seed + " gateY=" + r.preGateY +
                    " gate " + r.preGateId + "->" + r.postGateId + " feet=" + r.postFeetY +
                    " drop=" + f2(r.post.drop) + " into=" + r.post.type + " ghost=" + bit(r.ghostFloor));
            }
        } catch (Throwable t) {
            r.status = "ERROR";
            r.error = t.getClass().getName() + ": " + String.valueOf(t.getMessage());
            t.printStackTrace(System.err);
        }
        return r;
    }

    private static final class RawRow {
        String line;
        List<String> x;
        double drop;
    }

    private static void writeRows(File file, String header, List<RawRow> rows, int limit) throws Exception {
        PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(file, false)));
        pw.println(header);
        int n = limit <= 0 ? rows.size() : Math.min(limit, rows.size());
        for (int i = 0; i < n; ++i) pw.println(rows.get(i).line);
        pw.close();
    }

    private static void rebuildLists(File master, File outDir) throws Exception {
        BufferedReader br = new BufferedReader(new FileReader(master));
        String header = br.readLine();
        if (header == null) { br.close(); return; }
        List<String> h = splitCsv(header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < h.size(); ++i) ix.put(h.get(i), Integer.valueOf(i));
        ArrayList<RawRow> all = new ArrayList<RawRow>();
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            RawRow rr = new RawRow(); rr.line = line; rr.x = x;
            try { rr.drop = Double.parseDouble(x.get(ix.get("post_air_drop").intValue())); } catch (Exception e) { rr.drop = -1.0; }
            all.add(rr);
        }
        br.close();
        Collections.sort(all, new Comparator<RawRow>() {
            public int compare(RawRow a, RawRow b) { return a.drop > b.drop ? -1 : (a.drop < b.drop ? 1 : 0); }
        });

        ArrayList<RawRow> gate = new ArrayList<RawRow>();
        ArrayList<RawRow> ghost = new ArrayList<RawRow>();
        ArrayList<RawRow> f3 = new ArrayList<RawRow>();
        ArrayList<RawRow> f10 = new ArrayList<RawRow>();
        ArrayList<RawRow> f20 = new ArrayList<RawRow>();
        ArrayList<RawRow> lethal = new ArrayList<RawRow>();
        ArrayList<RawRow> water = new ArrayList<RawRow>();
        ArrayList<RawRow> lava = new ArrayList<RawRow>();
        ArrayList<RawRow> ok = new ArrayList<RawRow>();
        for (RawRow rr : all) {
            List<String> x = rr.x;
            if (!"OK".equals(x.get(ix.get("status").intValue()))) continue;
            ok.add(rr);
            if ("1".equals(x.get(ix.get("gate_destroyed").intValue()))) gate.add(rr);
            if ("1".equals(x.get(ix.get("ghost_floor").intValue()))) ghost.add(rr);
            if ("1".equals(x.get(ix.get("freefall_3").intValue()))) f3.add(rr);
            if ("1".equals(x.get(ix.get("freefall_10").intValue()))) f10.add(rr);
            if ("1".equals(x.get(ix.get("freefall_20").intValue()))) f20.add(rr);
            if ("1".equals(x.get(ix.get("lethal_estimate").intValue()))) lethal.add(rr);
            String type = x.get(ix.get("post_obstacle_type").intValue());
            if (rr.drop >= 3.0 && "WATER".equals(type)) water.add(rr);
            if (rr.drop >= 3.0 && "LAVA".equals(type)) lava.add(rr);
        }
        writeRows(new File(outDir, "gate_destroyed.csv"), header, gate, 0);
        writeRows(new File(outDir, "ghost_floor_any.csv"), header, ghost, 0);
        writeRows(new File(outDir, "freefall_3plus.csv"), header, f3, 0);
        writeRows(new File(outDir, "freefall_10plus.csv"), header, f10, 0);
        writeRows(new File(outDir, "freefall_20plus.csv"), header, f20, 0);
        writeRows(new File(outDir, "lethal_freefall.csv"), header, lethal, 0);
        writeRows(new File(outDir, "freefall_into_water.csv"), header, water, 0);
        writeRows(new File(outDir, "freefall_into_lava.csv"), header, lava, 0);
        writeRows(new File(outDir, "top_longest_drop.csv"), header, ok, 500);

        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(new File(outDir, "SUMMARY.txt"), false)));
        s.println("BETA 1.7.3 GHOST FLOOR P1 EXACT CLIENT-STARTUP SUMMARY");
        s.println("Verified rows: " + all.size());
        s.println("Gate destroyed after spawn selection: " + gate.size());
        s.println("GHOST FLOOR (pre drop <=1, post drop >=3): " + ghost.size());
        s.println("Freefall >=3 blocks: " + f3.size());
        s.println("Freefall >=10 blocks: " + f10.size());
        s.println("Freefall >=20 blocks: " + f20.size());
        s.println("Estimated lethal solid landing >=23 blocks: " + lethal.size());
        s.println("Freefall >=3 into water: " + water.size());
        s.println("Freefall >=3 into lava: " + lava.size());
        s.println();
        s.println("P1 target: water-lake population from chunk (-1,-1), with actual Beta client Building-terrain chunk-load order.");
        s.close();
    }

    public static void main(String[] args) throws Exception {
        Config cfg = parseArgs(args);
        cfg.outputDir.mkdirs();
        File master = new File(cfg.outputDir, "actual_ghost_floor_all.csv");
        if (!master.isFile()) {
            PrintWriter init = new PrintWriter(new BufferedWriter(new FileWriter(master, false)));
            init.println(HEADER); init.close();
        }
        Set<Long> processed = readProcessed(master);
        List<Candidate> candidates = readCandidates(cfg.input);
        PrintWriter append = new PrintWriter(new BufferedWriter(new FileWriter(master, true)));
        long start = System.nanoTime();
        int fresh = 0;
        for (Candidate c : candidates) {
            if (processed.contains(Long.valueOf(c.sequenceIndex))) continue;
            Result r = verify(c);
            append.println(r.toCsv());
            append.flush();
            processed.add(Long.valueOf(c.sequenceIndex));
            ++fresh;
            if (cfg.progressEvery > 0 && fresh % cfg.progressEvery == 0) {
                double sec = (System.nanoTime() - start) / 1.0e9;
                System.out.println("verify progress new=" + fresh + " rate=" + f2(fresh / Math.max(0.001, sec)) + " candidates/s");
            }
            if (cfg.maxSeeds > 0 && fresh >= cfg.maxSeeds) break;
        }
        append.close();
        rebuildLists(master, cfg.outputDir);
        double sec = (System.nanoTime() - start) / 1.0e9;
        System.out.println("VERIFY DONE new=" + fresh + " elapsed=" + f2(sec) + "s");
        System.out.println("Summary: " + new File(cfg.outputDir, "SUMMARY.txt").getAbsolutePath());
    }
}
