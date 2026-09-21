#define main highest_pillar_spawn_embedded_main
#include "../highest_pillar_spawn/HighestPillarSpawnGpuFinder.cpp"
#undef main

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <climits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tu4_floating_components {
using namespace highest_pillar_spawn_p1;

static constexpr int CHUNK = 16;
static constexpr int COLS_PER_CHUNK = 256;
static constexpr int Y_BASE = coarsecore::Y_BASE * 8;
static constexpr int Y_COUNT = 128 - Y_BASE;
static_assert(Y_COUNT > 0 && Y_COUNT <= 128, "packed exact verifier assumes <=128 vertical blocks");

struct ScoutRow {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    std::uint64_t score = 0;
    std::uint32_t proxyComponents = 0;
    std::uint32_t detachedColumns = 0;
    std::uint32_t detachedRuns = 0;
    std::uint32_t detachedBlocks = 0;
};

struct Candidate {
    std::int64_t seed = 0;
    std::uint64_t sequenceIndex = 0;
    std::uint64_t scoutScore = 0;
    std::uint32_t proxyComponents = 0;
};

struct ComponentInfo {
    std::uint64_t blocks = 0;
    int minX = 0, maxX = 0;
    int minY = 0, maxY = 0;
    int minZ = 0, maxZ = 0;
};

struct ExactRow {
    Candidate c;
    std::uint64_t floatingCount = 0;
    std::uint64_t floatingBlocks = 0;
    std::uint64_t largest = 0;
    std::uint64_t ge8 = 0;
    std::uint64_t ge32 = 0;
    std::uint64_t ge128 = 0;
    std::uint64_t ge512 = 0;
    std::uint64_t excludedBoundary = 0;
    std::vector<ComponentInfo> topComponents;
};

struct Config {
    std::string mode = "scout";
    std::filesystem::path output;
    std::filesystem::path input;
    std::uint64_t count = 2000;
    std::uint64_t startIndex = 0;
    std::uint64_t randomKey = 0;
    bool randomKeySet = false;
    SeedMode seedMode = SeedMode::Unique48;
    int worldSize = 800;
    int batch = 1024;
    int terrainThreads = 64;
    int top = 64;
    int verifyTop = 32;
    int verifyBatch = 4;
    int minBlocks = 1;
    bool includeBoundary = false;
};

static std::uint64_t parseU64(const std::string& s, const char* what) {
    std::size_t used = 0;
    const auto v = std::stoull(s, &used, 0);
    if (used != s.size()) throw std::invalid_argument(std::string("invalid ") + what + ": " + s);
    return v;
}
static int parseInt(const std::string& s, const char* what) {
    std::size_t used = 0;
    const auto v = std::stoi(s, &used, 0);
    if (used != s.size()) throw std::invalid_argument(std::string("invalid ") + what + ": " + s);
    return v;
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (++i >= argc) throw std::invalid_argument("missing value for " + a);
            return argv[i];
        };
        if (a == "--mode") c.mode = val();
        else if (a == "--output") c.output = val();
        else if (a == "--input") c.input = val();
        else if (a == "--count") c.count = parseU64(val(), "count");
        else if (a == "--start-index") c.startIndex = parseU64(val(), "start-index");
        else if (a == "--random-key") { c.randomKey = parseU64(val(), "random-key"); c.randomKeySet = true; }
        else if (a == "--seed-mode") {
            const auto m = val();
            if (m == "unique48") c.seedMode = SeedMode::Unique48;
            else if (m == "splitmix64") c.seedMode = SeedMode::SplitMix64;
            else throw std::invalid_argument("--seed-mode must be unique48 or splitmix64");
        }
        else if (a == "--world-size") c.worldSize = parseInt(val(), "world-size");
        else if (a == "--batch") c.batch = parseInt(val(), "batch");
        else if (a == "--terrain-threads") c.terrainThreads = parseInt(val(), "terrain-threads");
        else if (a == "--top") c.top = parseInt(val(), "top");
        else if (a == "--verify-top") c.verifyTop = parseInt(val(), "verify-top");
        else if (a == "--verify-batch") c.verifyBatch = parseInt(val(), "verify-batch");
        else if (a == "--min-blocks") c.minBlocks = parseInt(val(), "min-blocks");
        else if (a == "--include-boundary") c.includeBoundary = true;
        else throw std::invalid_argument("unknown argument: " + a);
    }
    if (c.mode != "scout" && c.mode != "verify") throw std::invalid_argument("--mode scout|verify");
    if (c.output.empty()) throw std::invalid_argument("--output is required");
    if (c.mode == "verify" && c.input.empty()) throw std::invalid_argument("--input is required for verify");
    if (c.worldSize < 32 || c.worldSize > 1024 || (c.worldSize % 32) != 0) {
        throw std::invalid_argument("--world-size must be 32..1024 and divisible by 32 (800 and 864 are valid)");
    }
    if (c.batch < 1 || c.batch > 8192) throw std::invalid_argument("--batch must be 1..8192");
    if (c.verifyBatch < 1 || c.verifyBatch > 32) throw std::invalid_argument("--verify-batch must be 1..32");
    if (c.terrainThreads != 64 && c.terrainThreads != 128 && c.terrainThreads != 256) {
        throw std::invalid_argument("--terrain-threads must be 64, 128, or 256");
    }
    if (c.top < 1 || c.top > 10000) throw std::invalid_argument("--top must be 1..10000");
    if (c.verifyTop < 1 || c.verifyTop > c.top) throw std::invalid_argument("--verify-top must be 1..top");
    if (c.minBlocks < 1) throw std::invalid_argument("--min-blocks must be >=1");
    if (c.seedMode == SeedMode::Unique48 && c.mode == "scout") {
        if (c.startIndex >= JAVA_SEED_PERIOD || c.count > JAVA_SEED_PERIOD - c.startIndex) {
            throw std::invalid_argument("unique48 sequence exceeds 2^48");
        }
    }
    if (!c.randomKeySet) {
        std::random_device rd;
        c.randomKey = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd()) ^
            static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }
    return c;
}

__device__ __forceinline__ double localNode(const double* d, std::size_t base, int x, int y, int z) {
    if (x < 0 || z < 0 || x >= 5 || z >= 5) return -10.0;
    if (y < 0) return 10.0;
    if (y >= coarsecore::Y_LEVELS) return -10.0;
    return d[base + static_cast<std::size_t>(coarsecore::index3(x, y, z))];
}

__device__ __forceinline__ bool localSolid(const double* d, std::size_t base, int lx, int y, int lz) {
    if (y < Y_BASE) return true;
    if (y >= 128) return false;
    const int ix = lx >> 2;
    const int iz = lz >> 2;
    const int iy = (y >> 3) - coarsecore::Y_BASE;
    const double fx = static_cast<double>(lx & 3) * 0.25;
    const double fz = static_cast<double>(lz & 3) * 0.25;
    const double fy = static_cast<double>(y & 7) * 0.125;
    const double d000 = localNode(d, base, ix,     iy,     iz);
    const double d001 = localNode(d, base, ix,     iy,     iz + 1);
    const double d100 = localNode(d, base, ix + 1, iy,     iz);
    const double d101 = localNode(d, base, ix + 1, iy,     iz + 1);
    const double d010 = localNode(d, base, ix,     iy + 1, iz);
    const double d011 = localNode(d, base, ix,     iy + 1, iz + 1);
    const double d110 = localNode(d, base, ix + 1, iy + 1, iz);
    const double d111 = localNode(d, base, ix + 1, iy + 1, iz + 1);
    const double a0 = d000 + (d100 - d000) * fx;
    const double a1 = d001 + (d101 - d001) * fx;
    const double b0 = d010 + (d110 - d010) * fx;
    const double b1 = d011 + (d111 - d011) * fx;
    const double low = a0 + (a1 - a0) * fz;
    const double high = b0 + (b1 - b0) * fz;
    return low + (high - low) * fy > 0.0;
}

static void launchChunkDensity(DeviceBuffers& b, int count, int terrainThreads, int chunkX, int chunkZ) {
#if defined(SKYBLOCK_COARSE_API_MODERN)
    hipLaunchKernelGGL(coarsegpu::generateCoarseSignsKernel,
        dim3(count), dim3(terrainThreads), 0, 0,
        b.seeds, count,
        b.temp, b.rain, b.climateBlend,
        b.noise1, b.noise2, b.noise3, b.noise4, b.noise5,
        b.signs,
        b.originSandNoise, b.originStoneNoise, b.originTemperature, b.originRainfall,
        chunkX * 4, chunkZ * 4,
        nullptr, nullptr, nullptr, nullptr, nullptr);
#else
#error "TU4 floating component finder requires the modern coarse GPU API"
#endif
    checkHip(hipGetLastError(), "launch TU4 chunk terrain");
}

__global__ void scoreChunkProxyKernel(
        const double* density, int count,
        std::uint32_t* componentSum,
        std::uint32_t* detachedColumnSum,
        std::uint32_t* detachedRunSum,
        std::uint32_t* detachedBlockSum) {
    const int seed = static_cast<int>(blockIdx.x);
    const int lane = static_cast<int>(threadIdx.x);
    if (seed >= count || lane >= 256) return;
    const std::size_t base = static_cast<std::size_t>(seed) * coarsecore::CELLS;
    const int lx = lane & 15;
    const int lz = lane >> 4;

    int runs = 0;
    int blocks = 0;
    bool seenAir = false;
    bool inSolid = false;
    bool detached = false;
    for (int y = Y_BASE; y < 128; ++y) {
        const bool solid = localSolid(density, base, lx, y, lz);
        if (solid) {
            if (!inSolid) {
                if (seenAir) { ++runs; detached = true; }
                inSolid = true;
            }
            if (seenAir) ++blocks;
        } else {
            seenAir = true;
            inSolid = false;
        }
    }

    __shared__ unsigned char mask[256];
    __shared__ unsigned char visited[256];
    __shared__ unsigned short queue[256];
    __shared__ unsigned int sRuns[256];
    __shared__ unsigned int sBlocks[256];
    __shared__ unsigned int sCols[256];
    mask[lane] = detached ? 1 : 0;
    visited[lane] = 0;
    sRuns[lane] = static_cast<unsigned int>(runs);
    sBlocks[lane] = static_cast<unsigned int>(blocks);
    sCols[lane] = detached ? 1u : 0u;
    __syncthreads();

    for (int stride = 128; stride > 0; stride >>= 1) {
        if (lane < stride) {
            sRuns[lane] += sRuns[lane + stride];
            sBlocks[lane] += sBlocks[lane + stride];
            sCols[lane] += sCols[lane + stride];
        }
        __syncthreads();
    }

    if (lane == 0) {
        unsigned int comps = 0;
        for (int start = 0; start < 256; ++start) {
            if (!mask[start] || visited[start]) continue;
            ++comps;
            int head = 0, tail = 0;
            queue[tail++] = static_cast<unsigned short>(start);
            visited[start] = 1;
            while (head < tail) {
                const int v = static_cast<int>(queue[head++]);
                const int x = v & 15;
                const int z = v >> 4;
                const int n[4] = {v - 1, v + 1, v - 16, v + 16};
                if (x > 0) { const int q=n[0]; if (mask[q] && !visited[q]) { visited[q]=1; queue[tail++]=static_cast<unsigned short>(q); } }
                if (x < 15){ const int q=n[1]; if (mask[q] && !visited[q]) { visited[q]=1; queue[tail++]=static_cast<unsigned short>(q); } }
                if (z > 0) { const int q=n[2]; if (mask[q] && !visited[q]) { visited[q]=1; queue[tail++]=static_cast<unsigned short>(q); } }
                if (z < 15){ const int q=n[3]; if (mask[q] && !visited[q]) { visited[q]=1; queue[tail++]=static_cast<unsigned short>(q); } }
            }
        }
        atomicAdd(componentSum + seed, comps);
        atomicAdd(detachedColumnSum + seed, sCols[0]);
        atomicAdd(detachedRunSum + seed, sRuns[0]);
        atomicAdd(detachedBlockSum + seed, sBlocks[0]);
    }
}

__global__ void packChunkKernel(const double* density, int count, std::uint64_t* packed) {
    const int seed = static_cast<int>(blockIdx.x);
    const int col = static_cast<int>(threadIdx.x);
    if (seed >= count || col >= 256) return;
    const std::size_t base = static_cast<std::size_t>(seed) * coarsecore::CELLS;
    const int lx = col & 15;
    const int lz = col >> 4;
    std::uint64_t lo = 0, hi = 0;
    for (int yr = 0; yr < Y_COUNT; ++yr) {
        if (!localSolid(density, base, lx, Y_BASE + yr, lz)) continue;
        if (yr < 64) lo |= (1ULL << yr);
        else hi |= (1ULL << (yr - 64));
    }
    const std::size_t out = (static_cast<std::size_t>(seed) * 256u + static_cast<std::size_t>(col)) * 2u;
    packed[out] = lo;
    packed[out + 1] = hi;
}

static std::uint64_t makeScoutScore(std::uint32_t comps, std::uint32_t runs, std::uint32_t cols, std::uint32_t blocks) {
    const std::uint64_t a = std::min<std::uint32_t>(comps, 999999u);
    const std::uint64_t b = std::min<std::uint32_t>(runs, 999999u);
    const std::uint64_t c = std::min<std::uint32_t>(cols, 999999u);
    const std::uint64_t d = std::min<std::uint32_t>(blocks, 999999u);
    return a * 1000000000000ULL + b * 1000000ULL + c + d / 1000000ULL;
}

static void insertTop(std::vector<ScoutRow>& rows, const ScoutRow& r, int limit) {
    auto it = std::lower_bound(rows.begin(), rows.end(), r, [](const ScoutRow& a, const ScoutRow& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.sequenceIndex < b.sequenceIndex;
    });
    rows.insert(it, r);
    if (static_cast<int>(rows.size()) > limit) rows.pop_back();
}

static void writeScoutCsv(const std::filesystem::path& path, const std::vector<ScoutRow>& rows) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path.string());
    f << "rank,seed,sequence_index,score,proxy_components,detached_columns,detached_runs,detached_blocks\n";
    for (std::size_t i=0;i<rows.size();++i) {
        const auto& r=rows[i];
        f << (i+1) << ',' << r.seed << ',' << r.sequenceIndex << ',' << r.score << ','
          << r.proxyComponents << ',' << r.detachedColumns << ',' << r.detachedRuns << ',' << r.detachedBlocks << '\n';
    }
}

static std::vector<Candidate> readCandidates(const std::filesystem::path& path, int limit) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    std::string line;
    std::getline(f,line);
    std::vector<Candidate> out;
    while (static_cast<int>(out.size()) < limit && std::getline(f,line)) {
        if (line.empty()) continue;
        std::stringstream ss(line); std::vector<std::string> x; std::string q;
        while (std::getline(ss,q,',')) x.push_back(q);
        if (x.size() < 8) continue;
        Candidate c;
        c.seed=std::stoll(x[1]);
        c.sequenceIndex=std::stoull(x[2]);
        c.scoutScore=std::stoull(x[3]);
        c.proxyComponents=static_cast<std::uint32_t>(std::stoul(x[4]));
        out.push_back(c);
    }
    return out;
}

static int runScout(const Config& c) {
    printDevice();
    const int chunks = c.worldSize / 16;
    const int chunkMin = -chunks / 2;
    const int chunkMax = chunkMin + chunks - 1;
    std::filesystem::create_directories(c.output);
    std::cout << "TU4 FLOATING COMPONENT SCOUT | Beta 1.7.3 base terrain\n"
              << "world=" << c.worldSize << 'x' << c.worldSize << " blocks (" << chunks << 'x' << chunks << " chunks)\n"
              << "objective proxy: detached-footprint component count FIRST\n"
              << "count=" << c.count << " batch=" << c.batch << " randomKey=" << c.randomKey << "\n";

    std::uint64_t completed=0;
    std::vector<ScoutRow> top;
    const auto started=std::chrono::steady_clock::now();
    while (completed < c.count) {
        const int n=static_cast<int>(std::min<std::uint64_t>(c.batch,c.count-completed));
        DeviceBuffers b=allocateBuffers(n);
        std::uint32_t *dComp=nullptr,*dCols=nullptr,*dRuns=nullptr,*dBlocks=nullptr;
        allocateArray(dComp,n,"allocate proxy components");
        allocateArray(dCols,n,"allocate proxy columns");
        allocateArray(dRuns,n,"allocate proxy runs");
        allocateArray(dBlocks,n,"allocate proxy blocks");
        checkHip(hipMemset(dComp,0,sizeof(std::uint32_t)*static_cast<std::size_t>(n)),"clear proxy components");
        checkHip(hipMemset(dCols,0,sizeof(std::uint32_t)*static_cast<std::size_t>(n)),"clear proxy columns");
        checkHip(hipMemset(dRuns,0,sizeof(std::uint32_t)*static_cast<std::size_t>(n)),"clear proxy runs");
        checkHip(hipMemset(dBlocks,0,sizeof(std::uint32_t)*static_cast<std::size_t>(n)),"clear proxy blocks");

        const int seedBlocks=(n+255)/256;
        hipLaunchKernelGGL(generateRandomSeedsKernel,dim3(seedBlocks),dim3(256),0,0,
            b.seeds,n,c.randomKey,c.startIndex+completed,static_cast<int>(c.seedMode));
        checkHip(hipGetLastError(),"generate TU4 seeds");

        int chunkDone=0;
        for (int cz=chunkMin;cz<=chunkMax;++cz) {
            for (int cx=chunkMin;cx<=chunkMax;++cx) {
                launchChunkDensity(b,n,c.terrainThreads,cx,cz);
                hipLaunchKernelGGL(scoreChunkProxyKernel,dim3(n),dim3(256),0,0,
                    b.noise1,n,dComp,dCols,dRuns,dBlocks);
                checkHip(hipGetLastError(),"score TU4 chunk proxy");
                ++chunkDone;
                if ((chunkDone % 250)==0 || chunkDone==chunks*chunks) {
                    std::cout << "  batch " << (completed+1) << ".." << (completed+n)
                              << " chunks=" << chunkDone << '/' << (chunks*chunks) << "\r" << std::flush;
                }
            }
        }
        checkHip(hipDeviceSynchronize(),"finish TU4 scout batch");
        std::cout << std::string(100,' ') << "\r";

        std::vector<std::int64_t> seeds(static_cast<std::size_t>(n));
        std::vector<std::uint32_t> comp(n),cols(n),runs(n),blocks(n);
        checkHip(hipMemcpy(seeds.data(),b.seeds,sizeof(std::int64_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy scout seeds");
        checkHip(hipMemcpy(comp.data(),dComp,sizeof(std::uint32_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy proxy components");
        checkHip(hipMemcpy(cols.data(),dCols,sizeof(std::uint32_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy proxy columns");
        checkHip(hipMemcpy(runs.data(),dRuns,sizeof(std::uint32_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy proxy runs");
        checkHip(hipMemcpy(blocks.data(),dBlocks,sizeof(std::uint32_t)*static_cast<std::size_t>(n),hipMemcpyDeviceToHost),"copy proxy blocks");
        for (int i=0;i<n;++i) {
            ScoutRow r;
            r.seed=seeds[static_cast<std::size_t>(i)];
            r.sequenceIndex=c.startIndex+completed+static_cast<std::uint64_t>(i);
            r.proxyComponents=comp[static_cast<std::size_t>(i)];
            r.detachedColumns=cols[static_cast<std::size_t>(i)];
            r.detachedRuns=runs[static_cast<std::size_t>(i)];
            r.detachedBlocks=blocks[static_cast<std::size_t>(i)];
            r.score=makeScoutScore(r.proxyComponents,r.detachedRuns,r.detachedColumns,r.detachedBlocks);
            insertTop(top,r,c.top);
        }
        (void)hipFree(dComp);(void)hipFree(dCols);(void)hipFree(dRuns);(void)hipFree(dBlocks);freeBuffers(b);
        completed += static_cast<std::uint64_t>(n);
        writeScoutCsv(c.output / "top_candidates.csv",top);
        const double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::cout << "progress checked=" << completed << '/' << c.count
                  << " rate=" << std::fixed << std::setprecision(2) << (sec>0?completed/sec:0.0) << " seeds/s";
        if (!top.empty()) std::cout << " bestProxyComponents=" << top.front().proxyComponents << " bestSeed=" << top.front().seed;
        std::cout << "\n";
    }
    if (!top.empty()) {
        std::cout << "SCOUT BEST seed=" << top.front().seed << " proxyComponents=" << top.front().proxyComponents
                  << " detachedRuns=" << top.front().detachedRuns << "\n";
    }
    std::cout << "CANDIDATES=" << (c.output/"top_candidates.csv").string() << "\n";
    return 0;
}

struct Segment { int col=0,y0=0,y1=0; };
struct DSU {
    std::vector<int> p;
    std::vector<unsigned char> rank;
    explicit DSU(std::size_t n):p(n),rank(n,0){for(std::size_t i=0;i<n;++i)p[i]=static_cast<int>(i);} 
    int find(int x){int r=x;while(p[r]!=r)r=p[r];while(p[x]!=x){int q=p[x];p[x]=r;x=q;}return r;}
    void unite(int a,int b){a=find(a);b=find(b);if(a==b)return;if(rank[a]<rank[b])std::swap(a,b);p[b]=a;if(rank[a]==rank[b])++rank[a];}
};

static ExactRow analyzePacked(const Candidate& c, const std::vector<std::uint64_t>& world, int side, int minBlocks, bool includeBoundary) {
    ExactRow out; out.c=c;
    const int columns=side*side;
    std::vector<int> start(static_cast<std::size_t>(columns)+1u,0);
    std::vector<Segment> segs;
    segs.reserve(static_cast<std::size_t>(columns)*2u);
    auto solid=[&](int col,int yr)->bool{
        const std::uint64_t w=world[static_cast<std::size_t>(col)*2u+(yr>=64?1u:0u)];
        return (w & (1ULL<<(yr&63)))!=0;
    };
    for(int col=0;col<columns;++col){
        start[static_cast<std::size_t>(col)]=static_cast<int>(segs.size());
        int y=0;
        while(y<Y_COUNT){
            while(y<Y_COUNT&&!solid(col,y))++y;
            if(y>=Y_COUNT)break;
            const int y0=y;
            while(y+1<Y_COUNT&&solid(col,y+1))++y;
            segs.push_back(Segment{col,y0,y});
            ++y;
        }
    }
    start[static_cast<std::size_t>(columns)]=static_cast<int>(segs.size());
    DSU dsu(segs.size());
    auto connect=[&](int aCol,int bCol){
        int i=start[static_cast<std::size_t>(aCol)],ie=start[static_cast<std::size_t>(aCol+1)];
        int j=start[static_cast<std::size_t>(bCol)],je=start[static_cast<std::size_t>(bCol+1)];
        while(i<ie&&j<je){
            const auto&a=segs[static_cast<std::size_t>(i)];const auto&b=segs[static_cast<std::size_t>(j)];
            if(a.y1<b.y0){++i;continue;}if(b.y1<a.y0){++j;continue;}
            dsu.unite(i,j);
            if(a.y1<=b.y1)++i; else ++j;
        }
    };
    for(int z=0;z<side;++z)for(int x=0;x<side;++x){
        const int col=z*side+x;
        if(x>0)connect(col,col-1);
        if(z>0)connect(col,col-side);
    }
    struct Agg{std::uint64_t blocks=0;bool ground=false,boundary=false;int minX=INT_MAX,maxX=INT_MIN,minZ=INT_MAX,maxZ=INT_MIN,minY=INT_MAX,maxY=INT_MIN;};
    std::vector<Agg> agg(segs.size());
    for(std::size_t i=0;i<segs.size();++i){
        const int r=dsu.find(static_cast<int>(i));const auto&s=segs[i];auto&a=agg[static_cast<std::size_t>(r)];
        const int x=s.col%side,z=s.col/side;
        a.blocks+=static_cast<std::uint64_t>(s.y1-s.y0+1);
        if(s.y0==0)a.ground=true;
        if(x==0||x==side-1||z==0||z==side-1)a.boundary=true;
        a.minX=std::min(a.minX,x);a.maxX=std::max(a.maxX,x);a.minZ=std::min(a.minZ,z);a.maxZ=std::max(a.maxZ,z);
        a.minY=std::min(a.minY,s.y0);a.maxY=std::max(a.maxY,s.y1);
    }
    std::vector<ComponentInfo> comps;
    const int origin=-side/2;
    for(std::size_t i=0;i<agg.size();++i){
        if(dsu.find(static_cast<int>(i))!=static_cast<int>(i))continue;
        const auto&a=agg[i];if(a.blocks==0||a.ground)continue;
        if(a.boundary&&!includeBoundary){++out.excludedBoundary;continue;}
        if(a.blocks<static_cast<std::uint64_t>(minBlocks))continue;
        ++out.floatingCount;out.floatingBlocks+=a.blocks;out.largest=std::max(out.largest,a.blocks);
        if(a.blocks>=8)++out.ge8;if(a.blocks>=32)++out.ge32;if(a.blocks>=128)++out.ge128;if(a.blocks>=512)++out.ge512;
        ComponentInfo ci;ci.blocks=a.blocks;ci.minX=origin+a.minX;ci.maxX=origin+a.maxX;ci.minZ=origin+a.minZ;ci.maxZ=origin+a.maxZ;ci.minY=Y_BASE+a.minY;ci.maxY=Y_BASE+a.maxY;comps.push_back(ci);
    }
    std::sort(comps.begin(),comps.end(),[](const ComponentInfo&a,const ComponentInfo&b){return a.blocks>b.blocks;});
    if(comps.size()>25)comps.resize(25);
    out.topComponents=std::move(comps);
    return out;
}

static void writeExactCsv(const std::filesystem::path& path, const std::vector<ExactRow>& rows) {
    std::ofstream f(path,std::ios::trunc);if(!f)throw std::runtime_error("cannot write "+path.string());
    f<<"rank,seed,sequence_index,floating_components,floating_blocks,largest_component,components_ge8,components_ge32,components_ge128,components_ge512,excluded_boundary,proxy_components,scout_score\n";
    for(std::size_t i=0;i<rows.size();++i){const auto&r=rows[i];f<<(i+1)<<','<<r.c.seed<<','<<r.c.sequenceIndex<<','<<r.floatingCount<<','<<r.floatingBlocks<<','<<r.largest<<','<<r.ge8<<','<<r.ge32<<','<<r.ge128<<','<<r.ge512<<','<<r.excludedBoundary<<','<<r.c.proxyComponents<<','<<r.c.scoutScore<<'\n';}
}

static void writeBestComponents(const std::filesystem::path& path, const ExactRow& r) {
    std::ofstream f(path,std::ios::trunc);if(!f)throw std::runtime_error("cannot write "+path.string());
    f<<"rank,seed,blocks,min_x,max_x,min_y,max_y,min_z,max_z,span_x,span_y,span_z\n";
    for(std::size_t i=0;i<r.topComponents.size();++i){const auto&c=r.topComponents[i];f<<(i+1)<<','<<r.c.seed<<','<<c.blocks<<','<<c.minX<<','<<c.maxX<<','<<c.minY<<','<<c.maxY<<','<<c.minZ<<','<<c.maxZ<<','<<(c.maxX-c.minX+1)<<','<<(c.maxY-c.minY+1)<<','<<(c.maxZ-c.minZ+1)<<'\n';}
}

static int runVerify(const Config& c) {
    printDevice();
    auto candidates=readCandidates(c.input,c.verifyTop);
    if(candidates.empty())throw std::runtime_error("no candidates to verify");
    const int chunks=c.worldSize/16,chunkMin=-chunks/2,chunkMax=chunkMin+chunks-1;
    std::filesystem::create_directories(c.output);
    std::cout<<"TU4 EXACT COMPONENT VERIFY | candidates="<<candidates.size()<<" world="<<c.worldSize<<'x'<<c.worldSize
             <<" minBlocks="<<c.minBlocks<<" boundary="<<(c.includeBoundary?"include":"exclude")<<"\n";
    std::vector<ExactRow> results;
    const auto started=std::chrono::steady_clock::now();
    for(std::size_t baseCand=0;baseCand<candidates.size();baseCand+=static_cast<std::size_t>(c.verifyBatch)){
        const int n=static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(c.verifyBatch),candidates.size()-baseCand));
        DeviceBuffers b=allocateBuffers(n);
        std::vector<std::int64_t> seeds(static_cast<std::size_t>(n));for(int i=0;i<n;++i)seeds[static_cast<std::size_t>(i)]=candidates[baseCand+static_cast<std::size_t>(i)].seed;
        checkHip(hipMemcpy(b.seeds,seeds.data(),sizeof(std::int64_t)*static_cast<std::size_t>(n),hipMemcpyHostToDevice),"copy verify seeds");
        std::uint64_t*dPacked=nullptr;allocateArray(dPacked,static_cast<std::size_t>(n)*256u*2u,"allocate packed chunk occupancy");
        std::vector<std::uint64_t> hostPacked(static_cast<std::size_t>(n)*256u*2u);
        std::vector<std::vector<std::uint64_t>> worlds(static_cast<std::size_t>(n),std::vector<std::uint64_t>(static_cast<std::size_t>(c.worldSize)*static_cast<std::size_t>(c.worldSize)*2u,0));
        int chunkDone=0;
        for(int cz=chunkMin;cz<=chunkMax;++cz)for(int cx=chunkMin;cx<=chunkMax;++cx){
            launchChunkDensity(b,n,c.terrainThreads,cx,cz);
            hipLaunchKernelGGL(packChunkKernel,dim3(n),dim3(256),0,0,b.noise1,n,dPacked);
            checkHip(hipGetLastError(),"pack TU4 chunk occupancy");checkHip(hipDeviceSynchronize(),"finish TU4 exact chunk");
            checkHip(hipMemcpy(hostPacked.data(),dPacked,sizeof(std::uint64_t)*hostPacked.size(),hipMemcpyDeviceToHost),"copy packed TU4 chunk");
            const int bx=(cx-chunkMin)*16,bz=(cz-chunkMin)*16;
            for(int s=0;s<n;++s)for(int lz=0;lz<16;++lz)for(int lx=0;lx<16;++lx){
                const int lc=lz*16+lx;const int gc=(bz+lz)*c.worldSize+(bx+lx);
                const std::size_t src=(static_cast<std::size_t>(s)*256u+static_cast<std::size_t>(lc))*2u;
                const std::size_t dst=static_cast<std::size_t>(gc)*2u;
                worlds[static_cast<std::size_t>(s)][dst]=hostPacked[src];worlds[static_cast<std::size_t>(s)][dst+1]=hostPacked[src+1];
            }
            ++chunkDone;if((chunkDone%250)==0||chunkDone==chunks*chunks)std::cout<<"  exact batch "<<(baseCand+1)<<".."<<(baseCand+n)<<" chunks="<<chunkDone<<'/'<<(chunks*chunks)<<"\r"<<std::flush;
        }
        std::cout<<std::string(100,' ')<<"\r";
        for(int i=0;i<n;++i){
            auto r=analyzePacked(candidates[baseCand+static_cast<std::size_t>(i)],worlds[static_cast<std::size_t>(i)],c.worldSize,c.minBlocks,c.includeBoundary);
            std::cout<<"EXACT seed="<<r.c.seed<<" floatingComponents="<<r.floatingCount<<" ge8="<<r.ge8<<" ge32="<<r.ge32<<" largest="<<r.largest<<" floatingBlocks="<<r.floatingBlocks<<"\n";
            results.push_back(std::move(r));
        }
        (void)hipFree(dPacked);freeBuffers(b);
    }
    std::sort(results.begin(),results.end(),[](const ExactRow&a,const ExactRow&b){
        if(a.floatingCount!=b.floatingCount)return a.floatingCount>b.floatingCount;
        if(a.ge8!=b.ge8)return a.ge8>b.ge8;
        if(a.floatingBlocks!=b.floatingBlocks)return a.floatingBlocks>b.floatingBlocks;
        return a.largest>b.largest;
    });
    writeExactCsv(c.output/"exact_results.csv",results);
    if(!results.empty())writeBestComponents(c.output/"best_components.csv",results.front());
    const double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    if(!results.empty())std::cout<<"BEST EXACT TU4-LIKE WORLD seed="<<results.front().c.seed<<" floatingComponents="<<results.front().floatingCount<<" ge8="<<results.front().ge8<<" ge32="<<results.front().ge32<<" largest="<<results.front().largest<<"\n";
    std::cout<<"verify_seconds="<<std::fixed<<std::setprecision(1)<<sec<<"\nEXACT_RESULTS="<<(c.output/"exact_results.csv").string()<<"\nBEST_COMPONENTS="<<(c.output/"best_components.csv").string()<<"\n";
    return 0;
}

} // namespace tu4_floating_components

int main(int argc,char**argv){
    try{
        auto c=tu4_floating_components::parseArgs(argc,argv);
        return c.mode=="scout"?tu4_floating_components::runScout(c):tu4_floating_components::runVerify(c);
    }catch(const std::exception&e){std::cerr<<"TU4FloatingComponents ERROR: "<<e.what()<<"\n";return 1;}
}
