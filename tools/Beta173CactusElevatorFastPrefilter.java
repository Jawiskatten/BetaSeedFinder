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
 * Exact four-chunk Beta 1.7.3 prefilter for the cactus spawn-elevator idea.
 *
 * The GPU stage proves a 2/3-high cactus would be geometrically capable of
 * bridging the player from the normal Y65 start into an overhead terrain mass.
 * This class then runs real Beta population for only chunk (-1,-1), checks
 * whether vanilla desert cactus generation actually put a cactus stack at
 * X=0,Z=0, and measures the real collision-push lift.
 */
public final class Beta173CactusElevatorFastPrefilter {
    private static final int AIR = 0;

    private static final String RESULT_HEADER =
        "status,seed,sequence_index,pre_gate_y,pre_gate_id,pre_feet_y," +
        "cactus_height,post_feet_y,lift,post_support_id,post_support_y,shortlist,error";

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
        File bestState;
        int minLift = 8;
        int progressEvery = 25;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        String rawLine;
    }

    private static final class InputData {
        String header;
        List<Candidate> rows = new ArrayList<Candidate>();
    }

    private static final class Result {
        Candidate c;
        String status = "OK";
        String error = "";
        int preGateY = -1;
        int preGateId = -1;
        int preFeetY = -1;
        int cactusHeight = 0;
        int postFeetY = -1;
        int lift = 0;
        int postSupportId = -1;
        int postSupportY = -1;
        boolean shortlist;

        String toCsv() {
            return joinCsv(new String[] {
                status, Long.toString(c.seed), Long.toString(c.sequenceIndex),
                Integer.toString(preGateY), Integer.toString(preGateId), Integer.toString(preFeetY),
                Integer.toString(cactusHeight), Integer.toString(postFeetY), Integer.toString(lift),
                Integer.toString(postSupportId), Integer.toString(postSupportY), bit(shortlist), error
            });
        }
    }

    private static final class BestState {
        int lift = -1;
        long seed = 0L;
        int cactusHeight = 0;
        int feetY = -1;
        int supportY = -1;
    }

    private static String bit(boolean x) { return x ? "1" : "0"; }

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
            else if ("--best-state".equals(a)) c.bestState = new File(args[++i]);
            else if ("--min-lift".equals(a)) c.minLift = Integer.parseInt(args[++i]);
            else if ("--progress-every".equals(a)) c.progressEvery = Integer.parseInt(args[++i]);
            else throw new IllegalArgumentException("unknown argument: " + a);
        }
        if (c.input == null) throw new IllegalArgumentException("--input is required");
        if (c.outputDir == null) throw new IllegalArgumentException("--output is required");
        if (c.minLift < 1 || c.minLift > 62) throw new IllegalArgumentException("--min-lift must be 1..62");
        return c;
    }

    private static InputData readCandidates(File file) throws Exception {
        InputData d = new InputData();
        BufferedReader br = new BufferedReader(new FileReader(file));
        d.header = br.readLine();
        if (d.header == null) throw new IllegalArgumentException("empty input CSV");
        List<String> h = splitCsv(d.header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < h.size(); ++i) ix.put(h.get(i), Integer.valueOf(i));
        if (!ix.containsKey("seed") || !ix.containsKey("sequence_index")) {
            throw new IllegalArgumentException("input needs seed and sequence_index columns");
        }
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            try {
                Candidate c = new Candidate();
                c.seed = Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex = Long.parseLong(x.get(ix.get("sequence_index").intValue()));
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
        while (feet < 128 && collidesAtFeet(world, x, z, feet)) ++feet;
        return feet;
    }

    private static int cactusHeightAtOrigin(World world) {
        int h = 0;
        for (int y = 64; y < 128 && world.getBlockId(0, y, 0) == Block.cactus.blockID; ++y) ++h;
        return h;
    }

    private static void populateOnlyMinusOneMinusOne(World world) {
        // The initial spawn check already loaded (0,0). Loading the other three
        // members completes only the (-1,-1) population neighborhood.
        world.getChunkFromChunkCoords(-1, -1);
        world.getChunkFromChunkCoords(-1,  0);
        world.getChunkFromChunkCoords( 0, -1);
        world.chunkProvider.populate(world.chunkProvider, -1, -1);
    }

    private static Result verify(Candidate c, int minLift) {
        Result r = new Result();
        r.c = c;
        try {
            World world = new World(new MemorySaveHandler(), "cactus-elevator-fast", c.seed, (WorldProvider)null);
            ChunkCoordinates sp = world.getSpawnPoint();
            if (sp.x != 0 || sp.z != 0) {
                r.status = "SPAWN_MOVED";
                return r;
            }

            r.preGateY = firstUncoveredY(world, 0, 0);
            r.preGateId = world.getBlockId(0, r.preGateY, 0);
            if (r.preGateY != 63 || r.preGateId != Block.sand.blockID) {
                r.status = "PRE_GATE_NOT_Y63_SAND";
                return r;
            }
            r.preFeetY = actualFeetY(world, 0, 0);
            if (r.preFeetY != 65) {
                r.status = "PRE_ALREADY_PUSHED";
                return r;
            }

            populateOnlyMinusOneMinusOne(world);

            r.cactusHeight = cactusHeightAtOrigin(world);
            r.postFeetY = actualFeetY(world, 0, 0);
            r.lift = r.postFeetY - r.preFeetY;
            r.postSupportY = r.postFeetY - 1;
            r.postSupportId = r.postSupportY >= 0 ? world.getBlockId(0, r.postSupportY, 0) : 0;
            if (r.cactusHeight < 2) {
                r.status = "NO_ORIGIN_CACTUS_BRIDGE";
                return r;
            }
            r.shortlist = r.lift >= minLift;
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

    private static BestState loadBest(File file) {
        BestState b = new BestState();
        if (file == null || !file.isFile()) return b;
        try {
            BufferedReader br = new BufferedReader(new FileReader(file));
            String line;
            while ((line = br.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String k = line.substring(0, eq);
                String v = line.substring(eq + 1);
                if ("LIFT".equals(k)) b.lift = Integer.parseInt(v);
                else if ("SEED".equals(k)) b.seed = Long.parseLong(v);
                else if ("CACTUS_HEIGHT".equals(k)) b.cactusHeight = Integer.parseInt(v);
                else if ("FEET_Y".equals(k)) b.feetY = Integer.parseInt(v);
                else if ("SUPPORT_Y".equals(k)) b.supportY = Integer.parseInt(v);
            }
            br.close();
        } catch (Exception ignored) {}
        return b;
    }

    private static void saveBest(File file, BestState b) throws Exception {
        if (file == null) return;
        File parent = file.getParentFile();
        if (parent != null) parent.mkdirs();
        PrintWriter p = new PrintWriter(new BufferedWriter(new FileWriter(file, false)));
        p.println("LIFT=" + b.lift);
        p.println("SEED=" + b.seed);
        p.println("CACTUS_HEIGHT=" + b.cactusHeight);
        p.println("FEET_Y=" + b.feetY);
        p.println("SUPPORT_Y=" + b.supportY);
        p.close();
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

        BestState best = loadBest(cfg.bestState);
        long start = System.nanoTime();
        int total = 0, spawnMoved = 0, preAlready = 0, cactus2 = 0, elevators = 0, shortlisted = 0, errors = 0;

        for (Candidate c : input.rows) {
            ++total;
            Result r = verify(c, cfg.minLift);
            if ("SPAWN_MOVED".equals(r.status)) ++spawnMoved;
            if ("PRE_ALREADY_PUSHED".equals(r.status)) ++preAlready;
            if ("ERROR".equals(r.status)) ++errors;
            if (r.cactusHeight >= 2) ++cactus2;
            if (r.cactusHeight >= 2 && r.lift > 0) ++elevators;

            if (r.cactusHeight >= 2 && r.lift > best.lift) {
                best.lift = r.lift;
                best.seed = c.seed;
                best.cactusHeight = r.cactusHeight;
                best.feetY = r.postFeetY;
                best.supportY = r.postSupportY;
                saveBest(cfg.bestState, best);
                System.out.println("NEW BEST CACTUS ELEVATOR seed=" + c.seed +
                    " lift=" + r.lift + " cactusHeight=" + r.cactusHeight +
                    " feetY=" + r.postFeetY + " supportY=" + r.postSupportY);
            }

            if (r.shortlist) {
                ++shortlisted;
                shortlist.println(c.rawLine);
                shortlist.flush();
                System.out.println("FAST CACTUS ELEVATOR HIT seed=" + c.seed +
                    " lift=" + r.lift + " cactusHeight=" + r.cactusHeight +
                    " feetY=" + r.postFeetY + " supportY=" + r.postSupportY);
            }
            results.println(r.toCsv());

            if (cfg.progressEvery > 0 && total % cfg.progressEvery == 0) {
                double sec = (System.nanoTime() - start) / 1.0e9;
                System.out.println("cactus-prefilter progress rows=" + total + "/" + input.rows.size() +
                    " rate=" + String.format(java.util.Locale.ROOT, "%.2f", total / Math.max(0.001, sec)) + "/s" +
                    " cactus2plus=" + cactus2 + " elevators=" + elevators +
                    " runBestLift=" + (best.lift >= 0 ? Integer.toString(best.lift) : "NONE") +
                    (best.lift >= 0 ? " seed=" + best.seed : "") + " shortlist=" + shortlisted);
            }
        }
        results.close();
        shortlist.close();
        saveBest(cfg.bestState, best);

        double sec = (System.nanoTime() - start) / 1.0e9;
        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(summaryFile, false)));
        s.println("BETA 1.7.3 CACTUS SPAWN ELEVATOR - FOUR CHUNK PREFILTER");
        s.println("GPU geometry candidates: " + total);
        s.println("Spawn moved from 0,0: " + spawnMoved);
        s.println("Already collision-pushed before population: " + preAlready);
        s.println("Origin cactus height >=2 after isolated (-1,-1) population: " + cactus2);
        s.println("Actual cactus-caused upward pushes: " + elevators);
        s.println("Shortlisted lift >=" + cfg.minLift + ": " + shortlisted);
        s.println("Run-wide best isolated lift: " + (best.lift >= 0 ? Integer.toString(best.lift) : "NONE"));
        if (best.lift >= 0) s.println("Run-wide best seed: " + best.seed + " cactusHeight=" + best.cactusHeight + " feetY=" + best.feetY + " supportY=" + best.supportY);
        s.println("Errors: " + errors);
        s.println("Elapsed: " + String.format(java.util.Locale.ROOT, "%.2f", sec) + " s");
        s.println("Shortlist: " + shortlistFile.getAbsolutePath());
        s.close();

        System.out.println("CACTUS PREFILTER DONE rows=" + total + " cactus2plus=" + cactus2 +
            " elevators=" + elevators + " shortlist=" + shortlisted +
            " runBestLift=" + (best.lift >= 0 ? Integer.toString(best.lift) : "NONE") +
            (best.lift >= 0 ? " bestSeed=" + best.seed : "") +
            " elapsed=" + String.format(java.util.Locale.ROOT, "%.2f", sec) + "s");
        System.out.println("SHORTLIST=" + shortlistFile.getAbsolutePath());
    }
}
