// File: src/common/world/gen/ProceduralChunkGenerator.cpp
#include "ProceduralChunkGenerator.hpp"
#include "../chunk/Chunk.hpp"
#include "server/world/interfaces/INeighborProvider.hpp"
#include "common/core/JobSystem.hpp"
#include "common/core/Log.hpp"
#include "../ext/FastNoiseLite.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include "glm/vec3.hpp"

namespace Game {

    // === TREE TEMPLATE IMPLEMENTATION ===

    TreeTemplate::TreeTemplate(int height, BlockID logType, BlockID leafType)
        : m_height(height), m_logType(logType), m_leafType(leafType) {
    }

    std::vector<StructureData> TreeTemplate::Generate(std::mt19937& rng) const {
        std::vector<StructureData> blocks;

        // Generate trunk
        for (int y = 0; y < m_height; ++y) {
            blocks.emplace_back(m_logType, glm::ivec3(0, y, 0));
        }

        // Generate crown (simple sphere)
        int crownRadius = 2;
        int crownCenter = m_height - 1;

        for (int y = crownCenter - crownRadius; y <= crownCenter + crownRadius; ++y) {
            for (int x = -crownRadius; x <= crownRadius; ++x) {
                for (int z = -crownRadius; z <= crownRadius; ++z) {
                    if (x == 0 && y < crownCenter && z == 0) {
                        continue; // Don't replace trunk
                    }

                    float distance = std::sqrt(x*x + (y-crownCenter)*(y-crownCenter) + z*z);
                    if (distance <= crownRadius) {
                        // Add some randomness to crown shape
                        if (rng() % 100 < 80) { // 80% chance
                            blocks.emplace_back(m_leafType, glm::ivec3(x, y, z));
                        }
                    }
                }
            }
        }

        return blocks;
    }

    glm::ivec3 TreeTemplate::GetSize() const {
        return glm::ivec3(5, m_height + 3, 5); // Crown extends beyond trunk
    }

    bool TreeTemplate::CanPlaceAt(int worldX, int worldY, int worldZ, const INeighborProvider* neighbor) const {
        if (!neighbor) return false;

        // Check if there's enough space
        glm::ivec3 size = GetSize();
        for (int y = 0; y < size.y; ++y) {
            for (int x = -size.x/2; x <= size.x/2; ++x) {
                for (int z = -size.z/2; z <= size.z/2; ++z) {
                    BlockID block = neighbor->GetBlock(worldX + x, worldY + y, worldZ + z);
                    if (y == 0) {
                        // Base should be on solid ground
                        if (block == BlockID::Air) return false;
                    } else {
                        // Upper levels should be air
                        if (block != BlockID::Air) return false;
                    }
                }
            }
        }
        return true;
    }

    // === PROCEDURAL CHUNK GENERATOR IMPLEMENTATION ===

    ProceduralChunkGenerator::ProceduralChunkGenerator(const GenerationConfig& config)
        : m_config(config) {
        Log::Debug("ProceduralChunkGenerator created with seed: %d", config.seed);
    }

    ProceduralChunkGenerator::~ProceduralChunkGenerator() {
        Shutdown();
    }

    // === CORE GENERATION INTERFACE ===

    ChunkGenerationResult ProceduralChunkGenerator::GenerateChunk(Math::ChunkPos position) {
        if (!m_initialized) {
            ChunkGenerationResult result;  // FIX: Use default constructor
            result.success = false;
            result.errorMessage = "Generator not initialized";
            return result;
        }

        std::vector<GenerationPass> allPasses = {
            GenerationPass::Terrain,
            GenerationPass::Ores,
            GenerationPass::Caves,
            GenerationPass::Structures,
            GenerationPass::Vegetation,
            GenerationPass::Fluids,
            GenerationPass::Final
        };

        return GenerateChunkInternal(position, allPasses);
    }

    std::future<ChunkGenerationResult> ProceduralChunkGenerator::GenerateChunkAsync(Math::ChunkPos position) {
        if (!m_initialized) {
            std::promise<ChunkGenerationResult> promise;
            ChunkGenerationResult result;  // FIX: Use default constructor
            result.success = false;
            result.errorMessage = "Generator not initialized";
            promise.set_value(result);
            return promise.get_future();
        }

        auto promise = std::make_shared<std::promise<ChunkGenerationResult>>();
        auto future = promise->get_future();

        JobSystem::g_ThreadPool.Enqueue([this, position, promise]() {
            ChunkGenerationResult result = GenerateChunk(position);
            promise->set_value(std::move(result));
        });

        return future;
    }

    std::vector<int> ProceduralChunkGenerator::GenerateHeightMap(Math::ChunkPos position) {
        if (!m_initialized) {
            return {};
        }

        // Check cache first
        if (m_heightMapCaching) {
            std::vector<int> cached = GetCachedHeightMap(position);
            if (!cached.empty()) {
                return cached;
            }
        }

        std::vector<int> heightMap;
        heightMap.reserve(16 * 16);

        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        for (int localZ = 0; localZ < 16; ++localZ) {
            for (int localX = 0; localX < 16; ++localX) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                BiomeType biome = DetermineBiome(worldX, worldZ);
                int height = GetTerrainHeight(worldX, worldZ, biome);
                heightMap.push_back(height);
            }
        }

        // Cache the result
        if (m_heightMapCaching) {
            CacheHeightMap(position, heightMap);
        }

        return heightMap;
    }

    std::string ProceduralChunkGenerator::GenerateBiome(Math::ChunkPos position) {
        if (!m_initialized) {
            return "unknown";
        }

        // Sample biome at chunk center
        int centerX = position.x * 16 + 8;
        int centerZ = position.z * 16 + 8;

        BiomeType biome = DetermineBiome(centerX, centerZ);
        return BiomeTypeToString(biome);
    }

    // === BATCH GENERATION ===

    std::vector<ChunkGenerationResult> ProceduralChunkGenerator::GenerateChunks(const std::vector<Math::ChunkPos>& positions) {
        std::vector<ChunkGenerationResult> results;
        results.reserve(positions.size());

        for (const auto& pos : positions) {
            results.push_back(GenerateChunk(pos));
        }

        return results;
    }

    std::vector<ChunkGenerationResult> ProceduralChunkGenerator::GenerateArea(Math::ChunkPos topLeft, Math::ChunkPos bottomRight) {
        std::vector<Math::ChunkPos> positions;

        for (int x = topLeft.x; x <= bottomRight.x; ++x) {
            for (int z = topLeft.z; z <= bottomRight.z; ++z) {
                positions.push_back({x, z});
            }
        }

        return GenerateChunks(positions);
    }

    // === CONFIGURATION ===

    void ProceduralChunkGenerator::SetConfig(const GenerationConfig& config) {
        std::lock_guard<std::mutex> lock(m_configMutex);

        bool seedChanged = (m_config.seed != config.seed);
        m_config = config;

        if (seedChanged && m_initialized) {
            InitializeNoiseGenerators();
            ClearHeightMapCache();
        }

        Log::Info("ProceduralChunkGenerator config updated");
    }

    GenerationConfig ProceduralChunkGenerator::GetConfig() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }

    void ProceduralChunkGenerator::SetSeed(int32_t seed) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.seed = seed;
        }

        if (m_initialized) {
            InitializeNoiseGenerators();
            ClearHeightMapCache();
        }
    }

    int32_t ProceduralChunkGenerator::GetSeed() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.seed;
    }

    void ProceduralChunkGenerator::SetWorldType(const std::string& worldType) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.worldType = worldType;
    }

    std::string ProceduralChunkGenerator::GetWorldType() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.worldType;
    }

    // === GENERATION LAYERS ===

    void ProceduralChunkGenerator::SetPassEnabled(GenerationPass pass, bool enabled) {
        m_passEnabled[pass] = enabled;
    }

    bool ProceduralChunkGenerator::IsPassEnabled(GenerationPass pass) const {
        auto it = m_passEnabled.find(pass);
        return it != m_passEnabled.end() ? it->second : true;
    }

    ChunkGenerationResult ProceduralChunkGenerator::GenerateWithPasses(Math::ChunkPos position,
                                                                      const std::vector<GenerationPass>& passes) {
        if (!m_initialized) {
            ChunkGenerationResult result;  // FIX: Use default constructor
            result.success = false;
            result.errorMessage = "Generator not initialized";
            return result;
        }

        return GenerateChunkInternal(position, passes);
    }

    // === LIFECYCLE ===

    bool ProceduralChunkGenerator::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing ProceduralChunkGenerator...");

        try {
            InitializeNoiseGenerators();
            InitializeBiomeConfigs();
            InitializeDefaultStructures();
            InitializePassFlags();

            // Reset statistics
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.Reset();
            }

            m_initialized = true;
            Log::Info("ProceduralChunkGenerator initialized successfully");
            return true;

        } catch (const std::exception& e) {
            SetLastError("Initialization failed: " + std::string(e.what()));
            Log::Error("ProceduralChunkGenerator initialization failed: %s", e.what());
            return false;
        }
    }

    void ProceduralChunkGenerator::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down ProceduralChunkGenerator...");

        // Clear caches
        ClearHeightMapCache();

        // Reset noise generators
        m_terrainNoise.reset();
        m_biomeNoise.reset();
        m_caveNoise.reset();
        m_oreNoise.reset();

        m_initialized = false;
        Log::Info("ProceduralChunkGenerator shutdown complete");
    }

    bool ProceduralChunkGenerator::IsReady() const {
        return m_initialized;
    }

    // === PERFORMANCE ===

    ProceduralChunkGenerator::GeneratorStats ProceduralChunkGenerator::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    void ProceduralChunkGenerator::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();
    }

    void ProceduralChunkGenerator::SetMaxGenerationTime(float maxTimeMs) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.maxGenerationTimeMs = maxTimeMs;
    }

    float ProceduralChunkGenerator::GetMaxGenerationTime() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.maxGenerationTimeMs;
    }

    // === VALIDATION ===

    bool ProceduralChunkGenerator::ValidateGeneratedChunk(const Chunk& chunk) const {
        // Use base class validation plus custom checks
        if (!IChunkGenerator::ValidateGeneratedChunk(chunk)) {
            return false;
        }

        // Check that bedrock exists at bottom
        bool hasBedrockLayer = false;
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                if (chunk.GetBlock(x, Math::WorldCoordinates::MIN_WORLD_Y, z) == BlockID::Bedrock) {
                    hasBedrockLayer = true;
                    break;
                }
            }
            if (hasBedrockLayer) break;
        }

        return hasBedrockLayer;
    }

    // === CUSTOMIZATION ===

    void ProceduralChunkGenerator::RegisterTerrainFunction(const std::string& name, TerrainFunction func) {
        m_terrainFunctions[name] = func;
    }

    void ProceduralChunkGenerator::RegisterFeatureFunction(const std::string& name, FeatureFunction func) {
        m_featureFunctions[name] = func;
    }

    void ProceduralChunkGenerator::SetTerrainFunction(const std::string& name) {
        if (m_terrainFunctions.find(name) != m_terrainFunctions.end()) {
            m_activeTerrainFunction = name;
        }
    }

    void ProceduralChunkGenerator::AddFeatureFunction(const std::string& name) {
        if (m_featureFunctions.find(name) != m_featureFunctions.end()) {
            m_activeFeatureFunctions.push_back(name);
        }
    }

    // === DEBUGGING ===

    ProceduralChunkGenerator::DebugInfo ProceduralChunkGenerator::GetDebugInfo(Math::ChunkPos position) {
        DebugInfo info;

        info.biome = GenerateBiome(position);
        info.heightMap = GenerateHeightMap(position);

        // Count blocks (simplified)
        auto result = GenerateChunk(position);
        if (result.success && result.chunk) {
            // Count different block types
            for (int x = 0; x < 16; ++x) {
                for (int y = Math::WorldCoordinates::MIN_WORLD_Y; y <= Math::WorldCoordinates::MAX_WORLD_Y; ++y) {
                    for (int z = 0; z < 16; ++z) {
                        BlockID block = result.chunk->GetBlock(x, y, z);
                        if (block != BlockID::Air) {
                            std::string blockName = "block_" + std::to_string(static_cast<int>(block));
                            info.blockCounts[blockName]++;
                        }
                    }
                }
            }
        }

        return info;
    }

    void ProceduralChunkGenerator::SetDebugMode(bool enabled) {
        m_debugMode = enabled;
    }

    bool ProceduralChunkGenerator::IsDebugMode() const {
        return m_debugMode;
    }

    std::string ProceduralChunkGenerator::GetLastError() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    void ProceduralChunkGenerator::ClearErrors() {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError.clear();
    }

    // === PROCEDURAL-SPECIFIC FEATURES ===

    void ProceduralChunkGenerator::SetBiomeConfig(BiomeType biome, const BiomeConfig& config) {
        m_biomeConfigs[biome] = config;
    }

    BiomeConfig ProceduralChunkGenerator::GetBiomeConfig(BiomeType biome) const {
        auto it = m_biomeConfigs.find(biome);
        return it != m_biomeConfigs.end() ? it->second : BiomeConfig{};
    }

    BiomeType ProceduralChunkGenerator::GetBiomeAt(int worldX, int worldZ) const {
        return DetermineBiome(worldX, worldZ);
    }

    void ProceduralChunkGenerator::SetTerrainNoise(float frequency, float amplitude, int octaves) {
        if (m_terrainNoise) {
            m_terrainNoise->SetFrequency(frequency);
            m_terrainNoise->SetFractalOctaves(octaves);
        }
    }

    void ProceduralChunkGenerator::SetBiomeNoise(float frequency, float scale) {
        if (m_biomeNoise) {
            m_biomeNoise->SetFrequency(frequency);
        }
    }

    void ProceduralChunkGenerator::SetCaveNoise(float frequency, float threshold) {
        if (m_caveNoise) {
            m_caveNoise->SetFrequency(frequency);
        }
    }

    void ProceduralChunkGenerator::RegisterStructureTemplate(const std::string& name, std::shared_ptr<StructureTemplate> templatePtr) {
        m_structureTemplates[name] = templatePtr;
    }

    void ProceduralChunkGenerator::SetStructureDensity(const std::string& structureName, float density) {
        m_structureDensities[structureName] = density;
    }

    void ProceduralChunkGenerator::SetHeightMapCaching(bool enabled) {
        m_heightMapCaching = enabled;
        if (!enabled) {
            ClearHeightMapCache();
        }
    }

    void ProceduralChunkGenerator::ClearHeightMapCache() {
        std::lock_guard<std::mutex> lock(m_heightMapMutex);
        const_cast<std::unordered_map<uint64_t, std::vector<int>>&>(m_heightMapCache).clear();
    }

    // === PROTECTED METHODS ===

    void ProceduralChunkGenerator::UpdateStats(const ChunkGenerationResult& result) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        m_stats.chunksGenerated++;
        m_stats.totalGenerationTimeMs += result.generationTimeMs;

        if (m_stats.chunksGenerated > 0) {
            m_stats.averageGenerationTimeMs = m_stats.totalGenerationTimeMs / m_stats.chunksGenerated;
        }

        m_stats.totalBlocksGenerated += result.blocksGenerated;
        m_stats.totalFeaturesGenerated += result.featuresGenerated;
    }

    // === CORE GENERATION METHODS ===

    ChunkGenerationResult ProceduralChunkGenerator::GenerateChunkInternal(Math::ChunkPos position, const std::vector<GenerationPass>& passes) {
        auto startTime = std::chrono::high_resolution_clock::now();

        ChunkGenerationResult result;  // FIX: Use default constructor
        result.chunk = nullptr;
        result.success = false;
        result.generationTimeMs = 0.0f;
        result.blocksGenerated = 0;

        try {
            // Create chunk
            auto chunk = std::make_shared<Chunk>();
            chunk->pos = position;

            // Create RNG for this chunk
            std::mt19937 rng(m_config.seed + position.x * 73 + position.z * 37);

            // Execute generation passes
            for (GenerationPass pass : passes) {
                if (IsPassEnabled(pass)) {
                    auto passStartTime = std::chrono::high_resolution_clock::now();

                    switch (pass) {
                        case GenerationPass::Terrain:
                            GenerateTerrain(*chunk, position, rng);
                            break;
                        case GenerationPass::Ores:
                            GenerateOres(*chunk, position, rng);
                            break;
                        case GenerationPass::Caves:
                            GenerateCaves(*chunk, position, rng);
                            break;
                        case GenerationPass::Structures:
                            GenerateStructures(*chunk, position, rng);
                            break;
                        case GenerationPass::Vegetation:
                            GenerateVegetation(*chunk, position, rng);
                            break;
                        case GenerationPass::Fluids:
                            GenerateFluids(*chunk, position, rng);
                            break;
                        case GenerationPass::Final:
                            FinalizeChunk(*chunk, position, rng);
                            break;
                    }

                    auto passEndTime = std::chrono::high_resolution_clock::now();
                    float passTime = std::chrono::duration<float, std::milli>(passEndTime - passStartTime).count();
                    UpdatePassStats(pass, passTime);
                }
            }

            // Count generated blocks
            result.blocksGenerated = chunk->GetNonAirBlockCount();
            result.chunk = chunk;
            result.success = true;

        } catch (const std::exception& e) {
            result.success = false;
            result.errorMessage = "Generation failed: " + std::string(e.what());
            SetLastError(result.errorMessage);
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.generationTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        UpdateStats(result);
        return result;
    }

    // === GENERATION PASSES ===

    void ProceduralChunkGenerator::GenerateTerrain(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        // Generate bedrock layer
        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int y = Math::WorldCoordinates::MIN_WORLD_Y; y < Math::WorldCoordinates::MIN_WORLD_Y + 5; ++y) {
                    chunk.SetBlock(localX, y, localZ, BlockID::Bedrock);
                }
            }
        }

        // Generate terrain
        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                BiomeType biome = DetermineBiome(worldX, worldZ);
                BiomeConfig biomeConfig = GetBiomeConfig(biome);

                int surfaceHeight = GetTerrainHeight(worldX, worldZ, biome);

                // Generate stone base
                for (int y = Math::WorldCoordinates::MIN_WORLD_Y + 5; y <= surfaceHeight - 4; ++y) {
                    chunk.SetBlock(localX, y, localZ, biomeConfig.stoneBlock);
                }

                // Generate dirt layer
                for (int y = surfaceHeight - 3; y <= surfaceHeight - 1; ++y) {
                    if (y > Math::WorldCoordinates::MIN_WORLD_Y + 5) {
                        chunk.SetBlock(localX, y, localZ, biomeConfig.fillerBlock);
                    }
                }

                // Generate surface block using sophisticated depth-based logic
                if (surfaceHeight >= Math::WorldCoordinates::MIN_WORLD_Y + 5) {
                    BlockID surfaceBlock = GetSurfaceBlockForDepth(surfaceHeight, biomeConfig, rng);
                    chunk.SetBlock(localX, surfaceHeight, localZ, surfaceBlock);
                }
            }
        }
    }

    void ProceduralChunkGenerator::GenerateOres(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        if (!m_config.generateOres) return;

        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        // Generate coal ore
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        for (int i = 0; i < 10; ++i) {
            if (dist(rng) < 0.3f) {
                int localX = rng() % 16;
                int localZ = rng() % 16;
                int worldY = Math::WorldCoordinates::MIN_WORLD_Y + 10 + (rng() % 50);

                if (chunk.GetBlock(localX, worldY, localZ) == BlockID::Stone) {
                    GenerateOreVein(chunk, BlockID::CoalOre, baseWorldX + localX, worldY, baseWorldZ + localZ, 3, rng);
                }
            }
        }

        // Generate iron ore
        for (int i = 0; i < 5; ++i) {
            if (dist(rng) < 0.2f) {
                int localX = rng() % 16;
                int localZ = rng() % 16;
                int worldY = Math::WorldCoordinates::MIN_WORLD_Y + 5 + (rng() % 40);

                if (chunk.GetBlock(localX, worldY, localZ) == BlockID::Stone) {
                    GenerateOreVein(chunk, BlockID::IronOre, baseWorldX + localX, worldY, baseWorldZ + localZ, 2, rng);
                }
            }
        }
    }

    void ProceduralChunkGenerator::GenerateCaves(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        if (!m_config.generateCaves || !m_caveNoise) return;

        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                for (int y = Math::WorldCoordinates::MIN_WORLD_Y + 10; y < Math::WorldCoordinates::MIN_WORLD_Y + 80; ++y) {
                    int worldX = baseWorldX + localX;
                    int worldZ = baseWorldZ + localZ;

                    if (IsCaveBlock(worldX, y, worldZ)) {
                        BlockID currentBlock = chunk.GetBlock(localX, y, localZ);
                        if (currentBlock == BlockID::Stone || currentBlock == BlockID::Dirt) {
                            chunk.SetBlock(localX, y, localZ, BlockID::Air);
                        }
                    }
                }
            }
        }
    }

    void ProceduralChunkGenerator::GenerateStructures(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        if (!m_config.generateStructures) return;

        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        // Generate trees
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        for (int localX = 2; localX < 14; ++localX) {
            for (int localZ = 2; localZ < 14; ++localZ) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                BiomeType biome = DetermineBiome(worldX, worldZ);
                BiomeConfig biomeConfig = GetBiomeConfig(biome);

                if (biomeConfig.generateTrees && dist(rng) < biomeConfig.treeChance) {
                    int surfaceHeight = GetTerrainHeight(worldX, worldZ, biome);

                    // Only place trees if they are above water level
                    if (surfaceHeight + 1 > biomeConfig.seaLevel && 
                        chunk.GetBlock(localX, surfaceHeight, localZ) == biomeConfig.topMaterial) {
                        GenerateTree(chunk, worldX, surfaceHeight + 1, worldZ, biome, rng);
                    }
                }
            }
        }
    }

    void ProceduralChunkGenerator::GenerateVegetation(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        // Implementation would add flowers, grass, etc.
        // For now, this is a placeholder
    }

    void ProceduralChunkGenerator::GenerateFluids(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        int baseWorldX = position.x * 16;
        int baseWorldZ = position.z * 16;

        // Fill areas below sea level with water
        for (int localX = 0; localX < 16; ++localX) {
            for (int localZ = 0; localZ < 16; ++localZ) {
                int worldX = baseWorldX + localX;
                int worldZ = baseWorldZ + localZ;

                BiomeType biome = DetermineBiome(worldX, worldZ);
                BiomeConfig biomeConfig = GetBiomeConfig(biome);

                if (biomeConfig.hasWater) {
                    for (int y = Math::WorldCoordinates::MIN_WORLD_Y; y <= biomeConfig.liquidLevel; ++y) {
                        if (chunk.GetBlock(localX, y, localZ) == BlockID::Air) {
                            chunk.SetBlock(localX, y, localZ, biomeConfig.liquidBlock);
                        }
                    }
                }
            }
        }
    }

    void ProceduralChunkGenerator::FinalizeChunk(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng) {
        // Final cleanup and validation
        // Could add lighting calculations, final block conversions, etc.
    }

    // === TERRAIN GENERATION ===

    int ProceduralChunkGenerator::GetTerrainHeight(int worldX, int worldZ, BiomeType biome) const {
        BiomeConfig config = GetBiomeConfig(biome);

        float noise = GetTerrainNoise(worldX, worldZ);
        float height = config.baseHeight + noise * config.heightVariation;

        return static_cast<int>(std::clamp(height,
                                          static_cast<float>(Math::WorldCoordinates::MIN_WORLD_Y + 10),
                                          static_cast<float>(Math::WorldCoordinates::MAX_WORLD_Y - 10)));
    }

    float ProceduralChunkGenerator::GetTerrainNoise(int worldX, int worldZ) const {
        if (!m_terrainNoise) return 0.0f;

        return GetNoise(*m_terrainNoise, worldX, worldZ);
    }

    float ProceduralChunkGenerator::GetBiomeBlendWeight(int worldX, int worldZ, BiomeType targetBiome) const {
        // Simple implementation - could be more sophisticated
        return DetermineBiome(worldX, worldZ) == targetBiome ? 1.0f : 0.0f;
    }

    BlockID ProceduralChunkGenerator::GetSurfaceBlock(int worldX, int worldY, int worldZ, BiomeType biome) const {
        BiomeConfig config = GetBiomeConfig(biome);
        return config.surfaceBlock;
    }

    BlockID ProceduralChunkGenerator::GetFillerBlock(int worldX, int worldY, int worldZ, BiomeType biome) const {
        BiomeConfig config = GetBiomeConfig(biome);
        return config.fillerBlock;
    }

    BlockID ProceduralChunkGenerator::GetSurfaceBlockForDepth(int surfaceHeight, const BiomeConfig& biomeConfig, std::mt19937& rng) const {
        // If surface is above sea level - 1, use topMaterial
        if (surfaceHeight >= biomeConfig.seaLevel - 1) {
            return biomeConfig.topMaterial;
        }
        
        // Calculate depth below sea level
        int depthBelowSea = biomeConfig.seaLevel - surfaceHeight;
        
        // Random depth limit for natural variation (3-6 blocks)
        std::uniform_int_distribution<int> depthLimitDist(3, 6);
        int depthLimit = depthLimitDist(rng);
        
        // If it's more than (7 + depthLimit) blocks below sea level, use underwaterMaterial
        if (depthBelowSea > (7 + depthLimit)) {
            return biomeConfig.underwaterMaterial;
        }
        
        // Otherwise, use underMaterial (shallow underwater)
        return biomeConfig.underMaterial;
    }

    // === BIOME GENERATION ===

    BiomeType ProceduralChunkGenerator::DetermineBiome(int worldX, int worldZ) const {
        if (!m_biomeNoise) return BiomeType::Plains;

        float temperature = GetTemperature(worldX, worldZ);
        float humidity = GetHumidity(worldX, worldZ);

        // Simple biome determination based on temperature and humidity
        if (temperature < 0.3f) {
            return BiomeType::Tundra;
        } else if (temperature > 0.8f) {
            return BiomeType::Desert;
        } else if (humidity > 0.7f) {
            return BiomeType::Swamp;
        } else if (humidity > 0.5f) {
            return BiomeType::Forest;
        } else {
            return BiomeType::Plains;
        }
    }

    float ProceduralChunkGenerator::GetTemperature(int worldX, int worldZ) const {
        if (!m_biomeNoise) return 0.5f;

        float noise = GetNoise(*m_biomeNoise, worldX * 0.1f, worldZ * 0.1f);
        return (noise + 1.0f) * 0.5f; // Convert from [-1, 1] to [0, 1]
    }

    float ProceduralChunkGenerator::GetHumidity(int worldX, int worldZ) const {
        if (!m_biomeNoise) return 0.5f;

        // Use slightly offset coordinates for humidity vs temperature
        float noise = GetNoise(*m_biomeNoise, (worldX + 1000) * 0.1f, (worldZ + 1000) * 0.1f);
        return (noise + 1.0f) * 0.5f; // Convert from [-1, 1] to [0, 1]
    }

    // === STRUCTURE GENERATION ===

    void ProceduralChunkGenerator::PlaceStructure(Chunk& chunk, const std::string& structureName,
                                                 int worldX, int worldY, int worldZ, std::mt19937& rng) {
        auto it = m_structureTemplates.find(structureName);
        if (it == m_structureTemplates.end()) {
            return;
        }

        auto structure = it->second->Generate(rng);

        for (const auto& block : structure) {
            int localX, localY, localZ;
            WorldToLocal(worldX + block.offset.x, worldY + block.offset.y, worldZ + block.offset.z,
                        chunk.pos, localX, localY, localZ);

            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16 &&
                IsWorldYValid(worldY + block.offset.y)) {
                chunk.SetBlock(localX, worldY + block.offset.y, localZ, block.blockType);
            }
        }
    }

    bool ProceduralChunkGenerator::CanPlaceStructure(const std::string& structureName,
                                                    int worldX, int worldY, int worldZ) const {
        auto it = m_structureTemplates.find(structureName);
        if (it == m_structureTemplates.end()) {
            return false;
        }

        // For now, always return true - would need neighbor provider for proper checking
        return true;
    }

    void ProceduralChunkGenerator::GenerateTree(Chunk& chunk, int worldX, int worldY, int worldZ,
                                               BiomeType biome, std::mt19937& rng) {
        // Determine tree type based on biome
        BlockID logType = BlockID::OakLog;
        BlockID leafType = BlockID::OakLeaves;

        switch (biome) {
            case BiomeType::Forest:
                logType = BlockID::OakLog;
                leafType = BlockID::OakLeaves;
                break;
            case BiomeType::Tundra:
                logType = BlockID::BirchLog;
                leafType = BlockID::BirchLeaves;
                break;
            default:
                break;
        }

        // Create and place tree
        TreeTemplate tree(4 + rng() % 3, logType, leafType); // Height 4-6
        auto structure = tree.Generate(rng);

        for (const auto& block : structure) {
            int localX, localY, localZ;
            WorldToLocal(worldX + block.offset.x, worldY + block.offset.y, worldZ + block.offset.z,
                        chunk.pos, localX, localY, localZ);

            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16 &&
                IsWorldYValid(worldY + block.offset.y)) {
                chunk.SetBlock(localX, worldY + block.offset.y, localZ, block.blockType);
            }
        }
    }

    // === ORE GENERATION ===

    void ProceduralChunkGenerator::GenerateOreVein(Chunk& chunk, BlockID oreType, int worldX, int worldY, int worldZ,
                                                  int veinSize, std::mt19937& rng) {
        std::uniform_int_distribution<int> offsetDist(-1, 1);

        int currentX = worldX;
        int currentY = worldY;
        int currentZ = worldZ;

        for (int i = 0; i < veinSize; ++i) {
            int localX, localY, localZ;
            WorldToLocal(currentX, currentY, currentZ, chunk.pos, localX, localY, localZ);

            if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16 && IsWorldYValid(currentY)) {
                if (chunk.GetBlock(localX, currentY, localZ) == BlockID::Stone) {
                    chunk.SetBlock(localX, currentY, localZ, oreType);
                }
            }

            // Move to next position in vein
            currentX += offsetDist(rng);
            currentY += offsetDist(rng);
            currentZ += offsetDist(rng);
        }
    }

    bool ProceduralChunkGenerator::ShouldGenerateOre(BlockID oreType, int worldY, float noiseValue) const {
        // Different ores have different Y-level preferences
        switch (oreType) {
            case BlockID::CoalOre:
                return worldY > Math::WorldCoordinates::MIN_WORLD_Y + 5 && worldY < Math::WorldCoordinates::MIN_WORLD_Y + 128;
            case BlockID::IronOre:
                return worldY > Math::WorldCoordinates::MIN_WORLD_Y + 5 && worldY < Math::WorldCoordinates::MIN_WORLD_Y + 64;
            case BlockID::GoldOre:
                return worldY > Math::WorldCoordinates::MIN_WORLD_Y + 5 && worldY < Math::WorldCoordinates::MIN_WORLD_Y + 32;
            case BlockID::DiamondOre:
                return worldY > Math::WorldCoordinates::MIN_WORLD_Y + 5 && worldY < Math::WorldCoordinates::MIN_WORLD_Y + 16;
            default:
                return false;
        }
    }

    // === CAVE GENERATION ===

    bool ProceduralChunkGenerator::IsCaveBlock(int worldX, int worldY, int worldZ) const {
        if (!m_caveNoise) return false;

        float noise = GetNoise3D(*m_caveNoise, worldX, worldY, worldZ);
        return noise > 0.6f; // Cave threshold
    }

    void ProceduralChunkGenerator::CarveCave(Chunk& chunk, int worldX, int worldY, int worldZ) {
        int localX, localY, localZ;
        WorldToLocal(worldX, worldY, worldZ, chunk.pos, localX, localY, localZ);

        if (localX >= 0 && localX < 16 && localZ >= 0 && localZ < 16 && IsWorldYValid(worldY)) {
            BlockID currentBlock = chunk.GetBlock(localX, worldY, localZ);
            if (currentBlock == BlockID::Stone || currentBlock == BlockID::Dirt) {
                chunk.SetBlock(localX, worldY, localZ, BlockID::Air);
            }
        }
    }

    // === UTILITY METHODS ===

    void ProceduralChunkGenerator::WorldToLocal(int worldX, int worldY, int worldZ, Math::ChunkPos chunkPos,
                                               int& localX, int& localY, int& localZ) const {
        localX = worldX - chunkPos.x * 16;
        localY = worldY; // World Y is used directly
        localZ = worldZ - chunkPos.z * 16;
    }

    bool ProceduralChunkGenerator::IsWorldYValid(int worldY) const {
        return Math::WorldCoordinates::IsValidWorldY(worldY);
    }

    float ProceduralChunkGenerator::GetNoise(FastNoiseLite& noise, int x, int z) const {
        return noise.GetNoise(static_cast<float>(x), static_cast<float>(z));
    }

    float ProceduralChunkGenerator::GetNoise3D(FastNoiseLite& noise, int x, int y, int z) const {
        return noise.GetNoise(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }

    uint64_t ProceduralChunkGenerator::GetChunkKey(Math::ChunkPos position) const {
        return (static_cast<uint64_t>(position.x) << 32) | static_cast<uint64_t>(position.z);
    }

    std::vector<int> ProceduralChunkGenerator::GetCachedHeightMap(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_heightMapMutex);

        uint64_t key = GetChunkKey(position);
        auto it = m_heightMapCache.find(key);
        return it != m_heightMapCache.end() ? it->second : std::vector<int>{};
    }

    void ProceduralChunkGenerator::CacheHeightMap(Math::ChunkPos position, const std::vector<int>& heightMap) const {
        std::lock_guard<std::mutex> lock(m_heightMapMutex);

        uint64_t key = GetChunkKey(position);
        const_cast<std::unordered_map<uint64_t, std::vector<int>>&>(m_heightMapCache)[key] = heightMap;

        // Simple cache size limit
        if (m_heightMapCache.size() > 1000) {
            auto it = m_heightMapCache.begin();
            const_cast<std::unordered_map<uint64_t, std::vector<int>>&>(m_heightMapCache).erase(it);
        }
    }

    bool ProceduralChunkGenerator::IsValidChunkPosition(Math::ChunkPos position) const {
        // Basic validation - could be more sophisticated
        return std::abs(position.x) < 1000000 && std::abs(position.z) < 1000000;
    }

    void ProceduralChunkGenerator::SetLastError(const std::string& error) const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        const_cast<std::string&>(m_lastError) = error;
    }

    void ProceduralChunkGenerator::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("ProceduralChunkGenerator %s: %s", operation.c_str(), error.c_str());
        SetLastError(operation + ": " + error);
    }

    void ProceduralChunkGenerator::UpdatePassStats(GenerationPass pass, float timeMs) const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        const_cast<GeneratorStats&>(m_stats).passTimeMs[pass] += timeMs;
    }

    // === INITIALIZATION HELPERS ===

    void ProceduralChunkGenerator::InitializeNoiseGenerators() {
        int seed = m_config.seed;

        // Terrain noise
        m_terrainNoise = std::make_unique<FastNoiseLite>(seed);
        m_terrainNoise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
        m_terrainNoise->SetFrequency(0.01f);
        m_terrainNoise->SetFractalType(FastNoiseLite::FractalType_FBm);
        m_terrainNoise->SetFractalOctaves(4);

        // Biome noise - FIX: Use correct noise type
        m_biomeNoise = std::make_unique<FastNoiseLite>(seed + 1000);
        m_biomeNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);  // FIX: Use correct constant
        m_biomeNoise->SetFrequency(0.005f);

        // Cave noise
        m_caveNoise = std::make_unique<FastNoiseLite>(seed + 2000);
        m_caveNoise->SetNoiseType(FastNoiseLite::NoiseType_Perlin);
        m_caveNoise->SetFrequency(0.02f);

        // Ore noise
        m_oreNoise = std::make_unique<FastNoiseLite>(seed + 3000);
        m_oreNoise->SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);  // FIX: Use correct constant
        m_oreNoise->SetFrequency(0.03f);

        Log::Debug("Initialized noise generators with seed: %d", seed);
    }

    void ProceduralChunkGenerator::InitializeBiomeConfigs() {
        // Plains
        BiomeConfig plains;
        plains.baseHeight = 64.0f;
        plains.heightVariation = 4.0f;
        plains.surfaceBlock = BlockID::Grass;
        plains.fillerBlock = BlockID::Dirt;
        plains.topMaterial = BlockID::Grass;
        plains.underMaterial = BlockID::Sand;
        plains.underwaterMaterial = BlockID::Gravel;
        plains.seaLevel = 62;
        plains.generateTrees = true;
        plains.treeChance = 0.01f;
        m_biomeConfigs[BiomeType::Plains] = plains;

        // Forest
        BiomeConfig forest;
        forest.baseHeight = 66.0f;
        forest.heightVariation = 8.0f;
        forest.surfaceBlock = BlockID::Grass;
        forest.fillerBlock = BlockID::Dirt;
        forest.topMaterial = BlockID::Grass;
        forest.underMaterial = BlockID::Sand;
        forest.underwaterMaterial = BlockID::Gravel;
        forest.seaLevel = 62;
        forest.generateTrees = true;
        forest.treeChance = 0.05f;
        m_biomeConfigs[BiomeType::Forest] = forest;

        // Mountains
        BiomeConfig mountains;
        mountains.baseHeight = 80.0f;
        mountains.heightVariation = 32.0f;
        mountains.surfaceBlock = BlockID::Stone;
        mountains.fillerBlock = BlockID::Stone;
        mountains.topMaterial = BlockID::Stone;
        mountains.underMaterial = BlockID::Gravel;
        mountains.underwaterMaterial = BlockID::Stone;
        mountains.seaLevel = 62;
        mountains.generateTrees = false;
        m_biomeConfigs[BiomeType::Mountains] = mountains;

        // Desert
        BiomeConfig desert;
        desert.baseHeight = 64.0f;
        desert.heightVariation = 6.0f;
        desert.surfaceBlock = BlockID::Sand;
        desert.fillerBlock = BlockID::Sand;
        desert.topMaterial = BlockID::Sand;
        desert.underMaterial = BlockID::Sand;
        desert.underwaterMaterial = BlockID::Gravel;
        desert.seaLevel = 62;
        desert.generateTrees = false;
        m_biomeConfigs[BiomeType::Desert] = desert;

        // Ocean
        BiomeConfig ocean;
        ocean.baseHeight = 45.0f;
        ocean.heightVariation = 5.0f;
        ocean.surfaceBlock = BlockID::Sand;
        ocean.fillerBlock = BlockID::Dirt;
        ocean.topMaterial = BlockID::Sand;
        ocean.underMaterial = BlockID::Sand;
        ocean.underwaterMaterial = BlockID::Gravel;
        ocean.seaLevel = 62;
        ocean.generateTrees = false;
        ocean.hasWater = true;
        ocean.liquidLevel = 62;
        m_biomeConfigs[BiomeType::Ocean] = ocean;

        // Tundra
        BiomeConfig tundra;
        tundra.baseHeight = 65.0f;
        tundra.heightVariation = 3.0f;
        tundra.surfaceBlock = BlockID::Snow;
        tundra.fillerBlock = BlockID::Dirt;
        tundra.topMaterial = BlockID::Snow;
        tundra.underMaterial = BlockID::Sand;
        tundra.underwaterMaterial = BlockID::Gravel;
        tundra.seaLevel = 62;
        tundra.generateTrees = true;
        tundra.treeChance = 0.02f;
        m_biomeConfigs[BiomeType::Tundra] = tundra;

        Log::Debug("Initialized biome configurations");
    }

    void ProceduralChunkGenerator::InitializeDefaultStructures() {
        // Register default tree templates
        RegisterStructureTemplate("oak_tree", std::make_shared<TreeTemplate>(5, BlockID::OakLog, BlockID::OakLeaves));
        RegisterStructureTemplate("birch_tree", std::make_shared<TreeTemplate>(6, BlockID::BirchLog, BlockID::BirchLeaves));

        // Set default densities
        SetStructureDensity("oak_tree", 0.02f);
        SetStructureDensity("birch_tree", 0.01f);

        Log::Debug("Initialized default structure templates");
    }

    void ProceduralChunkGenerator::InitializePassFlags() {
        m_passEnabled[GenerationPass::Terrain] = true;
        m_passEnabled[GenerationPass::Ores] = m_config.generateOres;
        m_passEnabled[GenerationPass::Caves] = m_config.generateCaves;
        m_passEnabled[GenerationPass::Structures] = m_config.generateStructures;
        m_passEnabled[GenerationPass::Vegetation] = m_config.generateVegetation;
        m_passEnabled[GenerationPass::Fluids] = true; // Always generate fluids
        m_passEnabled[GenerationPass::Final] = true; // Always finalize
    }

    // === UTILITY FUNCTIONS ===

    std::string BiomeTypeToString(BiomeType biome) {
        switch (biome) {
            case BiomeType::Plains: return "plains";
            case BiomeType::Forest: return "forest";
            case BiomeType::Mountains: return "mountains";
            case BiomeType::Desert: return "desert";
            case BiomeType::Ocean: return "ocean";
            case BiomeType::Swamp: return "swamp";
            case BiomeType::Tundra: return "tundra";
            case BiomeType::Beach: return "beach";
            default: return "unknown";
        }
    }

    BiomeType StringToBiomeType(const std::string& biomeStr) {
        if (biomeStr == "plains") return BiomeType::Plains;
        if (biomeStr == "forest") return BiomeType::Forest;
        if (biomeStr == "mountains") return BiomeType::Mountains;
        if (biomeStr == "desert") return BiomeType::Desert;
        if (biomeStr == "ocean") return BiomeType::Ocean;
        if (biomeStr == "swamp") return BiomeType::Swamp;
        if (biomeStr == "tundra") return BiomeType::Tundra;
        if (biomeStr == "beach") return BiomeType::Beach;
        return BiomeType::Plains; // Default
    }

    std::unique_ptr<IChunkGenerator> CreateProceduralGenerator(const GenerationConfig& config) {
        return std::make_unique<ProceduralChunkGenerator>(config);
    }

} // namespace Game