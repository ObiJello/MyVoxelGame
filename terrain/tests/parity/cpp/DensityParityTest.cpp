// Density engine parity, C++ side of java/DensityParityTest.java: every
// noise_settings router (and aquifer) function decoded from the datapack JSON
// (data/minecraft/worldgen) through WorldgenRegistries, sampled as uncached
// points, cached chunk and quart volumes, and cached points answered from
// those volumes. Writes the same text + binary stream as the Java test;
// compare_density_parity.py diffs the two.
//
// Usage: density_parity_test <seed> <out.txt> <out.bin>   (run from the repo,
// or with MC_DATA_ROOT pointing at data/)

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensityFunctionCompiler.h"
#include "levelgen/density/DensityVolume.h"
#include "levelgen/density/SamplerContext.h"
#include "levelgen/density/WorldgenRegistries.h"
#include "levelgen/density/terrain/RandomState.h"
#include "levelgen/density/terrain/TerrainSettings.h"
#include "world/level/block/Blocks.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace minecraft::levelgen::density;

namespace {

const char* const kSettings[] = {"overworld", "large_biomes", "amplified", "nether", "end", "caves", "floating_islands"};
const int kChunks[][2] = {{0, 0}, {-7, 12}, {123, -456}, {-3000, 2500}};

int floorMod(int a, int b) {
    const int m = a % b;
    return m < 0 ? m + b : m;
}
int pointX(int i) { return static_cast<int>((i * 2654435761LL) % 60001LL) - 30000; }
int pointZ(int i) { return static_cast<int>((i * 40503LL + 17LL) % 60001LL) - 30000; }

std::string hex(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%x", bits);
    return buffer;
}

void writeBigEndian(std::ofstream& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    const unsigned char bytes[4] = {static_cast<unsigned char>(bits >> 24), static_cast<unsigned char>(bits >> 16),
                                    static_cast<unsigned char>(bits >> 8), static_cast<unsigned char>(bits)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: density_parity_test <seed> <out.txt> <out.bin>\n";
        return 2;
    }
    const int64_t seed = std::stoll(argv[1]);
    minecraft::world::level::block::Blocks::bootstrap();
    WorldgenRegistries& registries = WorldgenRegistries::get();

    std::ofstream text(argv[2]);
    std::ofstream bin(argv[3], std::ios::binary);
    try {
        for (const char* name : kSettings) {
            std::shared_ptr<const TerrainSettings> settings = TerrainSettings::load(name, registries);
            RandomState randomState(seed, settings->useLegacyRandomSource, registries);
            std::vector<std::string> labels;
            std::vector<DensityFunctionPtr> functions;
            const NoiseRouter& r = settings->noiseRouter;
            labels = {"temperature", "vegetation", "continents", "erosion", "depth", "ridges", "chunk_surface_level",
                      "final_density"};
            functions = {r.temperature, r.vegetation, r.continents, r.erosion, r.depth, r.ridges, r.chunkSurfaceLevel,
                         r.finalDensity};
            if (settings->aquifers) {
                const Aquifer::Config& a = *settings->aquifers;
                for (const char* label : {"aq_barrier", "aq_floodedness", "aq_spread", "aq_lava", "aq_exclusion",
                                          "aq_surface_level"}) {
                    labels.emplace_back(label);
                }
                for (const DensityFunctionPtr& f : {a.barrierNoise, a.fluidLevelFloodednessNoise, a.fluidLevelSpreadNoise,
                                                    a.lavaNoise, a.exclusion, a.surfaceLevel}) {
                    functions.push_back(f);
                }
            }
            const int minY = settings->noiseSettings.minY;
            const int height = settings->noiseSettings.height;

            for (size_t f = 0; f < functions.size(); ++f) {
                for (int i = 0; i < 64; ++i) {
                    const int x = pointX(i), y = minY + floorMod(i * 37, height), z = pointZ(i);
                    const float v = randomState.sampleBlockValueUncached(functions[f], x, y, z);
                    text << "P " << name << ' ' << labels[f] << ' ' << x << ' ' << y << ' ' << z << ' ' << hex(v) << '\n';
                }
            }
            for (const auto& c : kChunks) {
                std::unique_ptr<DensityBufferPool> pool = randomState.acquireDensityBufferPool();
                {
                    SamplerContext sc = SamplerContext::builder().useBufferArena(*pool).enableCaches().build();
                    DensitySamplerSet samplers = randomState.samplersWithContext(sc);
                    const DensityVolume chunk(16, height, 16, c[0] * 16, minY, c[1] * 16);
                    const DensityVolume quart(4, height / 4, 4, c[0] * 16, minY, c[1] * 16, 4, 4, 4);
                    for (size_t f = 0; f < functions.size(); ++f) {
                        for (const DensityVolume* v : {&chunk, &quart}) {
                            ScopedBuffer buffer = samplers.get(functions[f]).sampleVolume(*v);
                            text << "V " << name << ' ' << labels[f] << ' ' << c[0] << ' ' << c[1] << ' ' << v->sizeX
                                 << 'x' << v->sizeY << 'x' << v->sizeZ << ' ' << v->size() << '\n';
                            for (int i = 0; i < v->size(); ++i) writeBigEndian(bin, buffer->get(i));
                        }
                        for (int i = 0; i < 16; ++i) {
                            const int x = c[0] * 16 + (i * 5) % 16, y = minY + floorMod(i * 53, height),
                                      z = c[1] * 16 + (i * 11) % 16;
                            const float v = samplers.sampleValue(functions[f], x, y, z);
                            text << "C " << name << ' ' << labels[f] << ' ' << x << ' ' << y << ' ' << z << ' ' << hex(v)
                                 << '\n';
                        }
                    }
                }
                randomState.releaseDensityBufferPool(std::move(pool));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "density_parity_test: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
