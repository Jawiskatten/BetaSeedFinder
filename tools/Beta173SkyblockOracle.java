package net.minecraft.src;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Exact Beta 1.7.3 world-generation oracle used after the fast P6 terrain scan.
 *
 * It runs the actual decompiled Beta server world generator entirely in memory,
 * loads/populates the spawn area, lets scheduled liquid updates run, then measures
 * the real post-population spawn island and writes separate result lists for:
 *   - natural tree spawns on the island
 *   - a natural tree exactly at x=0,z=0
 *   - small/lonely SkyBlock-style islands
 *   - visible water falls
 *   - visible lava falls
 *   - seeds with both water and lava falls
 */
public final class Beta173SkyblockOracle {
    private static final int AIR = 0;
    private static final int WATER_MOVING = 8;
    private static final int WATER_STILL = 9;
    private static final int LAVA_MOVING = 10;
    private static final int LAVA_STILL = 11;
    private static final int WOOD = 17;
    private static final int LEAVES = 18;

    private static final String MASTER_HEADER =
        "status,seed,sequence_index,p6_blocks,p6_footprint,spawn_x,spawn_z,actual_feet_y,actual_support_y," +
        "floating,window_clipped,component_blocks,footprint,span_x,span_z,min_y,max_y,min_x,max_x,min_z,max_z," +
        "nearest_elevated,nearest_same_height,clear_pct,skyblock_score,skyblock_candidate,tiny_skyblock," +
        "tree_count,tree_at_0_0,first_tree_x,first_tree_y,first_tree_z," +
        "waterfall,water_drop,water_x,water_y,water_z,lavafall,lava_drop,lava_x,lava_y,lava_z,both_fluids,error";

    private static final class MemorySaveHandler implements ISaveHandler {
        public WorldInfo func_22096_c() { return null; }
        public void func_22091_b() {}
        public IChunkLoader func_22092_a(WorldProvider provider) { return null; }
        public void func_22095_a(WorldInfo info, List players) {}
        public void func_22094_a(WorldInfo info) {}
        public IPlayerFileData func_22090_d() { return null; }
        public void func_22093_e() {}
        public File func_28111_b(String name) { return null; }
    }

    private static final class Config {
        File input;
        File outputDir;
        int chunkRadius = 4;
        int isolationRadius = 48;
        int liquidTicks = 96;
        int maxSeeds = 0;
        int progressEvery = 10;
    }

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        int p6Blocks;
        int p6Footprint;
        int p6SupportY;
    }

    private static final class Component {
        boolean floating;
        boolean clipped;
        int blocks;
        int footprint;
        int minX = Integer.MAX_VALUE, maxX = Integer.MIN_VALUE;
        int minY = 128, maxY = -1;
        int minZ = Integer.MAX_VALUE, maxZ = Integer.MIN_VALUE;
        int spanX, spanZ;
        final Set<Long> members = new HashSet<Long>();
        final List<int[]> coords = new ArrayList<int[]>();
        final Map<Long,Integer> topByColumn = new HashMap<Long,Integer>();
    }

    private static final class FeaturePoint {
        boolean present;
        int drop;
        int x, y, z;
    }

    private static final class Result {
        String status = "OK";
        String error = "";
        Candidate c;
        int spawnX, spawnZ;
        int feetY = -1, supportY = -1;
        Component comp = new Component();
        double nearestElevated;
        double nearestSame;
        double clearPct;
        double skyblockScore;
        boolean skyblockCandidate;
        boolean tinySkyblock;
        int treeCount;
        boolean treeAtOrigin;
        int treeX = 9999, treeY = -1, treeZ = 9999;
        FeaturePoint water = new FeaturePoint();
        FeaturePoint lava = new FeaturePoint();

        String toCsv() {
            Component x = comp;
            return joinCsv(new String[] {
                status,
                Long.toString(c.seed), Long.toString(c.sequenceIndex), Integer.toString(c.p6Blocks), Integer.toString(c.p6Footprint),
                Integer.toString(spawnX), Integer.toString(spawnZ), Integer.toString(feetY), Integer.toString(supportY),
                bit(x.floating), bit(x.clipped), Integer.toString(x.blocks), Integer.toString(x.footprint), Integer.toString(x.spanX), Integer.toString(x.spanZ),
                Integer.toString(x.minY == 128 ? -1 : x.minY), Integer.toString(x.maxY), Integer.toString(x.minX == Integer.MAX_VALUE ? 0 : x.minX),
                Integer.toString(x.maxX == Integer.MIN_VALUE ? 0 : x.maxX), Integer.toString(x.minZ == Integer.MAX_VALUE ? 0 : x.minZ), Integer.toString(x.maxZ == Integer.MIN_VALUE ? 0 : x.maxZ),
                f2(nearestElevated), f2(nearestSame), f2(clearPct), f3(skyblockScore), bit(skyblockCandidate), bit(tinySkyblock),
                Integer.toString(treeCount), bit(treeAtOrigin), Integer.toString(treeX), Integer.toString(treeY), Integer.toString(treeZ),
                bit(water.present), Integer.toString(water.drop), Integer.toString(water.x), Integer.toString(water.y), Integer.toString(water.z),
                bit(lava.present), Integer.toString(lava.drop), Integer.toString(lava.x), Integer.toString(lava.y), Integer.toString(lava.z),
                bit(water.present && lava.present), error
            });
        }
    }

    private static String bit(boolean b) { return b ? "1" : "0"; }
    private static String f2(double v) { return String.format(java.util.Locale.ROOT, "%.2f", v); }
    private static String f3(double v) { return String.format(java.util.Locale.ROOT, "%.3f", v); }

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
            else if ("--chunk-radius".equals(a)) c.chunkRadius = Integer.parseInt(args[++i]);
            else if ("--isolation-radius".equals(a)) c.isolationRadius = Integer.parseInt(args[++i]);
            else if ("--liquid-ticks".equals(a)) c.liquidTicks = Integer.parseInt(args[++i]);
            else if ("--max-seeds".equals(a)) c.maxSeeds = Integer.parseInt(args[++i]);
            else if ("--progress-every".equals(a)) c.progressEvery = Integer.parseInt(args[++i]);
            else throw new IllegalArgumentException("unknown argument: " + a);
        }
        if (c.input == null) throw new IllegalArgumentException("--input is required");
        if (c.outputDir == null) throw new IllegalArgumentException("--output is required");
        if (c.chunkRadius < 3 || c.chunkRadius > 8) throw new IllegalArgumentException("--chunk-radius must be 3..8");
        if (c.isolationRadius < 16 || c.isolationRadius > c.chunkRadius * 16 - 8) {
            throw new IllegalArgumentException("--isolation-radius must fit inside loaded chunk window");
        }
        if (c.liquidTicks < 0 || c.liquidTicks > 1000) throw new IllegalArgumentException("--liquid-ticks must be 0..1000");
        return c;
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

    private static List<Candidate> readCandidates(File file) throws Exception {
        BufferedReader br = new BufferedReader(new FileReader(file));
        String header = br.readLine();
        if (header == null) throw new IllegalArgumentException("empty input CSV");
        List<String> names = splitCsv(header);
        Map<String,Integer> ix = new HashMap<String,Integer>();
        for (int i = 0; i < names.size(); ++i) ix.put(names.get(i), Integer.valueOf(i));
        String[] required = {"seed","sequence_index","component_blocks","footprint_columns","support_y"};
        for (String r : required) if (!ix.containsKey(r)) throw new IllegalArgumentException("missing input column: " + r);
        List<Candidate> out = new ArrayList<Candidate>();
        String line;
        while ((line = br.readLine()) != null) {
            if (line.trim().isEmpty()) continue;
            List<String> x = splitCsv(line);
            try {
                Candidate c = new Candidate();
                c.seed = Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex = Long.parseLong(x.get(ix.get("sequence_index").intValue()));
                c.p6Blocks = Integer.parseInt(x.get(ix.get("component_blocks").intValue()));
                c.p6Footprint = Integer.parseInt(x.get(ix.get("footprint_columns").intValue()));
                c.p6SupportY = Integer.parseInt(x.get(ix.get("support_y").intValue()));
                out.add(c);
            } catch (RuntimeException ignored) {}
        }
        br.close();
        Collections.sort(out, new Comparator<Candidate>() {
            public int compare(Candidate a, Candidate b) {
                if (a.p6Footprint != b.p6Footprint) return a.p6Footprint < b.p6Footprint ? -1 : 1;
                if (a.p6Blocks != b.p6Blocks) return a.p6Blocks < b.p6Blocks ? -1 : 1;
                return a.sequenceIndex < b.sequenceIndex ? -1 : (a.sequenceIndex == b.sequenceIndex ? 0 : 1);
            }
        });
        return out;
    }

    private static Set<Long> readProcessed(File master) throws Exception {
        Set<Long> out = new HashSet<Long>();
        if (!master.isFile()) return out;
        BufferedReader br = new BufferedReader(new FileReader(master));
        String header = br.readLine();
        if (header == null) { br.close(); return out; }
        List<String> h = splitCsv(header);
        int seqIx = h.indexOf("sequence_index");
        String line;
        while ((line = br.readLine()) != null) {
            List<String> x = splitCsv(line);
            if (seqIx >= 0 && seqIx < x.size()) {
                try { out.add(Long.valueOf(Long.parseLong(x.get(seqIx)))); } catch (RuntimeException ignored) {}
            }
        }
        br.close();
        return out;
    }

    private static void loadSpawnArea(World world, int r) {
        world.getChunkFromChunkCoords(0, 0);
        for (int ring = 1; ring <= r; ++ring) {
            for (int x = -ring; x <= ring; ++x) {
                world.getChunkFromChunkCoords(x, -ring);
                world.getChunkFromChunkCoords(x, ring);
            }
            for (int z = -ring + 1; z <= ring - 1; ++z) {
                world.getChunkFromChunkCoords(-ring, z);
                world.getChunkFromChunkCoords(ring, z);
            }
        }
        for (int z = -r + 1; z <= r - 1; ++z) {
            for (int x = -r + 1; x <= r - 1; ++x) {
                world.chunkProvider.populate(world.chunkProvider, x, z);
            }
        }
    }

    private static void settleLiquids(World world, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            world.setWorldTime(world.getWorldTime() + 1L);
            world.TickUpdates(false);
        }
    }

    private static boolean collidesAtFeet(World world, int feetY) {
        AxisAlignedBB player = AxisAlignedBB.getBoundingBox(0.2D, (double)feetY, 0.2D, 0.8D, (double)feetY + 1.8D, 0.8D);
        for (int y = Math.max(0, feetY - 1); y <= Math.min(127, feetY + 2); ++y) {
            int id = world.getBlockId(0, y, 0);
            if (id == 0) continue;
            Block b = Block.blocksList[id];
            if (b == null) continue;
            AxisAlignedBB box = b.getCollisionBoundingBoxFromPool(world, 0, y, 0);
            if (box != null && box.intersectsWith(player)) return true;
        }
        return false;
    }

    private static int actualFeetY(World world) {
        int feet = 65;
        while (feet < 127 && collidesAtFeet(world, feet)) ++feet;
        return feet;
    }

    private static boolean isTerrainId(int id) {
        switch (id) {
            case 1: case 2: case 3: case 4: case 7: case 12: case 13:
            case 14: case 15: case 16: case 21: case 24: case 48: case 56:
            case 73: case 74: case 79: case 80: case 82:
                return true;
            default: return false;
        }
    }

    private static boolean isWater(int id) { return id == WATER_MOVING || id == WATER_STILL; }
    private static boolean isLava(int id) { return id == LAVA_MOVING || id == LAVA_STILL; }

    private static long blockKey(int x, int y, int z) {
        long xx = (long)(x + 1024) & 2047L;
        long zz = (long)(z + 1024) & 2047L;
        return (xx << 18) | (zz << 7) | (long)(y & 127);
    }

    private static long columnKey(int x, int z) {
        return ((long)x << 32) ^ ((long)z & 0xffffffffL);
    }

    private static int columnX(long k) { return (int)(k >> 32); }
    private static int columnZ(long k) { return (int)k; }

    private static int findSupport(World world, int feetY) {
        for (int y = Math.min(127, feetY - 1); y >= 64; --y) {
            if (isTerrainId(world.getBlockId(0, y, 0))) return y;
        }
        return -1;
    }

    private static Component traceComponent(World world, int startY, int bound) {
        Component c = new Component();
        if (startY < 0 || !isTerrainId(world.getBlockId(0, startY, 0))) return c;
        ArrayDeque<int[]> q = new ArrayDeque<int[]>();
        Set<Long> seen = c.members;
        q.add(new int[] {0, startY, 0});
        seen.add(Long.valueOf(blockKey(0, startY, 0)));
        final int[][] dirs = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        boolean ground = false;
        while (!q.isEmpty()) {
            int[] p = q.removeFirst();
            int x = p[0], y = p[1], z = p[2];
            c.coords.add(p);
            ++c.blocks;
            if (y <= 63) { ground = true; break; }
            if (Math.abs(x) >= bound || Math.abs(z) >= bound || c.blocks > 200000) { c.clipped = true; break; }
            if (x < c.minX) c.minX = x; if (x > c.maxX) c.maxX = x;
            if (y < c.minY) c.minY = y; if (y > c.maxY) c.maxY = y;
            if (z < c.minZ) c.minZ = z; if (z > c.maxZ) c.maxZ = z;
            long col = columnKey(x,z);
            Integer old = c.topByColumn.get(Long.valueOf(col));
            if (old == null || y > old.intValue()) c.topByColumn.put(Long.valueOf(col), Integer.valueOf(y));
            for (int[] d : dirs) {
                int nx=x+d[0], ny=y+d[1], nz=z+d[2];
                if (ny < 0 || ny >= 128) continue;
                if (Math.abs(nx) > bound || Math.abs(nz) > bound) { c.clipped = true; continue; }
                if (!isTerrainId(world.getBlockId(nx,ny,nz))) continue;
                Long k = Long.valueOf(blockKey(nx,ny,nz));
                if (seen.add(k)) q.addLast(new int[] {nx,ny,nz});
            }
        }
        c.footprint = c.topByColumn.size();
        if (c.minX != Integer.MAX_VALUE) {
            c.spanX = c.maxX - c.minX + 1;
            c.spanZ = c.maxZ - c.minZ + 1;
        }
        c.floating = !ground && !c.clipped && c.blocks > 0;
        return c;
    }

    private static void analyzeIsolation(World world, Result r, int radius) {
        Component c = r.comp;
        if (!c.floating) return;
        double nearestElev = radius + 1.0;
        double nearestSame = radius + 1.0;
        int clear = 0, total = 0;
        int elevY = Math.max(65, c.minY - 6);
        int sameY = Math.max(65, r.feetY - 8);
        int pad = 2;
        for (int z = -radius; z <= radius; z += 2) {
            for (int x = -radius; x <= radius; x += 2) {
                if (x*x + z*z > radius*radius) continue;
                if (x >= c.minX-pad && x <= c.maxX+pad && z >= c.minZ-pad && z <= c.maxZ+pad) continue;
                ++total;
                int top = world.getHeightValue(x,z) - 1;
                double d = Math.sqrt((double)x*(double)x + (double)z*(double)z);
                if (top >= elevY && d < nearestElev) nearestElev = d;
                else if (top < elevY) ++clear;
                if (top >= sameY && d < nearestSame) nearestSame = d;
            }
        }
        r.nearestElevated = nearestElev;
        r.nearestSame = nearestSame;
        r.clearPct = total == 0 ? 0.0 : 100.0 * (double)clear / (double)total;
        int maxSpan = Math.max(c.spanX, c.spanZ);
        r.skyblockScore =
              Math.min((double)radius + 1.0, nearestElev) * 5.0
            + Math.min((double)radius + 1.0, nearestSame) * 3.0
            + r.clearPct * 1.8
            + Math.max(0, 64 - c.footprint) * 1.6
            + Math.max(0, 16 - maxSpan) * 2.0
            + Math.max(0, c.minY - 65) * 0.25;
        r.skyblockCandidate = c.floating && c.footprint <= 64 && c.blocks <= 768 && c.spanX <= 16 && c.spanZ <= 16 && nearestElev >= 18.0;
        r.tinySkyblock = c.floating && c.footprint <= 32 && c.blocks <= 384 && c.spanX <= 12 && c.spanZ <= 12 && nearestElev >= 18.0;
    }

    private static void analyzeTrees(World world, Result r) {
        if (!r.comp.floating) return;
        int count = 0;
        for (Map.Entry<Long,Integer> e : r.comp.topByColumn.entrySet()) {
            int x = columnX(e.getKey().longValue());
            int z = columnZ(e.getKey().longValue());
            int y = e.getValue().intValue() + 1;
            if (y < 128 && world.getBlockId(x,y,z) == WOOD) {
                ++count;
                if (r.treeY < 0) { r.treeX=x; r.treeY=y; r.treeZ=z; }
                if (x == 0 && z == 0) r.treeAtOrigin = true;
            }
        }
        r.treeCount = count;
    }

    private static int verticalFluidDrop(World world, int x, int y, int z, boolean water) {
        int id = world.getBlockId(x,y,z);
        if (water ? !isWater(id) : !isLava(id)) return 0;
        int low = y;
        for (int yy = y - 1; yy >= 0; --yy) {
            int q = world.getBlockId(x,yy,z);
            if (water ? isWater(q) : isLava(q)) low = yy;
            else break;
        }
        return y - low;
    }

    private static void considerFluid(World world, Result r, int fx, int fy, int fz, boolean water, Set<Long> seen) {
        if (fy < 65 || fy >= 128) return;
        int id = world.getBlockId(fx,fy,fz);
        if (water ? !isWater(id) : !isLava(id)) return;
        Long k = Long.valueOf(blockKey(fx,fy,fz));
        if (!seen.add(k)) return;
        FeaturePoint best = water ? r.water : r.lava;
        int[][] dirs = {{0,0},{1,0},{-1,0},{0,1},{0,-1}};
        for (int[] d : dirs) {
            int x=fx+d[0], z=fz+d[1];
            int q = world.getBlockId(x,fy,z);
            if (!(water ? isWater(q) : isLava(q))) continue;
            int drop = verticalFluidDrop(world,x,fy,z,water);
            if (drop >= 3 && (!best.present || drop > best.drop)) {
                best.present=true; best.drop=drop; best.x=x; best.y=fy; best.z=z;
            }
        }
    }

    private static void analyzeFluids(World world, Result r) {
        if (!r.comp.floating) return;
        Set<Long> seenWater = new HashSet<Long>();
        Set<Long> seenLava = new HashSet<Long>();
        final int[][] dirs = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        for (int[] p : r.comp.coords) {
            for (int[] d : dirs) {
                int x=p[0]+d[0], y=p[1]+d[1], z=p[2]+d[2];
                if (y < 65 || y >= 128) continue;
                int id = world.getBlockId(x,y,z);
                if (isWater(id)) considerFluid(world,r,x,y,z,true,seenWater);
                else if (isLava(id)) considerFluid(world,r,x,y,z,false,seenLava);
            }
        }
    }

    private static Result analyzeSeed(Candidate c, Config cfg) {
        Result r = new Result();
        r.c = c;
        try {
            World world = new World(new MemorySaveHandler(), "oracle", c.seed, null);
            r.spawnX = world.worldInfo.getSpawnX();
            r.spawnZ = world.worldInfo.getSpawnZ();
            if (r.spawnX != 0 || r.spawnZ != 0) {
                r.status = "SPAWN_MOVED";
                r.error = "vanilla spawn was not 0,0";
                return r;
            }
            loadSpawnArea(world, cfg.chunkRadius);
            settleLiquids(world, cfg.liquidTicks);
            r.feetY = actualFeetY(world);
            r.supportY = findSupport(world, r.feetY);
            if (r.supportY < 0) {
                r.status = "NO_SUPPORT";
                r.error = "no terrain support above sea level";
                return r;
            }
            int bound = cfg.chunkRadius * 16 - 6;
            r.comp = traceComponent(world, r.supportY, bound);
            analyzeIsolation(world, r, cfg.isolationRadius);
            analyzeTrees(world, r);
            analyzeFluids(world, r);
        } catch (Throwable t) {
            r.status = "ERROR";
            String s = t.getClass().getSimpleName() + ": " + String.valueOf(t.getMessage());
            r.error = s.replace('\n',' ').replace('\r',' ');
        }
        return r;
    }

    private static List<Map<String,String>> readMasterRows(File master) throws Exception {
        List<Map<String,String>> rows = new ArrayList<Map<String,String>>();
        if (!master.isFile()) return rows;
        BufferedReader br = new BufferedReader(new FileReader(master));
        String header = br.readLine();
        if (header == null) { br.close(); return rows; }
        List<String> h = splitCsv(header);
        String line;
        while ((line = br.readLine()) != null) {
            List<String> x = splitCsv(line);
            LinkedHashMap<String,String> row = new LinkedHashMap<String,String>();
            for (int i=0;i<h.size();++i) row.put(h.get(i), i<x.size()?x.get(i):"");
            rows.add(row);
        }
        br.close();
        return rows;
    }

    private interface RowFilter { boolean keep(Map<String,String> r); }
    private interface RowScore { double score(Map<String,String> r); }

    private static int i(Map<String,String> r, String k) { try { return Integer.parseInt(r.get(k)); } catch(Exception e){return 0;} }
    private static double d(Map<String,String> r, String k) { try { return Double.parseDouble(r.get(k)); } catch(Exception e){return 0.0;} }

    private static void writeList(File path, List<Map<String,String>> all, final RowFilter filter, final RowScore score) throws Exception {
        List<Map<String,String>> rows = new ArrayList<Map<String,String>>();
        for (Map<String,String> r : all) if ("OK".equals(r.get("status")) && filter.keep(r)) rows.add(r);
        Collections.sort(rows, new Comparator<Map<String,String>>() {
            public int compare(Map<String,String> a, Map<String,String> b) {
                double aa=score.score(a), bb=score.score(b);
                if (aa != bb) return aa > bb ? -1 : 1;
                long sa=Long.parseLong(a.get("sequence_index")), sb=Long.parseLong(b.get("sequence_index"));
                return sa<sb?-1:(sa==sb?0:1);
            }
        });
        PrintWriter pw = new PrintWriter(new BufferedWriter(new FileWriter(path,false)));
        pw.println(MASTER_HEADER);
        for (Map<String,String> r : rows) {
            String[] h = MASTER_HEADER.split(",");
            String[] v = new String[h.length];
            for (int j=0;j<h.length;++j) v[j]=r.get(h[j]);
            pw.println(joinCsv(v));
        }
        pw.close();
    }

    private static void buildSeparateLists(File outDir, File master) throws Exception {
        List<Map<String,String>> all = readMasterRows(master);
        writeList(new File(outDir,"tree_spawns.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"tree_count")>0 && i(r,"floating")==1;}},
            new RowScore(){public double score(Map<String,String> r){return i(r,"tree_count")*100000.0 + d(r,"nearest_elevated");}});
        writeList(new File(outDir,"tree_at_0_0.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"tree_at_0_0")==1 && i(r,"floating")==1;}},
            new RowScore(){public double score(Map<String,String> r){return d(r,"nearest_elevated")*1000.0 - i(r,"footprint");}});
        writeList(new File(outDir,"good_skyblock_islands.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"skyblock_candidate")==1;}},
            new RowScore(){public double score(Map<String,String> r){return d(r,"skyblock_score");}});
        writeList(new File(outDir,"tiny_skyblock_islands.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"tiny_skyblock")==1;}},
            new RowScore(){public double score(Map<String,String> r){return d(r,"skyblock_score");}});
        writeList(new File(outDir,"waterfalls.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"waterfall")==1 && i(r,"floating")==1;}},
            new RowScore(){public double score(Map<String,String> r){return i(r,"water_drop")*1000.0 + d(r,"nearest_elevated");}});
        writeList(new File(outDir,"lavafalls.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"lavafall")==1 && i(r,"floating")==1;}},
            new RowScore(){public double score(Map<String,String> r){return i(r,"lava_drop")*1000.0 + d(r,"nearest_elevated");}});
        writeList(new File(outDir,"water_and_lava_falls.csv"), all,
            new RowFilter(){public boolean keep(Map<String,String> r){return i(r,"both_fluids")==1 && i(r,"floating")==1;}},
            new RowScore(){public double score(Map<String,String> r){return Math.min(i(r,"water_drop"),i(r,"lava_drop"))*100000.0 + d(r,"nearest_elevated");}});

        PrintWriter s = new PrintWriter(new BufferedWriter(new FileWriter(new File(outDir,"EXACT_SUMMARY.txt"),false)));
        s.println("BETA 1.7.3 EXACT SKYBLOCK ORACLE SUMMARY");
        s.println("Processed rows: " + all.size());
        String[] files = {"tree_spawns.csv","tree_at_0_0.csv","good_skyblock_islands.csv","tiny_skyblock_islands.csv","waterfalls.csv","lavafalls.csv","water_and_lava_falls.csv"};
        for (String f : files) {
            int n = Math.max(0, countLines(new File(outDir,f)) - 1);
            s.println(f + ": " + n);
        }
        s.println();
        s.println("Lists are intentionally separate: tree presence does not affect SkyBlock score, and fluid lists do not require a tree.");
        s.close();
    }

    private static int countLines(File f) throws Exception {
        if (!f.isFile()) return 0;
        BufferedReader br=new BufferedReader(new FileReader(f)); int n=0; while(br.readLine()!=null)++n; br.close(); return n;
    }

    public static void main(String[] args) throws Exception {
        Config cfg = parseArgs(args);
        cfg.outputDir.mkdirs();
        File master = new File(cfg.outputDir,"actual_features_all.csv");
        Set<Long> processed = readProcessed(master);
        List<Candidate> candidates = readCandidates(cfg.input);

        boolean newMaster = !master.isFile() || master.length()==0L;
        PrintWriter append = new PrintWriter(new BufferedWriter(new FileWriter(master,true)));
        if (newMaster) { append.println(MASTER_HEADER); append.flush(); }

        long start = System.nanoTime();
        int doneNow=0, attempted=0;
        System.out.println("Beta 1.7.3 exact population oracle");
        System.out.println("inputCandidates="+candidates.size()+" alreadyProcessed="+processed.size()+" chunkRadius="+cfg.chunkRadius+" isolationRadius="+cfg.isolationRadius);
        System.out.println("Processing smallest P6 footprints first. Results append immediately; rerun resumes.");

        for (Candidate c : candidates) {
            if (processed.contains(Long.valueOf(c.sequenceIndex))) continue;
            if (cfg.maxSeeds > 0 && attempted >= cfg.maxSeeds) break;
            ++attempted;
            Result r = analyzeSeed(c,cfg);
            append.println(r.toCsv());
            append.flush();
            processed.add(Long.valueOf(c.sequenceIndex));
            ++doneNow;
            if (r.treeCount>0 || r.water.present || r.lava.present || r.tinySkyblock) {
                System.out.println("HIT seed="+c.seed+" foot="+r.comp.footprint+" blocks="+r.comp.blocks+" tree="+r.treeCount+" tree0="+bit(r.treeAtOrigin)+" water="+bit(r.water.present)+"/"+r.water.drop+" lava="+bit(r.lava.present)+"/"+r.lava.drop+" tiny="+bit(r.tinySkyblock)+" near="+f2(r.nearestElevated));
            }
            if (doneNow % cfg.progressEvery == 0) {
                double sec=(System.nanoTime()-start)/1.0e9;
                System.out.println("progress new="+doneNow+" totalProcessed="+processed.size()+"/"+candidates.size()+" rate="+f2(doneNow/Math.max(0.001,sec))+" seeds/s");
                if ((doneNow % 100)==0) System.gc();
            }
        }
        append.close();
        buildSeparateLists(cfg.outputDir,master);
        double sec=(System.nanoTime()-start)/1.0e9;
        System.out.println("DONE new="+doneNow+" processedTotal="+processed.size()+" elapsed="+f2(sec)+"s");
        System.out.println("Summary: "+new File(cfg.outputDir,"EXACT_SUMMARY.txt").getAbsolutePath());
    }
}
