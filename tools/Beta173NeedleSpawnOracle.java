package net.minecraft.src;

import java.io.*;
import java.util.*;

/** Exact Beta 1.7.3 client-startup verifier for the 1x1 sand-needle spawn hunt. */
public final class Beta173NeedleSpawnOracle {
    private static final int AIR = 0;

    private static final class NullChunkLoader implements IChunkLoader {
        public Chunk loadChunk(World world,int x,int z) throws IOException { return null; }
        public void saveChunk(World world,Chunk chunk) throws IOException {}
        public void saveExtraChunkData(World world,Chunk chunk) throws IOException {}
        public void chunkTick() {}
        public void saveExtraData() {}
    }
    private static final class MemorySaveHandler implements ISaveHandler {
        private final IChunkLoader loader=new NullChunkLoader();
        public WorldInfo loadWorldInfo(){return null;}
        public void checkSessionLock(){}
        public IChunkLoader getChunkLoader(WorldProvider provider){return loader;}
        public void saveWorldInfoAndPlayer(WorldInfo info,List players){}
        public void saveWorldInfo(WorldInfo info){}
        public File getMapFile(String name){return null;}
    }
    private static final class Candidate {
        long seed,seq,scoutScore;
        int scoutTop,scoutMinDrop;
    }
    private static final class Result {
        Candidate c; String status="OK"; boolean hit;
        int preSurface=-1,finalTop=-1,playerFeet=-1;
        int north=-1,south=-1,east=-1,west=-1,minCard=-1,minEight=-1;
        int pillarDepth=0,r2=-1,r4=-1,clearR2=0,clearR4=0;
        long score=Long.MIN_VALUE;
    }

    private static List<String> splitCsv(String line){
        ArrayList<String> out=new ArrayList<String>();StringBuilder cur=new StringBuilder();boolean q=false;
        for(int i=0;i<line.length();++i){char ch=line.charAt(i);if(q){if(ch=='\"'){if(i+1<line.length()&&line.charAt(i+1)=='\"'){cur.append('\"');++i;}else q=false;}else cur.append(ch);}else{if(ch=='\"')q=true;else if(ch==','){out.add(cur.toString());cur.setLength(0);}else cur.append(ch);}}out.add(cur.toString());return out;
    }
    private static List<Candidate> read(File f)throws Exception{
        ArrayList<Candidate> out=new ArrayList<Candidate>();BufferedReader br=new BufferedReader(new FileReader(f));String hline=br.readLine();if(hline==null)return out;List<String>h=splitCsv(hline);
        int si=h.indexOf("seed"),qi=h.indexOf("sequence_index"),sci=h.indexOf("score"),ti=h.indexOf("top_y"),mi=h.indexOf("min_positive_drop");String line;
        while((line=br.readLine())!=null){if(line.trim().isEmpty())continue;List<String>x=splitCsv(line);try{Candidate c=new Candidate();c.seed=Long.parseLong(x.get(si));c.seq=Long.parseLong(x.get(qi));c.scoutScore=Long.parseLong(x.get(sci));c.scoutTop=Integer.parseInt(x.get(ti));c.scoutMinDrop=Integer.parseInt(x.get(mi));out.add(c);}catch(RuntimeException ignored){}}
        br.close();return out;
    }
    private static World fresh(long seed){return new World(new MemorySaveHandler(),"needle-spawn-oracle",seed,(WorldProvider)null);}
    private static void buildTerrainExactlyLikeClient(World world,int sx,int sz){
        IChunkProvider p=world.getIChunkProvider();if(p instanceof ChunkProviderLoadOrGenerate)((ChunkProviderLoadOrGenerate)p).setCurrentChunkOver(sx>>4,sz>>4);
        for(int dx=-128;dx<=128;dx+=16)for(int dz=-128;dz<=128;dz+=16)world.getBlockId(sx+dx,64,sz+dz);world.func_656_j();
    }
    private static int firstUncoveredY(World w,int x,int z){int y=63;while(y+1<128&&!w.isAirBlock(x,y+1,z))++y;return y;}
    private static int highestNonAir(World w,int x,int z,int maxY){for(int y=Math.min(127,maxY);y>=0;--y)if(w.getBlockId(x,y,z)!=AIR)return y;return -1;}
    private static boolean hasCollision(World w,int x,int y,int z){int id=w.getBlockId(x,y,z);if(id==AIR)return false;Block b=Block.blocksList[id];return b!=null&&b.getCollisionBoundingBoxFromPool(w,x,y,z)!=null;}
    private static boolean collidesAtFeet(World w,int feet){AxisAlignedBB player=AxisAlignedBB.getBoundingBox(0.2D,(double)feet,0.2D,0.8D,(double)feet+1.8D,0.8D);for(int y=Math.max(0,feet-1);y<=Math.min(127,feet+2);++y){int id=w.getBlockId(0,y,0);if(id==AIR)continue;Block b=Block.blocksList[id];if(b==null)continue;AxisAlignedBB box=b.getCollisionBoundingBoxFromPool(w,0,y,0);if(box!=null&&box.intersectsWith(player))return true;}return false;}
    private static int actualFeet(World w){int y=65;while(y<128&&collidesAtFeet(w,y))++y;return y;}
    private static int dropToNonAir(World w,int x,int z,int topY){int n=highestNonAir(w,x,z,topY-1);return n<0?topY+1:topY-n;}
    private static int minEightDrop(World w,int topY,int r){int best=999;int[][]p={{r,0},{-r,0},{0,r},{0,-r},{r,r},{r,-r},{-r,r},{-r,-r}};for(int[]q:p){int d=dropToNonAir(w,q[0],q[1],topY);if(d<best)best=d;}return best;}
    private static int clearAtTop(World w,int topY,int r){int n=0;for(int z=-r;z<=r;++z)for(int x=-r;x<=r;++x){if(x==0&&z==0)continue;if(w.getBlockId(x,topY,z)==AIR)++n;}return n;}
    private static boolean allEightAir(World w,int y){for(int z=-1;z<=1;++z)for(int x=-1;x<=1;++x){if(x==0&&z==0)continue;if(w.getBlockId(x,y,z)!=AIR)return false;}return true;}
    private static int pillarDepth(World w,int topY){int d=0;for(int y=topY;y>=0;--y){if(!hasCollision(w,0,y,0)||!allEightAir(w,y))break;++d;}return d;}

    private static Result verify(Candidate c,int minDrop,int minTop){
        Result r=new Result();r.c=c;
        try{
            World w=fresh(c.seed);ChunkCoordinates sp=w.getSpawnPoint();if(sp.x!=0||sp.z!=0){r.status="SPAWN_MOVED";return r;}
            r.preSurface=firstUncoveredY(w,0,0);if(r.preSurface<minTop||w.getBlockId(0,r.preSurface,0)!=Block.sand.blockID){r.status="PRE_TOP_NOT_HIGH_SAND";return r;}
            buildTerrainExactlyLikeClient(w,0,0);
            r.finalTop=highestNonAir(w,0,0,127);
            if(r.finalTop<minTop){r.status="FINAL_TOP_TOO_LOW";return r;}
            if(w.getBlockId(0,r.finalTop,0)!=Block.sand.blockID){r.status="FINAL_TOP_NOT_SAND";return r;}
            if(!allEightAir(w,r.finalTop)){r.status="TOP_NOT_1X1";return r;}
            r.north=dropToNonAir(w,0,-1,r.finalTop);r.south=dropToNonAir(w,0,1,r.finalTop);r.east=dropToNonAir(w,1,0,r.finalTop);r.west=dropToNonAir(w,-1,0,r.finalTop);
            r.minCard=Math.min(Math.min(r.north,r.south),Math.min(r.east,r.west));
            r.minEight=minEightDrop(w,r.finalTop,1);r.r2=minEightDrop(w,r.finalTop,2);r.r4=minEightDrop(w,r.finalTop,4);
            r.pillarDepth=pillarDepth(w,r.finalTop);r.clearR2=clearAtTop(w,r.finalTop,2);r.clearR4=clearAtTop(w,r.finalTop,4);r.playerFeet=actualFeet(w);
            r.hit=r.minCard>=minDrop;
            r.status=r.hit?"NEEDLE_HIT":"CARDINAL_DROP_TOO_SMALL";
            r.score=(long)r.minCard*1000000000000L+(long)r.finalTop*1000000000L+(long)Math.min(999,r.pillarDepth)*1000000L+(long)Math.max(0,Math.min(999,r.minEight))*1000L+(long)Math.max(0,Math.min(999,r.r2));
        }catch(Throwable t){r.status="ERROR:"+t.getClass().getSimpleName()+":"+String.valueOf(t.getMessage());}
        return r;
    }
    private static void writeCsv(File f,List<Result> rows)throws Exception{
        PrintWriter out=new PrintWriter(new BufferedWriter(new FileWriter(f,false)));
        out.println("rank,seed,sequence_index,status,hit,exact_score,pre_surface_y,final_top_y,player_feet_y,north_drop,south_drop,east_drop,west_drop,min_cardinal_drop,min_eight_drop,pillar_depth,r2_min_drop,r4_min_drop,clear_r2,clear_r4,scout_top_y,scout_min_positive_drop");
        for(int i=0;i<rows.size();++i){Result r=rows.get(i);out.println((i+1)+","+r.c.seed+","+r.c.seq+","+r.status+","+(r.hit?1:0)+","+r.score+","+r.preSurface+","+r.finalTop+","+r.playerFeet+","+r.north+","+r.south+","+r.east+","+r.west+","+r.minCard+","+r.minEight+","+r.pillarDepth+","+r.r2+","+r.r4+","+r.clearR2+","+r.clearR4+","+r.c.scoutTop+","+r.c.scoutMinDrop);}
        out.close();
    }
    public static void main(String[]args)throws Exception{
        File input=null,output=null;int minDrop=10,minTop=70;
        for(int i=0;i<args.length;++i){if("--input".equals(args[i]))input=new File(args[++i]);else if("--output".equals(args[i]))output=new File(args[++i]);else if("--min-drop".equals(args[i]))minDrop=Integer.parseInt(args[++i]);else if("--min-top-y".equals(args[i]))minTop=Integer.parseInt(args[++i]);else throw new IllegalArgumentException("unknown arg "+args[i]);}
        if(input==null||output==null)throw new IllegalArgumentException("--input and --output required");File parent=output.getParentFile();if(parent!=null)parent.mkdirs();
        List<Candidate> cs=read(input);ArrayList<Result> rows=new ArrayList<Result>();int hits=0,done=0;long started=System.nanoTime();
        for(Candidate c:cs){Result r=verify(c,minDrop,minTop);rows.add(r);if(r.hit){++hits;System.out.println("EXACT NEEDLE HIT seed="+c.seed+" topY="+r.finalTop+" minCardinalDrop="+r.minCard+" N/S/E/W="+r.north+"/"+r.south+"/"+r.east+"/"+r.west+" depth="+r.pillarDepth);}++done;if(done%25==0||done==cs.size()){double sec=(System.nanoTime()-started)/1e9;System.out.println("needle-oracle "+done+"/"+cs.size()+" rate="+String.format(Locale.ROOT,"%.2f",done/Math.max(sec,0.001))+"/s hits="+hits);}}
        Collections.sort(rows,new Comparator<Result>(){public int compare(Result a,Result b){if(a.hit!=b.hit)return a.hit?-1:1;if(a.score!=b.score)return a.score>b.score?-1:1;return a.c.seq<b.c.seq?-1:(a.c.seq==b.c.seq?0:1);}});
        writeCsv(output,rows);System.out.println("NEEDLE ORACLE DONE rows="+done+" hits="+hits+" OUTPUT="+output.getAbsolutePath());
        if(!rows.isEmpty()&&rows.get(0).hit){Result b=rows.get(0);System.out.println("BEST EXACT NEEDLE seed="+b.c.seed+" topY="+b.finalTop+" minCardinalDrop="+b.minCard+" depth="+b.pillarDepth+" r2="+b.r2+" r4="+b.r4);}
    }
}
