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
import java.util.Random;
import java.util.Set;

/**
 * Exact Beta 1.7.3 client-startup verifier for the sand-wake freefall idea.
 *
 * GPU/CPU scouting proves that the origin spawn sand is dormant over a dry cave.
 * This oracle first performs a controlled immediate sand wake in a fresh exact
 * Beta World to prove the geometry really can collapse. It then creates another
 * fresh World and reproduces Minecraft.func_6255_d's 17x17 Building-terrain
 * access order. A jackpot is counted only when vanilla population naturally
 * wakes/relocates that sand before player creation and leaves a dry SOLID fall.
 */
public final class Beta173SandWakeFreefallOracle {
    private static final int AIR = 0;
    private static final int WATER_MOVING = 8;
    private static final int WATER_STILL = 9;
    private static final int LAVA_MOVING = 10;
    private static final int LAVA_STILL = 11;

    private static final String HEADER =
        "status,seed,sequence_index,predicted_potential_drop,pre_gate_y,pre_sand_bottom_y,pre_sand_blocks," +
        "forced_gate_destroyed,forced_new_sand_below,forced_feet_y,forced_obstacle_type,forced_obstacle_y,forced_drop," +
        "post_gate_id,natural_gate_destroyed,natural_new_sand_below,post_feet_y,post_obstacle_type,post_obstacle_y,post_drop," +
        "natural_wake,hit,error";

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
        int minDrop = 5;
        int progressEvery = 1;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        int predictedDrop;
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
        int preGateY = -1;
        int preSandBottomY = -1;
        int preSandBlocks = 0;
        boolean forcedGateDestroyed;
        int forcedNewSandBelow = 0;
        int forcedFeetY = -1;
        Obstacle forcedObstacle = new Obstacle();
        int postGateId = -1;
        boolean naturalGateDestroyed;
        int naturalNewSandBelow = 0;
        int postFeetY = -1;
        Obstacle postObstacle = new Obstacle();
        boolean naturalWake;
        boolean hit;

        String toCsv() {
            return joinCsv(new String[] {
                status, Long.toString(c.seed), Long.toString(c.sequenceIndex), Integer.toString(c.predictedDrop),
                Integer.toString(preGateY), Integer.toString(preSandBottomY), Integer.toString(preSandBlocks),
                bit(forcedGateDestroyed), Integer.toString(forcedNewSandBelow), Integer.toString(forcedFeetY),
                forcedObstacle.type, Integer.toString(forcedObstacle.y), f2(forcedObstacle.drop),
                Integer.toString(postGateId), bit(naturalGateDestroyed), Integer.toString(naturalNewSandBelow),
                Integer.toString(postFeetY), postObstacle.type, Integer.toString(postObstacle.y), f2(postObstacle.drop),
                bit(naturalWake), bit(hit), error
            });
        }
    }

    private static final class BestState {
        double forcedDrop = -1.0;
        long forcedSeed = 0L;
        double naturalDrop = -1.0;
        long naturalSeed = 0L;
    }

    private static final class Ranked {
        long seed;
        double drop;
        int obstacleY;
        int newSand;
        int predictedDrop;
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
            else if ("--best-state".equals(a)) c.bestState = new File(args[++i]);
            else if ("--min-drop".equals(a)) c.minDrop = Integer.parseInt(args[++i]);
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
        int predIx = h.indexOf("potential_drop");
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            try {
                Candidate c = new Candidate();
                c.seed = Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex = Long.parseLong(x.get(ix.get("sequence_index").intValue()));
                c.predictedDrop = predIx >= 0 ? Integer.parseInt(x.get(predIx)) : -1;
                out.add(c);
            } catch (RuntimeException ignored) {}
        }
        br.close();
        Collections.sort(out, new Comparator<Candidate>() {
            public int compare(Candidate a, Candidate b) {
                if (a.predictedDrop != b.predictedDrop) return b.predictedDrop - a.predictedDrop;
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
        while (feet < 128 && collidesAtFeet(world, x, z, feet)) ++feet;
        return feet;
    }

    private static boolean isWater(int id) { return id == WATER_MOVING || id == WATER_STILL; }
    private static boolean isLava(int id) { return id == LAVA_MOVING || id == LAVA_STILL; }

    private static Obstacle firstObstacleBelow(World world, int x, int z, int feetY) {
        Obstacle o = new Obstacle();
        for (int y = Math.min(127, feetY - 1); y >= 0; --y) {
            int id = world.getBlockId(x, y, z);
            if (id == AIR) continue;
            if (isWater(id)) { o.type = "WATER"; o.y = y; o.topY = y + 1.0; o.drop = Math.max(0.0, feetY - o.topY); return o; }
            if (isLava(id)) { o.type = "LAVA"; o.y = y; o.topY = y + 1.0; o.drop = Math.max(0.0, feetY - o.topY); return o; }
            Block b = Block.blocksList[id];
            if (b == null) continue;
            AxisAlignedBB box = b.getCollisionBoundingBoxFromPool(world, x, y, z);
            if (box != null && box.maxY <= (double)feetY + 1.0e-9) {
                o.type = "SOLID"; o.y = y; o.topY = box.maxY; o.drop = Math.max(0.0, feetY - o.topY); return o;
            }
        }
        o.type = "VOID"; o.y = -1; o.topY = 0.0; o.drop = (double)feetY;
        return o;
    }

    private static int[] snapshotColumn(World world) {
        int[] ids = new int[128];
        for (int y = 0; y < 128; ++y) ids[y] = world.getBlockId(0, y, 0);
        return ids;
    }

    private static int bottomOfSandStack(int[] ids, int gateY) {
        int y = gateY;
        while (y - 1 >= 0 && ids[y - 1] == Block.sand.blockID) --y;
        return y;
    }

    private static int countNewSandBelow(int[] before, int[] after, int belowYExclusive) {
        int n = 0;
        for (int y = 0; y < belowYExclusive; ++y) {
            if (before[y] != Block.sand.blockID && after[y] == Block.sand.blockID) ++n;
        }
        return n;
    }

    private static void buildTerrainExactlyLikeClient(World world, int spawnX, int spawnZ) {
        IChunkProvider provider = world.getIChunkProvider();
        if (provider instanceof ChunkProviderLoadOrGenerate) {
            ((ChunkProviderLoadOrGenerate)provider).setCurrentChunkOver(spawnX >> 4, spawnZ >> 4);
        }
        for (int dx = -128; dx <= 128; dx += 16) {
            for (int dz = -128; dz <= 128; dz += 16) {
                world.getBlockId(spawnX + dx, 64, spawnZ + dz);
            }
        }
        world.func_656_j();
    }

    private static World freshWorld(long seed, String name) {
        return new World(new MemorySaveHandler(), name, seed, (WorldProvider)null);
    }

    private static Result verify(Candidate c, int minDrop) {
        Result r = new Result();
        r.c = c;
        try {
            // First prove the geometry with a controlled immediate wake.
            World forced = freshWorld(c.seed, "sand-wake-forced-proof");
            ChunkCoordinates fsp = forced.getSpawnPoint();
            if (fsp.x != 0 || fsp.z != 0) { r.status = "SPAWN_MOVED"; return r; }
            r.preGateY = firstUncoveredY(forced, 0, 0);
            int forcedGateId = forced.getBlockId(0, r.preGateY, 0);
            if (r.preGateY != 63 || forcedGateId != Block.sand.blockID) { r.status = "PRE_GATE_NOT_Y63_SAND"; return r; }
            int preFeet = actualFeetY(forced, 0, 0);
            if (preFeet != 65) { r.status = "PRE_ALREADY_COLLIDING"; return r; }
            int[] forcedBefore = snapshotColumn(forced);
            r.preSandBottomY = bottomOfSandStack(forcedBefore, r.preGateY);
            r.preSandBlocks = r.preGateY - r.preSandBottomY + 1;

            boolean oldFallInstantly = BlockSand.fallInstantly;
            boolean oldImmediate = forced.scheduledUpdatesAreImmediate;
            try {
                BlockSand.fallInstantly = true;
                forced.scheduledUpdatesAreImmediate = true;
                Block.sand.updateTick(forced, 0, r.preSandBottomY, 0, new Random(c.seed ^ 0x5A17D0A5L));
            } finally {
                forced.scheduledUpdatesAreImmediate = oldImmediate;
                BlockSand.fallInstantly = oldFallInstantly;
            }
            int[] forcedAfter = snapshotColumn(forced);
            r.forcedGateDestroyed = forcedAfter[r.preGateY] != Block.sand.blockID;
            r.forcedNewSandBelow = countNewSandBelow(forcedBefore, forcedAfter, r.preSandBottomY);
            r.forcedFeetY = actualFeetY(forced, 0, 0);
            r.forcedObstacle = firstObstacleBelow(forced, 0, 0, r.forcedFeetY);
            if (!r.forcedGateDestroyed || r.forcedNewSandBelow <= 0) {
                r.status = "FORCED_WAKE_DID_NOT_RELOCATE_SAND";
                return r;
            }

            // Now the authoritative test: no intervention, exact client startup.
            World world = freshWorld(c.seed, "sand-wake-natural-client-startup");
            ChunkCoordinates sp = world.getSpawnPoint();
            if (sp.x != 0 || sp.z != 0) { r.status = "SPAWN_MOVED_SECOND_WORLD"; return r; }
            int gateY = firstUncoveredY(world, 0, 0);
            if (gateY != r.preGateY || world.getBlockId(0, gateY, 0) != Block.sand.blockID) {
                r.status = "PRE_STATE_MISMATCH";
                return r;
            }
            int[] naturalBefore = snapshotColumn(world);

            buildTerrainExactlyLikeClient(world, 0, 0);

            int[] naturalAfter = snapshotColumn(world);
            r.postGateId = naturalAfter[r.preGateY];
            r.naturalGateDestroyed = r.postGateId != Block.sand.blockID;
            r.naturalNewSandBelow = countNewSandBelow(naturalBefore, naturalAfter, r.preSandBottomY);
            r.postFeetY = actualFeetY(world, 0, 0);
            r.postObstacle = firstObstacleBelow(world, 0, 0, r.postFeetY);
            r.naturalWake = r.naturalGateDestroyed && r.naturalNewSandBelow > 0;
            r.hit = r.naturalWake && r.postFeetY == 65 && "SOLID".equals(r.postObstacle.type) && r.postObstacle.drop >= minDrop;
            if (!r.naturalWake) r.status = "NO_NATURAL_WAKE";
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
                int eq = line.indexOf('='); if (eq <= 0) continue;
                String k = line.substring(0, eq), v = line.substring(eq + 1);
                if ("FORCED_DROP".equals(k)) b.forcedDrop = Double.parseDouble(v);
                else if ("FORCED_SEED".equals(k)) b.forcedSeed = Long.parseLong(v);
                else if ("NATURAL_DROP".equals(k)) b.naturalDrop = Double.parseDouble(v);
                else if ("NATURAL_SEED".equals(k)) b.naturalSeed = Long.parseLong(v);
            }
            br.close();
        } catch (Exception ignored) {}
        return b;
    }

    private static void saveBest(File file, BestState b) throws Exception {
        if (file == null) return;
        File parent = file.getParentFile(); if (parent != null) parent.mkdirs();
        PrintWriter p = new PrintWriter(new BufferedWriter(new FileWriter(file, false)));
        p.println("FORCED_DROP=" + f2(b.forcedDrop));
        p.println("FORCED_SEED=" + b.forcedSeed);
        p.println("NATURAL_DROP=" + f2(b.naturalDrop));
        p.println("NATURAL_SEED=" + b.naturalSeed);
        p.close();
    }

    private static void rebuildTopAndSummary(File master, File outputDir, int minDrop) throws Exception {
        ArrayList<Ranked> hits = new ArrayList<Ranked>();
        int total = 0, forced = 0, naturalWake = 0, hitCount = 0, errors = 0;
        if (master.isFile()) {
            BufferedReader br = new BufferedReader(new FileReader(master));
            String header = br.readLine();
            if (header != null) {
                List<String> h = splitCsv(header);
                int statusIx=h.indexOf("status"), seedIx=h.indexOf("seed"), predIx=h.indexOf("predicted_potential_drop");
                int forcedIx=h.indexOf("forced_gate_destroyed"), forcedNewIx=h.indexOf("forced_new_sand_below");
                int wakeIx=h.indexOf("natural_wake"), hitIx=h.indexOf("hit"), dropIx=h.indexOf("post_drop");
                int obsIx=h.indexOf("post_obstacle_y"), newIx=h.indexOf("natural_new_sand_below");
                String line;
                while ((line = br.readLine()) != null) {
                    List<String> x = splitCsv(line); ++total;
                    try {
                        if ("ERROR".equals(x.get(statusIx))) ++errors;
                        if ("1".equals(x.get(forcedIx)) && Integer.parseInt(x.get(forcedNewIx)) > 0) ++forced;
                        if ("1".equals(x.get(wakeIx))) ++naturalWake;
                        if ("1".equals(x.get(hitIx))) {
                            ++hitCount;
                            Ranked r = new Ranked();
                            r.seed=Long.parseLong(x.get(seedIx)); r.drop=Double.parseDouble(x.get(dropIx));
                            r.obstacleY=Integer.parseInt(x.get(obsIx)); r.newSand=Integer.parseInt(x.get(newIx));
                            r.predictedDrop=Integer.parseInt(x.get(predIx)); hits.add(r);
                        }
                    } catch (RuntimeException ignored) {}
                }
            }
            br.close();
        }
        Collections.sort(hits, new Comparator<Ranked>() {
            public int compare(Ranked a, Ranked b) {
                int d = Double.compare(b.drop, a.drop); if (d != 0) return d;
                return a.seed < b.seed ? -1 : (a.seed == b.seed ? 0 : 1);
            }
        });
        PrintWriter top = new PrintWriter(new BufferedWriter(new FileWriter(new File(outputDir, "top_natural_sand_wake_freefalls.csv"), false)));
        top.println("rank,seed,post_drop,post_obstacle_y,natural_new_sand_below,predicted_potential_drop");
        for (int i=0;i<Math.min(100,hits.size());++i) {
            Ranked r=hits.get(i); top.println((i+1)+","+r.seed+","+f2(r.drop)+","+r.obstacleY+","+r.newSand+","+r.predictedDrop);
        }
        top.close();
        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(new File(outputDir, "SUMMARY.txt"), false)));
        s.println("BETA 1.7.3 SAND-WAKE FREEFALL - AUTHORITATIVE CLIENT STARTUP");
        s.println("Rows verified: " + total);
        s.println("Controlled immediate-wake geometry proofs: " + forced);
        s.println("Natural population sand relocations before player creation: " + naturalWake);
        s.println("Natural dry SOLID freefalls >=" + minDrop + ": " + hitCount);
        s.println("Errors: " + errors);
        if (!hits.isEmpty()) s.println("Best natural freefall: seed=" + hits.get(0).seed + " drop=" + f2(hits.get(0).drop));
        s.close();
    }

    public static void main(String[] args) throws Exception {
        Config cfg = parseArgs(args);
        cfg.outputDir.mkdirs();
        List<Candidate> candidates = readCandidates(cfg.input);
        File master = new File(cfg.outputDir, "actual_sand_wake_all.csv");
        Set<Long> processed = readProcessed(master);
        boolean newFile = !master.isFile() || master.length() == 0L;
        PrintWriter out = new PrintWriter(new BufferedWriter(new FileWriter(master, true)));
        if (newFile) out.println(HEADER);
        BestState best = loadBest(cfg.bestState);
        long started = System.nanoTime();
        int done = 0, forcedProofs = 0, naturalWakes = 0, hits = 0;
        for (Candidate c : candidates) {
            if (processed.contains(Long.valueOf(c.sequenceIndex))) continue;
            Result r = verify(c, cfg.minDrop);
            out.println(r.toCsv()); out.flush();
            ++done;
            if (r.forcedGateDestroyed && r.forcedNewSandBelow > 0) ++forcedProofs;
            if (r.naturalWake) ++naturalWakes;
            if (r.hit) ++hits;

            if (r.forcedGateDestroyed && r.forcedNewSandBelow > 0 && "SOLID".equals(r.forcedObstacle.type) && r.forcedObstacle.drop > best.forcedDrop) {
                best.forcedDrop = r.forcedObstacle.drop; best.forcedSeed = c.seed; saveBest(cfg.bestState, best);
                System.out.println("NEW BEST FORCED-WAKE PROOF seed=" + c.seed + " drop=" + f2(r.forcedObstacle.drop) + " floorY=" + r.forcedObstacle.y);
            }
            if (r.hit && r.postObstacle.drop > best.naturalDrop) {
                best.naturalDrop = r.postObstacle.drop; best.naturalSeed = c.seed; saveBest(cfg.bestState, best);
                System.out.println("NEW BEST NATURAL SAND-WAKE FREEFALL seed=" + c.seed + " drop=" + f2(r.postObstacle.drop) + " floorY=" + r.postObstacle.y);
            }
            if (r.naturalWake) {
                System.out.println("NATURAL SAND WAKE seed=" + c.seed + " drop=" + f2(r.postObstacle.drop) + " obstacle=" + r.postObstacle.type + " newSandBelow=" + r.naturalNewSandBelow + " hit=" + bit(r.hit));
            }
            if (cfg.progressEvery > 0 && done % cfg.progressEvery == 0) {
                double sec=(System.nanoTime()-started)/1.0e9;
                System.out.println("sand-wake oracle progress rows="+done+"/"+candidates.size()+" rate="+String.format(java.util.Locale.ROOT,"%.2f",done/Math.max(0.001,sec))+"/s forcedProofs="+forcedProofs+" naturalWakes="+naturalWakes+" hits="+hits+" bestNatural="+(best.naturalDrop<0?"NONE":f2(best.naturalDrop)+" seed="+best.naturalSeed));
            }
        }
        out.close();
        saveBest(cfg.bestState, best);
        rebuildTopAndSummary(master, cfg.outputDir, cfg.minDrop);
        System.out.println("SAND-WAKE ORACLE DONE rows="+done+" forcedProofs="+forcedProofs+" naturalWakes="+naturalWakes+" hits="+hits+" runBestNatural="+(best.naturalDrop<0?"NONE":f2(best.naturalDrop)+" seed="+best.naturalSeed));
        System.out.println("MASTER="+master.getAbsolutePath());
    }
}
