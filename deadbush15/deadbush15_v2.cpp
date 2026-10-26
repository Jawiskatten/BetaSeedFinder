// Deadbush15 v2: broader chunk search, per-seed geographic sampling,
// and crash-safe append-only resumable progress. NOT terrain/biome verified.
// Reuses the calibrated Java 26.3 decoration RNG from v1.
#define main deadbush15_legacy_main
#include "deadbush15.cpp"
#undef main
#include <filesystem>
#include <map>
#include <stdexcept>

// A bitset covering the union of all chunks in the selected rectangular region.
// All chunk decoration streams are generated exactly once per candidate seed.
struct RegionGrid {
    int x0,z0,width,height,xWords,zWords;
    std::vector<u64> rows,cols;
    RegionGrid(int wChunks,int hChunks,int radius)
        : x0(-radius*16-7),z0(-radius*16-7),
          width((wChunks+2*radius)*16+14),height((hChunks+2*radius)*16+14),
          xWords((width+63)/64),zWords((height+63)/64),
          rows(size_t(height)*xWords,0),cols(size_t(width)*zWords,0) {}
    void put(int x,int z) {
        x-=x0;z-=z0;
        if(x<0||x>=width||z<0||z>=height)return;
        rows[size_t(z)*xWords+x/64]|=u64(1)<<(x%64);
        cols[size_t(x)*zWords+z/64]|=u64(1)<<(z%64);
    }
    bool get(int x,int z) const {
        return (rows[size_t(z)*xWords+x/64]>>(x%64))&1;
    }
    Hit best(bool diag) const {
        Hit h;
        // Score bit runs, carrying sequences across 64-bit word boundaries.
        for(int axis=0;axis<2;axis++) {
            int lines=axis==0?height:width;
            int words=axis==0?xWords:zWords;
            int length=axis==0?width:height;
            const auto &data=axis==0?rows:cols;
            for(int l=0;l<lines;l++) {
                int carry=0;
                for(int w=0;w<words;w++) {
                    u64 mask=data[size_t(l)*words+w];
                    int remaining=std::min(64,length-64*w);
                    if(remaining<64)mask&=(u64(1)<<remaining)-1;
                    int offset=0;
                    while(mask) {
                        int zeros=__builtin_ctzll(mask);
                        if(zeros)carry=0;
                        offset+=zeros;
                        mask>>=zeros;
                        int ones=(mask==~u64(0))?64:__builtin_ctzll(~mask);
                        int run=carry+ones;
                        if(run>h.run) {
                            int start=64*w+offset-carry;
                            h={run,axis==0?start+x0:l+x0,
                                axis==0?l+z0:start+z0,axis==0?1:0,axis==0?0:1};
                        }
                        // Carry applies only when a segment touches the next word.
                        carry=(offset+ones==64)?run:0;
                        if(ones>=64||offset+ones>=64)break;
                        mask>>=ones;
                        offset+=ones;
                    }
                    if(!data[size_t(l)*words+w])carry=0;
                    if(remaining<64)carry=0;
                }
            }
        }
        if(diag) for(int sign:{-1,1}) for(int z=0;z<height;z++) for(int x=0;x<width;x++) {
            if(!get(x,z))continue;
            // A run's predecessor should be empty (only inspect starts).
            if(z>0 && x-sign>=0 && x-sign<width && get(x-sign,z-1))continue;
            int len=1;
            while(z+len<height && x+sign*len>=0 && x+sign*len<width
                  && get(x+sign*len,z+len))++len;
            if(len>h.run)h={len,x+x0,z+z0,sign,1};
        }
        return h;
    }
};
static void generateRegionChunk(RegionGrid &g,u64 world,int absCx,int absCz,int relativeCx,int relativeCz,int index,bool flat) {
    Xoro r(feature_seed(world,absCx,absCz,index));
    for(int n=0;n<20;n++) {
        int baseX=relativeCx*16+r.pow2(4),baseZ=relativeCz*16+r.pow2(4);
        for(int t=0;t<4;t++) {
            int x=baseX+r.pow2(3)-r.pow2(3);
            int dy=r.pow2(2)-r.pow2(2);
            int z=baseZ+r.pow2(3)-r.pow2(3);
            if(!flat||dy==0)g.put(x,z);
        }
    }
}
static Hit evaluateRegion(u64 seed,int index,int radius,bool flat,bool diag,
                          int baseX,int baseZ,int width,int height) {
    if(width==1&&height==1) {
        if(baseX==0&&baseZ==0)return evaluate(seed,index,radius,flat,diag,false);
        Grid g;
        for(int x=-radius;x<=radius;x++)for(int z=-radius;z<=radius;z++)
            place_chunk(g,feature_seed(seed,baseX+x,baseZ+z,index),x,z,flat);
        return g.best(diag);
    }
    RegionGrid g(width,height,radius);
    for(int x=-radius;x<width+radius;x++)for(int z=-radius;z<height+radius;z++)
        generateRegionChunk(g,seed,baseX+x,baseZ+z,x,z,index,flat);
    return g.best(diag);
}
// FNV1a fingerprint protects resumes from accidentally changing search options.
static u64 hashString(const std::string &s) {
    u64 h=14695981039346656037ULL;
    for(unsigned char c:s){h^=c;h*=1099511628211ULL;}
    return h;
}
static std::string fingerprint(u64 start,u64 count,int index,int radius,int width,int height,
                               int cx,int cz,int spread,int report,int target,int threads,
                               bool flat,bool diag,const std::string &output) {
    std::ostringstream s;
    s<<"D15V2|"<<start<<'|'<<count<<'|'<<index<<'|'<<radius<<'|'<<width<<'|'<<height
     <<'|'<<cx<<'|'<<cz<<'|'<<spread<<'|'<<report<<'|'<<target<<'|'<<flat<<'|'<<diag<<'|'<<output;
    (void)threads; // Thread count can change across resume.
    return std::to_string(hashString(s.str()));
}
static void appendCheckpoint(const std::string &path,const std::string &signature,u64 offset) {
    if(path.empty())return;
    std::ofstream f(path,std::ios::app);
    if(!f)throw std::runtime_error("cannot write checkpoint: "+path);
    const std::string payload="D15V2 "+signature+" "+std::to_string(offset);
    f<<payload<<' '<<hashString(payload)<<'\n';
    f.flush();
    if(!f)throw std::runtime_error("checkpoint write failed: "+path);
}
static u64 readCheckpoint(const std::string &path,const std::string &signature,u64 count) {
    std::ifstream f(path);
    if(!f)return 0;
    std::string line;
    u64 last=0;
    bool any=false;
    while(std::getline(f,line)) {
        std::istringstream in(line);
        std::string version,sig,offset,check,extra;
        if(!(in>>version>>sig>>offset>>check)||(in>>extra))continue; // partial crash line
        const std::string payload=version+" "+sig+" "+offset;
        if(version!="D15V2"||std::to_string(hashString(payload))!=check)continue;
        if(sig!=signature)throw std::runtime_error("checkpoint belongs to different options/output; choose another checkpoint file");
        try {
            u64 n=parseU(offset);
            if(n<=count){last=n;any=true;}
        }catch(...){}
    }
    if(!any)throw std::runtime_error("no valid progress in existing checkpoint; choose a new path");
    return last;
}
static void usageV2() {
    std::cout<<"Deadbush15 v2 - Minecraft Java 26.3 decoration RNG scout (NOT terrain verified)\n"
      <<"  --self-test                  reference and region-equivalence checks\n"
      <<"  --search --start 0 --count 1000000 --threads 8 --radius 1\n"
      <<"  --chunk-x 0 --chunk-z 0      first region chunk (default world origin)\n"
      <<"  --region-width 4 --region-height 4    scan a rectangular union of chunks\n"
      <<"  --spread-span 1000000       choose a deterministic random center per seed\n"
      <<"  --checkpoint progress.log --resume   checkpoint each 5 sec; resume on restart\n"
      <<"  --output region_candidates.csv --report 10 --target 15 --mode flat\n"
      <<"Rows may span the whole rectangular region and its radius border.\n"
      <<"All candidates require vanilla terrain/biome verification.\n";
}
int main(int argc,char **argv) {
    u64 start=0,count=1000000;
    int index=75,radius=1,threads=std::max(1u,std::thread::hardware_concurrency());
    int cx=0,cz=0,width=1,height=1,spread=0,report=10,target=15;
    std::string output="region_candidates.csv",checkpoint="",mode="flat";
    bool resume=false,diag=false,selfTest=false;
    try {
        for(int i=1;i<argc;i++) {
            std::string a=argv[i];
            auto val=[&]() -> std::string {if(++i>=argc)throw std::runtime_error("missing value: "+a);return argv[i];};
            if(a=="--help"||a=="-h"){usageV2();return 0;}
            else if(a=="--self-test")selfTest=true;
            else if(a=="--search"){}
            else if(a=="--start")start=parseU(val());
            else if(a=="--count")count=parseU(val());
            else if(a=="--index")index=std::stoi(val());
            else if(a=="--radius")radius=std::stoi(val());
            else if(a=="--threads")threads=std::stoi(val());
            else if(a=="--chunk-x")cx=std::stoi(val());
            else if(a=="--chunk-z")cz=std::stoi(val());
            else if(a=="--region-width")width=std::stoi(val());
            else if(a=="--region-height")height=std::stoi(val());
            else if(a=="--spread-span")spread=std::stoi(val());
            else if(a=="--report")report=std::stoi(val());
            else if(a=="--target")target=std::stoi(val());
            else if(a=="--output")output=val();
            else if(a=="--checkpoint")checkpoint=val();
            else if(a=="--resume")resume=true;
            else if(a=="--diagonal")diag=true;
            else if(a=="--mode")mode=val();
            else throw std::runtime_error("unknown option: "+a);
        }
        if(radius<0||radius>1||threads<1||threads>256||width<1||width>32||height<1||height>32
           ||index<0||spread<0||spread>1800000||std::abs(int64_t(cx))+int64_t(spread)+width+radius>1870000
           ||std::abs(int64_t(cz))+int64_t(spread)+height+radius>1870000
           ||report<1||target<1||(mode!="flat"&&mode!="xz")||count>std::numeric_limits<u64>::max()-start)
            throw std::runtime_error("invalid options: radius 0..1, region dimensions 1..32, chunk positions within world border");
        if(resume&&checkpoint.empty())throw std::runtime_error("--resume requires --checkpoint FILE");
        const bool flat=mode=="flat";
        if(selfTest) {
            char selfArg[]="--self-test"; char *selfArgs[]={argv[0],selfArg};
            const int rc=deadbush15_legacy_main(2,selfArgs);
            if(rc)return rc;
            bool okay=true;
            const u64 seeds[]={0ULL,123456789ULL,static_cast<u64>(-6825794747614541757LL)};
            for(u64 s:seeds) for(int loc:{0,17,-81}) for(int r:{0,1}) {
                Hit original;
                Grid g;
                for(int x=-r;x<=r;x++)for(int z=-r;z<=r;z++)
                    place_chunk(g,feature_seed(s,loc+x,loc+z,index),x,z,true);
                original=g.best(false);
                Hit revised=evaluateRegion(s,index,r,true,false,loc,loc,1,1);
                if(original.run!=revised.run||original.x!=revised.x||original.z!=revised.z)okay=false;
                RegionGrid region(1,1,r);
                for(int x=-r;x<=r;x++)for(int z=-r;z<=r;z++)
                    generateRegionChunk(region,s,loc+x,loc+z,x,z,index,true);
                Hit unionHit=region.best(false);
                if(unionHit.run!=original.run||unionHit.x!=original.x||unionHit.z!=original.z)okay=false;
            }
            // Explicit brute-force oracle for multi-chunk region and word-boundary runs.
            for(u64 s:{0ULL,1ULL,923652124993224547ULL}) {
                RegionGrid g(4,3,1);
                for(int x=-1;x<5;x++)for(int z=-1;z<4;z++)
                    generateRegionChunk(g,s,156+x,-209+z,x,z,75,true);
                Hit fastHit=g.best(false),brute;
                for(int z=0;z<g.height;z++)for(int x=0;x<g.width;x++)if(g.get(x,z)) {
                    if(x==0||!g.get(x-1,z)) {
                        int n=1;while(x+n<g.width&&g.get(x+n,z))++n;
                        if(n>brute.run)brute={n,x+g.x0,z+g.z0,1,0};
                    }
                    if(z==0||!g.get(x,z-1)) {
                        int n=1;while(z+n<g.height&&g.get(x,z+n))++n;
                        if(n>brute.run)brute={n,x+g.x0,z+g.z0,0,1};
                    }
                }
                if(fastHit.run!=brute.run)okay=false;
            }
            std::cout<<(okay?"PASS":"FAIL")<<" region bitset, word boundaries and arbitrary chunk equivalence\n";
            return okay?0:1;
        }
        const std::string signature=fingerprint(start,count,index,radius,width,height,cx,cz,spread,report,target,threads,flat,diag,output);
        if(!checkpoint.empty()&&std::filesystem::exists(checkpoint)&&!resume)
            throw std::runtime_error("checkpoint already exists: add --resume to continue or choose a new file");
        u64 offset=(!checkpoint.empty()&&resume&&std::filesystem::exists(checkpoint))
                   ?readCheckpoint(checkpoint,signature,count):0;
        if(!checkpoint.empty()&&offset==0)appendCheckpoint(checkpoint,signature,0);
        const std::string header="seed_unsigned,seed_signed,run,start_x,start_z,dx,dz,mode,chunk_radius,global_index,region_chunk_x,region_chunk_z,region_width,region_height,spread_span,terrain_verified";
        if(std::filesystem::exists(output)&&std::filesystem::file_size(output)) {
            std::ifstream f(output);std::string first;std::getline(f,first);
            if(first!=header)throw std::runtime_error("existing CSV has different columns; use a new --output filename");
        }
        std::ofstream csv(output,std::ios::app);
        if(!csv)throw std::runtime_error("cannot open output "+output);
        if(csv.tellp()==0)csv<<header<<'\n';
        std::mutex csvMutex,frontierMutex;
        std::map<u64,u64> finished;
        std::atomic<u64> next(offset),done(offset),contiguous(offset);
        std::atomic<int> best(0);
        const u64 block=512;
        auto t0=std::chrono::steady_clock::now();
        std::cout<<"Deadbush15 v2: "<<count<<" world seeds from "<<start<<", resume="<<offset
                 <<", "<<width<<"x"<<height<<" chunk centers, radius="<<radius<<", spread="<<spread
                 <<", threads="<<threads<<", mode="<<mode<<"\n"<<std::flush;
        auto worker=[&]() {
            while(true) {
                u64 off=next.fetch_add(block,std::memory_order_relaxed);
                if(off>=count)break;
                u64 end=off+std::min(block,count-off);
                for(u64 j=off;j<end;j++) {
                    u64 s=start+j;
                    int x=cx,z=cz;
                    if(spread) {
                        const u64 span=2ULL*spread+1ULL;
                        x+=int(mix13(s^0xa0761d6478bd642fULL)%span)-spread;
                        z+=int(mix13(s^0xe7037ed1a0b428dbULL)%span)-spread;
                    }
                    Hit h=evaluateRegion(s,index,radius,flat,diag,x,z,width,height);
                    int old=best.load(std::memory_order_relaxed);
                    while(h.run>old&&!best.compare_exchange_weak(old,h.run)){}
                    if(h.run>=report) {
                        long long absX=x*16LL+h.x,absZ=z*16LL+h.z;
                        std::lock_guard<std::mutex> lock(csvMutex);
                        csv<<s<<','<<static_cast<int64_t>(s)<<','<<h.run<<','<<absX<<','<<absZ<<','<<h.dx<<','<<h.dz
                           <<','<<mode<<','<<radius<<','<<index<<','<<x<<','<<z<<','<<width<<','<<height<<','<<spread<<",0\n";
                        csv.flush();
                        if(h.run>=target)std::cout<<"CANDIDATE ONLY seed="<<static_cast<int64_t>(s)
                            <<" run="<<h.run<<" at "<<absX<<','<<absZ<<" (NOT VERIFIED)\n";
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(frontierMutex);
                    if(off==contiguous.load(std::memory_order_relaxed)) {
                        u64 prefix=end;
                        for(auto it=finished.find(prefix);it!=finished.end();it=finished.find(prefix)) {
                            prefix=it->second;finished.erase(it);
                        }
                        contiguous.store(prefix,std::memory_order_relaxed);
                    } else finished.emplace(off,end);
                }
                done.fetch_add(end-off,std::memory_order_relaxed);
            }
        };
        std::vector<std::thread> pool;
        for(int i=0;i<threads;i++)pool.emplace_back(worker);
        while(done.load(std::memory_order_relaxed)<count) {
            for(int tick=0;tick<50&&done.load(std::memory_order_relaxed)<count;tick++)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(done.load(std::memory_order_relaxed)>=count)break;
            u64 n=done.load(std::memory_order_relaxed);
            u64 prefix=contiguous.load(std::memory_order_relaxed);
            if(!checkpoint.empty())appendCheckpoint(checkpoint,signature,prefix);
            double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
            std::cout<<"Progress "<<n<<'/'<<count<<" ("<<std::fixed<<std::setprecision(1)
                     <<(count?100.*double(n)/double(count):100.)<<"%), committed="<<prefix
                     <<", best this session="<<best.load()<<", "<<std::setprecision(0)
                     <<((n-offset)/secs)<<" seeds/s\n"<<std::flush;
        }
        for(auto &t:pool)t.join();
        u64 prefix=contiguous.load(std::memory_order_relaxed);
        if(!checkpoint.empty())appendCheckpoint(checkpoint,signature,prefix);
        double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
        std::cout<<"Done: "<<(done-offset)<<" seeds this session in "<<std::fixed<<std::setprecision(2)
                 <<secs<<" s, "<<std::setprecision(0)<<((done-offset)/secs)<<" seeds/s; best="<<best.load()
                 <<" (RNG pattern only); saved to "<<output<<"\n";
        return 0;
    } catch(const std::exception &e) {std::cerr<<"ERROR: "<<e.what()<<'\n';return 2;}
}