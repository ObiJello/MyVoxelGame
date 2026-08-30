/**
 * Test BiomeManager.getBiome() vs direct BiomeSource.getNoiseBiome()
 * The actual surface building uses BiomeManager which applies "fiddling"
 * for smooth biome transitions. This could cause different biomes at boundaries.
 */

#include <iostream>
#include <iomanip>

#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"
#include "world/biome/BiomeManager.h"
#include "world/biome/Biomes.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Climate.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "core/BlockPos.h"
#include "core/QuartPos.h"

using namespace minecraft;
using namespace minecraft::levelgen;
using namespace minecraft::world;
using namespace minecraft::world::biome;

static const int64_t SEED = 12345L;

int main() {
    std::cout << "=== BiomeManager vs BiomeSource Comparison ===" << std::endl;
    std::cout << "Seed: " << SEED << std::endl;

    // Initialize registries
    NoiseRegistry::bootstrap();
    DensityFunctionRegistry::bootstrap(SEED);
    SurfaceRuleData::initialize();

    // Create RandomState
    NoiseRouter* router = NoiseRouterData::overworld(false, false);
    NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    NoiseGeneratorSettings* settings = new NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    RandomState* randomState = RandomState::create(settings, SEED);
    Climate::Sampler* sampler = randomState->sampler();

    // Create biome source and biome manager
    MultiNoiseBiomeSource biomeSource(MultiNoiseBiomeSource::Preset::OVERWORLD);

    // BiomeManager uses obfuscated seed
    int64_t obfuscatedSeed = BiomeManager::obfuscateSeed(SEED);
    std::cout << "Obfuscated seed: " << obfuscatedSeed << std::endl;

    BiomeManager biomeManager(&biomeSource, obfuscatedSeed);

    // Test positions from the mismatch analysis
    struct TestPos { int x, y, z; const char* name; };
    TestPos positions[] = {
        {720, 48, 430, "Position 1 (C++ packed_ice, Java water)"},
        {728, 48, 426, "Position 2 (C++ water, Java packed_ice)"},
        {732, 49, 430, "Position 3 (C++ sandstone, Java stone)"},
    };

    std::cout << std::endl;
    std::cout << "Testing biome lookup methods at different Y levels:" << std::endl;
    std::cout << "==================================================" << std::endl;

    for (const auto& pos : positions) {
        std::cout << "\n" << pos.name << std::endl;
        std::cout << "Block position: (" << pos.x << ", " << pos.y << ", " << pos.z << ")" << std::endl;

        // Test at various Y levels (including possible startingHeight values)
        for (int testY : {0, 48, 63, 64, 65, 80}) {
            int32_t quartX = core::QuartPos::fromBlock(pos.x);
            int32_t quartY = core::QuartPos::fromBlock(testY);
            int32_t quartZ = core::QuartPos::fromBlock(pos.z);

            // Direct BiomeSource lookup (what diagnostic used)
            BiomeKey directBiome = biomeSource.getNoiseBiome(quartX, quartY, quartZ, *sampler);

            // BiomeManager lookup (what actual code uses)
            core::BlockPos blockPos(pos.x, testY, pos.z);
            BiomeHolder biomeHolder = biomeManager.getBiome(blockPos);
            BiomeKey managerBiome = biomeHolder ? biomeHolder->getName() : "null";

            bool match = (directBiome == managerBiome);
            bool directFrozen = (directBiome == BiomeKeys::FROZEN_OCEAN ||
                                 directBiome == BiomeKeys::DEEP_FROZEN_OCEAN);
            bool managerFrozen = (managerBiome == BiomeKeys::FROZEN_OCEAN ||
                                  managerBiome == BiomeKeys::DEEP_FROZEN_OCEAN);

            std::cout << "  Y=" << testY << ": "
                      << "direct=" << directBiome << " (frozen:" << (directFrozen ? "T" : "F") << "), "
                      << "manager=" << managerBiome << " (frozen:" << (managerFrozen ? "T" : "F") << ") "
                      << (match ? "[MATCH]" : "[DIFFER]") << std::endl;
        }
    }

    std::cout << "\n=== Detailed comparison at Y=63 (sea level) ===" << std::endl;

    // Sweep across the chunk boundary area
    std::cout << "\nSweep across X from 716 to 736 at Z=430, Y=63:" << std::endl;
    for (int x = 716; x <= 736; x += 2) {
        core::BlockPos blockPos(x, 63, 430);
        BiomeHolder biomeHolder = biomeManager.getBiome(blockPos);
        BiomeKey managerBiome = biomeHolder ? biomeHolder->getName() : "null";

        int32_t quartX = core::QuartPos::fromBlock(x);
        int32_t quartY = core::QuartPos::fromBlock(63);
        int32_t quartZ = core::QuartPos::fromBlock(430);
        BiomeKey directBiome = biomeSource.getNoiseBiome(quartX, quartY, quartZ, *sampler);

        bool directFrozen = (directBiome == BiomeKeys::FROZEN_OCEAN ||
                             directBiome == BiomeKeys::DEEP_FROZEN_OCEAN);
        bool managerFrozen = (managerBiome == BiomeKeys::FROZEN_OCEAN ||
                              managerBiome == BiomeKeys::DEEP_FROZEN_OCEAN);

        std::cout << "  X=" << x << ": direct=" << directBiome.substr(10)  // Remove "minecraft:" prefix
                  << " manager=" << managerBiome.substr(10)
                  << (directFrozen != managerFrozen ? " [FROZEN MISMATCH!]" : "") << std::endl;
    }

    // Clean up
    delete randomState;
    delete settings;

    return 0;
}
