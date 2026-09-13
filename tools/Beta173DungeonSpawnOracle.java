package net.minecraft.src;

import java.io.*;
import java.util.*;

/**
 * Exact Beta 1.7.3 verifier for P1 dungeon-spawn GPU candidates.
 *
 * The GPU stage predicts the first dungeon attempt in population chunk (-1,-1)
 * for the lake-free RNG subset. This verifier runs the actual Beta server terrain,
 * caves, population, WorldGenDungeons, block collision boxes, and the real spawn
 * coordinate gate. A hit is only called spawn_inside_dungeon when the expected
 * mob spawner actually generated and preparePlayerToSpawn's upward collision rule
 * leaves the player's feet inside that dungeon room at x=z=0.
 */
public final class Beta173DungeonSpawnOracle {
    private static final int MOB_SPAWNER = 52;
    private static final int CHEST = 54;

    private static final String HEADER =
        "status,seed,sequence_index,prepop_sand_y,predicted_feet_y," +
        "dungeon_x,dungeon_y,dungeon_z,radius_x,radius_z," +
        "spawn_x,spawn_z,actual_feet_y,expected_spawner_generated," +
        "origin_inside_room,spawn_inside_dungeon,spawn_on_spawner,chest_count,error";

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

    private static final class Candidate {
        long seed, sequenceIndex;
        int sandY, predictedFeet;
        int dx, dy, dz, rx, rz;
    }

    private static final class Result {
        Candidate c;
        String status = "OK";
        String error = "";
        int spawnX, spawnZ;
        int actualFeet = -1;
        boolean generated;
        boolean originInside;
        boolean spawnInside;
        boolean onSpawner;
        int chestCount;

        String csv() {
            return join(new String[] {
                status, Long.toString(c.seed), Long.toString(c.sequenceIndex),
                Integer.toString(c.sandY), Integer.toString(c.predictedFeet),
                Integer.toString(c.dx), Integer.toString(c.dy), Integer.toString(c.dz),
                Integer.toString(c.rx), Integer.toString(c.rz),
                Integer.toString(spawnX), Integer.toString(spawnZ), Integer.toString(actualFeet),
                bit(generated), bit(originInside), bit(spawnInside), bit(onSpawner),
                Integer.toString(chestCount), error
            });
        }
    }

    private static String bit(boolean v) { return v ? "1" : "0"; }

    private static String join(String[] a) {
        StringBuilder b = new StringBuilder();
        for (int i=0;i<a.length;i++) {
            if (i>0) b.append(',');
            String s=a[i]==null?"":a[i];
            if (s.indexOf(',')>=0 || s.indexOf('"')>=0 || s.indexOf('\n')>=0 || s.indexOf('\r')>=0) {
                b.append('"').append(s.replace("\"","\"\"")).append('"');
            } else b.append(s);
        }
        return b.toString();
    }

    private static List<String> split(String line) {
        ArrayList<String> out=new ArrayList<String>();
        StringBuilder b=new StringBuilder();
        boolean q=false;
        for (int i=0;i<line.length();i++) {
            char ch=line.charAt(i);
            if (q) {
                if (ch=='"') {
                    if (i+1<line.length() && line.charAt(i+1)=='"') { b.append('"'); i++; }
                    else q=false;
                } else b.append(ch);
            } else {
                if (ch=='"') q=true;
                else if (ch==',') { out.add(b.toString()); b.setLength(0); }
                else b.append(ch);
            }
        }
        out.add(b.toString());
        return out;
    }

    private static List<Candidate> readCandidates(File f) throws Exception {
        BufferedReader br=new BufferedReader(new FileReader(f));
        String hline=br.readLine();
        if (hline==null) throw new IllegalArgumentException("empty candidate CSV: "+f);
        List<String> h=split(hline);
        Map<String,Integer> ix=new HashMap<String,Integer>();
        for (int i=0;i<h.size();i++) ix.put(h.get(i),Integer.valueOf(i));
        String[] need={"seed","sequence_index","prepop_sand_y","dungeon_x","dungeon_y","dungeon_z","radius_x","radius_z","predicted_feet_y"};
        for (String n:need) if(!ix.containsKey(n)) throw new IllegalArgumentException("missing candidate column "+n);
        ArrayList<Candidate> out=new ArrayList<Candidate>();
        String line;
        while((line=br.readLine())!=null) {
            if(line.trim().isEmpty()) continue;
            List<String> x=split(line);
            try {
                Candidate c=new Candidate();
                c.seed=Long.parseLong(x.get(ix.get("seed").intValue()));
                c.sequenceIndex=Long.parseLong(x.get(ix.get("sequence_index").intValue()));
                c.sandY=Integer.parseInt(x.get(ix.get("prepop_sand_y").intValue()));
                c.dx=Integer.parseInt(x.get(ix.get("dungeon_x").intValue()));
                c.dy=Integer.parseInt(x.get(ix.get("dungeon_y").intValue()));
                c.dz=Integer.parseInt(x.get(ix.get("dungeon_z").intValue()));
                c.rx=Integer.parseInt(x.get(ix.get("radius_x").intValue()));
                c.rz=Integer.parseInt(x.get(ix.get("radius_z").intValue()));
                c.predictedFeet=Integer.parseInt(x.get(ix.get("predicted_feet_y").intValue()));
                out.add(c);
            } catch(RuntimeException ignored) {}
        }
        br.close();
        return out;
    }

    private static Set<Long> processed(File master) throws Exception {
        HashSet<Long> out=new HashSet<Long>();
        if(!master.isFile()) return out;
        BufferedReader br=new BufferedReader(new FileReader(master));
        String hline=br.readLine();
        if(hline==null){br.close();return out;}
        List<String> h=split(hline);
        int ix=h.indexOf("sequence_index");
        String line;
        while((line=br.readLine())!=null) {
            List<String> x=split(line);
            if(ix>=0 && ix<x.size()) try{out.add(Long.valueOf(Long.parseLong(x.get(ix))));}catch(Exception ignored){}
        }
        br.close();
        return out;
    }

    private static void ensureDungeonPopulation(World w) {
        // World construction normally has chunk (0,0) loaded by the spawn gate.
        // Loading the other three chunks causes Beta's ChunkProvider to populate
        // (-1,-1) as soon as the required 2x2 neighborhood exists. Explicitly
        // calling populate afterwards is idempotent and covers alternate load state.
        w.getChunkFromChunkCoords(-1,-1);
        w.getChunkFromChunkCoords(-1, 0);
        w.getChunkFromChunkCoords( 0,-1);
        w.getChunkFromChunkCoords( 0, 0);
        w.chunkProvider.populate(w.chunkProvider,-1,-1);
    }

    private static boolean collidesAtFeet(World w,int feet) {
        AxisAlignedBB player=AxisAlignedBB.getBoundingBox(0.2D,(double)feet,0.2D,0.8D,(double)feet+1.8D,0.8D);
        for(int y=Math.max(0,feet-1);y<=Math.min(127,feet+2);y++) {
            int id=w.getBlockId(0,y,0);
            if(id==0) continue;
            Block block=Block.blocksList[id];
            if(block==null) continue;
            AxisAlignedBB box=block.getCollisionBoundingBoxFromPool(w,0,y,0);
            if(box!=null && box.intersectsWith(player)) return true;
        }
        return false;
    }

    private static int actualFeet(World w) {
        int y=65;
        while(y<127 && collidesAtFeet(w,y)) y++;
        return y;
    }

    private static Result verify(Candidate c) {
        Result r=new Result(); r.c=c;
        try {
            World w=new World(new MemorySaveHandler(),"dungeon-oracle",c.seed,null);
            r.spawnX=w.worldInfo.getSpawnX();
            r.spawnZ=w.worldInfo.getSpawnZ();
            if(r.spawnX!=0 || r.spawnZ!=0) {
                r.status="SPAWN_MOVED";
                r.error="vanilla spawn gate did not retain 0,0";
                return r;
            }

            ensureDungeonPopulation(w);
            r.generated=w.getBlockId(c.dx,c.dy,c.dz)==MOB_SPAWNER;
            r.actualFeet=actualFeet(w);
            r.originInside=(0>=c.dx-c.rx && 0<=c.dx+c.rx && 0>=c.dz-c.rz && 0<=c.dz+c.rz);
            r.spawnInside=r.generated && r.originInside && r.actualFeet>=c.dy && r.actualFeet<=c.dy+2;
            r.onSpawner=r.spawnInside && c.dx==0 && c.dz==0 && r.actualFeet==c.dy+1;

            int ch=0;
            for(int x=c.dx-c.rx;x<=c.dx+c.rx;x++) for(int z=c.dz-c.rz;z<=c.dz+c.rz;z++) {
                if(w.getBlockId(x,c.dy,z)==CHEST) ch++;
            }
            r.chestCount=ch;
            if(!r.generated) r.status="DUNGEON_FAILED";
            else if(!r.spawnInside) r.status="DUNGEON_GENERATED_NOT_SPAWN";
        } catch(Throwable t) {
            r.status="ERROR";
            r.error=(t.getClass().getSimpleName()+": "+String.valueOf(t.getMessage())).replace('\n',' ').replace('\r',' ');
        }
        return r;
    }

    private static List<Map<String,String>> readRows(File f) throws Exception {
        ArrayList<Map<String,String>> out=new ArrayList<Map<String,String>>();
        if(!f.isFile()) return out;
        BufferedReader br=new BufferedReader(new FileReader(f));
        String hline=br.readLine(); if(hline==null){br.close();return out;}
        List<String> h=split(hline); String line;
        while((line=br.readLine())!=null){List<String>x=split(line);LinkedHashMap<String,String>m=new LinkedHashMap<String,String>();for(int i=0;i<h.size();i++)m.put(h.get(i),i<x.size()?x.get(i):"");out.add(m);} br.close(); return out;
    }

    private static int iv(Map<String,String> r,String k){try{return Integer.parseInt(r.get(k));}catch(Exception e){return 0;}}
    private static long lv(Map<String,String> r,String k){try{return Long.parseLong(r.get(k));}catch(Exception e){return 0L;}}

    private interface Keep { boolean yes(Map<String,String> r); }
    private static void writeList(File f,List<Map<String,String>> rows,Keep keep,final Comparator<Map<String,String>> cmp)throws Exception{
        ArrayList<Map<String,String>> a=new ArrayList<Map<String,String>>();for(Map<String,String>r:rows)if(keep.yes(r))a.add(r);Collections.sort(a,cmp);
        PrintWriter pw=new PrintWriter(new BufferedWriter(new FileWriter(f,false)));pw.println(HEADER);String[]h=HEADER.split(",");for(Map<String,String>r:a){String[]v=new String[h.length];for(int i=0;i<h.length;i++)v[i]=r.get(h[i]);pw.println(join(v));}pw.close();
    }

    private static void rebuildLists(File out,File master)throws Exception{
        final List<Map<String,String>> rows=readRows(master);
        Comparator<Map<String,String>> insideCmp=new Comparator<Map<String,String>>(){public int compare(Map<String,String>a,Map<String,String>b){
            int ca=iv(a,"chest_count"),cb=iv(b,"chest_count"); if(ca!=cb)return cb-ca;
            int ya=iv(a,"actual_feet_y"),yb=iv(b,"actual_feet_y"); if(ya!=yb)return yb-ya;
            long sa=lv(a,"sequence_index"),sb=lv(b,"sequence_index");return sa<sb?-1:(sa==sb?0:1);
        }};
        Comparator<Map<String,String>> generatedCmp=new Comparator<Map<String,String>>(){public int compare(Map<String,String>a,Map<String,String>b){
            int da=Math.abs(iv(a,"dungeon_x"))+Math.abs(iv(a,"dungeon_z"));int db=Math.abs(iv(b,"dungeon_x"))+Math.abs(iv(b,"dungeon_z"));if(da!=db)return da-db;
            int ya=iv(a,"dungeon_y"),yb=iv(b,"dungeon_y");return yb-ya;
        }};
        writeList(new File(out,"spawn_inside_dungeon.csv"),rows,new Keep(){public boolean yes(Map<String,String>r){return iv(r,"spawn_inside_dungeon")==1;}},insideCmp);
        writeList(new File(out,"spawn_on_spawner.csv"),rows,new Keep(){public boolean yes(Map<String,String>r){return iv(r,"spawn_on_spawner")==1;}},insideCmp);
        writeList(new File(out,"dungeon_generated_at_origin.csv"),rows,new Keep(){public boolean yes(Map<String,String>r){return iv(r,"expected_spawner_generated")==1 && iv(r,"origin_inside_room")==1;}},generatedCmp);

        int inside=0,on=0,generated=0,failed=0,moved=0,errors=0;
        for(Map<String,String>r:rows){if(iv(r,"spawn_inside_dungeon")==1)inside++;if(iv(r,"spawn_on_spawner")==1)on++;if(iv(r,"expected_spawner_generated")==1)generated++;String s=r.get("status");if("DUNGEON_FAILED".equals(s))failed++;else if("SPAWN_MOVED".equals(s))moved++;else if("ERROR".equals(s))errors++;}
        PrintWriter pw=new PrintWriter(new BufferedWriter(new FileWriter(new File(out,"SUMMARY.txt"),false)));
        pw.println("BETA 1.7.3 DUNGEON SPAWN P1 EXACT SUMMARY");pw.println("Verified candidate rows: "+rows.size());pw.println("Expected first dungeons generated: "+generated);pw.println("SPAWN INSIDE DUNGEON: "+inside);pw.println("Spawn on center spawner: "+on);pw.println("Dungeon generation failed: "+failed);pw.println("Spawn moved from 0,0: "+moved);pw.println("Errors: "+errors);pw.println();pw.println("P1 searches the first dungeon attempt of population chunk (-1,-1) in the lake-free RNG subset (65.625% population coverage before other gates).");pw.close();
    }

    public static void main(String[] args)throws Exception{
        File input=null,out=null;int progress=100;
        for(int i=0;i<args.length;i++){if("--input".equals(args[i]))input=new File(args[++i]);else if("--output".equals(args[i]))out=new File(args[++i]);else if("--progress-every".equals(args[i]))progress=Integer.parseInt(args[++i]);else throw new IllegalArgumentException("unknown arg "+args[i]);}
        if(input==null||out==null)throw new IllegalArgumentException("--input and --output required");out.mkdirs();
        File master=new File(out,"verified_all.csv");Set<Long> done=processed(master);List<Candidate> candidates=readCandidates(input);
        boolean fresh=!master.isFile()||master.length()==0L;PrintWriter append=new PrintWriter(new BufferedWriter(new FileWriter(master,true)));if(fresh){append.println(HEADER);append.flush();}
        long start=System.nanoTime();int n=0;
        for(Candidate c:candidates){if(done.contains(Long.valueOf(c.sequenceIndex)))continue;Result r=verify(c);append.println(r.csv());append.flush();done.add(Long.valueOf(c.sequenceIndex));n++;
            if(r.spawnInside)System.out.println("DUNGEON SPAWN HIT seed="+c.seed+" dungeon=("+c.dx+","+c.dy+","+c.dz+") feet="+r.actualFeet+" chests="+r.chestCount+" onSpawner="+bit(r.onSpawner));
            if(n%progress==0){double sec=(System.nanoTime()-start)/1e9;System.out.println("verify progress new="+n+" rate="+String.format(java.util.Locale.ROOT,"%.2f",n/Math.max(.001,sec))+" candidates/s");}
        }
        append.close();rebuildLists(out,master);double sec=(System.nanoTime()-start)/1e9;System.out.println("VERIFY DONE new="+n+" elapsed="+String.format(java.util.Locale.ROOT,"%.2f",sec)+"s");System.out.println("Summary: "+new File(out,"SUMMARY.txt").getAbsolutePath());
    }
}
