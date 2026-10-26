// Minecraft Java 26.3 badlands dead-bush *decoration pattern* finder.
// This is an RNG/flat-surface scout, NOT a terrain/biome-verifying Minecraft generator.
// Compile: g++ -O3 -march=native -std=c++20 -pthread -o deadbush15 deadbush15.cpp
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using u64 = uint64_t;
static constexpr u64 SILVER = 7640891576956012809ULL;
static constexpr u64 GOLDEN = 0x9e3779b97f4a7c15ULL;
static constexpr int STEP = 9; // VEGETAL_DECORATION

static inline u64 rotl(u64 x, unsigned k) { return (x << k) | (x >> (64-k)); }
static inline u64 mix13(u64 z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
struct Xoro { // Xoroshiro128++ with Minecraft's RandomSupport 64->128 initialization
    u64 a, b;
    explicit Xoro(u64 seed): a(mix13(seed ^ SILVER)), b(mix13((seed ^ SILVER) + GOLDEN)) {
        if (!(a|b)) { a = GOLDEN; b = SILVER; }
    }
    inline u64 next() {
        u64 x=a, y=b, out=rotl(x+y,17)+x;
        y ^= x;
        a = rotl(x,49) ^ y ^ (y<<21);
        b = rotl(y,28);
        return out;
    }
    // WorldgenRandom extends BitRandomSource: nextInt(2^k) uses high k bits of Xoro nextLong.
    inline int pow2(int k) { return int(next() >> (64-k)); }
    inline u64 javaLong() {
        // BitRandomSource.nextLong: ((long)next(32) << 32) + (signed int)next(32)
        u64 hi = static_cast<uint32_t>(next() >> 32);
        auto lo = static_cast<int32_t>(next() >> 32);
        return (hi << 32) + static_cast<u64>(static_cast<int64_t>(lo));
    }
};

// Minecraft WorldgenRandom.setDecorationSeed(worldSeed, chunk min block X/Z),
// WorldgenRandom.setFeatureSeed(decorationSeed, globalFeatureIndex, 9).
static inline u64 feature_seed(u64 world, int32_t cx, int32_t cz, int index) {
    Xoro r(world);
    u64 xs = r.javaLong() | 1ULL;
    u64 zs = r.javaLong() | 1ULL;
    u64 dec = ((u64)(int64_t)(cx*16LL)*xs + (u64)(int64_t)(cz*16LL)*zs) ^ world;
    return dec + static_cast<u64>(index) + 10000ULL*STEP;
}

struct Hit { int run=0,x=0,z=0,dx=1,dz=0; };
struct Grid {
    static constexpr int W=64;
    static constexpr int BIAS=24; // radius <= 1 has local coordinate range [-23,38]
    std::array<u64, W> rows{};
    std::array<u64, W> cols{};
    // For radius 0 or 1, positions range -23..38 (projected to <=64 bits).
    inline void put(int x, int z) {
        int xx=x+BIAS, zz=z+BIAS;
        if (xx>=0 && xx<64 && zz>=0 && zz<W) {
            rows[zz] |= (1ULL << xx);
            cols[xx] |= (1ULL << zz);
        }
    }
    inline Hit best(bool diag=false) const {
        Hit h;
        auto score = [&](u64 mask,int base,int dir) {
            while(mask){
                unsigned begin=static_cast<unsigned>(__builtin_ctzll(mask));
                u64 shifted = mask >> begin;
                unsigned len=(shifted==~0ULL) ? 64u : static_cast<unsigned>(__builtin_ctzll(~shifted));
                if(int(len)>h.run){
                    h.run=int(len);h.x = dir==0 ? int(begin)-BIAS : base-BIAS;
                    h.z = dir==0 ? base-BIAS : int(begin)-BIAS;
                    h.dx=dir==0?1:0; h.dz=dir==0?0:1;
                }
                // 64-bit shift is undefined; special-case when remaining mask is all ones.
                if(len>=64 || begin+len >= 64) break;
                mask &= ~(((1ULL<<len)-1ULL)<<begin);
            }
        };
        for(int i=0;i<W;i++)score(rows[i],i,0);
        for(int i=0;i<64;i++)score(cols[i],i,1);
        if (diag) {
            // Intersect successive rows shifted one column per row; enough for reporting.
            for(int sign : {-1,1}) {
                for (int startz=0;startz<W;startz++) {
                    for(int startx=0;startx<64;startx++) {
                        if (!(rows[startz] & (1ULL<<startx))) continue;
                        int len=1;
                        while (startz+len<W && startx+sign*len>=0 && startx+sign*len<64
                               && (rows[startz+len] & (1ULL<<(startx+sign*len)))) ++len;
                        if(len>h.run){h={len,startx-BIAS,startz-BIAS,sign,1};}
                    }
                }
            }
        }
        return h;
    }
};

// Placement order: Count(20), InSquare(16,16), HEIGHTMAP, BiomeFilter,
// Count(4), Offset(triangle(7),triangle(3),triangle(7)), air filter, SimpleBlockFeature.
// Flat mode assumes constant surface height, air above, and badlands throughout feature.
// XY mode ignores offset Y entirely: optimistic X/Z pattern upper bound only.
static inline void place_chunk(Grid &g, u64 seed, int cx, int cz, bool flat, bool diagonal_unused=false) {
    (void)diagonal_unused;
    Xoro r(seed);
    for (int n=0;n<20;n++) {
        const int baseX=cx*16+r.pow2(4);
        const int baseZ=cz*16+r.pow2(4);
        for(int t=0;t<4;t++) {
            const int x=baseX+r.pow2(3)-r.pow2(3);
            const int dy=r.pow2(2)-r.pow2(2);
            const int z=baseZ+r.pow2(3)-r.pow2(3);
            if(!flat || dy==0) g.put(x,z);
        }
    }
}

static Hit evaluate(u64 seed, int index, int radius, bool flat, bool diag, bool featureSeedMode) {
    Grid grid;
    for(int cx=-radius;cx<=radius;cx++)for(int cz=-radius;cz<=radius;cz++) {
        if(featureSeedMode) {
            // Only meaningful for radius 0.
            place_chunk(grid,seed,cx,cz,flat);
        } else place_chunk(grid,feature_seed(seed,cx,cz,index),cx,cz,flat);
    }
    return grid.best(diag);
}
static void help() {
    std::cout << "Minecraft 26.3 dead-bush decoration RNG scout (not biome/terrain verified)\n\n"
       << "  deadbush15 --search --start 0 --count 1000000 --index 75 --target 15 --report 6\n"
       << "              [--threads 8] [--radius 0|1] [--mode flat|xz] [--diagonal]\n"
       << "  deadbush15 --inspect --seed -6825794747614541757 --chunk-x X --chunk-z Z --index 75\n"
       << "  deadbush15 --probe --seed -6825794747614541757 --chunk-x X --chunk-z Z\n"
       << "              [--index-limit 160] [--radius 1] [--mode xz]\n\n"
       << "Search/inspect chunk coordinates are CHUNK units; sample world is centered on (0,0).\n"
       << "Search enumerates world seeds at chunk (0,0), or use --feature-seeds for direct\n"
       << "64-bit feature-seed RNG pattern scanning (only radius 0).\n"
       << "  deadbush15 --self-test\n\n"
       << "--mode flat accepts only y-offset=0 (assuming uniform terrain). --mode xz\n"
       << "ignores height; it is an upper-bound prefilter, NOT a real plant placement.\n"
       << "Every hit is UNVERIFIED until checked in unmodified Java 26.3.\n";
}
static u64 parseU(const std::string&s){ return std::stoull(s,nullptr,0); }
static u64 parseS(const std::string&s){ return static_cast<u64>(std::stoll(s,nullptr,0)); }

int main(int argc,char**argv) {
    if(argc==1){help();return 0;}
    std::string command="--search", mode="flat", output="candidates.csv";
    u64 start=0, count=1000000, seed=0;
    int index=75, limit=170, target=15, report=6, threads=std::max(1u,std::thread::hardware_concurrency());
    int cx=0,cz=0,radius=0;bool diagonal=false, featureSeeds=false;
    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        auto val=[&]()->std::string{if(i+1>=argc)throw std::runtime_error("missing value for "+a);return argv[++i];};
        try {
            if(a=="--help"||a=="-h"){help();return 0;}
            else if(a=="--search"||a=="--inspect"||a=="--probe"||a=="--self-test")command=a;
            else if(a=="--start")start=parseU(val());
            else if(a=="--count")count=parseU(val());
            else if(a=="--seed")seed=parseS(val());
            else if(a=="--index")index=std::stoi(val());
            else if(a=="--index-limit")limit=std::stoi(val());
            else if(a=="--target")target=std::stoi(val());
            else if(a=="--report")report=std::stoi(val());
            else if(a=="--threads")threads=std::stoi(val());
            else if(a=="--chunk-x")cx=std::stoi(val());
            else if(a=="--chunk-z")cz=std::stoi(val());
            else if(a=="--radius")radius=std::stoi(val());
            else if(a=="--mode")mode=val();
            else if(a=="--output")output=val();
            else if(a=="--diagonal")diagonal=true;
            else if(a=="--feature-seeds")featureSeeds=true;
            else throw std::runtime_error("unknown option: "+a);
        } catch(const std::exception&e){std::cerr << e.what()<<"\n";return 2;}
    }
    if((mode!="flat"&&mode!="xz")||radius<0||radius>1||threads<1||threads>256||index<-1||target<1||report<1) {
        std::cerr<<"bad mode, radius, index, thread count, target, or report\n";return 2;
    }
    if(command=="--search" && (index<0&&!featureSeeds)){
        std::cerr<<"Set --index N (global feature index) or use --feature-seeds.\n";return 2;
    }
    if(featureSeeds && radius!=0){std::cerr<<"--feature-seeds requires --radius 0\n";return 2;}
    bool flat = mode=="flat";
    if(command=="--self-test"){
        struct Reference {u64 seed; int cx,cz,run; long long x,z;};
        const Reference cases[] = {
            {static_cast<u64>(-6825794747614541757LL),1033133,-1601222,14,16530130,-25619539},
            {923652124993224547ULL,353357,674814,12,5653714,10797037}
        };
        bool success=true;
        for(const auto &ref:cases) {
            Grid g;
            for(int x=ref.cx-1;x<=ref.cx+1;x++)for(int z=ref.cz-1;z<=ref.cz+1;z++)
                place_chunk(g,feature_seed(ref.seed,x,z,75),x-ref.cx,z-ref.cz,true);
            Hit h=g.best(false);
            long long actualX=ref.cx*16LL+h.x, actualZ=ref.cz*16LL+h.z;
            bool ok=h.run==ref.run && actualX==ref.x && actualZ==ref.z && h.dx==1 && h.dz==0;
            std::cout<<(ok?"PASS":"FAIL")<<" published "<<ref.run<<"-bush seed: reconstructed "<<h.run
                     <<" at ("<<actualX<<", "<<actualZ<<")"<<"\n";
            success &= ok;
        }
        return success ? 0 : 1;
    }
    if(command=="--inspect"){
        if(index<0){std::cerr<<"--inspect requires --index 75\n";return 2;}
        Grid g;
        for(int x=cx-radius;x<=cx+radius;x++)for(int z=cz-radius;z<=cz+radius;z++)
            place_chunk(g,feature_seed(seed,x,z,index),x-cx,z-cz,flat);
        Hit h=g.best(diagonal);
        std::cout<<"seed="<<static_cast<int64_t>(seed)<<" global_index="<<index<<" chunk="<<cx<<","<<cz
                 <<" radius="<<radius<<" mode="<<mode<<" best_run="<<h.run<<"\n";
        std::cout<<"start_block_x="<<cx*16LL+h.x<<" start_block_z="<<cz*16LL+h.z
                 <<" dx="<<h.dx<<" dz="<<h.dz<<"\n";
        // show a compact 64-column grid for a useful visual sanity check
        for(int z=0;z<Grid::W;z++) if(g.rows[z]) {
            std::cout<<"z="<<std::setw(4)<<z-Grid::BIAS<<" ";
            for(int x=10;x<62;x++) std::cout<<((g.rows[z]&(1ULL<<x))?'#':'.');
            std::cout<<"\n";
        }
        return 0;
    }
    if(command=="--probe"){
        if(limit<1||limit>1000){std::cerr<<"--index-limit must be 1..1000\n";return 2;}
        std::vector<std::pair<int,Hit>> ranks;
        for(int ix=0;ix<limit;ix++){
            Grid g;
            for(int x=cx-radius;x<=cx+radius;x++)for(int z=cz-radius;z<=cz+radius;z++)
                place_chunk(g,feature_seed(seed,x,z,ix),x-cx,z-cz,flat);
            ranks.push_back({ix,g.best(diagonal)});
        }
        std::sort(ranks.begin(),ranks.end(),[](const auto&a,const auto&b){return a.second.run>b.second.run;});
        std::cout<<"Probe candidate GLOBAL indices (not a substitute for registry lookup):\n";
        for(int i=0;i<std::min(20,limit);i++) {
            const auto &[ix,h]=ranks[i];
            std::cout<<" index="<<ix<<" run="<<h.run<<" x="<<cx*16LL+h.x<<" z="<<cz*16LL+h.z
                     <<" dir="<<h.dx<<","<<h.dz<<"\n";
        }
        return 0;
    }
    // Scan disjoint contiguous ranges in parallel, with single-threaded CSV writes.
    std::ofstream file(output,std::ios::app);
    if(!file){std::cerr<<"failed to open "<<output<<"\n";return 2;}
    if(file.tellp()==0)file<<"seed_unsigned,seed_signed,run,start_x,start_z,dx,dz,mode,chunk_radius,global_index,feature_seed_mode,terrain_verified\n";
    std::mutex writelock;
    std::atomic<u64> next(0),done(0);
    std::atomic<int> best(0);
    std::atomic<bool> stop(false);
    u64 block=4096;
    auto t0=std::chrono::steady_clock::now();
    std::cout<<"Searching "<<count<<" "<<(featureSeeds?"feature seeds":"world seeds")<<" starting at "<<start
             <<"; threads="<<threads<<"; mode="<<mode<<"; radius="<<radius
             <<"; index="<<index<<"; target="<<target<<"; report="<<report<<"\n";
    auto worker=[&]() {
        while(!stop.load(std::memory_order_relaxed)){
            u64 offset=next.fetch_add(block);
            if(offset>=count)break;
            u64 end=std::min(count,offset+block);
            for(u64 j=offset;j<end;j++){
                u64 s=start+j;
                Hit h=evaluate(s,index,radius,flat,diagonal,featureSeeds);
                int old=best.load(std::memory_order_relaxed);
                while(h.run>old&&!best.compare_exchange_weak(old,h.run)){}
                if(h.run>=report){
                    std::lock_guard<std::mutex> lock(writelock);
                    file << s <<","<<static_cast<int64_t>(s)<<","<<h.run<<","<<h.x<<","<<h.z<<","<<h.dx<<","<<h.dz
                         <<","<<mode<<","<<radius<<","<<index<<","<<(featureSeeds?1:0)<<",0\n";
                    file.flush();
                    if(h.run>=target){
                        std::cout<<"CANDIDATE ONLY seed="<<static_cast<int64_t>(s)<<" run="<<h.run
                                 <<" start_x="<<h.x<<" start_z="<<h.z<<" (NOT VERIFIED)\n";
                    }
                }
            }
            done.fetch_add(end-offset,std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> pool;
    for(int i=0;i<threads;i++)pool.emplace_back(worker);
    // No promise of hitting 15: report throughput/best from this bounded run.
    if(count>=20000000ULL) {
        while(done.load(std::memory_order_relaxed)<count) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const u64 n=done.load(std::memory_order_relaxed);
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
            std::cout<<"Progress "<<n<<"/"<<count<<" ("<<std::fixed<<std::setprecision(1)
                     <<(count ? 100.0*double(n)/double(count):100.0)<<"%), best="<<best.load()
                     <<", "<<std::setprecision(0)<<(elapsed>0?n/elapsed:0)<<" seeds/s\n" <<std::flush;
        }
    }
    for(auto &t:pool)t.join();
    const double secs=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    std::cout<<"Done "<<done.load()<<" searched in "<<secs<<" s: "<<std::fixed<<std::setprecision(0)
             <<(secs>0?done.load()/secs:0)<<" seeds/s; best flat/XZ pattern="<<best.load()
             <<". Candidate CSV: "<<output<<". Nothing is terrain verified.\n";
    return 0;
}