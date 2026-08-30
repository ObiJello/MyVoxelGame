// Quick test to output specific values for comparison with Java
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <tuple>
#include <vector>
#include <cmath>
#include <algorithm>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "world/level/block/Blocks.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "util/LinearCongruentialGenerator.h"
#include "util/SHA256.h"

using namespace minecraft;
namespace mwb = minecraft::world::biome;

int main() {
    std::cout << std::fixed << std::setprecision(17);

    int64_t SEED = 12345L;

    // Test 1: SHA256 hash of seed (obfuscated seed)
    int64_t obfuscatedSeed = mwb::BiomeManager::obfuscateSeed(SEED);
    std::cout << "=== Test 1: Obfuscated Seed ===" << std::endl;
    std::cout << "Input seed: " << SEED << std::endl;
    std::cout << "Obfuscated: " << obfuscatedSeed << std::endl;
    std::cout << std::endl;

    // Test 2: LinearCongruentialGenerator
    std::cout << "=== Test 2: LCG Values ===" << std::endl;
    int64_t lcg1 = util::LinearCongruentialGenerator::next(obfuscatedSeed, 100);
    int64_t lcg2 = util::LinearCongruentialGenerator::next(lcg1, 50);
    int64_t lcg3 = util::LinearCongruentialGenerator::next(lcg2, 25);
    std::cout << "LCG(obfSeed, 100): " << lcg1 << std::endl;
    std::cout << "LCG(prev, 50): " << lcg2 << std::endl;
    std::cout << "LCG(prev, 25): " << lcg3 << std::endl;
    std::cout << std::endl;

    // Test 3: getFiddle calculation
    std::cout << "=== Test 3: getFiddle Values ===" << std::endl;
    auto getFiddle = [](int64_t rval) -> double {
        double uniform = static_cast<double>(((rval >> 24) % 1024 + 1024) % 1024) / 1024.0;
        return (uniform - 0.5) * 0.9;
    };

    // Test with specific rval values
    int64_t testRvals[] = {lcg1, lcg2, lcg3, -1000000000LL, 1000000000LL, 0LL};
    for (int64_t rv : testRvals) {
        double fiddle = getFiddle(rv);
        int64_t shifted = rv >> 24;
        int64_t modResult = ((shifted % 1024) + 1024) % 1024;
        std::cout << "rval=" << rv << " >> 24=" << shifted
                  << " mod1024=" << modResult << " fiddle=" << fiddle << std::endl;
    }
    std::cout << std::endl;

    // Test 4: Bootstrap and check SURFACE noise
    std::cout << "=== Test 4: SURFACE Noise Values ===" << std::endl;
    levelgen::NoiseRegistry::bootstrap();
    levelgen::DensityFunctionRegistry::bootstrap(SEED);

    levelgen::NoiseRouter* router = levelgen::NoiseRouterData::overworld(false, false);
    levelgen::NoiseSettings noiseSettings = levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    levelgen::NoiseGeneratorSettings* settings = new levelgen::NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    levelgen::RandomState* randomState = levelgen::RandomState::create(settings, SEED);
    NormalNoise* surfaceNoise = randomState->getOrCreateNoise("minecraft:surface");

    // Test EXACT positions where blocks differ between Java and C++
    // These are from chunk (41, -11) where Java=grass_block, C++=podzol
    // World coords: chunkX*16+localX, Y, chunkZ*16+localZ
    std::cout << "=== Test 4: SURFACE Noise at KNOWN DIFFERING positions ===" << std::endl;
    double threshold = -0.95 / 8.25;  // -0.11515151515151514
    std::cout << "Podzol threshold: " << threshold << std::endl;
    std::cout << std::endl;

    // Positions where Java=grass_block but C++=podzol (from diff output)
    // Local (10, 94, 12-15), (11, 94, 12-15), (12, 94, 12-15), etc.
    // World: (666, 94, -164 to -161)
    int positions[][2] = {
        {666, -164}, {666, -163}, {666, -162}, {666, -161},  // local (10, 94, 12-15)
        {667, -164}, {667, -163}, {667, -162}, {667, -161},  // local (11, 94, 12-15)
        {668, -164}, {668, -163}, {668, -162}, {668, -161},  // local (12, 94, 12-15)
        {664, -163}, {665, -163}, {665, -162}, {665, -161}   // other differing positions
    };
    for (auto& pos : positions) {
        double noiseVal = surfaceNoise->getValue(static_cast<double>(pos[0]), 0.0, static_cast<double>(pos[1]));
        double threshold = -0.95 / 8.25;
        std::cout << "SURFACE noise at (" << pos[0] << ", 0, " << pos[1] << "): "
                  << noiseVal << " (threshold=" << threshold << ", above=" << (noiseVal >= threshold ? "yes" : "no") << ")" << std::endl;
    }
    std::cout << std::endl;

    // Test 5: Climate sampling at specific quart positions
    std::cout << "=== Test 5: Climate Values at Quart Positions ===" << std::endl;
    mwb::Climate::Sampler* sampler = randomState->sampler();

    // More quart positions for chunk (41, -11) at various Y levels
    int quartPositions[][3] = {
        {164, 20, -44}, {164, 21, -44}, {164, 22, -44}, {164, 23, -44},
        {165, 20, -44}, {165, 21, -44}, {165, 22, -44}, {165, 23, -44},
        {166, 20, -44}, {166, 21, -44}, {166, 22, -44}, {166, 23, -44},
        {166, 20, -45}, {166, 21, -45}, {166, 22, -45}, {166, 23, -45},
        {167, 20, -44}, {167, 21, -44}, {167, 22, -44}, {167, 23, -44}
    };
    for (auto& qpos : quartPositions) {
        mwb::Climate::TargetPoint target = sampler->sample(qpos[0], qpos[1], qpos[2]);
        std::cout << "Climate at quart (" << qpos[0] << "," << qpos[1] << "," << qpos[2] << "):" << std::endl;
        std::cout << "  temperature=" << target.temperature << std::endl;
        std::cout << "  humidity=" << target.humidity << std::endl;
        std::cout << "  continentalness=" << target.continentalness << std::endl;
        std::cout << "  erosion=" << target.erosion << std::endl;
        std::cout << "  depth=" << target.depth << std::endl;
        std::cout << "  weirdness=" << target.weirdness << std::endl;
    }
    std::cout << std::endl;

    // Test 6: Biome selection
    std::cout << "=== Test 6: Biome Selection ===" << std::endl;
    std::unique_ptr<mwb::MultiNoiseBiomeSource> biomeSource =
        mwb::MultiNoiseBiomeSource::createOverworld();

    for (auto& qpos : quartPositions) {
        mwb::BiomeKey biome = biomeSource->getNoiseBiome(qpos[0], qpos[1], qpos[2], *sampler);
        std::cout << "Biome at quart (" << qpos[0] << "," << qpos[1] << "," << qpos[2] << "): " << biome << std::endl;
    }

    // Test 7: BiomeManager.getBiome with fiddling (what surface rules see)
    std::cout << "\n=== Test 7: BiomeManager.getBiome (with fiddling) ===" << std::endl;

    // Create a mock NoiseBiomeSource that returns stored biomes
    // For testing, we'll use biomeSource directly
    class TestNoiseBiomeSource : public mwb::BiomeManager::NoiseBiomeSource {
    public:
        mwb::MultiNoiseBiomeSource* source;
        mwb::Climate::Sampler* sampler;

        TestNoiseBiomeSource(mwb::MultiNoiseBiomeSource* s, mwb::Climate::Sampler* sam)
            : source(s), sampler(sam) {}

        mwb::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
            mwb::BiomeKey key = source->getNoiseBiome(quartX, quartY, quartZ, *sampler);
            return mwb::Biomes::get(key);
        }
    };

    TestNoiseBiomeSource testSource(biomeSource.get(), sampler);
    mwb::BiomeManager biomeManager(&testSource, obfuscatedSeed);

    // Test block positions at EXACT Y=94 where blocks differ in chunk (41, -11)
    // These are world coords (666, 94, -164) etc where Java=grass, C++=podzol
    int blockPositions[][3] = {
        {666, 94, -164}, {666, 94, -163}, {666, 94, -162}, {666, 94, -161},
        {667, 94, -164}, {667, 94, -163}, {667, 94, -162}, {667, 94, -161},
        {668, 94, -164}, {668, 94, -163}, {668, 94, -162}, {668, 94, -161}
    };

    for (auto& bpos : blockPositions) {
        core::BlockPos pos(bpos[0], bpos[1], bpos[2]);
        mwb::BiomeHolder biome = biomeManager.getBiome(pos);
        std::cout << "BiomeManager.getBiome(" << bpos[0] << "," << bpos[1] << "," << bpos[2] << "): "
                  << (biome ? biome->getName() : "null") << std::endl;
    }

    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
