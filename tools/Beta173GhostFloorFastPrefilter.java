package net.minecraft.src;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Fast exact Beta 1.7.3 prefilter for the ghost-floor/freefall search.
 *
 * P1 sent every GPU lake/gate candidate through the full 17x17 client
 * "Building terrain" load (289 chunk touches), which is ~1 candidate/s.
 * This class instead recreates only the four chunks needed for population
 * chunk (-1,-1): (0,0), (-1,-1), (-1,0), (0,-1). Those chunks include the
 * real Beta cave generator. Loading the final chunk causes exactly (-1,-1)
 * to populate; an explicit populate call is then an idempotent safety net.
 *
 * Candidates are shortlisted only when the original spawn-gate sand is gone,
 * the first post-population obstacle below spawn is SOLID (not water/lava),
 * and the resulting origin-column fall is at least --min-drop blocks. The
 * slower full client-startup oracle remains authoritative for every shortlisted row.
 */
public final class Beta173GhostFloorFastPrefilter {
    private static final int AIR = 0;
    private static final int WATER_MOVING = 8;
    private static final int WATER_STILL = 9;
    private static final int LAVA_MOVING = 10;
    private static final int LAVA_STILL = 11;

    private static final String RESULT_HEADER =
        "status,seed,sequence_index,dry_mask,pre_gate_y,pre_gate_id,pre_feet_y,pre_obstacle_type,pre_obstacle_y,pre_air_drop," +
        "post_gate_id,gate_destroyed,post_feet_y,post_obstacle_type,post_obstacle_y,post_air_drop,drop_gain," +
        "shortlist,water_attempt_x,water_attempt_y,water_attempt_z,predicted_gate_local_y,predicted_mask_bits,error";

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
        double minDrop = 3.0;
        int progressEvery = 100;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        int attemptX, attemptY, attemptZ;
        int predictedGateLocalY;
        int predictedMaskBits;
        int dryCarvePredicted;
        String rawLine;
    }

    private static final class Obstacle {
        String type = "VOID";
        int y = -1;
        double topY = 0.0;
        double drop = 999.0;
    }

    private static final class Result {
        Candidate c;
        String status = "OK";
        String error = "";
        int preGateY = -1, preGateId = -1, preFeetY = -1;
        Obstacle pre = new Obstacle();
        int postGateId = -1, postFeetY = -1;
        Obstacle post = new Obstacle();
        boolean gateDestroyed;
        boolean shortlist;

        String toCsv() {
            return joinCsv(new String[] {
                status,
                Long.toString(c.seed), Long.toString(c.sequenceIndex), Integer.toString(c.dryCarvePredicted),
                Integer.toString(preGateY), Integer.toString(preGateId), Integer.toString(preFeetY),
                pre.type, Integer.toString(pre.y), f2(pre.drop),
                Integer.toString(postGateId), bit(gateDestroyed), Integer.toString(postFeetY),
                post.type, Integer.toString(post.y), f2(post.drop), f2(post.drop - pre.drop),
                bit(shortlist),
                Integer.toString(c.attemptX), Integer.toString(c.attemptY), Integer.toString(c.attemptZ),
                Integer.toString(c.predictedGateLocalY), Integer.toString(c.predictedMaskBits), error
            });
        }
    }

    private static String bit(boolean b) { return b ? "1" : "0"; }
    private static String f2(double v) { return String.format(java.util.Locale.ROOT, "%.2f", v); }

    private static String joinCsv(String[] values) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < values.length; ++i) {
            if (i != 0) sb.append(',');
            String s = values[i] == null ? "" : values[i];
            if (s.indexOf(',') >= 0 || s.indexOf('"') >= 0 || s.indexOf('\n') >= 0 || s.indexOf('\r') >= 0) {
                sb.append('"').append(s.replace("\"", "\"\"")).append('"');
            } else {
                sb.append(s);
            }
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
            else if ("--min-drop".equals(a)) c.minDrop = Double.parseDouble(args[++i]);
            else if ("--progress-every".equals(a)) c.progressEvery = Integer.parseInt(args[++i]);
            else throw new IllegalArgumentException("unknown argument: " + a);
        }
        if (c.input == null) throw new IllegalArgumentException("--input is required");
        if (c.outputDir == null) throw new IllegalArgumentException("--output is required");
        if (c.minDrop < 0.0 || c.minDrop > 128.0) throw new IllegalArgumentException("--min-drop must be 0..128");
        return c;
    }

    private static final class InputData {
        String header;
        List<Candidate> rows = new ArrayList<Candidate>();
    }

    private static InputData readCandidates(File file) throws Exception {
        InputData d = new InputData();
        BufferedReader br = new BufferedReader(new FileReader(file));
        d.header = br.readLine();
        if (d.header == null) throw new IllegalArgumentException("empty input CSV");
        List<String> h = splitCsv(d.header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < h.size(); ++i) ix.put(h.get(i), Integer.valueOf(i));
        String[] req = {"seed","sequence_index","water_attempt_x","water_attempt_y","water_attempt_z","origin_lake_local_y","mask_bits","dry_carve_predicted"};
        for (String r : req) if (!ix.containsKey(r)) throw new IllegalArgumentException("missing input column: " + r);
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
                c.rawLine = line;
                d.rows.add(c);
            } catch (RuntimeException ignored) {}
        }
        br.close();
        return d;
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
                o.type = "SOLID"; o.y = y; o.topY = box.maxY;
                o.drop = Math.max(0.0, feetY - o.topY); return o;
            }
        }
        o.type = "VOID"; o.y = -1; o.topY = 0.0; o.drop = (double)feetY;
        return o;
    }

    /**
     * Load only the missing three members of the 2x2 population neighborhood.
     * The world constructor's spawn check already loaded chunk (0,0). With this
     * exact order, loading (0,-1) last makes ChunkProvider populate (-1,-1)
     * and no other population chunk has a complete 2x2 neighborhood yet.
     */
    private static void populateOnlyMinusOneMinusOne(World world) {
        world.getChunkFromChunkCoords(-1, -1);
        world.getChunkFromChunkCoords(-1,  0);
        world.getChunkFromChunkCoords( 0, -1);
        // Idempotent if the automatic 2x2 trigger above already populated it.
        world.chunkProvider.populate(world.chunkProvider, -1, -1);
    }

    private static Result verify(Candidate c, double minDrop) {
        Result r = new Result();
        r.c = c;
        if (c.dryCarvePredicted == 0) {
            r.status = "GPU_WET_MASK";
            return r;
        }
        try {
            World world = new World(new MemorySaveHandler(), "ghost-floor-fast", c.seed, (WorldProvider)null);
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

            populateOnlyMinusOneMinusOne(world);

            r.postGateId = world.getBlockId(0, r.preGateY, 0);
            r.gateDestroyed = r.postGateId != Block.sand.blockID;
            r.postFeetY = actualFeetY(world, 0, 0);
            r.post = firstObstacleBelow(world, 0, 0, r.postFeetY);
            r.shortlist = r.gateDestroyed && "SOLID".equals(r.post.type) && r.post.drop >= minDrop;
        } catch (Throwable t) {
            r.status = "ERROR";
            r.error = t.getClass().getName() + ": " + String.valueOf(t.getMessage());
        }
        return r;
    }

    private static String stem(File f) {
        String n = f.getName();
        int dot = n.lastIndexOf('.');
        return dot > 0 ? n.substring(0, dot) : n;
    }

    public static void main(String[] args) throws Exception {
        Config cfg = parseArgs(args);
        cfg.outputDir.mkdirs();
        InputData input = readCandidates(cfg.input);
        String suffix = stem(cfg.input).replace("candidates_", "");
        File resultsFile = new File(cfg.outputDir, "prefilter_results_" + suffix + ".csv");
        File shortlistFile = new File(cfg.outputDir, "shortlist_" + suffix + ".csv");
        File summaryFile = new File(cfg.outputDir, "PREFILTER_SUMMARY_" + suffix + ".txt");

        PrintWriter results = new PrintWriter(new BufferedWriter(new FileWriter(resultsFile, false)));
        PrintWriter shortlist = new PrintWriter(new BufferedWriter(new FileWriter(shortlistFile, false)));
        results.println(RESULT_HEADER);
        shortlist.println(input.header);

        long start = System.nanoTime();
        int total = 0, dry = 0, exact = 0, moved = 0, gateDestroyed = 0;
        int solid3 = 0, solid10 = 0, solid20 = 0, shortlisted = 0, errors = 0;
        double bestSolidDrop = -1.0;
        long bestSolidSeed = 0L;
        int bestSolidFloorY = -1;

        for (Candidate c : input.rows) {
            ++total;
            if (c.dryCarvePredicted != 0) ++dry;
            Result r = verify(c, cfg.minDrop);
            if (!"GPU_WET_MASK".equals(r.status)) ++exact;
            if ("SPAWN_MOVED".equals(r.status)) ++moved;
            if ("ERROR".equals(r.status)) ++errors;
            if (r.gateDestroyed) ++gateDestroyed;
            boolean solidLanding = r.gateDestroyed && "SOLID".equals(r.post.type);
            if (solidLanding && r.post.drop >= 3.0) ++solid3;
            if (solidLanding && r.post.drop >= 10.0) ++solid10;
            if (solidLanding && r.post.drop >= 20.0) ++solid20;
            if (solidLanding && r.post.drop > bestSolidDrop) {
                bestSolidDrop = r.post.drop;
                bestSolidSeed = c.seed;
                bestSolidFloorY = r.post.y;
                System.out.println("NEW BEST SOLID DROP seed=" + bestSolidSeed +
                    " drop=" + f2(bestSolidDrop) + " feet=" + r.postFeetY +
                    " floorY=" + bestSolidFloorY);
            }
            if (r.shortlist) {
                ++shortlisted;
                shortlist.println(c.rawLine);
                shortlist.flush();
                System.out.println("FAST SOLID HIT seed=" + c.seed + " gateY=" + r.preGateY +
                    " gate " + r.preGateId + "->" + r.postGateId + " feet=" + r.postFeetY +
                    " drop=" + f2(r.post.drop) + " into=" + r.post.type);
            }
            results.println(r.toCsv());

            if (cfg.progressEvery > 0 && total % cfg.progressEvery == 0) {
                double sec = (System.nanoTime() - start) / 1.0e9;
                String best = bestSolidDrop >= 0.0
                    ? f2(bestSolidDrop) + " seed=" + bestSolidSeed + " floorY=" + bestSolidFloorY
                    : "none";
                System.out.println("fast-prefilter progress rows=" + total + "/" + input.rows.size() +
                    " rate=" + f2(total / Math.max(0.001, sec)) + "/s" +
                    " dry=" + dry + " exact=" + exact + " gateDestroyed=" + gateDestroyed +
                    " bestSolidDrop=" + best + " shortlist=" + shortlisted);
            }
        }
        results.close();
        shortlist.close();

        double sec = (System.nanoTime() - start) / 1.0e9;
        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(summaryFile, false)));
        s.println("BETA 1.7.3 GHOST FLOOR P2 FAST PREFILTER - SOLID LANDING ONLY");
        s.println("Input GPU candidates: " + total);
        s.println("Dry-origin lake masks: " + dry);
        s.println("Exact four-chunk candidates run: " + exact);
        s.println("Spawn moved from 0,0: " + moved);
        s.println("Gate destroyed by isolated (-1,-1) population: " + gateDestroyed);
        s.println("Gate destroyed + SOLID drop >=3: " + solid3);
        s.println("Gate destroyed + SOLID drop >=10: " + solid10);
        s.println("Gate destroyed + SOLID drop >=20: " + solid20);
        if (bestSolidDrop >= 0.0) {
            s.println("Best SOLID drop: " + f2(bestSolidDrop) + " blocks seed=" + bestSolidSeed + " floorY=" + bestSolidFloorY);
        } else {
            s.println("Best SOLID drop: none");
        }
        s.println("Shortlisted for full client startup (SOLID drop >=" + f2(cfg.minDrop) + "): " + shortlisted);
        s.println("Errors: " + errors);
        s.println("Elapsed: " + f2(sec) + " s");
        s.println("Rate over all input rows: " + f2(total / Math.max(0.001, sec)) + " rows/s");
        s.println("Shortlist: " + shortlistFile.getAbsolutePath());
        s.close();

        String best = bestSolidDrop >= 0.0
            ? f2(bestSolidDrop) + " seed=" + bestSolidSeed + " floorY=" + bestSolidFloorY
            : "none";
        System.out.println("FAST PREFILTER DONE rows=" + total + " dry=" + dry + " exact=" + exact +
            " gateDestroyed=" + gateDestroyed + " bestSolidDrop=" + best +
            " shortlist=" + shortlisted + " elapsed=" + f2(sec) + "s");
        System.out.println("SHORTLIST=" + shortlistFile.getAbsolutePath());
    }
}
