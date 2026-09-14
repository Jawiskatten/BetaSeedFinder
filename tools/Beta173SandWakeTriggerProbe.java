package net.minecraft.src;

import java.io.*;
import java.util.*;

/**
 * Exact Beta 1.7.3 diagnostic for WHY a proven sand-wake geometry does or does
 * not wake during normal client startup.
 *
 * It attaches an IWorldAccess before the 17x17 Building-terrain access pass.
 * setBlockWithNotify/setBlockAndMetadataWithNotify call the observer before
 * neighbor notifications are delivered, so we can see population block changes
 * that occur while World.scheduledUpdatesAreImmediate is true (the critical
 * WorldGenLiquids window). We then measure how close those changes came to the
 * original spawn sand stack and whether one was directly adjacent to it.
 */
public final class Beta173SandWakeTriggerProbe {
    private static final int AIR = 0;
    private static final int WATER_MOVING = 8;
    private static final int WATER_STILL = 9;
    private static final int LAVA_MOVING = 10;
    private static final int LAVA_STILL = 11;

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

    private static final class Candidate {
        long seed;
        long sequenceIndex;
        int predictedDrop;
    }

    private static final class TriggerTrace implements IWorldAccess {
        final World world;
        final int bottomY;
        final int gateY;
        int immediateEvents;
        int immediateLiquidEvents;
        int directImmediateEvents;
        int directImmediateLiquidEvents;
        int minImmediateDistance = Integer.MAX_VALUE;
        int minLiquidDistance = Integer.MAX_VALUE;
        int nearestLiquidX, nearestLiquidY, nearestLiquidZ, nearestLiquidId;
        int firstDirectX, firstDirectY, firstDirectZ, firstDirectId;
        boolean firstDirectSet;

        TriggerTrace(World world, int bottomY, int gateY) {
            this.world = world;
            this.bottomY = bottomY;
            this.gateY = gateY;
        }

        private int distanceToOriginalSand(int x, int y, int z) {
            int best = Integer.MAX_VALUE;
            for (int sy = bottomY; sy <= gateY; ++sy) {
                int d = Math.abs(x) + Math.abs(z) + Math.abs(y - sy);
                if (d < best) best = d;
            }
            return best;
        }

        public void markBlockAndNeighborsNeedsUpdate(int x, int y, int z) {
            if (!world.scheduledUpdatesAreImmediate) return;
            ++immediateEvents;
            int id = world.getBlockId(x, y, z);
            int d = distanceToOriginalSand(x, y, z);
            if (d < minImmediateDistance) minImmediateDistance = d;
            boolean liquid = id == WATER_MOVING || id == WATER_STILL || id == LAVA_MOVING || id == LAVA_STILL;
            if (liquid) {
                ++immediateLiquidEvents;
                if (d < minLiquidDistance) {
                    minLiquidDistance = d;
                    nearestLiquidX = x; nearestLiquidY = y; nearestLiquidZ = z; nearestLiquidId = id;
                }
            }
            // A block change at Manhattan distance 1 is exactly a change whose
            // notifyBlocksOfNeighborChange call includes one of the original
            // sand blocks.
            if (d == 1) {
                ++directImmediateEvents;
                if (liquid) ++directImmediateLiquidEvents;
                if (!firstDirectSet) {
                    firstDirectSet = true;
                    firstDirectX = x; firstDirectY = y; firstDirectZ = z; firstDirectId = id;
                }
            }
        }
        public void markBlockRangeNeedsUpdate(int a,int b,int c,int d,int e,int f) {}
        public void playSound(String s,double a,double b,double c,float d,float e) {}
        public void spawnParticle(String s,double a,double b,double c,double d,double e,double f) {}
        public void obtainEntitySkin(Entity e) {}
        public void releaseEntitySkin(Entity e) {}
        public void updateAllRenderers() {}
        public void playRecord(String s,int a,int b,int c) {}
        public void doNothingWithTileEntity(int a,int b,int c,TileEntity t) {}
        public void playAuxSFX(EntityPlayer p,int a,int b,int c,int d,int e) {}
    }

    private static List<String> splitCsv(String line) {
        ArrayList<String> out = new ArrayList<String>();
        StringBuilder cur = new StringBuilder();
        boolean quoted = false;
        for (int i=0;i<line.length();++i) {
            char ch=line.charAt(i);
            if (quoted) {
                if (ch=='\"') {
                    if (i+1<line.length() && line.charAt(i+1)=='\"') { cur.append('\"'); ++i; }
                    else quoted=false;
                } else cur.append(ch);
            } else {
                if (ch=='\"') quoted=true;
                else if (ch==',') { out.add(cur.toString()); cur.setLength(0); }
                else cur.append(ch);
            }
        }
        out.add(cur.toString());
        return out;
    }

    private static List<Candidate> readCandidates(File dir) throws Exception {
        ArrayList<Candidate> out = new ArrayList<Candidate>();
        File[] files = dir.listFiles(new FilenameFilter() {
            public boolean accept(File d, String n) { return n.startsWith("candidates_") && n.endsWith(".csv"); }
        });
        if (files == null) return out;
        Arrays.sort(files, new Comparator<File>() { public int compare(File a, File b) { return a.getName().compareTo(b.getName()); }});
        for (File file : files) {
            BufferedReader br = new BufferedReader(new FileReader(file));
            String header = br.readLine();
            if (header == null) { br.close(); continue; }
            List<String> h = splitCsv(header);
            int seedIx=h.indexOf("seed"), seqIx=h.indexOf("sequence_index"), predIx=h.indexOf("potential_drop");
            String line;
            while ((line=br.readLine())!=null) {
                if (line.trim().isEmpty()) continue;
                List<String> x=splitCsv(line);
                try {
                    Candidate c=new Candidate();
                    c.seed=Long.parseLong(x.get(seedIx));
                    c.sequenceIndex=Long.parseLong(x.get(seqIx));
                    c.predictedDrop=predIx>=0?Integer.parseInt(x.get(predIx)):-1;
                    out.add(c);
                } catch (RuntimeException ignored) {}
            }
            br.close();
        }
        Collections.sort(out,new Comparator<Candidate>() {
            public int compare(Candidate a,Candidate b) {
                return a.sequenceIndex<b.sequenceIndex?-1:(a.sequenceIndex==b.sequenceIndex?0:1);
            }
        });
        return out;
    }

    private static int firstUncoveredY(World world,int x,int z) {
        int y=63; while(y+1<128 && !world.isAirBlock(x,y+1,z)) ++y; return y;
    }
    private static int[] snapshotColumn(World world) {
        int[] ids=new int[128]; for(int y=0;y<128;++y) ids[y]=world.getBlockId(0,y,0); return ids;
    }
    private static int bottomOfSandStack(int[] ids,int gateY) {
        int y=gateY; while(y-1>=0 && ids[y-1]==Block.sand.blockID) --y; return y;
    }
    private static int countNewSandBelow(int[] before,int[] after,int belowExclusive) {
        int n=0; for(int y=0;y<belowExclusive;++y) if(before[y]!=Block.sand.blockID && after[y]==Block.sand.blockID) ++n; return n;
    }
    private static World freshWorld(long seed) { return new World(new MemorySaveHandler(),"sand-wake-trigger-probe",seed,(WorldProvider)null); }
    private static void buildTerrainExactlyLikeClient(World world,int spawnX,int spawnZ) {
        IChunkProvider provider=world.getIChunkProvider();
        if(provider instanceof ChunkProviderLoadOrGenerate) ((ChunkProviderLoadOrGenerate)provider).setCurrentChunkOver(spawnX>>4,spawnZ>>4);
        for(int dx=-128;dx<=128;dx+=16) for(int dz=-128;dz<=128;dz+=16) world.getBlockId(spawnX+dx,64,spawnZ+dz);
        world.func_656_j();
    }

    private static String dist(int x) { return x==Integer.MAX_VALUE?"NONE":Integer.toString(x); }

    public static void main(String[] args) throws Exception {
        File inputDir=null, output=null;
        for(int i=0;i<args.length;++i) {
            if("--input-dir".equals(args[i])) inputDir=new File(args[++i]);
            else if("--output".equals(args[i])) output=new File(args[++i]);
            else throw new IllegalArgumentException("unknown arg "+args[i]);
        }
        if(inputDir==null||output==null) throw new IllegalArgumentException("--input-dir and --output required");
        File parent=output.getParentFile(); if(parent!=null) parent.mkdirs();
        List<Candidate> candidates=readCandidates(inputDir);
        PrintWriter out=new PrintWriter(new BufferedWriter(new FileWriter(output,false)));
        out.println("seed,sequence_index,predicted_drop,status,pre_bottom_y,pre_sand_blocks,immediate_events,immediate_liquid_events,direct_immediate_events,direct_immediate_liquid_events,min_immediate_dist,min_liquid_dist,nearest_liquid_x,nearest_liquid_y,nearest_liquid_z,nearest_liquid_id,first_direct_x,first_direct_y,first_direct_z,first_direct_id,gate_destroyed,new_sand_below,natural_wake");
        int done=0, direct=0, directLiquid=0, wakes=0, anyImmediate=0, anyLiquid=0;
        long started=System.nanoTime();
        for(Candidate c:candidates) {
            String status="OK";
            int bottom=-1,stack=0; TriggerTrace tr=null; boolean gateDestroyed=false,naturalWake=false; int newSand=0;
            try {
                World world=freshWorld(c.seed);
                ChunkCoordinates sp=world.getSpawnPoint();
                if(sp.x!=0||sp.z!=0) status="SPAWN_MOVED";
                else {
                    int gate=firstUncoveredY(world,0,0);
                    if(gate!=63||world.getBlockId(0,gate,0)!=Block.sand.blockID) status="PRE_GATE_MISMATCH";
                    else {
                        int[] before=snapshotColumn(world);
                        bottom=bottomOfSandStack(before,gate); stack=gate-bottom+1;
                        tr=new TriggerTrace(world,bottom,gate);
                        world.addWorldAccess(tr);
                        buildTerrainExactlyLikeClient(world,0,0);
                        int[] after=snapshotColumn(world);
                        gateDestroyed=after[gate]!=Block.sand.blockID;
                        newSand=countNewSandBelow(before,after,bottom);
                        naturalWake=gateDestroyed&&newSand>0;
                    }
                }
            } catch(Throwable t) { status="ERROR:"+t.getClass().getSimpleName(); }
            if(tr==null) tr=new TriggerTrace(freshWorld(c.seed),bottom,63);
            if(tr.immediateEvents>0) ++anyImmediate;
            if(tr.immediateLiquidEvents>0) ++anyLiquid;
            if(tr.directImmediateEvents>0) ++direct;
            if(tr.directImmediateLiquidEvents>0) ++directLiquid;
            if(naturalWake) ++wakes;
            out.println(c.seed+","+c.sequenceIndex+","+c.predictedDrop+","+status+","+bottom+","+stack+","+
                tr.immediateEvents+","+tr.immediateLiquidEvents+","+tr.directImmediateEvents+","+tr.directImmediateLiquidEvents+","+
                dist(tr.minImmediateDistance)+","+dist(tr.minLiquidDistance)+","+
                (tr.minLiquidDistance==Integer.MAX_VALUE?"":Integer.toString(tr.nearestLiquidX))+","+
                (tr.minLiquidDistance==Integer.MAX_VALUE?"":Integer.toString(tr.nearestLiquidY))+","+
                (tr.minLiquidDistance==Integer.MAX_VALUE?"":Integer.toString(tr.nearestLiquidZ))+","+
                (tr.minLiquidDistance==Integer.MAX_VALUE?"":Integer.toString(tr.nearestLiquidId))+","+
                (tr.firstDirectSet?Integer.toString(tr.firstDirectX):"")+","+
                (tr.firstDirectSet?Integer.toString(tr.firstDirectY):"")+","+
                (tr.firstDirectSet?Integer.toString(tr.firstDirectZ):"")+","+
                (tr.firstDirectSet?Integer.toString(tr.firstDirectId):"")+","+
                (gateDestroyed?1:0)+","+newSand+","+(naturalWake?1:0));
            out.flush();
            ++done;
            double sec=(System.nanoTime()-started)/1.0e9;
            if(done%10==0||done==candidates.size()) System.out.println("trigger-probe "+done+"/"+candidates.size()+" rate="+String.format(Locale.ROOT,"%.2f",done/Math.max(sec,0.001))+"/s anyImmediate="+anyImmediate+" anyLiquid="+anyLiquid+" direct="+direct+" directLiquid="+directLiquid+" wakes="+wakes);
        }
        out.close();
        System.out.println("TRIGGER PROBE DONE rows="+done+" anyImmediate="+anyImmediate+" anyLiquid="+anyLiquid+" directImmediateNeighbor="+direct+" directImmediateLiquidNeighbor="+directLiquid+" naturalWakes="+wakes);
        System.out.println("OUTPUT="+output.getAbsolutePath());
    }
}
