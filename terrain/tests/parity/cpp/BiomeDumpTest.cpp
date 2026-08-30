/**
 * Dump biome at every quart position for a given chunk using direct biome source query.
 * Usage: biome_dump_test [chunkX chunkZ]
 */
#include <iostream>
#include <map>
#include <string>
#include <memory>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biomes.h"
#include "core/QuartPos.h"
#include "core/BlockPos.h"

static const int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

int main(int argc, char* argv[]) {
    int chunkX = 510, chunkZ = 502;
    if (argc >= 3) {
        chunkX = std::atoi(argv[1]);
        chunkZ = std::atoi(argv[2]);
    }

    // Bootstrap (same as CppBiomeDiagnostic.cpp)
    minecraft::world::level::block::Blocks::bootstrap();
    auto* airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
    auto* stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();
    auto* registry = new minecraft::world::BlockRegistry();
    registry->registerBlock(airBlock);
    registry->registerBlock(stoneBlock);
    registry->registerBlock(minecraft::world::level::block::Blocks::WATER->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::LAVA->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::DEEPSLATE->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::BEDROCK->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::GRASS_BLOCK->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::DIRT->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::SAND->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::GRAVEL->defaultBlockState());
    registry->registerBlock(minecraft::world::level::block::Blocks::TUFF->defaultBlockState());

    minecraft::levelgen::NoiseRegistry::bootstrap();
    minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);
    minecraft::levelgen::SurfaceRuleData::initialize();

    auto* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);
    auto noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;

    auto* settings = new minecraft::levelgen::NoiseGeneratorSettings(
        noiseSettings, stoneBlock,
        minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
        *router, nullptr, {}, 63, false, true, true, false
    );

    auto* randomState = minecraft::levelgen::RandomState::create(settings, SEED);
    auto biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

    auto* fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
        63, -54,
        minecraft::world::level::block::Blocks::WATER->defaultBlockState(),
        minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
    );
    auto* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();
    auto* generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
        settings, randomState->surfaceSystem(), surfaceRules,
        stoneBlock, airBlock, fluidPicker, nullptr
    );
    generator->setBiomeSource(biomeSource.get());

    // Generate chunk with biomes using ChunkGenerationRunner
    minecraft::levelgen::ChunkGenerationRunner::Config config;
    config.runNoise = false;
    config.runSurface = false;
    config.runCarvers = false;
    minecraft::levelgen::ChunkGenerationRunner runner(generator, randomState, SEED, config);

    minecraft::world::ChunkPos chunkPos(chunkX, chunkZ);
    auto* chunk = new minecraft::world::ProtoChunk(
        chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
    );

    // Just create biomes (not full generation)
    generator->createBiomes(randomState, nullptr, chunk);

    // Dump biome at every quart position
    int quartMinX = minecraft::core::QuartPos::fromBlock(chunkPos.getMinBlockX());
    int quartMinZ = minecraft::core::QuartPos::fromBlock(chunkPos.getMinBlockZ());
    int quartMinY = minecraft::core::QuartPos::fromBlock(MIN_Y);
    int quartMaxY = minecraft::core::QuartPos::fromBlock(MIN_Y + HEIGHT) - 1;

    std::cout << "# BiomeDump chunk=(" << chunkX << "," << chunkZ << ") seed=" << SEED << std::endl;
    std::cout << "# Format: quartX,quartY,quartZ,biomeName" << std::endl;

    // Also query direct sampler (no NoiseChunk caching) for comparison
    auto* directSampler = randomState->sampler();

    std::map<std::string, int> biomeCounts;
    std::map<std::string, int> directBiomeCounts;
    int mismatches = 0;
    for (int qx = quartMinX; qx < quartMinX + 4; qx++) {
        for (int qy = quartMinY; qy <= quartMaxY; qy++) {
            for (int qz = quartMinZ; qz < quartMinZ + 4; qz++) {
                auto biome = chunk->getNoiseBiome(qx, qy, qz);
                std::string name = biome ? biome->getName() : "null";
                biomeCounts[name]++;

                // Also query direct sampler
                auto directBiomeKey = biomeSource->getNoiseBiome(qx, qy, qz, *directSampler);
                auto* directBiome = minecraft::world::biome::Biomes::get(directBiomeKey);
                std::string directName = directBiome ? directBiome->getName() : "null";
                directBiomeCounts[directName]++;

                std::cout << qx << "," << qy << "," << qz << "," << name;
                if (name != directName) {
                    std::cout << "," << directName << ",MISMATCH";
                    mismatches++;
                }
                std::cout << std::endl;
            }
        }
    }

    std::cerr << "Cached sampler biome summary:" << std::endl;
    for (auto& [name, count] : biomeCounts) {
        std::cerr << "  " << name << ": " << count << std::endl;
    }
    std::cerr << "Direct sampler biome summary:" << std::endl;
    for (auto& [name, count] : directBiomeCounts) {
        std::cerr << "  " << name << ": " << count << std::endl;
    }
    std::cerr << "Mismatches between cached and direct: " << mismatches << std::endl;

    delete chunk;
    delete registry;
    return 0;
}
