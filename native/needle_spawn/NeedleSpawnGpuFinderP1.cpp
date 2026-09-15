#define main highest_pillar_spawn_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace needle_spawn_p1 {
using namespace highest_pillar_spawn_p1;

static constexpr int THREADS = 128;

struct NeedleResult {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    int qualified = 0;
    std::int64_t score = 0;
    int topY = -1;
    int sandReason = 0;
    int eastDrop = -1;
    int southDrop = -1;
    int southeastDrop = -1;
    int minPositiveDrop = -1;
    int quadrantDepth = 0;
    int r2MinDrop = -1;
    int r4MinDrop = -1;
    int clearPositiveTop = 0;
};

struct Config {
    std::filesystem::path outputDir;
    std::uint64_t count = 10000000;
    std::uint64_t startIndex = 0;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int batch = 32768;
    int terrainThreads = 64;
    int minDrop = 10;
    int minTopY = 70;
    int top = 500;
    int progressMs = 1000;
    int checkpointMs = 5000;
    bool resumeExisting = false;
};

__device__ __forceinline__ int nonAirTopApprox(
        const double* density, std::size_t base, int x, int z, int startY) {
    int solid = highestSolidAtOrBelow(density, base, x, z, startY);
    // Raw Beta terrain fills density-negative blocks below sea level with water.
    // Using Y63 as a conservative non-air floor makes the GPU drop estimate much
    // closer to the visual open-air drop that the exact Java verifier measures.
    if (startY >= 63 && solid < 63) return 63;
    return solid;
}

__device__ __forceinline__ int positiveQuadrantDepth(
        const double* density, std::size_t base, int topY) {
    int depth = 0;
    for (int y = topY; y >= 0; --y) {
        if (!terrainSolidAtBlock(density, base, 0, y, 0)) break;
        if (terrainSolidAtBlock(density, base, 1, y, 0)) break;
        if (terrainSolidAtBlock(density, base, 0, y, 1)) break;
        if (terrainSolidAtBlock(density, base, 1, y, 1)) break;
        ++depth;
    }
    return depth;
}

__device__ __forceinline__ int positiveRingMinDrop(
        const double* density, std::size_t base, int topY, int r) {
    const int pts[5][2] = {{r,0},{0,r},{r,r},{r,r/2},{r/2,r}};
    int minDrop = 999;
    for (int i=0;i<5;++i) {
        const int n = nonAirTopApprox(density,base,pts[i][0],pts[i][1],topY);
        const int d = topY - n;
        if (d < minDrop) minDrop = d;
    }
    return minDrop;
}

__global__ void scoreNeedleKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* originSandNoise,
        const double* originStoneNoise,
        const double* originTemperature,
        const double* originRainfall,
        int count,
        std::uint64_t baseIndex,
        int minDrop,
        int minTopY,
        NeedleResult* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= count) return;
    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;

    NeedleResult r{};
    r.seed = seeds[i];
    r.sequenceIndex = baseIndex + static_cast<std::uint64_t>(i);

    const int topY = exactSpawnCheckYAtOrigin(density,base,center,center);
    r.topY = topY;
    if (topY < minTopY) { out[i]=r; return; }

    // Exact surface replacement draws for the first column of chunk (0,0).
    p20::JavaRandom surface;
    surface.setSeed(0);
    const double sandJitter = surface.nextDouble();
    (void)surface.nextDouble();
    const double depthJitter = surface.nextDouble();
    const bool desert = betaBiomeIsDesert(originTemperature[i],originRainfall[i]);
    const bool beachSand = originSandNoise[i] + sandJitter * 0.2 > 0.0;
    const int depth = static_cast<int>(originStoneNoise[i] / 3.0 + 3.0 + depthJitter * 0.25);
    const bool beachBand = topY >= 60 && topY <= 65;
    r.sandReason = desert ? 2 : ((beachBand && beachSand) ? 1 : 0);
    if (depth <= 0 || r.sandReason == 0) { out[i]=r; return; }

    // Chunk (0,0) is authoritative for +X/+Z around the origin. We deliberately
    // hard-gate only this positive quadrant here. -X/-Z live in adjacent chunks,
    // whose boundary density nodes are generated independently in Beta; the exact
    // Java verifier checks all eight neighbors and all four cardinal drops.
    const bool eastAir = !terrainSolidAtBlock(density,base,1,topY,0);
    const bool southAir = !terrainSolidAtBlock(density,base,0,topY,1);
    const bool southeastAir = !terrainSolidAtBlock(density,base,1,topY,1);
    r.clearPositiveTop = (eastAir?1:0)+(southAir?1:0)+(southeastAir?1:0);
    if (r.clearPositiveTop != 3) { out[i]=r; return; }

    const int eTop = nonAirTopApprox(density,base,1,0,topY);
    const int sTop = nonAirTopApprox(density,base,0,1,topY);
    const int seTop = nonAirTopApprox(density,base,1,1,topY);
    r.eastDrop = topY - eTop;
    r.southDrop = topY - sTop;
    r.southeastDrop = topY - seTop;
    r.minPositiveDrop = min(r.eastDrop,min(r.southDrop,r.southeastDrop));
    if (r.minPositiveDrop < minDrop) { out[i]=r; return; }

    r.quadrantDepth = positiveQuadrantDepth(density,base,topY);
    r.r2MinDrop = positiveRingMinDrop(density,base,topY,2);
    r.r4MinDrop = positiveRingMinDrop(density,base,topY,4);
    r.qualified = 1;

    // Lexicographic intent: the minimum immediate drop dominates, then altitude,
    // then how many consecutive levels remain a 1x1 positive-quadrant shaft.
    r.score = static_cast<std::int64_t>(r.minPositiveDrop) * 1000000000000LL
            + static_cast<std::int64_t>(topY) * 1000000000LL
            + static_cast<std::int64_t>(std::min(999,r.quadrantDepth)) * 1000000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r2MinDrop))) * 1000LL
            + static_cast<std::int64_t>(std::max(0,std::min(999,r.r4MinDrop)));
    out[i]=r;
}

static std::uint64_t parseU64(const std::string& s) { return std::stoull(s,nullptr,0); }
static int parseInt(const std::string& s) { return std::stoi(s,nullptr,0); }

static Config parse(int argc,char** argv) {
    Config c;
    for(int i=1;i<argc;++i) {
        const std::string a=argv[i];
        auto v=[&](){if(++i>=argc) throw std::invalid_argument("missing value for "+a); return std::string(argv[i]);};
        if(a=="--output") c.outputDir=v();
        else if(a=="--count") c.count=parseU64(v());
        else if(a=="--start-index") c.startIndex=parseU64(v());
        else if(a=="--random-key"){c.randomKey=parseU64(v());c.randomKeySet=true;}
        else if(a=="--seed-mode") { const std::string m=v(); if(m=="unique48")c.seedMode=SeedMode::Unique48; else if(m=="splitmix64")c.seedMode=SeedMode::SplitMix64; else throw std::invalid_argument("bad seed mode"); }
        else if(a=="--batch") c.batch=parseInt(v());
        else if(a=="--terrain-threads") c.terrainThreads=parseInt(v());
        else if(a=="--min-drop") c.minDrop=parseInt(v());
        else if(a=="--min-top-y") c.minTopY=parseInt(v());
        else if(a=="--top") c.top=parseInt(v());
        else if(a=="--progress-ms") c.progressMs=parseInt(v());
        else if(a=="--checkpoint-ms") c.checkpointMs=parseInt(v());
        else if(a=="--resume-existing") c.resumeExisting=true;
        else throw std::invalid_argument("unknown argument: "+a);
    }
    if(c.outputDir.empty()) throw std::invalid_argument("--output required");
    if(c.batch<256||c.batch>32768) throw std::invalid_argument("--batch 256..32768");
    if(c.terrainThreads!=64&&c.terrainThreads!=128&&c.terrainThreads!=256) throw std::invalid_argument("--terrain-threads 64/128/256");
    if(c.minDrop<1||c.minDrop>100) throw std::invalid_argument("--min-drop 1..100");
    if(c.minTopY<63||c.minTopY>127) throw std::invalid_argument("--min-top-y 63..127");
    if(c.top<1||c.top>10000) throw std::invalid_argument("--top 1..10000");
    if(c.seedMode==SeedMode::Unique48 && (c.startIndex>=JAVA_SEED_PERIOD || c.count>JAVA_SEED_PERIOD-c.startIndex)) throw std::invalid_argument("unique48 range exceeds 2^48");
    if(!c.randomKeySet){std::random_device rd; c.randomKey=(static_cast<std::uint64_t>(rd())<<32)^rd()^static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());}
    return c;
}

static void insertTop(std::vector<NeedleResult>& rows,const NeedleResult& r,int limit) {
    if(!r.qualified) return;
    auto it=std::lower_bound(rows.begin(),rows.end(),r,[](const NeedleResult&a,const NeedleResult&b){
        if(a.score!=b.score) return a.score>b.score;
        return a.sequenceIndex<b.sequenceIndex;
    });
    if(static_cast<int>(rows.size())<limit || it!=rows.end()) {
        rows.insert(it,r);
        if(static_cast<int>(rows.size())>limit) rows.pop_back();
    }
}

static void writeCsv(const std::filesystem::path& p,const std::vector<NeedleResult>& rows) {
    std::ofstream f(p,std::ios::trunc);
    f << "rank,seed,sequence_index,score,top_y,sand_reason,east_drop,south_drop,southeast_drop,min_positive_drop,quadrant_depth,r2_min_drop,r4_min_drop,clear_positive_top\n";
    for(std::size_t i=0;i<rows.size();++i){const auto&r=rows[i];f<<(i+1)<<','<<r.seed<<','<<r.sequenceIndex<<','<<r.score<<','<<r.topY<<','<<r.sandReason<<','<<r.eastDrop<<','<<r.southDrop<<','<<r.southeastDrop<<','<<r.minPositiveDrop<<','<<r.quadrantDepth<<','<<r.r2MinDrop<<','<<r.r4MinDrop<<','<<r.clearPositiveTop<<'\n';}
}

static std::vector<NeedleResult> readCsv(const std::filesystem::path& p) {
    std::vector<NeedleResult> rows; std::ifstream f(p); if(!f)return rows; std::string line; std::getline(f,line);
    while(std::getline(f,line)){if(line.empty())continue;std::stringstream ss(line);std::vector<std::string>x;std::string q;while(std::getline(ss,q,','))x.push_back(q);if(x.size()!=14)continue;try{NeedleResult r{};r.qualified=1;r.seed=std::stoll(x[1]);r.sequenceIndex=std::stoull(x[2]);r.score=std::stoll(x[3]);r.topY=std::stoi(x[4]);r.sandReason=std::stoi(x[5]);r.eastDrop=std::stoi(x[6]);r.southDrop=std::stoi(x[7]);r.southeastDrop=std::stoi(x[8]);r.minPositiveDrop=std::stoi(x[9]);r.quadrantDepth=std::stoi(x[10]);r.r2MinDrop=std::stoi(x[11]);r.r4MinDrop=std::stoi(x[12]);r.clearPositiveTop=std::stoi(x[13]);rows.push_back(r);}catch(...){}}
    return rows;
}

static void save(const Config&c,std::uint64_t completed,const std::vector<NeedleResult>&rows) {
    std::filesystem::create_directories(c.outputDir); writeCsv(c.outputDir/"top_candidates.csv",rows);
    std::ofstream f(c.outputDir/"checkpoint.txt",std::ios::trunc);
    f<<"VERSION=NeedleSpawnP1\nSTART_INDEX="<<c.startIndex<<"\nCOUNT="<<c.count<<"\nCOMPLETED="<<completed<<"\nNEXT_INDEX="<<(c.startIndex+completed)<<"\nRANDOM_KEY="<<c.randomKey<<"\nSEED_MODE="<<(c.seedMode==SeedMode::Unique48?"unique48":"splitmix64")<<"\nMIN_DROP="<<c.minDrop<<"\nMIN_TOP_Y="<<c.minTopY<<"\n";
    if(!rows.empty())f<<"BEST_SEED="<<rows.front().seed<<"\nBEST_SCORE="<<rows.front().score<<"\nBEST_TOP_Y="<<rows.front().topY<<"\nBEST_MIN_POSITIVE_DROP="<<rows.front().minPositiveDrop<<"\n";
}

static int run(Config c) {
    printDevice();
    std::cout<<"NeedleSpawn P1 | exact origin sand gate + fast chunk(0,0) positive-quadrant needle scout\n";
    std::cout<<"Hard gates: topY >= "<<c.minTopY<<", +X/+Z/+XZ top cells clear, minimum positive-quadrant drop >= "<<c.minDrop<<".\n";
    std::cout<<"Important: -X/-Z are NOT trusted in GPU scoring because Beta regenerates boundary density nodes per chunk; exact Java verifier decides the real 1x1 needle.\n";
    std::cout<<"RandomKey="<<c.randomKey<<" seedMode="<<(c.seedMode==SeedMode::Unique48?"unique48":"splitmix64")<<"\n";

    std::uint64_t completed=0; std::vector<NeedleResult> top;
    if(c.resumeExisting){
        top=readCsv(c.outputDir/"top_candidates.csv"); if(static_cast<int>(top.size())>c.top)top.resize(c.top);
        std::ifstream ck(c.outputDir/"checkpoint.txt");std::string line;while(std::getline(ck,line)){if(line.rfind("COMPLETED=",0)==0)completed=std::stoull(line.substr(10));else if(line.rfind("RANDOM_KEY=",0)==0)c.randomKey=std::stoull(line.substr(11));}
        std::cout<<"Resuming completed="<<completed<<"\n";
    }

    DeviceBuffers b=allocateBuffers(c.batch); NeedleResult* dOut=nullptr; allocateArray(dOut,static_cast<std::size_t>(c.batch),"allocate needle results");
    std::vector<NeedleResult> host(static_cast<std::size_t>(c.batch));
    const auto started=std::chrono::steady_clock::now(); auto lastProgress=started,lastCheckpoint=started; std::uint64_t hits=0; std::int64_t printed=top.empty()?-1:top.front().score;
    try{
        while(completed<c.count){
            const int n=static_cast<int>(std::min<std::uint64_t>(c.batch,c.count-completed));
            const std::uint64_t seq=c.startIndex+completed;
            const int gb=(n+255)/256; hipLaunchKernelGGL(generateRandomSeedsKernel,dim3(gb),dim3(256),0,0,b.seeds,n,c.randomKey,seq,static_cast<int>(c.seedMode)); checkHip(hipGetLastError(),"generate needle seeds");
            launchTerrain(b,n,c.terrainThreads);
            const int sb=(n+THREADS-1)/THREADS; hipLaunchKernelGGL(scoreNeedleKernel,dim3(sb),dim3(THREADS),0,0,b.seeds,b.noise1,b.originSandNoise,b.originStoneNoise,b.originTemperature,b.originRainfall,n,seq,c.minDrop,c.minTopY,dOut); checkHip(hipGetLastError(),"score needle"); checkHip(hipDeviceSynchronize(),"finish needle batch");
            checkHip(hipMemcpy(host.data(),dOut,static_cast<std::size_t>(n)*sizeof(NeedleResult),hipMemcpyDeviceToHost),"copy needle results");
            for(int j=0;j<n;++j){const auto&r=host[static_cast<std::size_t>(j)];if(!r.qualified)continue;++hits;insertTop(top,r,c.top);if(r.score>printed){printed=r.score;std::cout<<"NEW BEST NEEDLE SCOUT seed="<<r.seed<<" topY="<<r.topY<<" minPositiveDrop="<<r.minPositiveDrop<<" depth="<<r.quadrantDepth<<" r2="<<r.r2MinDrop<<" r4="<<r.r4MinDrop<<"\n";}}
            completed+=static_cast<std::uint64_t>(n); const auto now=std::chrono::steady_clock::now();
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastProgress).count()>=c.progressMs||completed==c.count){const double sec=std::chrono::duration<double>(now-started).count();std::cout<<"progress checked="<<completed<<'/'<<c.count<<" rate="<<std::fixed<<std::setprecision(0)<<(sec>0?completed/sec:0)<<" seeds/s quadrantHits="<<hits;if(!top.empty())std::cout<<" bestTopY="<<top.front().topY<<" bestMinDrop="<<top.front().minPositiveDrop<<" bestSeed="<<top.front().seed;std::cout<<"\n";lastProgress=now;}
            if(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastCheckpoint).count()>=c.checkpointMs||completed==c.count){save(c,completed,top);lastCheckpoint=now;}
        }
    }catch(...){try{save(c,completed,top);}catch(...){} if(dOut)hipFree(dOut);freeBuffers(b);throw;}
    if(dOut)checkHip(hipFree(dOut),"free needle results"); freeBuffers(b); save(c,completed,top);
    if(!top.empty())std::cout<<"FINAL NEEDLE SCOUT BEST seed="<<top.front().seed<<" topY="<<top.front().topY<<" minPositiveDrop="<<top.front().minPositiveDrop<<"\n";
    std::cout<<"CANDIDATES="<<(c.outputDir/"top_candidates.csv").string()<<"\n"; return 0;
}

} // namespace needle_spawn_p1

int main(int argc,char**argv){try{return needle_spawn_p1::run(needle_spawn_p1::parse(argc,argv));}catch(const std::exception&e){std::cerr<<"NeedleSpawn P1 ERROR: "<<e.what()<<"\n";return 1;}}
