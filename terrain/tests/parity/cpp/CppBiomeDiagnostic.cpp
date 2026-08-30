/**
 * Diagnostic to compare biome results for chunk 41,-11 when generated:
 * 1. As the first chunk (fresh state)
 * 2. After generating other chunks (accumulated state)
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
#include "world/LevelChunkSection.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "core/BlockPos.h"

static const int64_t SEED = 12345L;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

std::map<std::string, int64_t> getBlockCounts(minecraft::world::IChunk* chunk) {
    std::map<std::string, int64_t> counts;
    minecraft::world::ChunkPos pos = chunk->getPos();
    int startX = pos.getMinBlockX();
    int startZ = pos.getMinBlockZ();

    for (int y = MIN_Y; y < MIN_Y + HEIGHT; y++) {
        for (int x = 0; x < 16; x++) {
            for (int z = 0; z < 16; z++) {
                minecraft::BlockState* state = chunk->getBlockState(minecraft::core::BlockPos(startX + x, y, startZ + z));
                std::string name = state ? state->getIdentifier() : "minecraft:air";
                counts[name]++;
            }
        }
    }
    return counts;
}

int main() {
    std::cout << "=== Biome Diagnostic: Single vs Bulk Comparison ===" << std::endl;

    // Get block types
    minecraft::BlockState* airBlock = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
    minecraft::BlockState* stoneBlock = minecraft::world::level::block::Blocks::STONE->defaultBlockState();
    minecraft::world::BlockRegistry* registry = new minecraft::world::BlockRegistry();
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

    // Bootstrap registries
    minecraft::levelgen::NoiseRegistry::bootstrap();
    minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);
    minecraft::levelgen::SurfaceRuleData::initialize();

    // Build noise router
    minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);

    // Create settings
    minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    

    minecraft::levelgen::NoiseGeneratorSettings* settings = new minecraft::levelgen::NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, 63, false, true, true, false
    );

    // Create RandomState
    minecraft::levelgen::RandomState* randomState = minecraft::levelgen::RandomState::create(settings, SEED);

    // Create surface rules
    minecraft::levelgen::RuleSource* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

    // Create FluidPicker
    minecraft::levelgen::FluidPicker* fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
        63, -54, minecraft::world::level::block::Blocks::WATER->defaultBlockState(), minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
    );

    // Create biome source
    std::unique_ptr<minecraft::world::biome::MultiNoiseBiomeSource> biomeSource =
        minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

    // Create chunk generator
    minecraft::levelgen::NoiseBasedChunkGenerator* generator = new minecraft::levelgen::NoiseBasedChunkGenerator(
        settings, randomState->surfaceSystem(), surfaceRules, stoneBlock, airBlock, fluidPicker, nullptr
    );
    generator->setBiomeSource(biomeSource.get());

    // Create runner
    minecraft::levelgen::ChunkGenerationRunner::Config config =
        minecraft::levelgen::ChunkGenerationRunner::Config::terrainWithCaves();
    config.runCarvers = true;
    config.runSurface = true;
    minecraft::levelgen::ChunkGenerationRunner runner(generator, randomState, SEED, config);

    // TEST 1: Generate chunk 41,-11 as FIRST chunk
    std::cout << "\n=== TEST 1: Generate 41,-11 as FIRST chunk ===" << std::endl;
    {
        minecraft::world::ChunkPos chunkPos(41, -11);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
        );
        runner.generateChunk(chunk);

        auto counts = getBlockCounts(chunk);
        std::cout << "Block counts:" << std::endl;
        for (const auto& [name, count] : counts) {
            if (name.find("podzol") != std::string::npos || name.find("grass_block") != std::string::npos) {
                std::cout << "  " << name << ": " << count << std::endl;
            }
        }
        delete chunk;
    }

    // TEST 2: Generate 100 other chunks, then generate chunk 41,-11 again
    std::cout << "\n=== TEST 2: Generate 100 other chunks first ===" << std::endl;
    for (int i = 0; i < 100; i++) {
        int cx = -50 + (i % 10);
        int cz = -50 + (i / 10);
        minecraft::world::ChunkPos chunkPos(cx, cz);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
        );
        runner.generateChunk(chunk);
        delete chunk;
    }

    std::cout << "Now generating 41,-11 after 100 chunks..." << std::endl;
    {
        minecraft::world::ChunkPos chunkPos(41, -11);
        minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
            chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
        );
        runner.generateChunk(chunk);

        auto counts = getBlockCounts(chunk);
        std::cout << "Block counts:" << std::endl;
        for (const auto& [name, count] : counts) {
            if (name.find("podzol") != std::string::npos || name.find("grass_block") != std::string::npos) {
                std::cout << "  " << name << ": " << count << std::endl;
            }
        }
        delete chunk;
    }

    // Cleanup
    delete generator;
    delete randomState;
    delete settings;
    delete registry;

    return 0;
}
