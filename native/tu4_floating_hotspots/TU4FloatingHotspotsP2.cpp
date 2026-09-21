#define main tu4_components_p1_embedded_main
#include "../tu4_floating_components/TU4FloatingComponents.cpp"
#undef main

#include <cmath>
#include <set>

namespace tu4_floating_hotspots_p2 {
using namespace tu4_floating_components;
using namespace highest_pillar_spawn_p1;

static constexpr int WINDOW_COUNT = 8;
static constexpr int WINDOW_BLOCKS[WINDOW_COUNT] = {64,80,96,112,128,160,192,256};

struct Hotspot {
    int count = 0;
    int size = 0;
    int minX = 0, maxX = -1;
    int minZ = 0, maxZ = -1;
    std::uint64_t blocks = 0;
    int ge8 = 0, ge32 = 0, ge128 = 0;
};

static int hotspotArea(const Hotspot& h) { return h.size * h.size; }
static bool betterHotspot(const Hotspot& a, const Hotspot& b) {
    if (b.size == 0) return true;
    const unsigned long long left = static_cast<unsigned long long>(a.count) * static_cast<unsigned long long>(a.count) * static_cast<unsigned long long>(hotspotArea(b));
    const unsigned long long right = static_cast<unsigned long long>(b.count) * static_cast<unsigned long long>(b.count) * static_cast<unsigned long long>(hotspotArea(a));
    if (left != right) return left > right;
    if (a.count != b.count) return a.count > b.count;
    if (a.size != b.size) return a.size < b.size;
    if (a.blocks != b.blocks) return a.blocks > b.blocks;
    if (a.minX != b.minX) return a.minX < b.minX;
    return a.minZ < b.minZ;
}
static double densityScore(const Hotspot& h) {
    if (h.size <= 0) return 0.0;
    return static_cast<double>(h.count) * static_cast<double>(h.count) / static_cast<double>(hotspotArea(h));
}
static double componentsPer10k(const Hotspot& h) {
    if (h.size <= 0) return 0.0;
    return static_cast<double>(h.count) * 10000.0 / static_cast<double>(hotspotArea(h));
}

struct Config2 {
    std::string mode = "scout";
    std::filesystem::path output;
    std::filesystem::path input;
    std::uint64_t count = 5000;
    std::uint64_t startIndex = 0;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int canvas = 512;
    int batch = 512;
    int terrainThreads = 64;
    int top = 128;
    int verifyTop = 64;
    int verifyBatch = 4;
    int minBlocks = 1;
    bool includeBoundary = false;
};

static std::uint64_t parseU64_2(const std::string& s,const char* what){std::size_t used=0;auto v=std::stoull(s,&used,0);if(used!=s.size())throw std::invalid_argument(std::string("invalid ")+what+": "+s);return v;}
static int parseInt_2(const std::string& s,const char* what){std::size_t used=0;auto v=std::stoi(s,&used,0);if(used!=s.size())throw std::invalid_argument(std::string("invalid ")+what+": "+s);return v;}
static Config2 parseArgs2(int argc,char**argv){
    Config2 c;
    for(int i=1;i<argc;++i){
        const std::string a=argv[i];
        auto val=[&](){if(++i>=argc)throw std::invalid_argument("missing value for "+a);return std::string(argv[i]);};
        if(a=="--mode")c.mode=val();
        else if(a=="--output")c.output=val();
        else if(a=="--input")c.input=val();
        else if(a=="--count")c.count=parseU64_2(val(),"count");
        else if(a=="--start-index")c.startIndex=parseU64_2(val(),"start-index");
        else if(a=="--random-key"){c.randomKey=parseU64_2(val(),"random-key");c.randomKeySet=true;}
        else if(a=="--seed-mode"){const auto m=val();if(m=="unique48")c.seedMode=SeedMode::Unique48;else if(m=="splitmix64")c.seedMode=SeedMode::SplitMix64;else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");}
        else if(a=="--canvas")c.canvas=parseInt_2(val(),"canvas");
        else if(a=="--batch")c.batch=parseInt_2(val(),"batch");
        else if(a=="--terrain-threads")c.terrainThreads=parseInt_2(val(),"terrain-threads");
        else if(a=="--top")c.top=parseInt_2(val(),"top");
        else if(a=="--verify-top")c.verifyTop=parseInt_2(val(),"verify-top");
        else if(a=="--verify-batch")c.verifyBatch=parseInt_2(val(),"verify-batch");
        else if(a=="--min-blocks")c.minBlocks=parseInt_2(val(),"min-blocks");
        else if(a=="--include-boundary")c.includeBoundary=true;
        else throw std::invalid_argument("unknown argument: "+a);
    }
    if(c.mode!="scout"&&c.mode!="verify")throw std::invalid_argument("--mode scout|verify");
    if(c.output.empty())throw std::invalid_argument("--output is required");
    if(c.mode=="verify"&&c.input.empty())throw std::invalid_argument("--input is required for verify");
    if(c.canvas<256||c.canvas>1024||(c.canvas%16)!=0)throw std::invalid_argument("--canvas must be 256..1024 and divisible by 16");
    if(c.canvas<256)throw std::invalid_argument("canvas must fit the 256x256 hotspot window");
    if(c.batch<1||c.batch>4096)throw std::invalid_argument("--batch must be 1..4096");
    if(c.verifyBatch<1||c.verifyBatch>16)throw std::invalid_argument("--verify-batch must be 1..16");
    if(c.terrainThreads!=64&&c.terrainThreads!=128&&c.terrainThreads!=256)throw std::invalid_argument("--terrain-threads must be 64,128,256");
    if(c.top<1||c.top>10000)throw std::invalid_argument("--top must be 1..10000");
    if(c.verifyTop<1||c.verifyTop>c.top)throw std::invalid_argument("--verify-top must be 1..top");
    if(c.minBlocks<1)throw std::invalid_argument("--min-blocks must be >=1");
    if(c.seedMode==SeedMode::Unique48&&c.mode=="scout"&&(c.startIndex>=JAVA_SEED_PERIOD||c.count>JAVA_SEED_PERIOD-c.startIndex))throw std::invalid_argument("unique48 sequence exceeds 2^48");
    if(!c.randomKeySet){std::random_device rd;c.randomKey=(static_cast<std::uint64_t>(rd())<<32)^static_cast<std::uint64_t>(rd())^static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());}
    return c;
}

__global__ void scoreChunkProxyGridKernel(const double* density,int count,std::uint16_t* componentGrid,std::uint16_t* runGrid,int chunkIndex,int chunkCount){
    const int seed=static_cast<int>(blockIdx.x),lane=static_cast<int>(threadIdx.x);
    if(seed>=count||lane>=256)return;
    const std::size_t base=static_cast<std::size_t>(seed)*coarsecore::CELLS;
    const int lx=lane&15,lz=lane>>4;
    int runs=0;bool seenAir=false,inSolid=false,detached=false;
    for(int y=Y_BASE;y<128;++y){
        const bool solid=localSolid(density,base,lx,y,lz);
        if(solid){if(!inSolid){if(seenAir){++runs;detached=true;}inSolid=true;}}
        else{seenAir=true;inSolid=false;}
    }
    __shared__ unsigned char mask[256];
    __shared__ unsigned char visited[256];
    __shared__ unsigned short queue[256];
    __shared__ unsigned int sRuns[256];
    mask[lane]=detached?1:0;visited[lane]=0;sRuns[lane]=static_cast<unsigned int>(runs);__syncthreads();
    for(int stride=128;stride>0;stride>>=1){if(lane<stride)sRuns[lane]+=sRuns[lane+stride];__syncthreads();}
    if(lane==0){
        unsigned int comps=0;
        for(int start=0;start<256;++start){
            if(!mask[start]||visited[start])continue;
            ++comps;int head=0,tail=0;queue[tail++]=static_cast<unsigned short>(start);visited[start]=1;
            while(head<tail){const int v=static_cast<int>(queue[head++]);const int x=v&15,z=v>>4;
                if(x>0){int q=v-1;if(mask[q]&&!visited[q]){visited[q]=1;queue[tail++]=static_cast<unsigned short>(q);}}
                if(x<15){int q=v+1;if(mask[q]&&!visited[q]){visited[q]=1;queue[tail++]=static_cast<unsigned short>(q);}}
                if(z>0){int q=v-16;if(mask[q]&&!visited[q]){visited[q]=1;queue[tail++]=static_cast<unsigned short>(q);}}
                if(z<15){int q=v+16;if(mask[q]&&!visited[q]){visited[q]=1;queue[tail++]=static_cast<unsigned short>(q);}}
            }
        }
        const std::size_t idx=static_cast<std::size_t>(seed)*static_cast<std::size_t>(chunkCount)+static_cast<std::size_t>(chunkIndex);
        componentGrid[idx]=static_cast<std::uint16_t>(std::min<unsigned int>(65535u,comps));
        runGrid[idx]=static_cast<std::uint16_t>(std::min<unsigned int>(65535u,sRuns[0]));
    }
}

static int rectSum(const std::vector<int>& p,int side,int x,int z,int w){
    const int s=side+1,x2=x+w,z2=z+w;
    return p[z2*s+x2]-p[z*s+x2]-p[z2*s+x]+p[z*s+x];
}
static Hotspot bestProxyHotspot(const std::uint16_t* comps,const std::uint16_t* runs,int chunkSide,int chunkMin){
    std::vector<int> pc(static_cast<std::size_t>(chunkSide+1)*static_cast<std::size_t>(chunkSide+1),0);
    std::vector<int> pr(pc.size(),0);
    const int ps=chunkSide+1;
    for(int z=0;z<chunkSide;++z)for(int x=0;x<chunkSide;++x){
        const int src=z*chunkSide+x;
        pc[(z+1)*ps+x+1]=static_cast<int>(comps[src])+pc[z*ps+x+1]+pc[(z+1)*ps+x]-pc[z*ps+x];
        pr[(z+1)*ps+x+1]=static_cast<int>(runs[src])+pr[z*ps+x+1]+pr[(z+1)*ps+x]-pr[z*ps+x];
    }
    Hotspot best;
    for(int wi=0;wi<WINDOW_COUNT;++wi){
        const int bs=WINDOW_BLOCKS[wi],w=bs/16;if(w>chunkSide)continue;
        for(int z=0;z+w<=chunkSide;++z)for(int x=0;x+w<=chunkSide;++x){
            Hotspot h;h.count=rectSum(pc,chunkSide,x,z,w);h.size=bs;h.blocks=static_cast<std::uint64_t>(std::max(0,rectSum(pr,chunkSide,x,z,w)));
            h.minX=(chunkMin+x)*16;h.minZ=(chunkMin+z)*16;h.maxX=h.minX+bs-1;h.maxZ=h.minZ+bs-1;
            if(betterHotspot(h,best))best=h;
        }
    }
    return best;
}

struct Scout2{std::int64_t seed=0;std::uint64_t seq=0;Hotspot hot;};
static bool betterScout2(const Scout2&a,const Scout2&b){if(b.hot.size==0)return true;if(betterHotspot(a.hot,b.hot))return true;if(betterHotspot(b.hot,a.hot))return false;return a.seq<b.seq;}
static void insertScout2(std::vector<Scout2>&v,const Scout2&r,int limit){auto it=std::lower_bound(v.begin(),v.end(),r,[](const Scout2&a,const Scout2&b){return betterScout2(a,b);});v.insert(it,r);if(static_cast<int>(v.size())>limit)v.pop_back();}
static void writeScout2(const std::filesystem::path&p,const std::vector<Scout2>&v){std::ofstream f(p,std::ios::trunc);if(!f)throw std::runtime_error("cannot write "+p.string());f<<"rank,seed,sequence_index,proxy_density_score,proxy_components,window_size,min_x,max_x,min_z,max_z,proxy_runs\n";for(std::size_t i=0;i<v.size();++i){const auto&r=v[i];f<<(i+1)<<','<<r.seed<<','<<r.seq<<','<<std::setprecision(12)<<densityScore(r.hot)<<','<<r.hot.count<<','<<r.hot.size<<','<<r.hot.minX<<','<<r.hot.maxX<<','<<r.hot.minZ<<','<<r.hot.maxZ<<','<<r.hot.blocks<<'\n';}}

static int runScout2(const Config2&c){
    printDevice();const int chunks=c.canvas/16,chunkMin=-chunks/2,chunkCount=chunks*chunks;std::filesystem::create_directories(c.output);
    std::cout<<"TU4 FLOATING HOTSPOT P2 SCOUT | canvas="<<c.canvas<<'x'<<c.canvas<<" windows=64..256 dynamic\n";
    std::cout<<"objective proxy: components^2 / window_area\ncount="<<c.count<<" batch="<<c.batch<<" randomKey="<<c.randomKey<<"\n";
    std::uint64_t completed=0;std::vector<Scout2>top;const auto started=std::chrono::steady_clock::now();
    while(completed<c.count){
        const int n=static_cast<int>(std::min<std::uint64_t>(c.batch,c.count-completed));DeviceBuffers b=allocateBuffers(n);
        std::uint16_t*dComp=nullptr,*dRuns=nullptr;allocateArray(dComp,static_cast<std::size_t>(n)*chunkCount,"alloc hotspot proxy comps");allocateArray(dRuns,static_cast<std::size_t>(n)*chunkCount,"alloc hotspot proxy runs");
        const int sb=(n+255)/256;hipLaunchKernelGGL(generateRandomSeedsKernel,dim3(sb),dim3(256),0,0,b.seeds,n,c.randomKey,c.startIndex+completed,static_cast<int>(c.seedMode));checkHip(hipGetLastError(),"generate hotspot seeds");
        int ci=0;for(int cz=chunkMin;cz<chunkMin+chunks;++cz)for(int cx=chunkMin;cx<chunkMin+chunks;++cx,++ci){launchChunkDensity(b,n,c.terrainThreads,cx,cz);hipLaunchKernelGGL(scoreChunkProxyGridKernel,dim3(n),dim3(256),0,0,b.noise1,n,dComp,dRuns,ci,chunkCount);checkHip(hipGetLastError(),"score hotspot proxy chunk");}
        checkHip(hipDeviceSynchronize(),"finish hotspot scout batch");
        std::vector<std::int64_t>seeds(static_cast<std::size_t>(n));std::vector<std::uint16_t>hc(static_cast<std::size_t>(n)*chunkCount),hr(hc.size());
        checkHip(hipMemcpy(seeds.data(),b.seeds,sizeof(std::int64_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy hotspot seeds");checkHip(hipMemcpy(hc.data(),dComp,sizeof(std::uint16_t)*hc.size(),hipMemcpyDeviceToHost),"copy hotspot comps");checkHip(hipMemcpy(hr.data(),dRuns,sizeof(std::uint16_t)*hr.size(),hipMemcpyDeviceToHost),"copy hotspot runs");
        for(int s=0;s<n;++s){Scout2 r;r.seed=seeds[static_cast<std::size_t>(s)];r.seq=c.startIndex+completed+static_cast<std::uint64_t>(s);r.hot=bestProxyHotspot(hc.data()+static_cast<std::size_t>(s)*chunkCount,hr.data()+static_cast<std::size_t>(s)*chunkCount,chunks,chunkMin);insertScout2(top,r,c.top);}
        (void)hipFree(dComp);(void)hipFree(dRuns);freeBuffers(b);completed+=static_cast<std::uint64_t>(n);writeScout2(c.output/"top_candidates.csv",top);
        const double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::cout<<"progress checked="<<completed<<'/'<<c.count<<" rate="<<std::fixed<<std::setprecision(2)<<(sec>0?completed/sec:0.0)<<" seeds/s";if(!top.empty())std::cout<<" bestProxyComponents="<<top.front().hot.count<<" window="<<top.front().hot.size<<" score="<<std::setprecision(6)<<densityScore(top.front().hot)<<" bestSeed="<<top.front().seed;std::cout<<"\n";
    }
    if(!top.empty())std::cout<<"SCOUT HOTSPOT BEST seed="<<top.front().seed<<" components="<<top.front().hot.count<<" window="<<top.front().hot.size<<" bounds="<<top.front().hot.minX<<".."<<top.front().hot.maxX<<","<<top.front().hot.minZ<<".."<<top.front().hot.maxZ<<" score="<<densityScore(top.front().hot)<<"\n";
    std::cout<<"CANDIDATES="<<(c.output/"top_candidates.csv").string()<<"\n";return 0;
}

struct Cand2{std::int64_t seed=0;std::uint64_t seq=0;double proxyScore=0;int proxyCount=0,proxyWindow=0;};
static std::vector<Cand2>readCand2(const std::filesystem::path&p,int limit){std::ifstream f(p);if(!f)throw std::runtime_error("cannot open "+p.string());std::string line;std::getline(f,line);std::vector<Cand2>o;while(static_cast<int>(o.size())<limit&&std::getline(f,line)){if(line.empty())continue;std::stringstream ss(line);std::vector<std::string>x;std::string q;while(std::getline(ss,q,','))x.push_back(q);if(x.size()<11)continue;Cand2 c;c.seed=std::stoll(x[1]);c.seq=std::stoull(x[2]);c.proxyScore=std::stod(x[3]);c.proxyCount=std::stoi(x[4]);c.proxyWindow=std::stoi(x[5]);o.push_back(c);}return o;}

__global__ void packChunkIntoWorldKernel(const double*density,int count,std::uint64_t*world,int side,int bx,int bz){const int seed=static_cast<int>(blockIdx.x),col=static_cast<int>(threadIdx.x);if(seed>=count||col>=256)return;const std::size_t base=static_cast<std::size_t>(seed)*coarsecore::CELLS;const int lx=col&15,lz=col>>4;std::uint64_t lo=0,hi=0;for(int yr=0;yr<Y_COUNT;++yr){if(!localSolid(density,base,lx,Y_BASE+yr,lz))continue;if(yr<64)lo|=(1ULL<<yr);else hi|=(1ULL<<(yr-64));}const int gc=(bz+lz)*side+(bx+lx);const std::size_t out=(static_cast<std::size_t>(seed)*static_cast<std::size_t>(side)*static_cast<std::size_t>(side)+static_cast<std::size_t>(gc))*2u;world[out]=lo;world[out+1]=hi;}

struct Exact2{Cand2 c;Hotspot hot;std::uint64_t totalFloating=0,totalBlocks=0,largest=0,excludedBoundary=0;};
static Hotspot bestExactHotspot(const std::vector<ComponentInfo>&comps,int side){
    const int origin=-side/2,ps=side+1;std::vector<int>grid(static_cast<std::size_t>(side)*side,0);std::vector<std::uint64_t>bgrid(grid.size(),0);
    for(const auto&c:comps){const int wx=(c.minX+c.maxX)/2,wz=(c.minZ+c.maxZ)/2,x=wx-origin,z=wz-origin;if(x<0||x>=side||z<0||z>=side)continue;const std::size_t idx=static_cast<std::size_t>(z)*side+x;++grid[idx];bgrid[idx]+=c.blocks;}
    std::vector<int>pc(static_cast<std::size_t>(ps)*ps,0);std::vector<std::uint64_t>pb(pc.size(),0);
    for(int z=0;z<side;++z)for(int x=0;x<side;++x){const std::size_t si=static_cast<std::size_t>(z)*side+x,di=static_cast<std::size_t>(z+1)*ps+x+1;pc[di]=grid[si]+pc[di-1]+pc[di-ps]-pc[di-ps-1];pb[di]=bgrid[si]+pb[di-1]+pb[di-ps]-pb[di-ps-1];}
    auto cnt=[&](int x,int z,int w){int x2=x+w,z2=z+w;return pc[static_cast<std::size_t>(z2)*ps+x2]-pc[static_cast<std::size_t>(z)*ps+x2]-pc[static_cast<std::size_t>(z2)*ps+x]+pc[static_cast<std::size_t>(z)*ps+x];};
    auto blks=[&](int x,int z,int w){int x2=x+w,z2=z+w;return pb[static_cast<std::size_t>(z2)*ps+x2]-pb[static_cast<std::size_t>(z)*ps+x2]-pb[static_cast<std::size_t>(z2)*ps+x]+pb[static_cast<std::size_t>(z)*ps+x];};
    Hotspot best;for(int wi=0;wi<WINDOW_COUNT;++wi){int w=WINDOW_BLOCKS[wi];if(w>side)continue;for(int z=0;z+w<=side;++z)for(int x=0;x+w<=side;++x){Hotspot h;h.count=cnt(x,z,w);h.size=w;h.blocks=blks(x,z,w);h.minX=origin+x;h.maxX=h.minX+w-1;h.minZ=origin+z;h.maxZ=h.minZ+w-1;if(betterHotspot(h,best))best=h;}}
    for(const auto&c:comps){const int wx=(c.minX+c.maxX)/2,wz=(c.minZ+c.maxZ)/2;if(wx<best.minX||wx>best.maxX||wz<best.minZ||wz>best.maxZ)continue;if(c.blocks>=8)++best.ge8;if(c.blocks>=32)++best.ge32;if(c.blocks>=128)++best.ge128;}
    return best;
}

static Exact2 analyzeExact2(const Cand2&cand,const std::uint64_t*world,int side,int minBlocks,bool includeBoundary){
    Exact2 out;out.c=cand;const int columns=side*side,origin=-side/2;std::vector<int>start(static_cast<std::size_t>(columns)+1u,0);std::vector<Segment>segs;segs.reserve(static_cast<std::size_t>(columns)*2u);
    auto solid=[&](int col,int yr){const std::uint64_t w=world[static_cast<std::size_t>(col)*2u+(yr>=64?1u:0u)];return (w&(1ULL<<(yr&63)))!=0;};
    for(int col=0;col<columns;++col){start[static_cast<std::size_t>(col)]=static_cast<int>(segs.size());int y=0;while(y<Y_COUNT){while(y<Y_COUNT&&!solid(col,y))++y;if(y>=Y_COUNT)break;int y0=y;while(y+1<Y_COUNT&&solid(col,y+1))++y;segs.push_back(Segment{col,y0,y});++y;}}start[static_cast<std::size_t>(columns)]=static_cast<int>(segs.size());DSU dsu(segs.size());
    auto connect=[&](int aCol,int bCol){int i=start[static_cast<std::size_t>(aCol)],ie=start[static_cast<std::size_t>(aCol+1)],j=start[static_cast<std::size_t>(bCol)],je=start[static_cast<std::size_t>(bCol+1)];while(i<ie&&j<je){const auto&a=segs[static_cast<std::size_t>(i)];const auto&b=segs[static_cast<std::size_t>(j)];if(a.y1<b.y0){++i;continue;}if(b.y1<a.y0){++j;continue;}dsu.unite(i,j);if(a.y1<=b.y1)++i;else ++j;}};
    for(int z=0;z<side;++z)for(int x=0;x<side;++x){int col=z*side+x;if(x>0)connect(col,col-1);if(z>0)connect(col,col-side);}
    struct Agg{std::uint64_t blocks=0;bool ground=false,boundary=false;int minX=INT_MAX,maxX=INT_MIN,minZ=INT_MAX,maxZ=INT_MIN,minY=INT_MAX,maxY=INT_MIN;};std::vector<Agg>agg(segs.size());
    for(std::size_t i=0;i<segs.size();++i){int r=dsu.find(static_cast<int>(i));const auto&s=segs[i];auto&a=agg[static_cast<std::size_t>(r)];int x=s.col%side,z=s.col/side;a.blocks+=static_cast<std::uint64_t>(s.y1-s.y0+1);if(s.y0==0)a.ground=true;if(x==0||x==side-1||z==0||z==side-1)a.boundary=true;a.minX=std::min(a.minX,x);a.maxX=std::max(a.maxX,x);a.minZ=std::min(a.minZ,z);a.maxZ=std::max(a.maxZ,z);a.minY=std::min(a.minY,s.y0);a.maxY=std::max(a.maxY,s.y1);}
    std::vector<ComponentInfo>comps;for(std::size_t i=0;i<agg.size();++i){if(dsu.find(static_cast<int>(i))!=static_cast<int>(i))continue;const auto&a=agg[i];if(a.blocks==0||a.ground)continue;if(a.boundary&&!includeBoundary){++out.excludedBoundary;continue;}if(a.blocks<static_cast<std::uint64_t>(minBlocks))continue;ComponentInfo ci;ci.blocks=a.blocks;ci.minX=origin+a.minX;ci.maxX=origin+a.maxX;ci.minZ=origin+a.minZ;ci.maxZ=origin+a.maxZ;ci.minY=Y_BASE+a.minY;ci.maxY=Y_BASE+a.maxY;comps.push_back(ci);++out.totalFloating;out.totalBlocks+=a.blocks;out.largest=std::max(out.largest,a.blocks);}
    out.hot=bestExactHotspot(comps,side);return out;
}

static bool betterExact2(const Exact2&a,const Exact2&b){if(betterHotspot(a.hot,b.hot))return true;if(betterHotspot(b.hot,a.hot))return false;if(a.totalFloating!=b.totalFloating)return a.totalFloating>b.totalFloating;if(a.totalBlocks!=b.totalBlocks)return a.totalBlocks>b.totalBlocks;return a.c.seq<b.c.seq;}
static void writeExact2(const std::filesystem::path&p,const std::vector<Exact2>&v){std::ofstream f(p,std::ios::trunc);if(!f)throw std::runtime_error("cannot write "+p.string());f<<"rank,seed,sequence_index,density_score,components_per_10k,hotspot_components,window_size,min_x,max_x,min_z,max_z,hotspot_blocks,hotspot_ge8,hotspot_ge32,hotspot_ge128,total_floating_components,total_floating_blocks,largest_component,excluded_boundary,proxy_score,proxy_components,proxy_window\n";for(std::size_t i=0;i<v.size();++i){const auto&r=v[i];f<<(i+1)<<','<<r.c.seed<<','<<r.c.seq<<','<<std::setprecision(12)<<densityScore(r.hot)<<','<<componentsPer10k(r.hot)<<','<<r.hot.count<<','<<r.hot.size<<','<<r.hot.minX<<','<<r.hot.maxX<<','<<r.hot.minZ<<','<<r.hot.maxZ<<','<<r.hot.blocks<<','<<r.hot.ge8<<','<<r.hot.ge32<<','<<r.hot.ge128<<','<<r.totalFloating<<','<<r.totalBlocks<<','<<r.largest<<','<<r.excludedBoundary<<','<<r.c.proxyScore<<','<<r.c.proxyCount<<','<<r.c.proxyWindow<<'\n';}}

static int runVerify2(const Config2&c){
    printDevice();auto cand=readCand2(c.input,c.verifyTop);if(cand.empty())throw std::runtime_error("no candidates to verify");const int chunks=c.canvas/16,chunkMin=-chunks/2;std::filesystem::create_directories(c.output);
    std::cout<<"TU4 FLOATING HOTSPOT P2 EXACT | candidates="<<cand.size()<<" canvas="<<c.canvas<<" windows=64..256 minBlocks="<<c.minBlocks<<"\n";std::vector<Exact2>res;const auto started=std::chrono::steady_clock::now();
    for(std::size_t bc=0;bc<cand.size();bc+=static_cast<std::size_t>(c.verifyBatch)){
        const int n=static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c.verifyBatch),cand.size()-bc));DeviceBuffers b=allocateBuffers(n);std::vector<std::int64_t>seeds(static_cast<std::size_t>(n));for(int i=0;i<n;++i)seeds[static_cast<std::size_t>(i)]=cand[bc+static_cast<std::size_t>(i)].seed;checkHip(hipMemcpy(b.seeds,seeds.data(),sizeof(std::int64_t)*static_cast<std::size_t>(n),hipMemcpyHostToDevice),"copy exact hotspot seeds");
        const std::size_t words=static_cast<std::size_t>(n)*static_cast<std::size_t>(c.canvas)*static_cast<std::size_t>(c.canvas)*2u;std::uint64_t*dWorld=nullptr;allocateArray(dWorld,words,"allocate exact hotspot world");int ci=0;for(int cz=chunkMin;cz<chunkMin+chunks;++cz)for(int cx=chunkMin;cx<chunkMin+chunks;++cx,++ci){launchChunkDensity(b,n,c.terrainThreads,cx,cz);const int bx=(cx-chunkMin)*16,bz=(cz-chunkMin)*16;hipLaunchKernelGGL(packChunkIntoWorldKernel,dim3(n),dim3(256),0,0,b.noise1,n,dWorld,c.canvas,bx,bz);checkHip(hipGetLastError(),"pack exact hotspot world");}
        checkHip(hipDeviceSynchronize(),"finish exact hotspot world batch");std::vector<std::uint64_t>host(words);checkHip(hipMemcpy(host.data(),dWorld,sizeof(std::uint64_t)*words,hipMemcpyDeviceToHost),"copy exact hotspot world once");const std::size_t per=static_cast<std::size_t>(c.canvas)*static_cast<std::size_t>(c.canvas)*2u;
        for(int i=0;i<n;++i){auto r=analyzeExact2(cand[bc+static_cast<std::size_t>(i)],host.data()+static_cast<std::size_t>(i)*per,c.canvas,c.minBlocks,c.includeBoundary);std::cout<<"EXACT HOTSPOT seed="<<r.c.seed<<" components="<<r.hot.count<<" window="<<r.hot.size<<" bounds="<<r.hot.minX<<".."<<r.hot.maxX<<","<<r.hot.minZ<<".."<<r.hot.maxZ<<" score="<<std::fixed<<std::setprecision(6)<<densityScore(r.hot)<<" ge8="<<r.hot.ge8<<" ge32="<<r.hot.ge32<<" hotspotBlocks="<<r.hot.blocks<<" totalFloating="<<r.totalFloating<<"\n";res.push_back(std::move(r));}
        (void)hipFree(dWorld);freeBuffers(b);
    }
    std::sort(res.begin(),res.end(),[](const Exact2&a,const Exact2&b){return betterExact2(a,b);});writeExact2(c.output/"exact_hotspots.csv",res);const double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();if(!res.empty()){const auto&b=res.front();std::cout<<"BEST EXACT FLOATING HOTSPOT seed="<<b.c.seed<<" components="<<b.hot.count<<" window="<<b.hot.size<<" bounds="<<b.hot.minX<<".."<<b.hot.maxX<<","<<b.hot.minZ<<".."<<b.hot.maxZ<<" score="<<densityScore(b.hot)<<" componentsPer10k="<<componentsPer10k(b.hot)<<"\n";}std::cout<<"verify_seconds="<<std::fixed<<std::setprecision(1)<<sec<<"\nEXACT_HOTSPOTS="<<(c.output/"exact_hotspots.csv").string()<<"\n";return 0;
}

} // namespace tu4_floating_hotspots_p2

int main(int argc,char**argv){try{auto c=tu4_floating_hotspots_p2::parseArgs2(argc,argv);return c.mode=="scout"?tu4_floating_hotspots_p2::runScout2(c):tu4_floating_hotspots_p2::runVerify2(c);}catch(const std::exception&e){std::cerr<<"TU4FloatingHotspotsP2 ERROR: "<<e.what()<<"\n";return 1;}}
