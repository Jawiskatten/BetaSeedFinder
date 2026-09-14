#define main highest_pillar_spawn_sand_wake_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main
#include "OriginCaveSim.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace sand_wake_freefall_p1v2 {
using namespace highest_pillar_spawn_p1;
namespace cave = sand_wake_cave;

struct Basic {
    std::int64_t seed;
    std::uint64_t seq;
    int pass;
    int desert;
    int sandReason;
    int surfaceDepth;
};

struct Config {
    std::filesystem::path output;
    std::filesystem::path bestState;
    std::uint64_t count = 1000000;
    std::uint64_t start = 0;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int batch = 32768;
    int terrainThreads = 64;
    int minDrop = 5;
    int progressMs = 1000;
};

struct Best {
    int drop = -1;
    std::int64_t seed = 0;
    int floorY = -1;
};

__global__ void basicKernel(
        const std::int64_t* seeds,
        const double* density,
        const double* sandNoise,
        const double* stoneNoise,
        const double* temperature,
        const double* rainfall,
        int n,
        std::uint64_t seqBase,
        Basic* out) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= n) return;
    const std::size_t base = static_cast<std::size_t>(i) * coarsecore::CELLS;
    const int center = -coarsecore::FROM_COARSE;
    Basic r{};
    r.seed = seeds[i];
    r.seq = seqBase + static_cast<std::uint64_t>(i);
    if (exactSpawnCheckYAtOrigin(density, base, center, center) != 63) { out[i] = r; return; }

    p20::JavaRandom surface;
    surface.setSeed(0);
    const bool sandPatch = sandNoise[i] + surface.nextDouble() * 0.2 > 0.0;
    (void)surface.nextDouble();
    const int depth = static_cast<int>(stoneNoise[i] / 3.0 + 3.0 + surface.nextDouble() * 0.25);
    const bool desert = betaBiomeIsDesert(temperature[i], rainfall[i]);
    const int reason = desert ? 2 : (sandPatch ? 1 : 0);
    r.desert = desert ? 1 : 0;
    r.sandReason = reason;
    r.surfaceDepth = depth;
    if (depth <= 0 || reason == 0) { out[i] = r; return; }
    if (terrainSolidAtBlock(density, base, 0, 65, 0) || terrainSolidAtBlock(density, base, 0, 66, 0)) { out[i] = r; return; }
    r.pass = 1;
    out[i] = r;
}

static std::array<int,128> exactOriginColumn(
        const double* density, double sandNoise, double stoneNoise,
        double temperature, double rainfall) {
    std::array<int,128> b{};
    for (int y=0;y<128;++y) {
        if (cave::solidAtBlockHost(density,0,y,0)) b[static_cast<std::size_t>(y)] = cave::STONE;
        else b[static_cast<std::size_t>(y)] = y < 64 ? cave::WATER_STILL : cave::AIR;
    }

    cave::JRandom rnd(0);
    const bool sandPatch = sandNoise + rnd.nextDouble()*0.2 > 0.0;
    (void)rnd.nextDouble(); // gravel noise itself is unavailable; Java is authoritative for rare desert/gravel false positives.
    const int depth = static_cast<int>(stoneNoise/3.0 + 3.0 + rnd.nextDouble()*0.25);
    const bool desert = cave::biomeIsDesert(temperature,rainfall);
    int remaining=-1;
    int top=desert?cave::SAND:cave::GRASS;
    int filler=desert?cave::SAND:cave::DIRT;

    for (int y=127;y>=0;--y) {
        const int bedrockRoll=rnd.nextInt(5);
        if (y<=bedrockRoll) { b[static_cast<std::size_t>(y)] = 7; continue; }
        const int id=b[static_cast<std::size_t>(y)];
        if (id==cave::AIR || id==cave::WATER_STILL) { remaining=-1; continue; }
        if (id!=cave::STONE) continue;
        if (remaining==-1) {
            top=desert?cave::SAND:cave::GRASS;
            filler=desert?cave::SAND:cave::DIRT;
            if (depth<=0) { top=cave::AIR; filler=cave::STONE; }
            else if (y>=60 && y<=65 && sandPatch) { top=cave::SAND; filler=cave::SAND; }
            if (y<64 && top==cave::AIR) top=cave::WATER_STILL;
            remaining=depth;
            b[static_cast<std::size_t>(y)] = y>=63 ? top : filler;
        } else if (remaining>0) {
            --remaining;
            b[static_cast<std::size_t>(y)] = filler;
            if (remaining==0 && filler==cave::SAND) {
                remaining=rnd.nextInt(4);
                filler=cave::SANDSTONE;
            }
        }
    }
    return b;
}

static bool analyze(const Basic& g, const double* density,
                    double sandNoise, double stoneNoise, double temp, double rain,
                    int minDrop, std::ostream& csv, Best& best,
                    const std::filesystem::path& bestState,
                    std::uint64_t& dryCandidates) {
    auto blocks=exactOriginColumn(density,sandNoise,stoneNoise,temp,rain);
    cave::CaveOriginSimulator sim(g.seed,density,blocks);
    sim.generateTargetChunkZero();
    if (cave::firstUncovered(blocks)!=63 || blocks[63]!=cave::SAND) return false;

    int bottom=63;
    while (bottom>0 && blocks[static_cast<std::size_t>(bottom-1)]==cave::SAND) --bottom;
    const int sandBlocks=64-bottom;
    if (bottom<=0 || blocks[static_cast<std::size_t>(bottom-1)]!=cave::AIR) return false;

    int floor=bottom-1;
    while (floor>=0 && blocks[static_cast<std::size_t>(floor)]==cave::AIR) --floor;
    if (floor<0 || !cave::fullCollisionBlock(blocks[static_cast<std::size_t>(floor)])) return false;
    const int relocatedTop=floor+sandBlocks;
    const int landingFeet=relocatedTop+1;
    const int drop=65-landingFeet;
    if (drop<minDrop) return false;

    ++dryCandidates;
    int sandstone=0;
    for (int y=0;y<63;++y) if (blocks[static_cast<std::size_t>(y)]==cave::SANDSTONE) ++sandstone;
    csv << g.seed << ',' << g.seq << ",63," << bottom << ',' << sandBlocks << ',' << sandstone << ','
        << floor << ',' << relocatedTop << ',' << landingFeet << ',' << drop << ',' << sim.carvedOriginY().size() << ','
        << g.desert << ',' << g.sandReason << ',' << g.surfaceDepth << '\n';

    if (drop>best.drop) {
        best.drop=drop; best.seed=g.seed; best.floorY=floor;
        if (!bestState.empty()) {
            std::ofstream f(bestState,std::ios::trunc);
            f << "DROP=" << best.drop << "\nSEED=" << best.seed << "\nFLOOR_Y=" << best.floorY << "\n";
        }
        std::cout << "NEW BEST SAND-WAKE POTENTIAL seed=" << g.seed << " drop=" << drop
                  << " floorY=" << floor << " sandBlocks=" << sandBlocks << " bottomSandY=" << bottom << "\n";
    }
    return true;
}

static std::uint64_t u64(const std::string& s) { return std::stoull(s,nullptr,0); }
static int i32(const std::string& s) { return std::stoi(s,nullptr,0); }

static Config parse(int argc,char** argv) {
    Config c;
    for (int i=1;i<argc;++i) {
        const std::string a=argv[i];
        auto v=[&](){ if (++i>=argc) throw std::invalid_argument("missing value for "+a); return std::string(argv[i]); };
        if (a=="--output") c.output=v();
        else if (a=="--best-state") c.bestState=v();
        else if (a=="--count") c.count=u64(v());
        else if (a=="--start-index") c.start=u64(v());
        else if (a=="--random-key") { c.randomKey=u64(v()); c.randomKeySet=true; }
        else if (a=="--batch") c.batch=i32(v());
        else if (a=="--terrain-threads") c.terrainThreads=i32(v());
        else if (a=="--min-potential-drop") c.minDrop=i32(v());
        else if (a=="--progress-ms") c.progressMs=i32(v());
        else if (a=="--seed-mode") { const std::string m=v(); c.seedMode=m=="unique48"?SeedMode::Unique48:m=="splitmix64"?SeedMode::SplitMix64:throw std::invalid_argument("bad seed mode"); }
        else throw std::invalid_argument("unknown argument: "+a);
    }
    if (c.output.empty()) throw std::invalid_argument("--output required");
    if (c.batch<256 || c.batch>32768) throw std::invalid_argument("--batch 256..32768");
    if (c.terrainThreads!=64 && c.terrainThreads!=128 && c.terrainThreads!=256) throw std::invalid_argument("--terrain-threads 64/128/256");
    if (c.minDrop<1 || c.minDrop>60) throw std::invalid_argument("--min-potential-drop 1..60");
    if (c.seedMode==SeedMode::Unique48 && (c.start>=JAVA_SEED_PERIOD || c.count>JAVA_SEED_PERIOD-c.start)) throw std::invalid_argument("unique48 range exceeds 2^48");
    if (!c.randomKeySet) { std::random_device rd; c.randomKey=(static_cast<std::uint64_t>(rd())<<32)^rd()^static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()); }
    return c;
}

static Best loadBest(const std::filesystem::path& p) {
    Best b; if (p.empty()) return b; std::ifstream f(p); std::string s;
    while (std::getline(f,s)) { const auto e=s.find('='); if(e==std::string::npos) continue; try { if(s.substr(0,e)=="DROP") b.drop=std::stoi(s.substr(e+1)); else if(s.substr(0,e)=="SEED") b.seed=std::stoll(s.substr(e+1)); else if(s.substr(0,e)=="FLOOR_Y") b.floorY=std::stoi(s.substr(e+1)); } catch(...){} }
    return b;
}

static int run(const Config& c) {
    printDevice();
    std::filesystem::create_directories(c.output);
    const auto csvPath=c.output/("candidates_"+std::to_string(c.start)+".csv");
    std::ofstream csv(csvPath,std::ios::trunc);
    csv << "seed,sequence_index,gate_y,sand_bottom_y,sand_blocks,sandstone_blocks,cave_floor_y,relocated_top_y,landing_feet_y,potential_drop,cave_carved_origin_blocks,desert,sand_reason,surface_depth\n";

    DeviceBuffers b=allocateBuffers(c.batch);
    Basic* dBasic=nullptr; allocateArray(dBasic,static_cast<std::size_t>(c.batch),"allocate sand-wake basic");
    std::vector<Basic> host(static_cast<std::size_t>(c.batch)), compact(static_cast<std::size_t>(c.batch));
    std::vector<std::int64_t> compactSeeds(static_cast<std::size_t>(c.batch));
    Best best=loadBest(c.bestState);
    std::uint64_t checked=0, sandGate=0, dryCandidates=0;
    const auto started=std::chrono::steady_clock::now(); auto last=started;

    try {
        while (checked<c.count) {
            const int n=static_cast<int>(std::min<std::uint64_t>(c.batch,c.count-checked));
            const std::uint64_t seqBase=c.start+checked;
            const int tb=256, bb=(n+tb-1)/tb;
            hipLaunchKernelGGL(generateRandomSeedsKernel,dim3(bb),dim3(tb),0,0,b.seeds,n,c.randomKey,seqBase,static_cast<int>(c.seedMode));
            checkHip(hipGetLastError(),"generate sand-wake seeds");
            launchTerrain(b,n,c.terrainThreads);
            const int ts=128, bs=(n+ts-1)/ts;
            hipLaunchKernelGGL(basicKernel,dim3(bs),dim3(ts),0,0,b.seeds,b.noise1,b.originSandNoise,b.originStoneNoise,b.originTemperature,b.originRainfall,n,seqBase,dBasic);
            checkHip(hipGetLastError(),"launch sand-wake basic"); checkHip(hipDeviceSynchronize(),"finish sand-wake basic");
            checkHip(hipMemcpy(host.data(),dBasic,static_cast<std::size_t>(n)*sizeof(Basic),hipMemcpyDeviceToHost),"copy sand-wake basic");

            int m=0;
            for (int i=0;i<n;++i) if (host[static_cast<std::size_t>(i)].pass) { compact[static_cast<std::size_t>(m)]=host[static_cast<std::size_t>(i)]; compactSeeds[static_cast<std::size_t>(m)]=host[static_cast<std::size_t>(i)].seed; ++m; }
            sandGate += static_cast<std::uint64_t>(m);
            if (m>0) {
                checkHip(hipMemcpy(b.seeds,compactSeeds.data(),static_cast<std::size_t>(m)*sizeof(std::int64_t),hipMemcpyHostToDevice),"upload compact sand-wake seeds");
                launchTerrain(b,m,c.terrainThreads);
                std::vector<double> density(static_cast<std::size_t>(m)*coarsecore::CELLS), sand(static_cast<std::size_t>(m)), stone(static_cast<std::size_t>(m)), temp(static_cast<std::size_t>(m)), rain(static_cast<std::size_t>(m));
                checkHip(hipMemcpy(density.data(),b.noise1,density.size()*sizeof(double),hipMemcpyDeviceToHost),"copy density");
                checkHip(hipMemcpy(sand.data(),b.originSandNoise,static_cast<std::size_t>(m)*sizeof(double),hipMemcpyDeviceToHost),"copy sand noise");
                checkHip(hipMemcpy(stone.data(),b.originStoneNoise,static_cast<std::size_t>(m)*sizeof(double),hipMemcpyDeviceToHost),"copy stone noise");
                checkHip(hipMemcpy(temp.data(),b.originTemperature,static_cast<std::size_t>(m)*sizeof(double),hipMemcpyDeviceToHost),"copy temp");
                checkHip(hipMemcpy(rain.data(),b.originRainfall,static_cast<std::size_t>(m)*sizeof(double),hipMemcpyDeviceToHost),"copy rain");
                for (int j=0;j<m;++j) analyze(compact[static_cast<std::size_t>(j)],density.data()+static_cast<std::size_t>(j)*coarsecore::CELLS,sand[j],stone[j],temp[j],rain[j],c.minDrop,csv,best,c.bestState,dryCandidates);
            }
            checked += static_cast<std::uint64_t>(n);
            const auto now=std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now-last).count()>=c.progressMs || checked==c.count) {
                csv.flush(); const double sec=std::chrono::duration<double>(now-started).count();
                std::cout << "progress checked=" << checked << '/' << c.count << " rate=" << static_cast<std::uint64_t>(checked/std::max(0.001,sec)) << " seeds/s sandGate=" << sandGate << " dryCaveCandidates=" << dryCandidates;
                if (best.drop>=0) std::cout << " runBestPotential=" << best.drop << " seed=" << best.seed; else std::cout << " runBestPotential=NONE";
                std::cout << '\n'; last=now;
            }
        }
    } catch (...) { if(dBasic)(void)hipFree(dBasic); freeBuffers(b); throw; }
    if(dBasic) checkHip(hipFree(dBasic),"free sand-wake basic"); freeBuffers(b);
    std::cout << "DONE checked=" << checked << " sandGate=" << sandGate << " candidates=" << dryCandidates << " file=" << csvPath.string() << '\n';
    std::cout << "Candidate = dormant origin sand over an exact dry Beta cave with >= threshold potential drop. Java client-startup oracle decides whether vanilla population naturally wakes it.\n";
    return 0;
}

} // namespace sand_wake_freefall_p1v2

int main(int argc,char** argv) {
    try { return sand_wake_freefall_p1v2::run(sand_wake_freefall_p1v2::parse(argc,argv)); }
    catch(const std::exception& e) { std::cerr << "SandWakeFreefall P1V2 ERROR: " << e.what() << '\n'; return 1; }
}
