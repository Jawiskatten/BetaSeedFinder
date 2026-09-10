#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace singlebiome {

static constexpr int SEARCH_THREADS = 64;
static constexpr int EXACT_THREADS = 256;
static constexpr double PI = 3.141592653589793238462643383279502884;

enum BiomeId : unsigned char {
    RAINFOREST = 0,
    SWAMPLAND = 1,
    SEASONAL_FOREST = 2,
    FOREST = 3,
    SAVANNA = 4,
    SHRUBLAND = 5,
    TAIGA = 6,
    DESERT = 7,
    PLAINS = 8,
    TUNDRA = 9
};

const char* biomeName(unsigned char id) {
    switch (id) {
        case RAINFOREST: return "RAINFOREST";
        case SWAMPLAND: return "SWAMPLAND";
        case SEASONAL_FOREST: return "SEASONAL_FOREST";
        case FOREST: return "FOREST";
        case SAVANNA: return "SAVANNA";
        case SHRUBLAND: return "SHRUBLAND";
        case TAIGA: return "TAIGA";
        case DESERT: return "DESERT";
        case PLAINS: return "PLAINS";
        case TUNDRA: return "TUNDRA";
        default: return "UNKNOWN";
    }
}

struct ClimateState {
    p20::PerlinState temp[4];
    p20::PerlinState rain[4];
    p20::PerlinState blend[2];
    int baseBiome;
    int mismatch;
    int groupMinD2;
};

struct ExactPoint {
    int dx;
    int dz;
    int d2;
};

struct ExactResult {
    int safeRadius;
    int firstMismatchD2;
    int baseBiome;
};

struct Options {
    int target = 432;
    int centerX = 0;
    int centerZ = 0;
    int batch = 16384;
    int topExact = 1;
    double statusSeconds = 2.0;
    std::uint64_t sequence = 0;
    bool sequenceSpecified = false;
    std::uint64_t startAttempt = 0;
    std::uint64_t maxAttempts = 0;
    bool continueAfterHit = false;
    bool verifyOnly = false;
    std::int64_t verifySeed = 0;
    std::string logPath = "single_biome_radius_hits.csv";
};

#define HIP_CHECK(call) do { \
    hipError_t _err = (call); \
    if (_err != hipSuccess) { \
        throw std::runtime_error(std::string(#call) + " failed: " + hipGetErrorString(_err)); \
    } \
} while (0)

P20_HD std::int64_t multipliedSeed(std::int64_t seed, std::uint64_t multiplier) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * multiplier);
}

__device__ __forceinline__ void initClimate(ClimateState& s, std::int64_t seed) {
    const int lane = static_cast<int>(threadIdx.x);

    if (lane == 0) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 9871ULL));
        for (int i = 0; i < 4; ++i) p20::initPerlin(rng, s.temp[i]);
    } else if (lane == 1) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 39811ULL));
        for (int i = 0; i < 4; ++i) p20::initPerlin(rng, s.rain[i]);
    } else if (lane == 2) {
        p20::JavaRandom rng;
        rng.setSeed(multipliedSeed(seed, 543321ULL));
        for (int i = 0; i < 2; ++i) p20::initPerlin(rng, s.blend[i]);
    }
    __syncthreads();
}

__device__ __forceinline__ double octaveNoise4(
        const p20::PerlinState states[4],
        double x,
        double z,
        double baseScale,
        double octaveScale
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 4; ++octave) {
        const double scale = (baseScale / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= octaveScale;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double octaveNoise2(
        const p20::PerlinState states[2],
        double x,
        double z
) {
    double total = 0.0;
    double d6 = 1.0;
    double d7 = 1.0;
    for (int octave = 0; octave < 2; ++octave) {
        const double scale = (0.25 / 1.5) * d7;
        total += p20::simplex2(states[octave], x * scale, z * scale) * (0.55 / d6);
        d7 *= 0.5882352941176471;
        d6 *= 0.5;
    }
    return total;
}

__device__ __forceinline__ double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

__device__ __forceinline__ unsigned char classifyQuantizedBiome(double temperature, double rain) {
    // Beta 1.7.3 does not classify the raw doubles directly. It first indexes a
    // 64x64 lookup table with int(value * 63), and that table was built using
    // float i/63 and j/63. Reproduce that quantization exactly here.
    int ti = static_cast<int>(temperature * 63.0);
    int ri = static_cast<int>(rain * 63.0);
    if (ti < 0) ti = 0;
    if (ti > 63) ti = 63;
    if (ri < 0) ri = 0;
    if (ri > 63) ri = 63;

    const float f = static_cast<float>(ti) / 63.0f;
    float f1 = static_cast<float>(ri) / 63.0f;
    f1 *= f;

    if (f < 0.1f) return TUNDRA;
    if (f1 < 0.2f) {
        if (f < 0.5f) return TUNDRA;
        if (f < 0.95f) return SAVANNA;
        return DESERT;
    }
    if (f1 > 0.5f && f < 0.7f) return SWAMPLAND;
    if (f < 0.5f) return TAIGA;
    if (f < 0.97f) return f1 < 0.35f ? SHRUBLAND : FOREST;
    if (f1 < 0.45f) return PLAINS;
    if (f1 < 0.9f) return SEASONAL_FOREST;
    return RAINFOREST;
}

__device__ __forceinline__ unsigned char biomeAt(const ClimateState& s, int blockX, int blockZ) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);

    const double tempRaw = octaveNoise4(
            s.temp, x, z, 0.02500000037252903, 0.25);
    const double rainRaw = octaveNoise4(
            s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    const double blendRaw = octaveNoise2(s.blend, x, z);

    const double d0 = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + d0 * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    temperature = clamp01(temperature);
    rain = clamp01(rain);

    return classifyQuantizedBiome(temperature, rain);
}

__global__ void searchKernel(
        std::uint64_t sequence,
        std::uint64_t startAttempt,
        int count,
        int centerX,
        int centerZ,
        const int* probeDx,
        const int* probeDz,
        const int* ringRadii,
        int ringCount,
        unsigned short* probeRadius,
        unsigned char* baseBiome
) {
    const int seedIndex = static_cast<int>(blockIdx.x);
    const int lane = static_cast<int>(threadIdx.x);
    if (seedIndex >= count || lane >= SEARCH_THREADS) return;

    __shared__ ClimateState s;
    const std::uint64_t attempt = startAttempt + static_cast<std::uint64_t>(seedIndex);
    const std::int64_t seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(sequence, attempt));
    initClimate(s, seed);

    if (lane == 0) {
        s.baseBiome = static_cast<int>(biomeAt(s, centerX, centerZ));
        probeRadius[seedIndex] = 0;
        baseBiome[seedIndex] = static_cast<unsigned char>(s.baseBiome);
    }
    __syncthreads();

    for (int ring = 0; ring < ringCount; ++ring) {
        if (lane == 0) s.mismatch = 0;
        __syncthreads();

        const int pointIndex = ring * SEARCH_THREADS + lane;
        const unsigned char b = biomeAt(
                s,
                centerX + probeDx[pointIndex],
                centerZ + probeDz[pointIndex]);
        if (static_cast<int>(b) != s.baseBiome) atomicExch(&s.mismatch, 1);
        __syncthreads();

        if (s.mismatch != 0) return;
        if (lane == 0) probeRadius[seedIndex] = static_cast<unsigned short>(ringRadii[ring]);
        __syncthreads();
    }
}

__global__ void exactKernel(
        std::int64_t seed,
        int centerX,
        int centerZ,
        const ExactPoint* points,
        int pointCount,
        int target,
        ExactResult* result
) {
    const int lane = static_cast<int>(threadIdx.x);
    __shared__ ClimateState s;
    initClimate(s, seed);

    if (lane == 0) {
        s.baseBiome = static_cast<int>(biomeAt(s, centerX, centerZ));
        result->safeRadius = 0;
        result->firstMismatchD2 = -1;
        result->baseBiome = s.baseBiome;
    }
    __syncthreads();

    for (int base = 0; base < pointCount; base += EXACT_THREADS) {
        if (lane == 0) s.groupMinD2 = 0x7fffffff;
        __syncthreads();

        const int idx = base + lane;
        if (idx < pointCount) {
            const ExactPoint p = points[idx];
            const unsigned char b = biomeAt(s, centerX + p.dx, centerZ + p.dz);
            if (static_cast<int>(b) != s.baseBiome) atomicMin(&s.groupMinD2, p.d2);
        }
        __syncthreads();

        if (s.groupMinD2 != 0x7fffffff) {
            if (lane == 0) {
                const int d2 = s.groupMinD2;
                int root = static_cast<int>(sqrt(static_cast<double>(d2)));
                while ((root + 1) * (root + 1) <= d2) ++root;
                while (root * root > d2) --root;
                const int ceilRoot = root * root == d2 ? root : root + 1;
                int safe = ceilRoot - 1;
                if (safe < 0) safe = 0;
                if (safe > target) safe = target;
                result->safeRadius = safe;
                result->firstMismatchD2 = d2;
            }
            return;
        }
    }

    if (lane == 0) {
        result->safeRadius = target;
        result->firstMismatchD2 = -1;
    }
}

std::uint64_t parseU64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const unsigned long long parsed = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    return static_cast<std::uint64_t>(parsed);
}

std::int64_t parseI64(const std::string& value, const char* name) {
    std::size_t used = 0;
    const long long parsed = std::stoll(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    return static_cast<std::int64_t>(parsed);
}

int parseInt(const std::string& value, const char* name) {
    std::size_t used = 0;
    const long parsed = std::stol(value, &used, 0);
    if (used != value.size()) throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Out of range ") + name + ": " + value);
    }
    return static_cast<int>(parsed);
}

void printHelp() {
    std::cout
        << "SingleBiomeRadiusFinder - Minecraft Beta 1.7.3 climate search\n\n"
        << "Search mode:\n"
        << "  SingleBiomeRadiusFinder.exe [options]\n\n"
        << "Exact verify mode:\n"
        << "  SingleBiomeRadiusFinder.exe --verify-seed SEED --center-x X --center-z Z [--target 432]\n\n"
        << "Options:\n"
        << "  --target N             Required same-biome disk radius (default 432)\n"
        << "  --center-x N           Search/verify center X (default 0)\n"
        << "  --center-z N           Search/verify center Z (default 0)\n"
        << "  --batch N              Seeds per GPU batch (default 16384)\n"
        << "  --top-exact N          Exact-check this many top probe seeds per batch (default 1)\n"
        << "  --sequence N           Reproducible SplitMix sequence key\n"
        << "  --start-attempt N      Resume attempt index (default 0)\n"
        << "  --max-attempts N       Stop after N attempts; 0 = unlimited\n"
        << "  --status-seconds X     Status print interval (default 2.0)\n"
        << "  --log PATH             Record/JACKPOT CSV (default single_biome_radius_hits.csv)\n"
        << "  --continue-after-hit   Keep searching after a full target hit\n"
        << "  --verify-seed SEED     Exact-check one known seed instead of searching\n"
        << "  --help                  Show this help\n\n"
        << "The search probes concentric rings cheaply, but every reported RECORD/JACKPOT\n"
        << "is block-by-block exact inside its measured integer disk. Full target hits\n"
        << "cannot be lost to the probe stage: a truly uniform disk necessarily passes\n"
        << "every probe inside that disk.\n";
}

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value after ") + flag);
            return std::string(argv[++i]);
        };

        if (arg == "--help" || arg == "-h") {
            printHelp();
            std::exit(0);
        } else if (arg == "--target") {
            o.target = parseInt(need("--target"), "target");
        } else if (arg == "--center-x") {
            o.centerX = parseInt(need("--center-x"), "center-x");
        } else if (arg == "--center-z") {
            o.centerZ = parseInt(need("--center-z"), "center-z");
        } else if (arg == "--batch") {
            o.batch = parseInt(need("--batch"), "batch");
        } else if (arg == "--top-exact") {
            o.topExact = parseInt(need("--top-exact"), "top-exact");
        } else if (arg == "--sequence") {
            o.sequence = parseU64(need("--sequence"), "sequence");
            o.sequenceSpecified = true;
        } else if (arg == "--start-attempt") {
            o.startAttempt = parseU64(need("--start-attempt"), "start-attempt");
        } else if (arg == "--max-attempts") {
            o.maxAttempts = parseU64(need("--max-attempts"), "max-attempts");
        } else if (arg == "--status-seconds") {
            o.statusSeconds = std::stod(need("--status-seconds"));
        } else if (arg == "--log") {
            o.logPath = need("--log");
        } else if (arg == "--continue-after-hit") {
            o.continueAfterHit = true;
        } else if (arg == "--verify-seed") {
            o.verifySeed = parseI64(need("--verify-seed"), "verify-seed");
            o.verifyOnly = true;
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (o.target <= 0 || o.target > 20000) throw std::runtime_error("--target must be 1..20000");
    if (o.batch <= 0) throw std::runtime_error("--batch must be positive");
    if (o.topExact < 0) throw std::runtime_error("--top-exact cannot be negative");
    if (!(o.statusSeconds > 0.0)) throw std::runtime_error("--status-seconds must be positive");
    return o;
}

std::vector<int> makeRingRadii(int target) {
    std::vector<int> radii;
    for (int r = 32; r < target; r += 32) radii.push_back(r);
    if (radii.empty() || radii.back() != target) radii.push_back(target);
    return radii;
}

void makeProbePoints(
        const std::vector<int>& radii,
        std::vector<int>& dx,
        std::vector<int>& dz
) {
    dx.resize(radii.size() * SEARCH_THREADS);
    dz.resize(radii.size() * SEARCH_THREADS);

    for (std::size_t ring = 0; ring < radii.size(); ++ring) {
        const int r = radii[ring];
        const double phase = (ring & 1U) ? PI / static_cast<double>(SEARCH_THREADS) : 0.0;
        for (int lane = 0; lane < SEARCH_THREADS; ++lane) {
            const double angle = 2.0 * PI * static_cast<double>(lane) /
                                 static_cast<double>(SEARCH_THREADS) + phase;
            int x = static_cast<int>(std::llround(std::cos(angle) * static_cast<double>(r)));
            int z = static_cast<int>(std::llround(std::sin(angle) * static_cast<double>(r)));

            // Rounding a circle can place a lattice point a fraction outside r.
            // Pull it inward so a valid target disk can never be falsely rejected.
            while (static_cast<long long>(x) * x + static_cast<long long>(z) * z >
                   static_cast<long long>(r) * r) {
                if (std::abs(x) >= std::abs(z) && x != 0) x += x > 0 ? -1 : 1;
                else if (z != 0) z += z > 0 ? -1 : 1;
                else break;
            }
            dx[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = x;
            dz[ring * SEARCH_THREADS + static_cast<std::size_t>(lane)] = z;
        }
    }
}

std::vector<ExactPoint> makeExactPoints(int target) {
    const long long r2 = static_cast<long long>(target) * target;
    std::vector<ExactPoint> points;
    const double estimate = PI * static_cast<double>(target) * static_cast<double>(target);
    points.reserve(static_cast<std::size_t>(estimate + target * 8.0 + 64.0));

    for (int dz = -target; dz <= target; ++dz) {
        for (int dx = -target; dx <= target; ++dx) {
            if (dx == 0 && dz == 0) continue;
            const long long d2 = static_cast<long long>(dx) * dx + static_cast<long long>(dz) * dz;
            if (d2 <= r2) points.push_back({dx, dz, static_cast<int>(d2)});
        }
    }
    std::sort(points.begin(), points.end(), [](const ExactPoint& a, const ExactPoint& b) {
        return a.d2 < b.d2;
    });
    return points;
}

template <typename T>
T* deviceAlloc(std::size_t count) {
    T* ptr = nullptr;
    HIP_CHECK(hipMalloc(reinterpret_cast<void**>(&ptr), count * sizeof(T)));
    return ptr;
}

ExactResult runExact(
        std::int64_t seed,
        int centerX,
        int centerZ,
        int target,
        const ExactPoint* dPoints,
        int pointCount,
        ExactResult* dResult
) {
    hipLaunchKernelGGL(
            exactKernel,
            dim3(1), dim3(EXACT_THREADS), 0, 0,
            seed, centerX, centerZ, dPoints, pointCount, target, dResult);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    ExactResult result{};
    HIP_CHECK(hipMemcpy(&result, dResult, sizeof(result), hipMemcpyDeviceToHost));
    return result;
}

void ensureLogHeader(const std::string& path) {
    std::ifstream existing(path, std::ios::binary | std::ios::ate);
    if (existing && existing.tellg() > 0) return;
    std::ofstream out(path, std::ios::app);
    if (!out) throw std::runtime_error("Cannot open log file: " + path);
    out << "checked,attempt,seed,biome,safeRadius,firstMismatchDistance,centerX,centerZ,target,sequence\n";
}

void appendHit(
        const Options& o,
        std::uint64_t checked,
        std::uint64_t attempt,
        std::int64_t seed,
        const ExactResult& result
) {
    ensureLogHeader(o.logPath);
    std::ofstream out(o.logPath, std::ios::app);
    if (!out) throw std::runtime_error("Cannot append log file: " + o.logPath);
    out << checked << ',' << attempt << ',' << seed << ',' << biomeName(static_cast<unsigned char>(result.baseBiome)) << ','
        << result.safeRadius << ',';
    if (result.firstMismatchD2 < 0) out << "FULL";
    else out << std::fixed << std::setprecision(6) << std::sqrt(static_cast<double>(result.firstMismatchD2));
    out << ',' << o.centerX << ',' << o.centerZ << ',' << o.target << ',' << o.sequence << '\n';
}

void printExactResult(const Options& o, std::int64_t seed, const ExactResult& result) {
    std::cout << "seed=" << seed
              << " biome=" << biomeName(static_cast<unsigned char>(result.baseBiome))
              << " safeRadius=" << result.safeRadius;
    if (result.firstMismatchD2 < 0) {
        std::cout << " firstDifferent=NONE_WITHIN_" << o.target;
    } else {
        std::cout << " firstDifferentDistance=" << std::fixed << std::setprecision(3)
                  << std::sqrt(static_cast<double>(result.firstMismatchD2));
    }
    std::cout << " center=(" << o.centerX << ',' << o.centerZ << ")\n";
}

int run(const Options& optionsIn) {
    Options o = optionsIn;
    if (!o.sequenceSpecified) {
        const auto ticks = static_cast<std::uint64_t>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count());
        o.sequence = ticks ^ 0xA0761D6478BD642FULL;
    }

    int device = 0;
    HIP_CHECK(hipGetDevice(&device));
    hipDeviceProp_t prop{};
    HIP_CHECK(hipGetDeviceProperties(&prop, device));

    std::cout << "SingleBiomeRadiusFinder | Minecraft Beta 1.7.3\n"
              << "GPU: " << prop.name << "\n"
              << "targetRadius=" << o.target
              << " center=(" << o.centerX << ',' << o.centerZ << ")\n";

    std::cout << "Preparing exact disk points..." << std::flush;
    std::vector<ExactPoint> exactPoints = makeExactPoints(o.target);
    std::cout << " " << exactPoints.size() << " block positions\n";

    ExactPoint* dExactPoints = deviceAlloc<ExactPoint>(exactPoints.size());
    ExactResult* dExactResult = deviceAlloc<ExactResult>(1);
    HIP_CHECK(hipMemcpy(
            dExactPoints, exactPoints.data(), exactPoints.size() * sizeof(ExactPoint), hipMemcpyHostToDevice));

    if (o.verifyOnly) {
        const ExactResult result = runExact(
                o.verifySeed, o.centerX, o.centerZ, o.target,
                dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);
        std::cout << "[VERIFY] ";
        printExactResult(o, o.verifySeed, result);
        hipFree(dExactResult);
        hipFree(dExactPoints);
        return result.safeRadius >= o.target ? 0 : 2;
    }

    const std::vector<int> ringRadii = makeRingRadii(o.target);
    std::vector<int> probeDx;
    std::vector<int> probeDz;
    makeProbePoints(ringRadii, probeDx, probeDz);

    int* dProbeDx = deviceAlloc<int>(probeDx.size());
    int* dProbeDz = deviceAlloc<int>(probeDz.size());
    int* dRingRadii = deviceAlloc<int>(ringRadii.size());
    unsigned short* dProbeRadius = deviceAlloc<unsigned short>(static_cast<std::size_t>(o.batch));
    unsigned char* dBiome = deviceAlloc<unsigned char>(static_cast<std::size_t>(o.batch));

    HIP_CHECK(hipMemcpy(dProbeDx, probeDx.data(), probeDx.size() * sizeof(int), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dProbeDz, probeDz.data(), probeDz.size() * sizeof(int), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(dRingRadii, ringRadii.data(), ringRadii.size() * sizeof(int), hipMemcpyHostToDevice));

    std::vector<unsigned short> hProbeRadius(static_cast<std::size_t>(o.batch));
    std::vector<unsigned char> hBiome(static_cast<std::size_t>(o.batch));

    std::cout << "sequence=" << o.sequence
              << " startAttempt=" << o.startAttempt
              << " batch=" << o.batch
              << " rings=" << ringRadii.size()
              << " topExactPerBatch=" << o.topExact << "\n";
    std::cout << "A full target hit is always exact-verified before being reported. Ctrl+C stops the run.\n\n";

    const auto startTime = std::chrono::steady_clock::now();
    auto lastStatus = startTime;
    std::uint64_t checked = 0;
    std::uint64_t attemptBase = o.startAttempt;
    int bestProbe = -1;
    int bestExact = -1;
    std::int64_t bestExactSeed = 0;
    unsigned char bestExactBiome = 255;
    bool stop = false;

    while (!stop && (o.maxAttempts == 0 || checked < o.maxAttempts)) {
        int count = o.batch;
        if (o.maxAttempts != 0) {
            const std::uint64_t remaining = o.maxAttempts - checked;
            if (remaining < static_cast<std::uint64_t>(count)) count = static_cast<int>(remaining);
        }

        hipLaunchKernelGGL(
                searchKernel,
                dim3(count), dim3(SEARCH_THREADS), 0, 0,
                o.sequence, attemptBase, count, o.centerX, o.centerZ,
                dProbeDx, dProbeDz, dRingRadii, static_cast<int>(ringRadii.size()),
                dProbeRadius, dBiome);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(hProbeRadius.data(), dProbeRadius,
                            static_cast<std::size_t>(count) * sizeof(unsigned short), hipMemcpyDeviceToHost));
        HIP_CHECK(hipMemcpy(hBiome.data(), dBiome,
                            static_cast<std::size_t>(count) * sizeof(unsigned char), hipMemcpyDeviceToHost));

        checked += static_cast<std::uint64_t>(count);

        std::vector<int> order(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) order[static_cast<std::size_t>(i)] = i;
        const int wantedTop = std::min(o.topExact, count);
        if (wantedTop > 0) {
            std::partial_sort(order.begin(), order.begin() + wantedTop, order.end(),
                    [&](int a, int b) {
                        if (hProbeRadius[static_cast<std::size_t>(a)] != hProbeRadius[static_cast<std::size_t>(b)])
                            return hProbeRadius[static_cast<std::size_t>(a)] > hProbeRadius[static_cast<std::size_t>(b)];
                        return a < b;
                    });
        }

        std::vector<int> exactCandidates;
        exactCandidates.reserve(static_cast<std::size_t>(wantedTop) + 8U);
        std::unordered_set<int> seen;
        for (int i = 0; i < wantedTop; ++i) {
            const int index = order[static_cast<std::size_t>(i)];
            exactCandidates.push_back(index);
            seen.insert(index);
        }
        // Mandatory: exact-check EVERY seed whose probes reached the target.
        // Therefore a truly uniform target disk can never be discarded by the scout.
        for (int i = 0; i < count; ++i) {
            if (static_cast<int>(hProbeRadius[static_cast<std::size_t>(i)]) == o.target &&
                seen.insert(i).second) {
                exactCandidates.push_back(i);
            }
        }

        for (int i = 0; i < count; ++i) {
            const int probe = static_cast<int>(hProbeRadius[static_cast<std::size_t>(i)]);
            if (probe > bestProbe) {
                bestProbe = probe;
                const std::uint64_t attempt = attemptBase + static_cast<std::uint64_t>(i);
                const std::int64_t seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(o.sequence, attempt));
                std::cout << "[PROBE RECORD] seed=" << seed
                          << " biome=" << biomeName(hBiome[static_cast<std::size_t>(i)])
                          << " passedProbeRadius=" << bestProbe << "\n";
            }
        }

        for (int index : exactCandidates) {
            const std::uint64_t attempt = attemptBase + static_cast<std::uint64_t>(index);
            const std::int64_t seed = static_cast<std::int64_t>(p20::splitMixDeterministicSeed(o.sequence, attempt));
            const ExactResult result = runExact(
                    seed, o.centerX, o.centerZ, o.target,
                    dExactPoints, static_cast<int>(exactPoints.size()), dExactResult);

            if (result.safeRadius > bestExact) {
                bestExact = result.safeRadius;
                bestExactSeed = seed;
                bestExactBiome = static_cast<unsigned char>(result.baseBiome);
                std::cout << "[RECORD] ";
                printExactResult(o, seed, result);
                appendHit(o, checked, attempt, seed, result);
            }

            if (result.safeRadius >= o.target) {
                std::cout << "\n[JACKPOT] Full " << o.target
                          << "-block disk is one biome.\n[JACKPOT] ";
                printExactResult(o, seed, result);
                appendHit(o, checked, attempt, seed, result);
                if (!o.continueAfterHit) {
                    stop = true;
                    break;
                }
            }
        }

        attemptBase += static_cast<std::uint64_t>(count);
        const auto now = std::chrono::steady_clock::now();
        const double sinceStatus = std::chrono::duration<double>(now - lastStatus).count();
        if (sinceStatus >= o.statusSeconds || stop) {
            const double elapsed = std::chrono::duration<double>(now - startTime).count();
            const double rate = elapsed > 0.0 ? static_cast<double>(checked) / elapsed : 0.0;
            std::cout << "checked=" << checked
                      << " rate=" << std::fixed << std::setprecision(1) << rate << " seeds/s"
                      << " nextAttempt=" << attemptBase
                      << " probeBest=" << bestProbe
                      << " exactBest=" << bestExact;
            if (bestExact >= 0) {
                std::cout << " bestSeed=" << bestExactSeed
                          << " biome=" << biomeName(bestExactBiome);
            }
            std::cout << " elapsed=" << std::setprecision(1) << elapsed << "s\n";
            lastStatus = now;
        }
    }

    hipFree(dBiome);
    hipFree(dProbeRadius);
    hipFree(dRingRadii);
    hipFree(dProbeDz);
    hipFree(dProbeDx);
    hipFree(dExactResult);
    hipFree(dExactPoints);
    return 0;
}

} // namespace singlebiome

int main(int argc, char** argv) {
    try {
        return singlebiome::run(singlebiome::parseOptions(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
