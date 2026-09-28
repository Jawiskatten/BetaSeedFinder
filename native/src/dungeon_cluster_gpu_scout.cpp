#include "gpu_runtime_compat.hpp"
#include "p20_exact_math.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dungeon_cluster_gpu {

static constexpr float PI = 3.1415927f;
static constexpr int ATTEMPTS = 8;
static constexpr int CAVE_RANGE = 8;
static constexpr int ROOM_PAD = 2;

struct RoomCandidate {
    int x;
    int y;
    int z;
    int halfX;
    int halfZ;
};

struct ScoutHit {
    std::int64_t seed;
    std::uint32_t nearCaveMask;
    std::uint32_t reserved;
};

struct Config {
    std::filesystem::path candidateOut = "out/dungeon_gpu_candidates.csv";
    std::uint64_t start = 0;
    std::uint64_t count = 1000000;
    int batch = 1024;
    int populationChunkX = 0;
    int populationChunkZ = 0;
    int progressMs = 1000;
    bool selfTest = false;
};

[[noreturn]] static void failHip(const char* operation, hipError_t error) {
    throw std::runtime_error(std::string(operation) + ": " + hipGetErrorString(error));
}
static void checkHip(hipError_t error, const char* operation) {
    if (error != hipSuccess) failHip(operation, error);
}
template <typename T>
static void allocateArray(T*& pointer, std::size_t count, const char* label) {
    checkHip(hipMalloc(reinterpret_cast<void**>(&pointer), count * sizeof(T)), label);
}

__host__ __device__ __forceinline__ float nextFloat(p20::JavaRandom& random) {
    return static_cast<float>(random.nextBits(24)) / 16777216.0f;
}

__host__ __device__ __forceinline__ std::int64_t javaNextLong(p20::JavaRandom& random) {
    const std::int32_t hi = static_cast<std::int32_t>(random.nextBits(32));
    const std::int32_t lo = static_cast<std::int32_t>(random.nextBits(32));
    std::uint64_t bits = static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32;
    bits += static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
    return static_cast<std::int64_t>(bits);
}

__host__ __device__ __forceinline__ std::int64_t javaOddLong(std::int64_t value) {
    return (value / 2) * 2 + 1;
}

__host__ __device__ __forceinline__ std::int64_t javaLongMix(
        int chunkX, std::int64_t oddX, int chunkZ, std::int64_t oddZ, std::int64_t seed) {
    std::uint64_t a = static_cast<std::uint64_t>(static_cast<std::int64_t>(chunkX))
            * static_cast<std::uint64_t>(oddX);
    std::uint64_t b = static_cast<std::uint64_t>(static_cast<std::int64_t>(chunkZ))
            * static_cast<std::uint64_t>(oddZ);
    return static_cast<std::int64_t>((a + b) ^ static_cast<std::uint64_t>(seed));
}

__host__ __device__ __forceinline__ bool initFailurePrefixRooms(
        std::int64_t seed,
        int populationChunkX,
        int populationChunkZ,
        RoomCandidate rooms[ATTEMPTS]) {
    // Exact ChunkProviderGenerate population seeding.
    p20::JavaRandom world;
    world.setSeed(seed);
    const std::int64_t oddX = javaOddLong(javaNextLong(world));
    const std::int64_t oddZ = javaOddLong(javaNextLong(world));

    p20::JavaRandom random;
    random.setSeed(javaLongMix(populationChunkX, oddX, populationChunkZ, oddZ, seed));

    // V1 scout deliberately owns only the no-lake-trigger subset. If either
    // pre-dungeon lake path triggers, RNG consumption becomes world-state
    // dependent and we send that coverage to a later stage instead of guessing.
    if (random.nextInt(4) == 0) return false;
    if (random.nextInt(8) == 0) return false;

    const int baseX = populationChunkX * 16;
    const int baseZ = populationChunkZ * 16;

    // Assume every preceding dungeon failed. These coordinates are exact up to
    // and including the first successful dungeon in the real population stream.
    for (int i = 0; i < ATTEMPTS; ++i) {
        rooms[i].x = baseX + random.nextInt(16) + 8;
        rooms[i].y = random.nextInt(128);
        rooms[i].z = baseZ + random.nextInt(16) + 8;
        rooms[i].halfX = random.nextInt(2) + 2;
        rooms[i].halfZ = random.nextInt(2) + 2;
    }
    return true;
}


static constexpr double CARVE_EPS = 0.035;
static constexpr double TWO_PI_OVER_65536 = 0.00009587379924285257;

__device__ __forceinline__ float betaSin(float value) {
    const int index = static_cast<int>(value * 10430.378f) & 65535;
    return static_cast<float>(sin(static_cast<double>(index) * TWO_PI_OVER_65536));
}

__device__ __forceinline__ float betaCos(float value) {
    const int index = (static_cast<int>(value * 10430.378f + 16384.0f)) & 65535;
    return static_cast<float>(sin(static_cast<double>(index) * TWO_PI_OVER_65536));
}

struct DoorState {
    std::uint32_t low[ATTEMPTS];
    std::uint32_t high[ATTEMPTS];
    std::uint32_t roomMask;
};

__device__ __forceinline__ bool nodeMayReachRoom(
        double x, double y, double z,
        double radiusXZ, double radiusY,
        const RoomCandidate& room) {
    const double minX = static_cast<double>(room.x - room.halfX - 1) + 0.5;
    const double maxX = static_cast<double>(room.x + room.halfX + 1) + 0.5;
    const double minZ = static_cast<double>(room.z - room.halfZ - 1) + 0.5;
    const double maxZ = static_cast<double>(room.z + room.halfZ + 1) + 0.5;
    const double minY = static_cast<double>(room.y) + 0.5;
    const double maxY = static_cast<double>(room.y + 1) + 0.5;
    return x + radiusXZ + 0.75 >= minX && x - radiusXZ - 0.75 <= maxX
        && z + radiusXZ + 0.75 >= minZ && z - radiusXZ - 0.75 <= maxZ
        && y + radiusY + 0.75 >= minY && y - radiusY - 0.75 <= maxY;
}

__device__ __forceinline__ bool caveWouldAirBlock(
        double x, double y, double z,
        double radiusXZ, double radiusY,
        int blockX, int blockY, int blockZ) {
    // Beta caves write lava below Y10, not air, so such blocks cannot form a
    // dungeon doorway. Ignore material/water cancellation here deliberately:
    // doing so can only create false positives for the Java verifier.
    if (blockY < 10) return false;
    const double nx = (static_cast<double>(blockX) + 0.5 - x) / radiusXZ;
    const double nz = (static_cast<double>(blockZ) + 0.5 - z) / radiusXZ;
    const double ny = (static_cast<double>(blockY) + 0.5 - y) / radiusY;
    if (ny <= -0.7 - CARVE_EPS) return false;
    return nx * nx + ny * ny + nz * nz < 1.0 + CARVE_EPS;
}

__device__ __forceinline__ int floorDiv16(int value) {
    if (value >= 0) return value >> 4;
    return -(((-value) + 15) >> 4);
}

__device__ __forceinline__ bool updateDoorwaysForNode(
        double x, double y, double z,
        double radiusXZ, double radiusY,
        const RoomCandidate rooms[ATTEMPTS],
        DoorState& state,
        int targetChunkX,
        int targetChunkZ) {
    for (int roomIndex = 0; roomIndex < ATTEMPTS; ++roomIndex) {
        if ((state.roomMask & (1u << roomIndex)) != 0) continue;
        const RoomCandidate& room = rooms[roomIndex];
        if (!nodeMayReachRoom(x, y, z, radiusXZ, radiusY, room)) continue;

        const int minX = room.x - room.halfX - 1;
        const int maxX = room.x + room.halfX + 1;
        const int minZ = room.z - room.halfZ - 1;
        const int maxZ = room.z + room.halfZ + 1;
        int doorIndex = 0;

        for (int bx = minX; bx <= maxX; ++bx) {
            for (int bz = minZ; bz <= maxZ; ++bz) {
                const bool perimeter =
                        bx == minX || bx == maxX || bz == minZ || bz == maxZ;
                if (!perimeter) continue;

                const std::uint32_t bit = 1u << doorIndex++;
                if (floorDiv16(bx) != targetChunkX || floorDiv16(bz) != targetChunkZ) {
                    continue;
                }
                if (caveWouldAirBlock(
                        x, y, z, radiusXZ, radiusY,
                        bx, room.y, bz)) {
                    state.low[roomIndex] |= bit;
                }
                if (caveWouldAirBlock(
                        x, y, z, radiusXZ, radiusY,
                        bx, room.y + 1, bz)) {
                    state.high[roomIndex] |= bit;
                }
            }
        }

        if ((state.low[roomIndex] & state.high[roomIndex]) != 0) {
            state.roomMask |= (1u << roomIndex);
            return true;
        }
    }
    return false;
}

struct CaveParams {
    double x;
    double y;
    double z;
    float width;
    float yaw;
    float pitch;
    int step;
    int maxStep;
    double verticalScale;
};

__device__ __forceinline__ bool simulateNoBranchNode(
        p20::JavaRandom& sourceRandom,
        CaveParams p,
        const RoomCandidate rooms[ATTEMPTS],
        DoorState& state,
        int targetChunkX,
        int targetChunkZ) {
    float yawVelocity = 0.0f;
    float pitchVelocity = 0.0f;

    p20::JavaRandom local;
    local.setSeed(javaNextLong(sourceRandom));

    if (p.maxStep <= 0) {
        const int max = CAVE_RANGE * 16 - 16;
        p.maxStep = max - local.nextInt(max / 4);
    }

    bool singleNode = false;
    if (p.step == -1) {
        p.step = p.maxStep / 2;
        singleNode = true;
    }

    (void)local.nextInt(p.maxStep / 2);
    const bool gentlePitch = local.nextInt(6) == 0;
    const double targetCenterX = static_cast<double>(targetChunkX * 16 + 8);
    const double targetCenterZ = static_cast<double>(targetChunkZ * 16 + 8);

    for (; p.step < p.maxStep; ++p.step) {
        const double radiusXZ =
                1.5 + static_cast<double>(
                        betaSin(static_cast<float>(p.step) * PI / static_cast<float>(p.maxStep))
                        * p.width);
        const double radiusY = radiusXZ * p.verticalScale;

        const float cosPitch = betaCos(p.pitch);
        const float sinPitch = betaSin(p.pitch);
        p.x += static_cast<double>(betaCos(p.yaw) * cosPitch);
        p.y += static_cast<double>(sinPitch);
        p.z += static_cast<double>(betaSin(p.yaw) * cosPitch);

        if (gentlePitch) p.pitch *= 0.92f;
        else p.pitch *= 0.7f;

        p.pitch += pitchVelocity * 0.1f;
        p.yaw += yawVelocity * 0.1f;
        pitchVelocity *= 0.9f;
        yawVelocity *= 0.75f;
        pitchVelocity += (nextFloat(local) - nextFloat(local)) * nextFloat(local) * 2.0f;
        yawVelocity += (nextFloat(local) - nextFloat(local)) * nextFloat(local) * 4.0f;

        bool carveStep = singleNode;
        if (!singleNode) carveStep = local.nextInt(4) != 0;
        if (carveStep) {
            const double dx = p.x - targetCenterX;
            const double dz = p.z - targetCenterZ;
            const double remaining = static_cast<double>(p.maxStep - p.step);
            const double maxReach = static_cast<double>(p.width + 2.0f + 16.0f);
            if (dx * dx + dz * dz - remaining * remaining > maxReach * maxReach) {
                return false;
            }

            const bool intersectsTarget =
                    p.x >= targetCenterX - 16.0 - radiusXZ * 2.0
                    && p.z >= targetCenterZ - 16.0 - radiusXZ * 2.0
                    && p.x <= targetCenterX + 16.0 + radiusXZ * 2.0
                    && p.z <= targetCenterZ + 16.0 + radiusXZ * 2.0;
            if (intersectsTarget && updateDoorwaysForNode(
                    p.x, p.y, p.z, radiusXZ, radiusY,
                    rooms, state, targetChunkX, targetChunkZ)) {
                return true;
            }
        }

        if (singleNode) break;
    }
    return false;
}

__device__ __forceinline__ bool simulateNode(
        p20::JavaRandom& sourceRandom,
        CaveParams p,
        const RoomCandidate rooms[ATTEMPTS],
        DoorState& state,
        int targetChunkX,
        int targetChunkZ) {
    float yawVelocity = 0.0f;
    float pitchVelocity = 0.0f;

    p20::JavaRandom local;
    local.setSeed(javaNextLong(sourceRandom));

    if (p.maxStep <= 0) {
        const int max = CAVE_RANGE * 16 - 16;
        p.maxStep = max - local.nextInt(max / 4);
    }

    bool singleNode = false;
    if (p.step == -1) {
        p.step = p.maxStep / 2;
        singleNode = true;
    }

    const int branchStep = local.nextInt(p.maxStep / 2) + p.maxStep / 4;
    const bool gentlePitch = local.nextInt(6) == 0;
    const double targetCenterX = static_cast<double>(targetChunkX * 16 + 8);
    const double targetCenterZ = static_cast<double>(targetChunkZ * 16 + 8);

    for (; p.step < p.maxStep; ++p.step) {
        const double radiusXZ =
                1.5 + static_cast<double>(
                        betaSin(static_cast<float>(p.step) * PI / static_cast<float>(p.maxStep))
                        * p.width);
        const double radiusY = radiusXZ * p.verticalScale;

        const float cosPitch = betaCos(p.pitch);
        const float sinPitch = betaSin(p.pitch);
        p.x += static_cast<double>(betaCos(p.yaw) * cosPitch);
        p.y += static_cast<double>(sinPitch);
        p.z += static_cast<double>(betaSin(p.yaw) * cosPitch);

        if (gentlePitch) p.pitch *= 0.92f;
        else p.pitch *= 0.7f;

        p.pitch += pitchVelocity * 0.1f;
        p.yaw += yawVelocity * 0.1f;
        pitchVelocity *= 0.9f;
        yawVelocity *= 0.75f;
        pitchVelocity += (nextFloat(local) - nextFloat(local)) * nextFloat(local) * 2.0f;
        yawVelocity += (nextFloat(local) - nextFloat(local)) * nextFloat(local) * 4.0f;

        if (!singleNode && p.step == branchStep && p.width > 1.0f) {
            CaveParams left = p;
            left.width = nextFloat(local) * 0.5f + 0.5f;
            left.yaw = p.yaw - 1.5707964f;
            left.pitch = p.pitch / 3.0f;
            left.verticalScale = 1.0;
            if (simulateNoBranchNode(sourceRandom, left, rooms, state, targetChunkX, targetChunkZ)) return true;

            CaveParams right = p;
            right.width = nextFloat(local) * 0.5f + 0.5f;
            right.yaw = p.yaw + 1.5707964f;
            right.pitch = p.pitch / 3.0f;
            right.verticalScale = 1.0;
            if (simulateNoBranchNode(sourceRandom, right, rooms, state, targetChunkX, targetChunkZ)) return true;
            return false;
        }

        bool carveStep = singleNode;
        if (!singleNode) carveStep = local.nextInt(4) != 0;
        if (carveStep) {
            const double dx = p.x - targetCenterX;
            const double dz = p.z - targetCenterZ;
            const double remaining = static_cast<double>(p.maxStep - p.step);
            const double maxReach = static_cast<double>(p.width + 2.0f + 16.0f);
            if (dx * dx + dz * dz - remaining * remaining > maxReach * maxReach) {
                return false;
            }

            const bool intersectsTarget =
                    p.x >= targetCenterX - 16.0 - radiusXZ * 2.0
                    && p.z >= targetCenterZ - 16.0 - radiusXZ * 2.0
                    && p.x <= targetCenterX + 16.0 + radiusXZ * 2.0
                    && p.z <= targetCenterZ + 16.0 + radiusXZ * 2.0;
            if (intersectsTarget && updateDoorwaysForNode(
                    p.x, p.y, p.z, radiusXZ, radiusY,
                    rooms, state, targetChunkX, targetChunkZ)) {
                return true;
            }
        }

        if (singleNode) break;
    }
    return false;
}

__device__ __forceinline__ std::uint32_t caveDoorwayMask(
        std::int64_t seed,
        int populationChunkX,
        int populationChunkZ,
        const RoomCandidate rooms[ATTEMPTS]) {
    p20::JavaRandom master;
    master.setSeed(seed);
    const std::int64_t oddX = javaOddLong(javaNextLong(master));
    const std::int64_t oddZ = javaOddLong(javaNextLong(master));

    DoorState state{};

    // MapGenBase replays source caves independently for each target chunk.
    // Early reach exits can happen before a branch and therefore change how
    // many sourceRandom.nextLong() values recursive child nodes consume. We
    // must preserve that target-specific execution to keep later cave RNG exact.
    for (int targetChunkX = populationChunkX;
         targetChunkX <= populationChunkX + 1; ++targetChunkX) {
        for (int targetChunkZ = populationChunkZ;
             targetChunkZ <= populationChunkZ + 1; ++targetChunkZ) {

            const int minSourceX = targetChunkX - CAVE_RANGE;
            const int maxSourceX = targetChunkX + CAVE_RANGE;
            const int minSourceZ = targetChunkZ - CAVE_RANGE;
            const int maxSourceZ = targetChunkZ + CAVE_RANGE;

            for (int sourceX = minSourceX; sourceX <= maxSourceX; ++sourceX) {
                for (int sourceZ = minSourceZ; sourceZ <= maxSourceZ; ++sourceZ) {
                    p20::JavaRandom random;
                    random.setSeed(javaLongMix(sourceX, oddX, sourceZ, oddZ, seed));

                    int count = random.nextInt(random.nextInt(random.nextInt(40) + 1) + 1);
                    if (random.nextInt(15) != 0) count = 0;

                    for (int cave = 0; cave < count; ++cave) {
                        const double x =
                                static_cast<double>(sourceX * 16 + random.nextInt(16));
                        const double y =
                                static_cast<double>(random.nextInt(random.nextInt(120) + 8));
                        const double z =
                                static_cast<double>(sourceZ * 16 + random.nextInt(16));
                        int tunnels = 1;

                        if (random.nextInt(4) == 0) {
                            CaveParams large{
                                x, y, z,
                                1.0f + nextFloat(random) * 6.0f,
                                0.0f, 0.0f,
                                -1, -1, 0.5
                            };
                            if (simulateNode(
                                    random, large, rooms, state,
                                    targetChunkX, targetChunkZ)) {
                                return state.roomMask;
                            }
                            tunnels += random.nextInt(4);
                        }

                        for (int tunnel = 0; tunnel < tunnels; ++tunnel) {
                            const float yaw = nextFloat(random) * PI * 2.0f;
                            const float pitch =
                                    (nextFloat(random) - 0.5f) * 2.0f / 8.0f;
                            const float width =
                                    nextFloat(random) * 2.0f + nextFloat(random);
                            CaveParams p{x, y, z, width, yaw, pitch, 0, 0, 1.0};
                            if (simulateNode(
                                    random, p, rooms, state,
                                    targetChunkX, targetChunkZ)) {
                                return state.roomMask;
                            }
                        }
                    }
                }
            }
        }
    }
    return state.roomMask;
}

__global__ void scoutKernel(
        std::uint64_t start,
        std::uint64_t count,
        int populationChunkX,
        int populationChunkZ,
        ScoutHit* hits,
        unsigned int* hitCount) {
    const std::uint64_t i =
            static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i >= count) return;

    const std::int64_t seed = static_cast<std::int64_t>(start + i);
    RoomCandidate rooms[ATTEMPTS];
    if (!initFailurePrefixRooms(seed, populationChunkX, populationChunkZ, rooms)) {
        return;
    }

    const std::uint32_t mask =
            caveDoorwayMask(seed, populationChunkX, populationChunkZ, rooms);
    if (mask == 0) return;

    const unsigned int out = atomicAdd(hitCount, 1u);
    hits[out].seed = seed;
    hits[out].nearCaveMask = mask;
    hits[out].reserved = 0;
}

static std::uint64_t parseU64(const std::string& value, const char* name) {
    try {
        std::size_t used = 0;
        const auto parsed = std::stoull(value, &used, 0);
        if (used != value.size()) throw std::invalid_argument("trailing characters");
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid --") + name + ": " + value);
    }
}
static int parseInt(const std::string& value, const char* name) {
    try {
        std::size_t used = 0;
        const int parsed = std::stoi(value, &used, 0);
        if (used != value.size()) throw std::invalid_argument("trailing characters");
        return parsed;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid --") + name + ": " + value);
    }
}

static Config parseArgs(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* option) -> std::string {
            if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + option);
            return argv[i];
        };

        if (arg == "--candidate-out") c.candidateOut = value("--candidate-out");
        else if (arg == "--start") c.start = parseU64(value("--start"), "start");
        else if (arg == "--count") c.count = parseU64(value("--count"), "count");
        else if (arg == "--batch") c.batch = parseInt(value("--batch"), "batch");
        else if (arg == "--chunk-x") c.populationChunkX = parseInt(value("--chunk-x"), "chunk-x");
        else if (arg == "--chunk-z") c.populationChunkZ = parseInt(value("--chunk-z"), "chunk-z");
        else if (arg == "--progress-ms") c.progressMs = parseInt(value("--progress-ms"), "progress-ms");
        else if (arg == "--self-test") c.selfTest = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout
                << "Beta 1.7.3 dungeon-cluster GPU cave-doorway scout\n\n"
                << "  --candidate-out <csv>\n"
                << "  --start <u64>\n"
                << "  --count <u64>\n"
                << "  --batch <int>       default 262144\n"
                << "  --chunk-x <int>     population chunk X, default 0\n"
                << "  --chunk-z <int>     population chunk Z, default 0\n"
                << "  --self-test\n\n"
                << "V2 coverage: exact no-lake-trigger population streams; conservative cave geometry\n"
                << "around failure-prefix dungeon attempts. Java exact verification is mandatory.\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    if (c.batch < 1) throw std::invalid_argument("--batch must be >= 1");
    if (c.progressMs < 0) throw std::invalid_argument("--progress-ms must be >= 0");
    return c;
}

static bool hostNoLake(std::int64_t seed, int chunkX, int chunkZ) {
    RoomCandidate rooms[ATTEMPTS];
    return initFailurePrefixRooms(seed, chunkX, chunkZ, rooms);
}

static void runSelfTest() {
    if (!hostNoLake(9602, 0, 0)) {
        throw std::runtime_error("self-test: seed 9602 should be in no-lake subset");
    }
    if (hostNoLake(180674, 0, 0)) {
        throw std::runtime_error("self-test: seed 180674 should be excluded by pre-dungeon lake trigger");
    }
    if (!hostNoLake(501789, 0, 0)) {
        throw std::runtime_error("self-test: seed 501789 should be in no-lake subset");
    }

    ScoutHit* dHits = nullptr;
    unsigned int* dCount = nullptr;
    allocateArray(dHits, 1, "self-test hipMalloc hits");
    allocateArray(dCount, 1, "self-test hipMalloc count");

    auto checkSeed = [&](std::uint64_t seed, bool expectedHit) {
        checkHip(hipMemset(dCount, 0, sizeof(unsigned int)), "self-test reset count");
        hipLaunchKernelGGL(
                scoutKernel, dim3(1), dim3(1), 0, 0,
                seed, 1ULL, 0, 0, dHits, dCount);
        checkHip(hipGetLastError(), "self-test launch");
        checkHip(hipDeviceSynchronize(), "self-test synchronize");
        unsigned int count = 0;
        checkHip(hipMemcpy(&count, dCount, sizeof(count), hipMemcpyDeviceToHost),
                 "self-test copy count");
        const bool hit = count == 1;
        if (hit != expectedHit) {
            throw std::runtime_error(
                    "self-test: unexpected GPU cave scout result for seed "
                    + std::to_string(seed)
                    + " expectedHit=" + (expectedHit ? "true" : "false")
                    + " actualHit=" + (hit ? "true" : "false"));
        }
    };

    // Known no-lake exact Java dungeon hits from the reference finder.
    checkSeed(9602, true);
    checkSeed(3426, true);
    checkSeed(8908, true);
    checkSeed(501789, true);
    // Known water-lake-trigger seed; V1 intentionally excludes it.
    checkSeed(180674, false);

    checkHip(hipFree(dCount), "self-test free count");
    checkHip(hipFree(dHits), "self-test free hits");
    std::cout << "SELF_TEST_OK known dungeon cave-door hits retained; lake-trigger seed excluded\n";
}

} // namespace dungeon_cluster_gpu

int main(int argc, char** argv) {
    using namespace dungeon_cluster_gpu;
    try {
        const Config c = parseArgs(argc, argv);
        if (c.selfTest) {
            runSelfTest();
            return 0;
        }

        int device = 0;
        checkHip(hipGetDevice(&device), "hipGetDevice");
        hipDeviceProp_t prop{};
        checkHip(hipGetDeviceProperties(&prop, device), "hipGetDeviceProperties");
        std::cout << "GPU=" << prop.name << "\n";
        std::cout << "DungeonCluster GPU Scout V2\n";
        std::cout << "coverage=no-lake-trigger + cave-doorway first-success necessary condition\n";
        std::cout << "start=" << c.start << " count=" << c.count
                  << " batch=" << c.batch
                  << " populationChunk=(" << c.populationChunkX << "," << c.populationChunkZ << ")\n";

        const auto parent = c.candidateOut.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream out(c.candidateOut);
        if (!out) throw std::runtime_error("cannot open candidate output: " + c.candidateOut.string());
        out << "seed,near_cave_mask,pop_chunk_x,pop_chunk_z,scout_version\n";

        ScoutHit* dHits = nullptr;
        unsigned int* dHitCount = nullptr;
        allocateArray(dHits, static_cast<std::size_t>(c.batch), "hipMalloc hits");
        allocateArray(dHitCount, 1, "hipMalloc hitCount");

        std::vector<ScoutHit> hostHits(static_cast<std::size_t>(c.batch));
        std::uint64_t done = 0;
        std::uint64_t totalHits = 0;
        const auto started = std::chrono::steady_clock::now();
        auto nextProgress = started + std::chrono::milliseconds(c.progressMs);

        while (done < c.count) {
            const std::uint64_t n64 = std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(c.batch), c.count - done);
            const int n = static_cast<int>(n64);

            checkHip(hipMemset(dHitCount, 0, sizeof(unsigned int)), "reset hit count");
            constexpr int THREADS = 128;
            const int blocks = (n + THREADS - 1) / THREADS;
            hipLaunchKernelGGL(
                    scoutKernel,
                    dim3(blocks), dim3(THREADS), 0, 0,
                    c.start + done,
                    n64,
                    c.populationChunkX,
                    c.populationChunkZ,
                    dHits,
                    dHitCount);
            checkHip(hipGetLastError(), "launch dungeon scout");
            checkHip(hipDeviceSynchronize(), "finish dungeon scout batch");

            unsigned int hits = 0;
            checkHip(hipMemcpy(&hits, dHitCount, sizeof(hits), hipMemcpyDeviceToHost),
                     "copy hit count");
            if (hits > static_cast<unsigned int>(n)) {
                throw std::runtime_error("GPU candidate counter exceeded batch size");
            }
            if (hits != 0) {
                checkHip(hipMemcpy(
                        hostHits.data(), dHits,
                        static_cast<std::size_t>(hits) * sizeof(ScoutHit),
                        hipMemcpyDeviceToHost),
                        "copy dungeon hits");
                for (unsigned int i = 0; i < hits; ++i) {
                    out << hostHits[i].seed << ','
                        << hostHits[i].nearCaveMask << ','
                        << c.populationChunkX << ','
                        << c.populationChunkZ << ','
                        << "cave_doorway_v2\n";
                }
                totalHits += hits;
            }

            done += n64;
            const auto now = std::chrono::steady_clock::now();
            if (c.progressMs == 0 || now >= nextProgress || done == c.count) {
                const double seconds =
                        std::chrono::duration<double>(now - started).count();
                const double rate = seconds > 0.0 ? static_cast<double>(done) / seconds : 0.0;
                const double pct = c.count == 0 ? 100.0 : 100.0 * static_cast<double>(done) / static_cast<double>(c.count);
                std::cout << std::fixed << std::setprecision(2)
                          << "progress=" << done << "/" << c.count
                          << " (" << pct << "%)"
                          << " rate=" << std::setprecision(1) << rate << " seeds/s"
                          << " candidates=" << totalHits
                          << " candidateRate=" << std::setprecision(4)
                          << (done == 0 ? 0.0 : 100.0 * static_cast<double>(totalHits) / static_cast<double>(done))
                          << "%\n";
                nextProgress = now + std::chrono::milliseconds(c.progressMs);
            }
        }

        out.flush();
        checkHip(hipFree(dHitCount), "free hitCount");
        checkHip(hipFree(dHits), "free hits");

        const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "\nDONE checked=" << c.count
                  << " candidates=" << totalHits
                  << " elapsed=" << std::fixed << std::setprecision(3) << seconds
                  << "s rate=" << std::setprecision(1)
                  << (seconds > 0.0 ? static_cast<double>(c.count) / seconds : 0.0)
                  << " seeds/s\n";
        std::cout << "CSV=" << std::filesystem::absolute(c.candidateOut).string() << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }
}
