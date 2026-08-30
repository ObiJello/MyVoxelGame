/**
 * C++ diagnostic to compare with Java podzol differences
 * Tests chunk (41, -11) which has grass_block -17, podzol +17 difference
 */

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <memory>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/Noises.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/Biomes.h"
#include "world/biome/Climate.h"
#include "core/QuartPos.h"
#include "util/LinearCongruentialGenerator.h"

using namespace minecraft;
using namespace minecraft::world::biome;
using namespace minecraft::levelgen;
using namespace minecraft::core;

static const int64_t SEED = 12345L;
static const int CHUNK_X = 41;
static const int CHUNK_Z = -11;

int main() {
    std::cout << "=== C++ PODZOL DIAGNOSTIC ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;
    std::cout << "Chunk: (" << CHUNK_X << ", " << CHUNK_Z << ")" << std::endl;

    // Bootstrap registries
    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);

    // Create settings and RandomState
    NoiseRouter* router = NoiseRouterData::overworld(false, false);
    NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    RandomState* randomState = RandomState::create(settings, SEED);

    // Create biome source
    std::unique_ptr<MultiNoiseBiomeSource> biomeSource = MultiNoiseBiomeSource::createOverworld();

    // Get SURFACE noise
    NormalNoise* surfaceNoise = randomState->getOrCreateNoise("minecraft:surface");

    // Get climate sampler
    const Climate::Sampler& sampler = randomState->sampler();

    // Obfuscated seed for BiomeManager fiddling
    int64_t obfuscatedSeed = BiomeManager::obfuscateSeed(SEED);

    std::cout << "\nObfuscated seed: " << obfuscatedSeed << std::endl;
    std::cout << "\n--- Testing positions in chunk ---" << std::endl;
    std::cout << "Format: (x,y,z) | directBiome | fiddledBiome | surfaceNoise | podzolThreshold" << std::endl;

    int baseX = CHUNK_X * 16;
    int baseZ = CHUNK_Z * 16;

    // Test several Y levels
    for (int y : {90, 80, 70, 64}) {
        std::cout << "\n-- Y=" << y << " --" << std::endl;

        for (int lx = 0; lx < 16; lx += 4) {
            for (int lz = 0; lz < 16; lz += 4) {
                int blockX = baseX + lx;
                int blockZ = baseZ + lz;

                // Direct biome lookup (no fiddling) - quart coords
                int quartX = QuartPos::fromBlock(blockX);
                int quartY = QuartPos::fromBlock(y);
                int quartZ = QuartPos::fromBlock(blockZ);
                BiomeKey directBiome = biomeSource->getNoiseBiome(quartX, quartY, quartZ, sampler);

                // Fiddled biome lookup (what BiomeManager.getBiome returns)
                int32_t absX = blockX - 2;
                int32_t absY = y - 2;
                int32_t absZ = blockZ - 2;
                int32_t parentX = absX >> 2;
                int32_t parentY = absY >> 2;
                int32_t parentZ = absZ >> 2;

                double fractX = static_cast<double>(absX & 3) / 4.0;
                double fractY = static_cast<double>(absY & 3) / 4.0;
                double fractZ = static_cast<double>(absZ & 3) / 4.0;

                int minI = 0;
                double minDist = std::numeric_limits<double>::infinity();

                for (int i = 0; i < 8; i++) {
                    bool xEven = (i & 4) == 0;
                    bool yEven = (i & 2) == 0;
                    bool zEven = (i & 1) == 0;

                    int32_t cx = xEven ? parentX : parentX + 1;
                    int32_t cy = yEven ? parentY : parentY + 1;
                    int32_t cz = zEven ? parentZ : parentZ + 1;

                    double dx = xEven ? fractX : fractX - 1.0;
                    double dy = yEven ? fractY : fractY - 1.0;
                    double dz = zEven ? fractZ : fractZ - 1.0;

                    // Compute fiddled distance
                    int64_t rval = util::LinearCongruentialGenerator::next(obfuscatedSeed, static_cast<int64_t>(cx));
                    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(cy));
                    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(cz));
                    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(cx));
                    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(cy));
                    rval = util::LinearCongruentialGenerator::next(rval, static_cast<int64_t>(cz));

                    auto getFiddle = [](int64_t rv) -> double {
                        double uniform = static_cast<double>(((rv >> 24) % 1024 + 1024) % 1024) / 1024.0;
                        return (uniform - 0.5) * 0.9;
                    };

                    double fx = getFiddle(rval);
                    rval = util::LinearCongruentialGenerator::next(rval, obfuscatedSeed);
                    double fy = getFiddle(rval);
                    rval = util::LinearCongruentialGenerator::next(rval, obfuscatedSeed);
                    double fz = getFiddle(rval);

                    double dist = (dz + fz) * (dz + fz) + (dy + fy) * (dy + fy) + (dx + fx) * (dx + fx);

                    if (dist < minDist) {
                        minDist = dist;
                        minI = i;
                    }
                }

                int32_t biomeX = (minI & 4) == 0 ? parentX : parentX + 1;
                int32_t biomeY = (minI & 2) == 0 ? parentY : parentY + 1;
                int32_t biomeZ = (minI & 1) == 0 ? parentZ : parentZ + 1;

                BiomeKey fiddledBiome = biomeSource->getNoiseBiome(biomeX, biomeY, biomeZ, sampler);

                // SURFACE noise value
                double noiseVal = surfaceNoise->getValue(static_cast<double>(blockX), 0.0, static_cast<double>(blockZ));
                double threshold = -0.95 / 8.25;  // podzol threshold
                bool aboveThreshold = noiseVal >= threshold;

                bool isOldGrowthDirect = (directBiome == BiomeKeys::OLD_GROWTH_PINE_TAIGA ||
                                          directBiome == BiomeKeys::OLD_GROWTH_SPRUCE_TAIGA);
                bool isOldGrowthFiddled = (fiddledBiome == BiomeKeys::OLD_GROWTH_PINE_TAIGA ||
                                           fiddledBiome == BiomeKeys::OLD_GROWTH_SPRUCE_TAIGA);

                // Shorten biome names for output
                std::string directShort = directBiome;
                std::string fiddledShort = fiddledBiome;
                if (directShort.find("minecraft:") == 0) directShort = directShort.substr(10);
                if (fiddledShort.find("minecraft:") == 0) fiddledShort = fiddledShort.substr(10);

                // Only print if there's a discrepancy or it's old_growth
                if (isOldGrowthDirect || isOldGrowthFiddled || directBiome != fiddledBiome) {
                    std::cout << std::fixed << std::setprecision(6);
                    std::cout << "(" << blockX << "," << y << "," << blockZ << ") | "
                              << directShort << " | " << fiddledShort << " | "
                              << noiseVal << " | "
                              << (aboveThreshold ? "PODZOL" : "grass") << std::endl;
                }
            }
        }
    }

    std::cout << "\n=== Done ===" << std::endl;

    return 0;
}
