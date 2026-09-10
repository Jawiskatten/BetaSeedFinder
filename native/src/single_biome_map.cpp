#include "p20_exact_math.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace singlebiomemap {

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

struct RGB {
    unsigned char r;
    unsigned char g;
    unsigned char b;
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

RGB biomeColor(unsigned char id) {
    switch (id) {
        case RAINFOREST:      return {38, 112, 54};
        case SWAMPLAND:       return {78, 96, 66};
        case SEASONAL_FOREST: return {94, 143, 64};
        case FOREST:          return {65, 125, 55};
        case SAVANNA:         return {190, 173, 89};
        case SHRUBLAND:       return {137, 147, 91};
        case TAIGA:           return {76, 116, 111};
        case DESERT:          return {224, 201, 128};
        case PLAINS:          return {132, 172, 86};
        case TUNDRA:          return {185, 213, 220};
        default:              return {255, 0, 255};
    }
}

struct ClimateState {
    p20::PerlinState temp[4];
    p20::PerlinState rain[4];
    p20::PerlinState blend[2];
};

struct Options {
    std::int64_t seed = 0;
    bool seedSpecified = false;
    int centerX = 0;
    int centerZ = 0;
    int target = 432;
    int margin = 0;
    int scale = 3;
    std::string output;
};

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
    if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        throw std::runtime_error(std::string("Out of range ") + name + ": " + value);
    return static_cast<int>(parsed);
}

Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("Missing value after ") + flag);
            return std::string(argv[++i]);
        };
        if (arg == "--seed") { o.seed = parseI64(need("--seed"), "seed"); o.seedSpecified = true; }
        else if (arg == "--center-x") o.centerX = parseInt(need("--center-x"), "center-x");
        else if (arg == "--center-z") o.centerZ = parseInt(need("--center-z"), "center-z");
        else if (arg == "--target") o.target = parseInt(need("--target"), "target");
        else if (arg == "--margin") o.margin = parseInt(need("--margin"), "margin");
        else if (arg == "--scale") o.scale = parseInt(need("--scale"), "scale");
        else if (arg == "--output") o.output = need("--output");
        else throw std::runtime_error("Unknown option: " + arg);
    }
    if (!o.seedSpecified) throw std::runtime_error("--seed is required");
    if (o.target <= 0 || o.target > 20000) throw std::runtime_error("--target must be 1..20000");
    if (o.margin < 0 || o.margin > 20000) throw std::runtime_error("--margin must be 0..20000");
    if (o.scale <= 0 || o.scale > 32) throw std::runtime_error("--scale must be 1..32");
    if (o.output.empty()) {
        o.output = "single_biome_map_" + std::to_string(o.seed) + "_864.bmp";
    }
    return o;
}

std::int64_t multipliedSeed(std::int64_t seed, std::uint64_t multiplier) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(seed) * multiplier);
}

void initClimate(ClimateState& s, std::int64_t seed) {
    p20::JavaRandom rng;
    rng.setSeed(multipliedSeed(seed, 9871ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, s.temp[i]);
    rng.setSeed(multipliedSeed(seed, 39811ULL));
    for (int i = 0; i < 4; ++i) p20::initPerlin(rng, s.rain[i]);
    rng.setSeed(multipliedSeed(seed, 543321ULL));
    for (int i = 0; i < 2; ++i) p20::initPerlin(rng, s.blend[i]);
}

double octaveNoise4(const p20::PerlinState states[4], double x, double z, double baseScale, double octaveScale) {
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

double octaveNoise2(const p20::PerlinState states[2], double x, double z) {
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

double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

unsigned char classifyQuantizedBiome(double temperature, double rain) {
    int ti = static_cast<int>(temperature * 63.0);
    int ri = static_cast<int>(rain * 63.0);
    ti = std::max(0, std::min(63, ti));
    ri = std::max(0, std::min(63, ri));

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

unsigned char biomeAt(const ClimateState& s, int blockX, int blockZ) {
    const double x = static_cast<double>(blockX);
    const double z = static_cast<double>(blockZ);
    const double tempRaw = octaveNoise4(s.temp, x, z, 0.02500000037252903, 0.25);
    const double rainRaw = octaveNoise4(s.rain, x, z, 0.05000000074505806, 0.3333333333333333);
    const double blendRaw = octaveNoise2(s.blend, x, z);
    const double d0 = blendRaw * 1.1 + 0.5;
    double temperature = (tempRaw * 0.15 + 0.7) * 0.99 + d0 * 0.01;
    double rain = (rainRaw * 0.15 + 0.5) * 0.998 + d0 * 0.002;
    temperature = 1.0 - (1.0 - temperature) * (1.0 - temperature);
    return classifyQuantizedBiome(clamp01(temperature), clamp01(rain));
}

void writeU16(std::ofstream& out, std::uint16_t v) {
    out.put(static_cast<char>(v));
    out.put(static_cast<char>(v >> 8));
}

void writeU32(std::ofstream& out, std::uint32_t v) {
    out.put(static_cast<char>(v));
    out.put(static_cast<char>(v >> 8));
    out.put(static_cast<char>(v >> 16));
    out.put(static_cast<char>(v >> 24));
}

int run(const Options& o) {
    ClimateState climate{};
    initClimate(climate, o.seed);

    // Exact finder target: target=432 -> offsets [-432,+431] -> 864x864.
    // Optional margin only adds visual context around that exact square.
    const int targetSide = o.target * 2;
    const int viewHalf = o.target + o.margin;
    const int width = targetSide + o.margin * 2;
    const int height = width;
    const int minDx = -viewHalf;
    const int maxDz = viewHalf - 1;

    std::vector<unsigned char> biomes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::uint64_t tundraCount = 0;
    std::uint64_t targetCount = 0;
    std::vector<std::pair<int,int>> firstWrong;

    for (int py = 0; py < height; ++py) {
        const int dz = maxDz - py;
        for (int px = 0; px < width; ++px) {
            const int dx = minDx + px;
            const unsigned char biome = biomeAt(climate, o.centerX + dx, o.centerZ + dz);
            biomes[static_cast<std::size_t>(py) * width + px] = biome;

            const bool inTarget = dx >= -o.target && dx < o.target && dz >= -o.target && dz < o.target;
            if (inTarget) {
                ++targetCount;
                if (biome == TUNDRA) ++tundraCount;
                else if (firstWrong.size() < 16U) firstWrong.emplace_back(dx, dz);
            }
        }
    }

    const int sw = width * o.scale;
    const int sh = height * o.scale;
    std::vector<unsigned char> image(static_cast<std::size_t>(sw) * static_cast<std::size_t>(sh) * 3U);
    const RGB edge = {35, 43, 46};
    const RGB targetFrame = {245, 245, 245};

    auto biomeAtPixel = [&](int x, int y) -> unsigned char {
        return biomes[static_cast<std::size_t>(y) * width + x];
    };

    auto setPixel = [&](int x, int y, RGB c) {
        const std::size_t idx = (static_cast<std::size_t>(y) * sw + x) * 3U;
        image[idx + 0] = c.r;
        image[idx + 1] = c.g;
        image[idx + 2] = c.b;
    };

    // Fill each block with its biome color. At scale >= 2, draw a one-pixel
    // outline only on sides where the neighboring block is a different biome.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned char biome = biomeAtPixel(x, y);
            const RGB fill = biomeColor(biome);
            const bool topEdge = y > 0 && biomeAtPixel(x, y - 1) != biome;
            const bool bottomEdge = y + 1 < height && biomeAtPixel(x, y + 1) != biome;
            const bool leftEdge = x > 0 && biomeAtPixel(x - 1, y) != biome;
            const bool rightEdge = x + 1 < width && biomeAtPixel(x + 1, y) != biome;

            for (int sy = 0; sy < o.scale; ++sy) {
                for (int sx = 0; sx < o.scale; ++sx) {
                    RGB c = fill;
                    if (o.scale >= 2 &&
                        ((topEdge && sy == 0) || (bottomEdge && sy == o.scale - 1) ||
                         (leftEdge && sx == 0) || (rightEdge && sx == o.scale - 1))) {
                        c = edge;
                    }
                    setPixel(x * o.scale + sx, y * o.scale + sy, c);
                }
            }
        }
    }

    // If a visual margin is requested, outline the exact 864x864 search square.
    if (o.margin > 0) {
        const int left = o.margin * o.scale;
        const int top = o.margin * o.scale;
        const int right = (o.margin + targetSide) * o.scale - 1;
        const int bottom = right;
        for (int x = left; x <= right; ++x) {
            setPixel(x, top, targetFrame);
            setPixel(x, bottom, targetFrame);
        }
        for (int y = top; y <= bottom; ++y) {
            setPixel(left, y, targetFrame);
            setPixel(right, y, targetFrame);
        }
    }

    // White center marker at world offset (0,0).
    const int centerPx = (o.margin + o.target) * o.scale;
    const int centerPy = (o.margin + o.target - 1) * o.scale;
    const int markerRadius = std::max(2, o.scale * 2);
    for (int d = -markerRadius; d <= markerRadius; ++d) {
        const int x = centerPx + d;
        const int y = centerPy + d;
        if (x >= 0 && x < sw && centerPy >= 0 && centerPy < sh) setPixel(x, centerPy, targetFrame);
        if (centerPx >= 0 && centerPx < sw && y >= 0 && y < sh) setPixel(centerPx, y, targetFrame);
    }

    const int rowBytes = sw * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const std::uint32_t pixelBytes = static_cast<std::uint32_t>((rowBytes + padding) * sh);
    std::ofstream out(o.output, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open output: " + o.output);

    out.put('B'); out.put('M');
    writeU32(out, 54U + pixelBytes);
    writeU16(out, 0); writeU16(out, 0); writeU32(out, 54);
    writeU32(out, 40); writeU32(out, static_cast<std::uint32_t>(sw)); writeU32(out, static_cast<std::uint32_t>(sh));
    writeU16(out, 1); writeU16(out, 24); writeU32(out, 0); writeU32(out, pixelBytes);
    writeU32(out, 2835); writeU32(out, 2835); writeU32(out, 0); writeU32(out, 0);

    for (int srcY = sh - 1; srcY >= 0; --srcY) {
        for (int x = 0; x < sw; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(srcY) * sw + x) * 3U;
            out.put(static_cast<char>(image[idx + 2]));
            out.put(static_cast<char>(image[idx + 1]));
            out.put(static_cast<char>(image[idx + 0]));
        }
        for (int p = 0; p < padding; ++p) out.put(0);
    }
    out.close();

    const std::uint64_t wrongCount = targetCount - tundraCount;
    const double tundraCoverage = targetCount == 0 ? 0.0 :
        100.0 * static_cast<double>(tundraCount) / static_cast<double>(targetCount);

    std::cout << "SingleBiomeMap | exact 864x864 target\n"
              << "seed=" << o.seed
              << " center=(" << o.centerX << ',' << o.centerZ << ")"
              << " targetOffsets=[-" << o.target << ",+" << (o.target - 1) << "] on X/Z\n"
              << "targetSize=" << targetSide << 'x' << targetSide
              << " targetBlocks=" << targetCount << '\n'
              << "tundraCoverage=" << std::fixed << std::setprecision(6) << tundraCoverage << "%"
              << " tundraBlocks=" << tundraCount << '/' << targetCount
              << " wrongBlocks=" << wrongCount << '\n';

    if (!firstWrong.empty()) {
        std::cout << "firstWrongOffsets=";
        for (std::size_t i = 0; i < firstWrong.size(); ++i) {
            if (i != 0) std::cout << ' ';
            std::cout << '(' << firstWrong[i].first << ',' << firstWrong[i].second << ')';
        }
        if (wrongCount > firstWrong.size()) std::cout << " ...";
        std::cout << '\n';
    }

    std::cout << "legend: TUNDRA=icy blue, TAIGA=blue-green, FORESTS=green, SAVANNA/PLAINS=yellow-green, DESERT=sand, SWAMP=olive\n"
              << "saved=" << o.output << '\n';
    return 0;
}

} // namespace singlebiomemap

int main(int argc, char** argv) {
    try {
        return singlebiomemap::run(singlebiomemap::parseOptions(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
