package net.minecraft.src;

import java.io.*;
import java.util.*;

/**
 * Exact Beta 1.7.3 geometry probe for a one-step liquid trigger of dormant
 * spawn sand. For each proven dry-cave candidate, inspect the four blocks
 * horizontally adjacent to the air block directly below the bottom spawn-sand
 * block. A WorldGenLiquids spring at one of those four locations is a perfect
 * one-step trigger iff its exact vanilla placement predicate is already true:
 * stone above + below, center air/stone, and exactly 3 horizontal stone + 1 air.
 * The unique horizontal air neighbor must be the under-sand target, so the
 * source's immediate BlockFlowing update has only that horizontal exit.
 */
public final class Beta173SandWakeSpringPocketProbe {
    private static final int AIR = 0;

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
        long seed, sequenceIndex;
        int predictedDrop;
    }

    private static List<String> splitCsv(String line) {
        ArrayList<String> out=new ArrayList<String>(); StringBuilder cur=new StringBuilder(); boolean q=false;
        for(int i=0;i<line.length();++i){char ch=line.charAt(i); if(q){if(ch=='\"'){if(i+1<line.length()&&line.charAt(i+1)=='\"'){cur.append('\"');++i;}else q=false;}else cur.append(ch);}else{if(ch=='\"')q=true;else if(ch==','){out.add(cur.toString());cur.setLength(0);}else cur.append(ch);}}
        out.add(cur.toString()); return out;
    }
    private static List<Candidate> readCandidates(File dir) throws Exception {
        ArrayList<Candidate> out=new ArrayList<Candidate>();
        File[] fs=dir.listFiles(new FilenameFilter(){public boolean accept(File d,String n){return n.startsWith("candidates_")&&n.endsWith(".csv");}});
        if(fs==null)return out; Arrays.sort(fs,new Comparator<File>(){public int compare(File a,File b){return a.getName().compareTo(b.getName());}});
        for(File f:fs){BufferedReader br=new BufferedReader(new FileReader(f)); String hline=br.readLine(); if(hline==null){br.close();continue;} List<String> h=splitCsv(hline); int si=h.indexOf("seed"), qi=h.indexOf("sequence_index"), pi=h.indexOf("potential_drop"); String line; while((line=br.readLine())!=null){if(line.trim().isEmpty())continue; List<String>x=splitCsv(line); try{Candidate c=new Candidate();c.seed=Long.parseLong(x.get(si));c.sequenceIndex=Long.parseLong(x.get(qi));c.predictedDrop=pi>=0?Integer.parseInt(x.get(pi)):-1;out.add(c);}catch(RuntimeException ignored){}} br.close();}
        Collections.sort(out,new Comparator<Candidate>(){public int compare(Candidate a,Candidate b){return a.sequenceIndex<b.sequenceIndex?-1:(a.sequenceIndex==b.sequenceIndex?0:1);}}); return out;
    }

    private static World fresh(long seed){return new World(new MemorySaveHandler(),"sand-wake-spring-pocket",seed,(WorldProvider)null);}
    private static int firstUncoveredY(World w){int y=63;while(y+1<128&&!w.isAirBlock(0,y+1,0))++y;return y;}
    private static int bottomSand(World w,int gate){int y=gate;while(y>0&&w.getBlockId(0,y-1,0)==Block.sand.blockID)--y;return y;}

    private static boolean springEligible(World w,int x,int y,int z) {
        if(w.getBlockId(x,y+1,z)!=Block.stone.blockID) return false;
        if(w.getBlockId(x,y-1,z)!=Block.stone.blockID) return false;
        int center=w.getBlockId(x,y,z);
        if(center!=AIR && center!=Block.stone.blockID) return false;
        int stone=0,air=0;
        int[][] d={{-1,0},{1,0},{0,-1},{0,1}};
        for(int[] q:d){int id=w.getBlockId(x+q[0],y,z+q[1]);if(id==Block.stone.blockID)++stone;if(id==AIR)++air;}
        return stone==3 && air==1;
    }

    private static boolean uniqueAirIsTarget(World w,int sx,int y,int sz,int tx,int tz){
        int[][]d={{-1,0},{1,0},{0,-1},{0,1}}; int ax=999,az=999,n=0;
        for(int[]q:d) if(w.isAirBlock(sx+q[0],y,sz+q[1])){ax=sx+q[0];az=sz+q[1];++n;}
        return n==1&&ax==tx&&az==tz;
    }

    public static void main(String[] args) throws Exception {
        File in=null,outFile=null;
        for(int i=0;i<args.length;++i){if("--input-dir".equals(args[i]))in=new File(args[++i]);else if("--output".equals(args[i]))outFile=new File(args[++i]);else throw new IllegalArgumentException("unknown arg "+args[i]);}
        if(in==null||outFile==null)throw new IllegalArgumentException("--input-dir and --output required");
        File p=outFile.getParentFile();if(p!=null)p.mkdirs(); List<Candidate> cs=readCandidates(in);
        PrintWriter out=new PrintWriter(new BufferedWriter(new FileWriter(outFile,false)));
        out.println("seed,sequence_index,predicted_drop,status,gate_y,bottom_sand_y,target_y,target_is_air,pocket_count,best_source_x,best_source_y,best_source_z,best_source_center_id");
        int done=0,pocketRows=0,totalPockets=0,bestDrop=-1; long bestSeed=0;
        int[][] dirs={{-1,0},{1,0},{0,-1},{0,1}};
        for(Candidate c:cs){String status="OK";int gate=-1,bottom=-1,ty=-1,targetAir=0,count=0,bx=999,bz=999,bc=-1;
            try{World w=fresh(c.seed);ChunkCoordinates sp=w.getSpawnPoint();if(sp.x!=0||sp.z!=0)status="SPAWN_MOVED";else{gate=firstUncoveredY(w);if(gate!=63||w.getBlockId(0,gate,0)!=Block.sand.blockID)status="PRE_GATE_MISMATCH";else{bottom=bottomSand(w,gate);ty=bottom-1;targetAir=w.isAirBlock(0,ty,0)?1:0;if(targetAir==1){for(int[]d:dirs){int sx=d[0],sz=d[1];if(springEligible(w,sx,ty,sz)&&uniqueAirIsTarget(w,sx,ty,sz,0,0)){++count;if(bx==999){bx=sx;bz=sz;bc=w.getBlockId(sx,ty,sz);}}}}}}}catch(Throwable t){status="ERROR:"+t.getClass().getSimpleName();}
            if(count>0){++pocketRows;totalPockets+=count;if(c.predictedDrop>bestDrop){bestDrop=c.predictedDrop;bestSeed=c.seed;}System.out.println("SPRING POCKET seed="+c.seed+" predictedDrop="+c.predictedDrop+" pockets="+count+" source=("+bx+","+ty+","+bz+") target=(0,"+ty+",0)");}
            out.println(c.seed+","+c.sequenceIndex+","+c.predictedDrop+","+status+","+gate+","+bottom+","+ty+","+targetAir+","+count+","+(bx==999?"":Integer.toString(bx))+","+(bx==999?"":Integer.toString(ty))+","+(bx==999?"":Integer.toString(bz))+","+(bx==999?"":Integer.toString(bc)));out.flush();++done;
            if(done%25==0||done==cs.size())System.out.println("spring-pocket "+done+"/"+cs.size()+" pocketRows="+pocketRows+" totalPockets="+totalPockets+" bestPocketDrop="+(bestDrop<0?"NONE":Integer.toString(bestDrop)+" seed="+bestSeed));
        }
        out.close(); System.out.println("SPRING POCKET PROBE DONE rows="+done+" pocketRows="+pocketRows+" totalPockets="+totalPockets+" bestPocketDrop="+(bestDrop<0?"NONE":Integer.toString(bestDrop)+" seed="+bestSeed)); System.out.println("OUTPUT="+outFile.getAbsolutePath());
    }
}
