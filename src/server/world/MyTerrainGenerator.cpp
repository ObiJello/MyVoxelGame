// File: src/server/world/MyTerrainGenerator.cpp
#include "MyTerrainGenerator.hpp"
#include <chrono>
#include <future>

// Terrain library namespace imports
using world::MinecraftBlocks;
using world::IBlockType;
using world::ProtoChunk;
using world::ChunkPos;
using minecraft::world::BlockRegistry;
using minecraft::levelgen::NoiseRegistry;
using minecraft::levelgen::DensityFunctionRegistry;
using minecraft::levelgen::NoiseRouter;
using minecraft::levelgen::NoiseRouterData;
using minecraft::levelgen::NoiseSettings;
using minecraft::levelgen::NoiseGeneratorSettings;
using minecraft::levelgen::RandomState;
using minecraft::levelgen::OverworldFluidPicker;
using minecraft::levelgen::Beardifier;
using minecraft::levelgen::NoiseBasedChunkGenerator;
using minecraft::levelgen::SurfaceSystem;
using minecraft::levelgen::SurfaceRuleData;
using minecraft::levelgen::RuleSource;
using minecraft::levelgen::ChunkGenerationRunner;
using minecraft::world::biome::MultiNoiseBiomeSource;
using minecraft::Blender;

namespace Game {

    MyTerrainGenerator::MyTerrainGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Info("[MyTerrainGenerator] Created with seed: %d", config.seed);
    }

    MyTerrainGenerator::~MyTerrainGenerator() {
        Shutdown();
    }

    bool MyTerrainGenerator::Initialize() {
        if (m_initialized) {
            Log::Warning("[MyTerrainGenerator] Already initialized");
            return true;
        }

        try {
            Log::Info("[MyTerrainGenerator] Initializing with seed: %lld", (int64_t)m_config.seed);

            // ========================================================================
            // ONE-TIME SETUP (Bootstrap your terrain library)
            // ========================================================================

            // Step 1: Bootstrap registries (once per program)
            NoiseRegistry::bootstrap();
            DensityFunctionRegistry::bootstrap(m_config.seed);
            Log::Info("[MyTerrainGenerator] Registries bootstrapped");

            // Step 2: Create noise router
            NoiseRouter* router = NoiseRouterData::overworld(false, false);
            NoiseSettings noiseSettings = NoiseSettings::OVERWORLD_NOISE_SETTINGS;

            // Step 3: Create noise generator settings
            m_settings = new NoiseGeneratorSettings(
                noiseSettings,
                MinecraftBlocks::STONE(),  // defaultBlock
                MinecraftBlocks::WATER(),  // defaultFluid
                *router,
                nullptr,  // surfaceRule (will use defaults)
                {},       // spawnTarget
                63,       // seaLevel
                false,    // disableMobGeneration
                true,     // aquifersEnabled
                true,     // oreVeinsEnabled
                false     // useLegacyRandomSource
            );
            Log::Info("[MyTerrainGenerator] Settings created (sea level: 63)");

            // Step 4: Create random state
            m_randomState = RandomState::create(m_settings, m_config.seed);
            Log::Info("[MyTerrainGenerator] RandomState created");

            // Step 5: Create fluid picker (controls water/lava placement)
            m_fluidPicker = new OverworldFluidPicker(
                63,   // seaLevel
                -54,  // lavaLevel
                MinecraftBlocks::WATER(),
                MinecraftBlocks::LAVA()
            );
            Log::Info("[MyTerrainGenerator] FluidPicker created");

            // Step 6: Create beardifier (for structure blending)
            m_beardifier = new Beardifier();

            // Step 7: Initialize surface rules
            SurfaceRuleData::initialize();
            m_surfaceRules = SurfaceRuleData::overworld();
            Log::Info("[MyTerrainGenerator] Surface rules initialized");

            // Step 8: Create surface system
            m_surfaceSystem = new SurfaceSystem(
                m_randomState,
                MinecraftBlocks::STONE(),
                63,  // seaLevel
                m_randomState->random()
            );
            Log::Info("[MyTerrainGenerator] SurfaceSystem created");

            // Step 9: Create the main chunk generator with surface rules
            m_generator = new NoiseBasedChunkGenerator(
                m_settings,
                m_surfaceSystem,
                m_surfaceRules,
                MinecraftBlocks::STONE(),  // Default block
                MinecraftBlocks::WATER(),  // Default fluid (was AIR - bug fix)
                m_fluidPicker,
                m_beardifier
            );
            Log::Info("[MyTerrainGenerator] NoiseBasedChunkGenerator created with surface rules");

            // Step 10: Create and set BiomeSource (needed for surface rules to know biome)
            m_biomeSource = MultiNoiseBiomeSource::createOverworld().release();
            m_generator->setBiomeSource(m_biomeSource);
            Log::Info("[MyTerrainGenerator] BiomeSource created and set");

            // Step 11: Create ChunkGenerationRunner (handles full pipeline including carvers)
            ChunkGenerationRunner::Config runnerConfig = ChunkGenerationRunner::Config::terrainWithCaves();
            runnerConfig.runFeatures = true;  // Enable ore generation
            m_runner = new ChunkGenerationRunner(m_generator, m_randomState, m_config.seed, runnerConfig);
            Log::Info("[MyTerrainGenerator] ChunkGenerationRunner created (terrain + caves + features)");

            // Step 12: Create block registry and register all possible block types
            m_blockRegistry = new BlockRegistry();

            // Register all block types that the terrain generator might use
            m_blockRegistry->registerBlock(MinecraftBlocks::AIR());
            m_blockRegistry->registerBlock(MinecraftBlocks::CAVE_AIR());
            m_blockRegistry->registerBlock(MinecraftBlocks::STONE());
            m_blockRegistry->registerBlock(MinecraftBlocks::WATER());
            m_blockRegistry->registerBlock(MinecraftBlocks::LAVA());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE());
            m_blockRegistry->registerBlock(MinecraftBlocks::BEDROCK());
            m_blockRegistry->registerBlock(MinecraftBlocks::GRASS_BLOCK());
            m_blockRegistry->registerBlock(MinecraftBlocks::DIRT());
            m_blockRegistry->registerBlock(MinecraftBlocks::SAND());
            m_blockRegistry->registerBlock(MinecraftBlocks::GRAVEL());
            m_blockRegistry->registerBlock(MinecraftBlocks::COPPER_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_IRON_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::RAW_COPPER_BLOCK());
            m_blockRegistry->registerBlock(MinecraftBlocks::RAW_IRON_BLOCK());
            m_blockRegistry->registerBlock(MinecraftBlocks::GRANITE());
            m_blockRegistry->registerBlock(MinecraftBlocks::TUFF());
            m_blockRegistry->registerBlock(MinecraftBlocks::SNOW_BLOCK());
            m_blockRegistry->registerBlock(MinecraftBlocks::PACKED_ICE());
            m_blockRegistry->registerBlock(MinecraftBlocks::ICE());
            m_blockRegistry->registerBlock(MinecraftBlocks::SANDSTONE());
            m_blockRegistry->registerBlock(MinecraftBlocks::POWDER_SNOW());
            m_blockRegistry->registerBlock(MinecraftBlocks::OAK_LEAVES());
            m_blockRegistry->registerBlock(MinecraftBlocks::SPRUCE_LEAVES());
            m_blockRegistry->registerBlock(MinecraftBlocks::BIRCH_LEAVES());

            // Ore blocks for features phase
            m_blockRegistry->registerBlock(MinecraftBlocks::COAL_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_COAL_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::IRON_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::GOLD_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_GOLD_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::REDSTONE_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_REDSTONE_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DIAMOND_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_DIAMOND_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::LAPIS_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_LAPIS_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::EMERALD_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_EMERALD_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DEEPSLATE_COPPER_ORE());
            m_blockRegistry->registerBlock(MinecraftBlocks::DIORITE());
            m_blockRegistry->registerBlock(MinecraftBlocks::ANDESITE());

            Log::Info("[MyTerrainGenerator] BlockRegistry initialized with %d block types",
                     static_cast<int>(42)); // Updated count

            m_initialized = true;
            Log::Info("[MyTerrainGenerator] ✓ Initialization complete!");
            return true;

        } catch (const std::exception& e) {
            Log::Error("[MyTerrainGenerator] Initialization failed: %s", e.what());
            Shutdown();
            return false;
        }
    }

    void MyTerrainGenerator::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("[MyTerrainGenerator] Shutting down...");

        // Clean up in reverse order of creation
        delete m_blockRegistry;
        m_blockRegistry = nullptr;

        delete m_runner;
        m_runner = nullptr;

        delete m_biomeSource;
        m_biomeSource = nullptr;

        delete m_generator;
        m_generator = nullptr;

        delete m_surfaceSystem;
        m_surfaceSystem = nullptr;
        // Note: m_surfaceRules is not owned by us (static singleton from SurfaceRuleData)
        m_surfaceRules = nullptr;

        delete m_beardifier;
        m_beardifier = nullptr;

        delete m_fluidPicker;
        m_fluidPicker = nullptr;

        delete m_randomState;
        m_randomState = nullptr;

        delete m_settings;
        m_settings = nullptr;

        m_initialized = false;
        Log::Info("[MyTerrainGenerator] ✓ Shutdown complete");
    }

    ChunkGenerationResult MyTerrainGenerator::GenerateChunk(Math::ChunkPos position) {
        ChunkGenerationResult result;
        result.success = false;

        if (!m_initialized) {
            result.errorMessage = "Generator not initialized";
            Log::Error("[MyTerrainGenerator] Cannot generate - not initialized");
            return result;
        }

        auto startTime = std::chrono::high_resolution_clock::now();

        try {
            // ========================================================================
            // PER-CHUNK GENERATION
            // ========================================================================

            // Step 1: Create ChunkPos for your library
            ChunkPos chunkPos(position.x, position.z);

            // Step 2: Create empty ProtoChunk (your library's chunk format)
            ProtoChunk* protoChunk = new ProtoChunk(
                chunkPos,
                -64,                       // minY (matches Minecraft 1.18+)
                384,                       // height (from -64 to 319)
                MinecraftBlocks::AIR(),    // defaultBlock
                MinecraftBlocks::AIR(),    // defaultFluid
                m_blockRegistry
            );

            // Step 3: GENERATE TERRAIN using ChunkGenerationRunner (full pipeline)
            // This handles: biomes -> noise -> surface -> carvers
            Log::Info("[MyTerrainGenerator] Generating chunk (%d, %d) with seed %lld",
                      position.x, position.z, (int64_t)m_config.seed);
            m_runner->generateChunk(protoChunk);

            Log::Debug("[MyTerrainGenerator] Generated ProtoChunk for (%d, %d)", position.x, position.z);

            // Debug: For chunk (0,0), log block counts to verify terrain generation
            if (position.x == 0 && position.z == 0) {
                int stoneCount = 0, waterCount = 0, grassCount = 0, dirtCount = 0, airCount = 0;
                for (int y = -64; y < 320; y++) {
                    for (int lx = 0; lx < 16; lx++) {
                        for (int lz = 0; lz < 16; lz++) {
                            IBlockType* bt = protoChunk->getBlockState(lx, y, lz);
                            if (bt == MinecraftBlocks::STONE()) stoneCount++;
                            else if (bt == MinecraftBlocks::WATER()) waterCount++;
                            else if (bt == MinecraftBlocks::GRASS_BLOCK()) grassCount++;
                            else if (bt == MinecraftBlocks::DIRT()) dirtCount++;
                            else if (bt == MinecraftBlocks::AIR() || bt == MinecraftBlocks::CAVE_AIR()) airCount++;
                        }
                    }
                }
                Log::Info("[MyTerrainGenerator] Chunk (0,0) block counts: stone=%d, water=%d, grass=%d, dirt=%d, air=%d",
                         stoneCount, waterCount, grassCount, dirtCount, airCount);
            }

            // ========================================================================
            // CONVERT TO GAME CHUNK FORMAT
            // ========================================================================

            // Step 4: Create game's chunk
            auto gameChunk = std::make_shared<Chunk>();
            gameChunk->pos = position;

            // Step 5: Copy blocks from ProtoChunk to Game::Chunk
            int blocksSet = 0;
            int worldMinX = chunkPos.getMinBlockX();  // position.x * 16
            int worldMinZ = chunkPos.getMinBlockZ();  // position.z * 16

            for (int y = -64; y < 320; y++) {
                for (int localX = 0; localX < 16; localX++) {
                    for (int localZ = 0; localZ < 16; localZ++) {
                        // Get block from your library's chunk
                        int worldX = worldMinX + localX;
                        int worldZ = worldMinZ + localZ;
                        IBlockType* blockType = protoChunk->getBlockState(worldX, y, worldZ);

                        // Map to game's BlockID
                        BlockID gameBlockId = MapBlockType(blockType);

                        // Set in game chunk
                        gameChunk->SetBlock(localX, y, localZ, gameBlockId);

                        if (gameBlockId != BlockID::Air) {
                            blocksSet++;
                        }
                    }
                }
            }

            // Step 6: Cleanup ProtoChunk
            delete protoChunk;

            // ========================================================================
            // RETURN SUCCESS
            // ========================================================================

            auto endTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            result.success = true;
            result.chunk = gameChunk;

            // Update stats
            m_stats.chunksGenerated++;
            m_stats.totalGenerationTimeMs += duration.count();

            Log::Debug("[MyTerrainGenerator] ✓ Chunk (%d, %d) generated in %lldms (%d non-air blocks)",
                      position.x, position.z, duration.count(), blocksSet);

        } catch (const std::exception& e) {
            result.errorMessage = std::string("Exception during generation: ") + e.what();
            Log::Error("[MyTerrainGenerator] Generation failed for chunk (%d, %d): %s",
                      position.x, position.z, e.what());
        }

        return result;
    }

    BlockID MyTerrainGenerator::MapBlockType(IBlockType* blockType) const {
        // Map your library's block types to game's BlockIDs
        if (blockType == MinecraftBlocks::AIR() || blockType == MinecraftBlocks::CAVE_AIR()) {
            return BlockID::Air;
        }
        else if (blockType == MinecraftBlocks::STONE()) {
            return BlockID::Stone;
        }
        else if (blockType == MinecraftBlocks::WATER()) {
            return BlockID::Water;
        }
        else if (blockType == MinecraftBlocks::LAVA()) {
            return BlockID::Lava;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE()) {
            return BlockID::Deepslate;
        }
        else if (blockType == MinecraftBlocks::BEDROCK()) {
            return BlockID::Bedrock;
        }
        else if (blockType == MinecraftBlocks::GRASS_BLOCK()) {
            return BlockID::Grass;
        }
        else if (blockType == MinecraftBlocks::DIRT()) {
            return BlockID::Dirt;
        }
        else if (blockType == MinecraftBlocks::SAND()) {
            return BlockID::Sand;
        }
        else if (blockType == MinecraftBlocks::GRAVEL()) {
            return BlockID::Gravel;
        }
        else if (blockType == MinecraftBlocks::COPPER_ORE()) {
            return BlockID::CopperOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_IRON_ORE()) {
            return BlockID::DeepslateIronOre;
        }
        else if (blockType == MinecraftBlocks::RAW_IRON_BLOCK()) {
            return BlockID::RawIronBlock;
        }
        else if (blockType == MinecraftBlocks::GRANITE()) {
            return BlockID::Granite;
        }
        else if (blockType == MinecraftBlocks::TUFF()) {
            return BlockID::Tuff;
        }
        else if (blockType == MinecraftBlocks::SNOW_BLOCK()) {
            return BlockID::Snow;
        }
        else if (blockType == MinecraftBlocks::ICE() || blockType == MinecraftBlocks::PACKED_ICE()) {
            return BlockID::Ice;
        }
        else if (blockType == MinecraftBlocks::SANDSTONE()) {
            return BlockID::Sandstone;
        }
        else if (blockType == MinecraftBlocks::OAK_LEAVES()) {
            return BlockID::OakLeaves;
        }
        else if (blockType == MinecraftBlocks::BIRCH_LEAVES()) {
            return BlockID::BirchLeaves;
        }
        else if (blockType == MinecraftBlocks::SPRUCE_LEAVES()) {
            return BlockID::SpruceLeaves;
        }
        else if (blockType == MinecraftBlocks::POWDER_SNOW()) {
            return BlockID::Snow;
        }
        // Ore blocks
        else if (blockType == MinecraftBlocks::COAL_ORE()) {
            return BlockID::CoalOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_COAL_ORE()) {
            return BlockID::DeepslateCoalOre;
        }
        else if (blockType == MinecraftBlocks::IRON_ORE()) {
            return BlockID::IronOre;
        }
        else if (blockType == MinecraftBlocks::GOLD_ORE()) {
            return BlockID::GoldOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_GOLD_ORE()) {
            return BlockID::DeepslateGoldOre;
        }
        else if (blockType == MinecraftBlocks::REDSTONE_ORE()) {
            return BlockID::RedstoneOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_REDSTONE_ORE()) {
            return BlockID::DeepslateRedstoneOre;
        }
        else if (blockType == MinecraftBlocks::DIAMOND_ORE()) {
            return BlockID::DiamondOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_DIAMOND_ORE()) {
            return BlockID::DeepslateDiamondOre;
        }
        else if (blockType == MinecraftBlocks::LAPIS_ORE()) {
            return BlockID::LapisOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_LAPIS_ORE()) {
            return BlockID::DeepslateLapisOre;
        }
        else if (blockType == MinecraftBlocks::EMERALD_ORE()) {
            return BlockID::EmeraldOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_EMERALD_ORE()) {
            return BlockID::DeepslateEmeraldOre;
        }
        else if (blockType == MinecraftBlocks::DEEPSLATE_COPPER_ORE()) {
            return BlockID::DeepslateCopperOre;
        }
        else if (blockType == MinecraftBlocks::DIORITE()) {
            return BlockID::Diorite;
        }
        else if (blockType == MinecraftBlocks::ANDESITE()) {
            return BlockID::Andesite;
        }
        else {
            // Unknown block type - log the identifier and default to stone
            if (blockType) {
                Log::Warning("[MyTerrainGenerator] Unknown block type '%s', defaulting to Stone",
                           blockType->getIdentifier().c_str());
            }
            return BlockID::Stone;
        }
    }

    void MyTerrainGenerator::SetConfig(const GenerationConfig& config) {
        m_config = config;
        // Note: Changing config after initialization may require reinitializing
        if (m_initialized && config.seed != m_config.seed) {
            Log::Warning("[MyTerrainGenerator] Seed changed after initialization - requires restart");
        }
    }

    void MyTerrainGenerator::SetSeed(int32_t seed) {
        m_config.seed = seed;
        if (m_initialized) {
            Log::Warning("[MyTerrainGenerator] Seed changed after initialization - requires restart");
        }
    }

    int32_t MyTerrainGenerator::GetSeed() const {
        return m_config.seed;
    }

    void MyTerrainGenerator::SetPassEnabled(GenerationPass pass, bool enabled) {
        // Your library handles all generation internally, so this is a no-op
        // Could log if needed for debugging
    }

    IChunkGenerator::GeneratorStats MyTerrainGenerator::GetStats() const {
        return m_stats;
    }

    void MyTerrainGenerator::ResetStats() {
        m_stats = IChunkGenerator::GeneratorStats{};
    }

    // === STUB IMPLEMENTATIONS FOR EXTENDED INTERFACE ===

    std::future<ChunkGenerationResult> MyTerrainGenerator::GenerateChunkAsync(Math::ChunkPos position) {
        // Simple async wrapper around GenerateChunk
        return std::async(std::launch::async, [this, position]() {
            return GenerateChunk(position);
        });
    }

    std::vector<int> MyTerrainGenerator::GenerateHeightMap(Math::ChunkPos position) {
        // Not implemented - terrain library doesn't expose heightmap separately
        return std::vector<int>(16 * 16, 64);  // Return flat heightmap at y=64
    }

    std::string MyTerrainGenerator::GenerateBiome(Math::ChunkPos position) {
        // Not implemented - terrain library handles biomes internally
        return "plains";
    }

    GenerationConfig MyTerrainGenerator::GetConfig() const {
        return m_config;
    }

    void MyTerrainGenerator::SetWorldType(const std::string& worldType) {
        // Not implemented - terrain library uses fixed overworld generation
    }

    std::string MyTerrainGenerator::GetWorldType() const {
        return "overworld";
    }

    bool MyTerrainGenerator::IsPassEnabled(GenerationPass pass) const {
        // Terrain library does all passes internally
        return true;
    }

    ChunkGenerationResult MyTerrainGenerator::GenerateWithPasses(Math::ChunkPos position, const std::vector<GenerationPass>& passes) {
        // Terrain library doesn't support selective passes - generate normally
        return GenerateChunk(position);
    }

    bool MyTerrainGenerator::IsReady() const {
        return m_initialized;
    }

    void MyTerrainGenerator::SetMaxGenerationTime(float maxTimeMs) {
        // Not implemented
    }

    float MyTerrainGenerator::GetMaxGenerationTime() const {
        return 0.0f;
    }

    void MyTerrainGenerator::RegisterTerrainFunction(const std::string& name, TerrainFunction func) {
        // Not implemented - terrain library uses internal functions
    }

    void MyTerrainGenerator::RegisterFeatureFunction(const std::string& name, FeatureFunction func) {
        // Not implemented - terrain library uses internal features
    }

    void MyTerrainGenerator::SetTerrainFunction(const std::string& name) {
        // Not implemented
    }

    void MyTerrainGenerator::AddFeatureFunction(const std::string& name) {
        // Not implemented
    }

    IChunkGenerator::DebugInfo MyTerrainGenerator::GetDebugInfo(Math::ChunkPos position) {
        DebugInfo info;
        info.biome = "plains";
        info.heightMap = std::vector<int>(16 * 16, 64);
        for (int i = 0; i < 7; ++i) {
            info.generationTimePerPass[i] = 0.0f;
        }
        return info;
    }

    void MyTerrainGenerator::SetDebugMode(bool enabled) {
        // Not implemented
    }

    bool MyTerrainGenerator::IsDebugMode() const {
        return false;
    }

    std::string MyTerrainGenerator::GetLastError() const {
        return "";
    }

    void MyTerrainGenerator::ClearErrors() {
        // Not implemented
    }

} // namespace Game