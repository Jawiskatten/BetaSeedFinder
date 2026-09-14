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

/** Exact Beta 1.7.3 CLIENT-startup verifier for cactus spawn elevators. */
public final class Beta173CactusElevatorOracle {
    private static final int AIR = 0;
    private static final String MASTER_HEADER =
        "status,seed,sequence_index,pre_gate_y,pre_gate_id,pre_feet_y," +
        "cactus_height,post_feet_y,lift,post_support_id,post_support_y,hit,error";

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
        int progressEvery = 1;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
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
        int supportId = -1;
        int supportY = -1;
        boolean hit;

        String toCsv() {
            return joinCsv(new String[] {
                status, Long.toString(c.seed), Long.toString(c.sequenceIndex),
                Integer.toString(preGateY), Integer.toString(preGateId), Integer.toString(preFeetY),
                Integer.toString(cactusHeight), Integer.toString(postFeetY), Integer.toString(lift),
                Integer.toString(supportId), Integer.toString(supportY), hit ? "1" : "0", error
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

    private static final class RankedLine {
        String line;
        long seed;
        int lift;
        int cactusHeight;
        int feetY;
        int supportY;
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
        return c;
    }

    private static List<Candidate> readCandidates(File file) throws Exception {
        ArrayList<Candidate> out = new ArrayList<Candidate>();
        BufferedReader br = new BufferedReader(new FileReader(file));
        String header = br.readLine();
        if (header == null) throw new IllegalArgumentException("empty input CSV");
        List<String> h = splitCsv(header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < h.size(); ++i) ix.put(h.get(i), Integer.valueOf(i));
        if (!ix.containsKey("seed") || !ix.containsKey("sequence_index")) throw new IllegalArgumentException("input needs seed and sequence_index");
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            try {
                Candidate c = new Candidate();
                c.seed = Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex = Long.parseLong(x.get(ix.get("sequence_index").intValue()));
                out.add(c);
            } catch (RuntimeException ignored) {}
        }
        br.close();
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
        while (feet < 128 && collidesAtFeet(world, x, z, feet)) ++feet;
        return feet;
    }

    private static int cactusHeightAtOrigin(World world) {
        int h = 0;
        for (int y = 64; y < 128 && world.getBlockId(0, y, 0) == Block.cactus.blockID; ++y) ++h;
        return h;
    }

    private static void buildTerrainExactlyLikeClient(World world, int spawnX, int spawnZ) {
        IChunkProvider provider = world.getIChunkProvider();
        if (provider instanceof ChunkProviderLoadOrGenerate) {
            ((ChunkProviderLoadOrGenerate)provider).setCurrentChunkOver(spawnX >> 4, spawnZ >> 4);
        }
        // Exact Minecraft.func_6255_d order: X outer, Z inner.
        for (int dx = -128; dx <= 128; dx += 16) {
            for (int dz = -128; dz <= 128; dz += 16) {
                world.getBlockId(spawnX + dx, 64, spawnZ + dz);
            }
        }
        world.func_656_j();
    }

    private static Result verify(Candidate c, int minLift) {
        Result r = new Result();
        r.c = c;
        try {
            World world = new World(new MemorySaveHandler(), "cactus-elevator-oracle", c.seed, (WorldProvider)null);
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

            buildTerrainExactlyLikeClient(world, 0, 0);

            r.cactusHeight = cactusHeightAtOrigin(world);
            r.postFeetY = actualFeetY(world, 0, 0);
            r.lift = r.postFeetY - r.preFeetY;
            r.supportY = r.postFeetY - 1;
            r.supportId = r.supportY >= 0 ? world.getBlockId(0, r.supportY, 0) : 0;
            if (r.cactusHeight < 2) {
                r.status = "NO_ORIGIN_CACTUS_BRIDGE";
                return r;
            }
            r.hit = r.lift >= minLift;
        } catch (Throwable t) {
            r.status = "ERROR";
            r.error = t.getClass().getName() + ": " + String.valueOf(t.getMessage());
        }
        return r;
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
                String k = line.substring(0, eq), v = line.substring(eq + 1);
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

    private static List<RankedLine> readHits(File master) throws Exception {
        ArrayList<RankedLine> out = new ArrayList<RankedLine>();
        if (!master.isFile()) return out;
        BufferedReader br = new BufferedReader(new FileReader(master));
        String header = br.readLine();
        if (header == null) { br.close(); return out; }
        List<String> h = splitCsv(header);
        int seedIx = h.indexOf("seed"), liftIx = h.indexOf("lift"), cactusIx = h.indexOf("cactus_height");
        int feetIx = h.indexOf("post_feet_y"), supportIx = h.indexOf("post_support_y"), hitIx = h.indexOf("hit");
        String line;
        while ((line = br.readLine()) != null) {
            List<String> x = splitCsv(line);
            try {
                if (!"1".equals(x.get(hitIx))) continue;
                RankedLine r = new RankedLine();
                r.line = line;
                r.seed = Long.parseLong(x.get(seedIx));
                r.lift = Integer.parseInt(x.get(liftIx));
                r.cactusHeight = Integer.parseInt(x.get(cactusIx));
                r.feetY = Integer.parseInt(x.get(feetIx));
                r.supportY = Integer.parseInt(x.get(supportIx));
                out.add(r);
            } catch (RuntimeException ignored) {}
        }
        br.close();
        Collections.sort(out, new Comparator<RankedLine>() {
            public int compare(RankedLine a, RankedLine b) {
                if (a.lift != b.lift) return b.lift - a.lift;
                return a.seed < b.seed ? -1 : (a.seed == b.seed ? 0 : 1);
            }
        });
        return out;
    }

    private static void rebuildTopAndSummary(File master, File outputDir, int minLift) throws Exception {
        List<RankedLine> hits = readHits(master);
        File top = new File(outputDir, "top_cactus_elevators.csv");
        PrintWriter p = new PrintWriter(new BufferedWriter(new FileWriter(top, false)));
        p.println("rank,seed,lift,cactus_height,post_feet_y,post_support_y");
        int n = Math.min(100, hits.size());
        for (int i = 0; i < n; ++i) {
            RankedLine r = hits.get(i);
            p.println((i + 1) + "," + r.seed + "," + r.lift + "," + r.cactusHeight + "," + r.feetY + "," + r.supportY);
        }
        p.close();

        int total = 0;
        if (master.isFile()) {
            BufferedReader br = new BufferedReader(new FileReader(master));
            if (br.readLine() != null) while (br.readLine() != null) ++total;
            br.close();
        }
        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(new File(outputDir, "SUMMARY.txt"), false)));
        s.println("BETA 1.7.3 CACTUS SPAWN ELEVATOR - AUTHORITATIVE CLIENT STARTUP");
        s.println("Full-startup rows verified: " + total);
        s.println("Authoritative cactus elevators lift >=" + minLift + ": " + hits.size());
        if (!hits.isEmpty()) {
            RankedLine b = hits.get(0);
            s.println("BEST seed=" + b.seed + " lift=" + b.lift + " cactusHeight=" + b.cactusHeight + " feetY=" + b.feetY + " supportY=" + b.supportY);
        }
        s.println("Master: " + master.getAbsolutePath());
        s.println("Top: " + top.getAbsolutePath());
        s.close();
    }

    public static void main(String[] args) throws Exception {
        Config cfg = parseArgs(args);
        cfg.outputDir.mkdirs();
        List<Candidate> input = readCandidates(cfg.input);
        File master = new File(cfg.outputDir, "actual_cactus_elevator_all.csv");
        Set<Long> processed = readProcessed(master);
        boolean fresh = !master.isFile() || master.length() == 0L;
        PrintWriter out = new PrintWriter(new BufferedWriter(new FileWriter(master, true)));
        if (fresh) out.println(MASTER_HEADER);

        BestState best = loadBest(cfg.bestState);
        int done = 0, skipped = 0, hits = 0, errors = 0;
        for (Candidate c : input) {
            if (processed.contains(Long.valueOf(c.sequenceIndex))) { ++skipped; continue; }
            Result r = verify(c, cfg.minLift);
            ++done;
            if ("ERROR".equals(r.status)) ++errors;
            if (r.hit) {
                ++hits;
                System.out.println("AUTHORITATIVE CACTUS ELEVATOR seed=" + c.seed +
                    " lift=" + r.lift + " cactusHeight=" + r.cactusHeight +
                    " feetY=" + r.postFeetY + " supportY=" + r.supportY);
            }
            if (r.cactusHeight >= 2 && r.lift > best.lift) {
                best.lift = r.lift;
                best.seed = c.seed;
                best.cactusHeight = r.cactusHeight;
                best.feetY = r.postFeetY;
                best.supportY = r.supportY;
                saveBest(cfg.bestState, best);
                System.out.println("NEW AUTHORITATIVE BEST seed=" + c.seed + " lift=" + r.lift +
                    " cactusHeight=" + r.cactusHeight + " feetY=" + r.postFeetY);
            }
            out.println(r.toCsv());
            out.flush();
            if (cfg.progressEvery > 0 && done % cfg.progressEvery == 0) {
                System.out.println("client-oracle progress new=" + done + "/" + input.size() +
                    " hits=" + hits + " runBestLift=" + (best.lift >= 0 ? Integer.toString(best.lift) : "NONE") +
                    (best.lift >= 0 ? " seed=" + best.seed : ""));
            }
        }
        out.close();
        saveBest(cfg.bestState, best);
        rebuildTopAndSummary(master, cfg.outputDir, cfg.minLift);
        System.out.println("CACTUS CLIENT ORACLE DONE new=" + done + " skipped=" + skipped + " hits=" + hits +
            " errors=" + errors + " runBestLift=" + (best.lift >= 0 ? Integer.toString(best.lift) : "NONE") +
            (best.lift >= 0 ? " bestSeed=" + best.seed : ""));
    }
}
