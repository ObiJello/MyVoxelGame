/**
 * BiomeSearchTool - fast biome-coverage search over seeds/regions using only
 * the climate sampler (no chunk generation). Used by coverage_report.py to
 * find regions containing biomes not yet covered by any parity golden.
 *
 * Usage:
 *   biome_search --list-possible
 *       Print every biome the overworld source can produce (coverage target).
 *   biome_search --find biome1,biome2,... [--seed S] [--range CHUNKS]
 *                [--step QUARTS] [--max-hits N]
 *       Scan quart positions out to +-range chunks from origin at several
 *       Y levels (surface + cave depths) and print the first N hits per
 *       requested biome:  HIT,<biome>,<seed>,<chunkX>,<chunkZ>,<qx>,<qy>,<qz>
 *
 * Setup mirrors BiomeDumpTest.cpp (direct sampler, no NoiseChunk caching).
 */
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biomes.h"
#include "core/QuartPos.h"

namespace mc = minecraft;

namespace {

struct Sampling {
    mc::levelgen::RandomState* randomState;
    std::unique_ptr<mc::world::biome::MultiNoiseBiomeSource> biomeSource;
};

Sampling makeSampler(int64_t seed) {
    mc::levelgen::DensityFunctionRegistry::bootstrap(seed);
    auto* stoneBlock = mc::world::level::block::Blocks::STONE->defaultBlockState();
    auto* router = mc::levelgen::NoiseRouterData::overworld(false, false);
    auto noiseSettings = mc::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    auto* settings = new mc::levelgen::NoiseGeneratorSettings(
        noiseSettings, stoneBlock,
        mc::world::level::block::Blocks::WATER->defaultBlockState(),
        *router, nullptr, {}, 63, false, true, true, false);
    Sampling s;
    s.randomState = mc::levelgen::RandomState::create(settings, seed);
    s.biomeSource = mc::world::biome::MultiNoiseBiomeSource::createOverworld();
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    bool listPossible = false;
    std::string findList;
    int64_t seed = 12345;
    int rangeChunks = 2048;
    int stepQuarts = 8;
    int maxHits = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--list-possible") {
            listPossible = true;
        } else if (arg == "--find" && i + 1 < argc) {
            findList = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = std::atoll(argv[++i]);
        } else if (arg == "--range" && i + 1 < argc) {
            rangeChunks = std::atoi(argv[++i]);
        } else if (arg == "--step" && i + 1 < argc) {
            stepQuarts = std::atoi(argv[++i]);
        } else if (arg == "--max-hits" && i + 1 < argc) {
            maxHits = std::atoi(argv[++i]);
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 2;
        }
    }

    mc::world::level::block::Blocks::bootstrap();
    mc::levelgen::NoiseRegistry::bootstrap();

    if (listPossible) {
        auto source = mc::world::biome::MultiNoiseBiomeSource::createOverworld();
        for (const auto& key : source->possibleBiomes()) {
            std::cout << key << "\n";
        }
        return 0;
    }

    if (findList.empty()) {
        std::cerr << "Usage: biome_search --list-possible | --find b1,b2,... "
                     "[--seed S] [--range CHUNKS] [--step QUARTS] [--max-hits N]\n";
        return 2;
    }

    std::map<std::string, int> wanted;  // biome -> hits still needed
    {
        std::stringstream ss(findList);
        std::string b;
        while (std::getline(ss, b, ',')) {
            if (b.rfind("minecraft:", 0) != 0) b = "minecraft:" + b;
            wanted[b] = maxHits;
        }
    }

    Sampling s = makeSampler(seed);
    auto* sampler = s.randomState->sampler();

    // Y levels: surface plus cave-biome depths (deep_dark sits ~y=-50,
    // lush/dripstone caves mid-depth). Quart Y = blockY >> 2.
    const int yQuarts[] = {16, 5, -2, -13};

    const int rangeQuarts = rangeChunks * 4;
    long long sampled = 0;
    int remaining = static_cast<int>(wanted.size());

    // Expanding square rings so near-origin hits (cheap Java verification
    // runs) are found first.
    for (int ring = 0; ring <= rangeQuarts && remaining > 0; ring += stepQuarts) {
        for (int qx = -ring; qx <= ring && remaining > 0; qx += stepQuarts) {
            for (int qz = -ring; qz <= ring && remaining > 0; qz += stepQuarts) {
                // ring shell only (skip interior already scanned)
                if (std::max(std::abs(qx), std::abs(qz)) + stepQuarts <= ring) continue;
                for (int qy : yQuarts) {
                    auto key = s.biomeSource->getNoiseBiome(qx, qy, qz, *sampler);
                    sampled++;
                    auto it = wanted.find(key);
                    if (it != wanted.end() && it->second > 0) {
                        std::cout << "HIT," << key << "," << seed << ","
                                  << (mc::core::QuartPos::toBlock(qx) >> 4) << ","
                                  << (mc::core::QuartPos::toBlock(qz) >> 4) << ","
                                  << qx << "," << qy << "," << qz << std::endl;
                        if (--it->second == 0 && --remaining == 0) break;
                    }
                }
            }
        }
    }

    for (const auto& [biome, left] : wanted) {
        if (left > 0) {
            std::cout << "MISS," << biome << "," << seed << " (after "
                      << sampled << " samples, range " << rangeChunks
                      << " chunks)" << std::endl;
        }
    }
    return remaining == 0 ? 0 : 1;
}
