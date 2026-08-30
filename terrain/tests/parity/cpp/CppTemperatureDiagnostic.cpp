/**
 * Temperature Diagnostic - Test exact position (-753, 124, 462) where C++ and Java differ
 * C++ outputs snow_block, Java outputs grass_block
 *
 * This test generates the actual chunk to see what biome is used during surface rule application.
 */

#include <iostream>
#include <iomanip>
#include <memory>

#include "levelgen/ChunkGenerationRunner.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/FluidPicker.h"
#include "world/ProtoChunk.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"
#include "core/BlockPos.h"

static const int64_t SEED = 12345L;
static const int SEA_LEVEL = 63;
static const int MIN_Y = -64;
static const int HEIGHT = 384;

int main() {
    std::cout << "=== Temperature Diagnostic ===" << std::endl;
    std::cout << std::fixed << std::setprecision(15);

    // Bootstrap
    minecraft::levelgen::NoiseRegistry::bootstrap();
    minecraft::levelgen::DensityFunctionRegistry::bootstrap(SEED);
    minecraft::levelgen::SurfaceRuleData::initialize();

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
    registry->registerBlock(minecraft::world::level::block::Blocks::SNOW_BLOCK->defaultBlockState());

    // Create biome source
    auto biomeSource = minecraft::world::biome::MultiNoiseBiomeSource::createOverworld();

    // Create settings
    minecraft::levelgen::NoiseRouter* router = minecraft::levelgen::NoiseRouterData::overworld(false, false);
    minecraft::levelgen::NoiseSettings noiseSettings = minecraft::levelgen::NoiseSettings::OVERWORLD_NOISE_SETTINGS;
    
    
    minecraft::levelgen::NoiseGeneratorSettings* settings = new minecraft::levelgen::NoiseGeneratorSettings(
        noiseSettings, ::minecraft::world::level::block::Blocks::STONE->defaultBlockState(), ::minecraft::world::level::block::Blocks::WATER->defaultBlockState(), *router, nullptr, {}, SEA_LEVEL, false, true, true, false
    );

    // Create RandomState
    minecraft::levelgen::RandomState* randomState = minecraft::levelgen::RandomState::create(settings, SEED);
    auto* sampler = randomState->sampler();

    // Create surface rules
    minecraft::levelgen::RuleSource* surfaceRules = minecraft::levelgen::SurfaceRuleData::overworld();

    // Create FluidPicker
    minecraft::levelgen::FluidPicker* fluidPicker = new minecraft::levelgen::OverworldFluidPicker(
        63, -54, minecraft::world::level::block::Blocks::WATER->defaultBlockState(), minecraft::world::level::block::Blocks::LAVA->defaultBlockState()
    );

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

    // Test exact position: (-753, 124, 462) - where C++/Java differ
    // This is chunk (-48, 28) at local position (15, 124, 14)
    int chunkX = -48, chunkZ = 28;
    int localX = 15, localZ = 14;
    int x = chunkX * 16 + localX;  // -753
    int y = 124;
    int z = chunkZ * 16 + localZ;  // 462

    std::cout << "\n=== Position Info ===" << std::endl;
    std::cout << "Chunk: (" << chunkX << ", " << chunkZ << ")" << std::endl;
    std::cout << "Local: (" << localX << ", " << y << ", " << localZ << ")" << std::endl;
    std::cout << "World: (" << x << ", " << y << ", " << z << ")" << std::endl;

    // Generate the chunk
    std::cout << "\n=== Generating Chunk ===" << std::endl;
    minecraft::world::ChunkPos chunkPos(chunkX, chunkZ);
    minecraft::world::ProtoChunk* chunk = new minecraft::world::ProtoChunk(
        chunkPos, MIN_Y, HEIGHT, airBlock, stoneBlock, registry
    );
    runner.generateChunk(chunk);

    // Get the block at the test position
    minecraft::core::BlockPos blockPos(x, y, z);
    minecraft::BlockState* block = chunk->getBlockState(blockPos);
    std::cout << "Block at position: " << (block ? block->getIdentifier() : "null") << std::endl;

    // Get biome from chunk at this position
    int quartX = x >> 2;
    int quartY = y >> 2;
    int quartZ = z >> 2;
    std::cout << "\nQuart coords: (" << quartX << ", " << quartY << ", " << quartZ << ")" << std::endl;

    // Direct biome lookup
    minecraft::world::biome::BiomeKey biomeKey = biomeSource->getNoiseBiome(quartX, quartY, quartZ, *sampler);
    const minecraft::world::biome::Biome* biome = minecraft::world::biome::Biomes::get(biomeKey);
    std::cout << "Biome (direct): " << biome->getName() << std::endl;
    std::cout << "Base temperature: " << biome->getBaseTemperature() << std::endl;

    // Get temperature
    minecraft::core::BlockPos pos(x, y, z);
    float adjustedTemp = biome->getTemperature(pos, SEA_LEVEL);
    std::cout << "Adjusted temperature: " << adjustedTemp << std::endl;
    std::cout << "coldEnoughToSnow (temp < 0.15): " << (adjustedTemp < 0.15f ? "TRUE" : "FALSE") << std::endl;

    // Check chunk's biome storage
    std::cout << "\n=== Chunk Biome Check ===" << std::endl;
    // The chunk stores biomes - let's see what biome is stored at this position

    std::cout << "\n=== Done ===" << std::endl;

    delete chunk;
    delete generator;
    delete randomState;
    delete settings;
    delete registry;

    return 0;
}
