// File: src/engine/world/generation/ProceduralChunkGenerator.hpp
#pragma once

#include "../interfaces/IChunkGenerator.hpp"
#include "../../../game/WorldCoordinates.hpp"
#include "../../../core/Log.hpp"
#include <random>
#include <unordered_map>
#include <functional>
#include <memory>

#include "glm/vec3.hpp"

// Forward declare FastNoise
class FastNoiseLite;

namespace Game {

    // Biome types for terrain generation
    enum class BiomeType {
        Plains,
        Forest,
        Mountains,
        Desert,
        Ocean,
        Swamp,
        Tundra,
        Beach
    };

    // Terrain generation parameters per biome
    struct BiomeConfig {
        // Height generation
        float baseHeight = 64.0f;
        float heightVariation = 32.0f;
        float hilliness = 0.5f;

        // Surface blocks
        BlockID surfaceBlock = BlockID::Grass;
        BlockID fillerBlock = BlockID::Dirt;
        BlockID stoneBlock = BlockID::Stone;

        // Generation features
        bool generateTrees = true;
        bool generateFlowers = true;
        bool generateCaves = true;
        float treeChance = 0.02f;
        float flowerChance = 0.05f;

        // Water/liquid settings
        bool hasWater = true;
        BlockID liquidBlock = BlockID::Water;
        int liquidLevel = 62;
    };

    // Structure generation data
    struct StructureData {
        BlockID blockType;
        glm::ivec3 offset;

        StructureData(BlockID block, glm::ivec3 pos) : blockType(block), offset(pos) {}
    };

    // Simple structure templates
    class StructureTemplate {
    public:
        virtual ~StructureTemplate() = default;
        virtual std::vector<StructureData> Generate(std::mt19937& rng) const = 0;
        virtual glm::ivec3 GetSize() const = 0;
        virtual bool CanPlaceAt(int worldX, int worldY, int worldZ, const class INeighborProvider* neighbor) const = 0;
    };

    // Tree structure template
    class TreeTemplate : public StructureTemplate {
    public:
        explicit TreeTemplate(int height = 5, BlockID logType = BlockID::OakLog, BlockID leafType = BlockID::OakLeaves);
        std::vector<StructureData> Generate(std::mt19937& rng) const override;
        glm::ivec3 GetSize() const override;
        bool CanPlaceAt(int worldX, int worldY, int worldZ, const class INeighborProvider* neighbor) const override;

    private:
        int m_height;
        BlockID m_logType;
        BlockID m_leafType;
    };

    // Procedural terrain generator - creates realistic Minecraft-style worlds
    class ProceduralChunkGenerator : public IChunkGenerator {
    public:
        explicit ProceduralChunkGenerator(const GenerationConfig& config = GenerationConfig{});
        ~ProceduralChunkGenerator() override;

        // === CORE GENERATION INTERFACE ===
        ChunkGenerationResult GenerateChunk(Math::ChunkPos position) override;
        std::future<ChunkGenerationResult> GenerateChunkAsync(Math::ChunkPos position) override;
        std::vector<int> GenerateHeightMap(Math::ChunkPos position) override;
        std::string GenerateBiome(Math::ChunkPos position) override;

        // === BATCH GENERATION ===
        std::vector<ChunkGenerationResult> GenerateChunks(const std::vector<Math::ChunkPos>& positions) override;
        std::vector<ChunkGenerationResult> GenerateArea(Math::ChunkPos topLeft, Math::ChunkPos bottomRight) override;

        // === CONFIGURATION ===
        void SetConfig(const GenerationConfig& config) override;
        GenerationConfig GetConfig() const override;
        void SetSeed(int32_t seed) override;
        int32_t GetSeed() const override;
        void SetWorldType(const std::string& worldType) override;
        std::string GetWorldType() const override;

        // === GENERATION LAYERS ===
        void SetPassEnabled(GenerationPass pass, bool enabled) override;
        bool IsPassEnabled(GenerationPass pass) const override;
        ChunkGenerationResult GenerateWithPasses(Math::ChunkPos position,
                                                const std::vector<GenerationPass>& passes) override;

        // === LIFECYCLE ===
        bool Initialize() override;
        void Shutdown() override;
        bool IsReady() const override;

        // === PERFORMANCE ===
        GeneratorStats GetStats() const override;
        void ResetStats() override;
        void SetMaxGenerationTime(float maxTimeMs) override;
        float GetMaxGenerationTime() const override;

        // === VALIDATION ===
        bool ValidateGeneratedChunk(const Chunk& chunk) const override;

        // === CUSTOMIZATION ===
        void RegisterTerrainFunction(const std::string& name, TerrainFunction func) override;
        void RegisterFeatureFunction(const std::string& name, FeatureFunction func) override;
        void SetTerrainFunction(const std::string& name) override;
        void AddFeatureFunction(const std::string& name) override;

        // === DEBUGGING ===
        DebugInfo GetDebugInfo(Math::ChunkPos position) override;
        void SetDebugMode(bool enabled) override;
        bool IsDebugMode() const override;
        std::string GetLastError() const override;
        void ClearErrors() override;

        // === PROCEDURAL-SPECIFIC FEATURES ===

        // Biome configuration
        void SetBiomeConfig(BiomeType biome, const BiomeConfig& config);
        BiomeConfig GetBiomeConfig(BiomeType biome) const;
        BiomeType GetBiomeAt(int worldX, int worldZ) const;

        // Noise configuration
        void SetTerrainNoise(float frequency, float amplitude, int octaves = 4);
        void SetBiomeNoise(float frequency, float scale = 1.0f);
        void SetCaveNoise(float frequency, float threshold = 0.6f);

        // Structure registration
        void RegisterStructureTemplate(const std::string& name, std::shared_ptr<StructureTemplate> templatePtr);
        void SetStructureDensity(const std::string& structureName, float density);

        // Height map caching
        void SetHeightMapCaching(bool enabled);
        void ClearHeightMapCache();

    protected:
        void UpdateStats(const ChunkGenerationResult& result) override;

    private:
        // Configuration
        GenerationConfig m_config;
        mutable std::mutex m_configMutex;

        // Noise generators
        std::unique_ptr<FastNoiseLite> m_terrainNoise;
        std::unique_ptr<FastNoiseLite> m_biomeNoise;
        std::unique_ptr<FastNoiseLite> m_caveNoise;
        std::unique_ptr<FastNoiseLite> m_oreNoise;

        // Biome configurations
        std::unordered_map<BiomeType, BiomeConfig> m_biomeConfigs;

        // Custom generation functions
        std::unordered_map<std::string, TerrainFunction> m_terrainFunctions;
        std::unordered_map<std::string, FeatureFunction> m_featureFunctions;
        std::string m_activeTerrainFunction;
        std::vector<std::string> m_activeFeatureFunctions;

        // Structure templates
        std::unordered_map<std::string, std::shared_ptr<StructureTemplate>> m_structureTemplates;
        std::unordered_map<std::string, float> m_structureDensities;

        // Performance and debugging
        mutable std::mutex m_statsMutex;
        mutable GeneratorStats m_stats;
        bool m_debugMode = false;
        bool m_initialized = false;

        // Error handling
        mutable std::mutex m_errorMutex;
        mutable std::string m_lastError;

        // Generation pass flags
        std::unordered_map<GenerationPass, bool> m_passEnabled;

        // Height map caching
        bool m_heightMapCaching = true;
        mutable std::mutex m_heightMapMutex;
        mutable std::unordered_map<uint64_t, std::vector<int>> m_heightMapCache;

        // === CORE GENERATION METHODS ===

        // Main chunk generation implementation
        ChunkGenerationResult GenerateChunkInternal(Math::ChunkPos position, const std::vector<GenerationPass>& passes);

        // Generation passes
        void GenerateTerrain(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void GenerateOres(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void GenerateCaves(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void GenerateStructures(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void GenerateVegetation(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void GenerateFluids(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);
        void FinalizeChunk(Chunk& chunk, Math::ChunkPos position, std::mt19937& rng);

        // === TERRAIN GENERATION ===

        // Height generation
        int GetTerrainHeight(int worldX, int worldZ, BiomeType biome) const;
        float GetTerrainNoise(int worldX, int worldZ) const;
        float GetBiomeBlendWeight(int worldX, int worldZ, BiomeType targetBiome) const;

        // Surface block selection
        BlockID GetSurfaceBlock(int worldX, int worldY, int worldZ, BiomeType biome) const;
        BlockID GetFillerBlock(int worldX, int worldY, int worldZ, BiomeType biome) const;

        // === BIOME GENERATION ===

        // Biome determination
        BiomeType DetermineBiome(int worldX, int worldZ) const;
        float GetTemperature(int worldX, int worldZ) const;
        float GetHumidity(int worldX, int worldZ) const;

        // === STRUCTURE GENERATION ===

        // Structure placement
        void PlaceStructure(Chunk& chunk, const std::string& structureName,
                           int worldX, int worldY, int worldZ, std::mt19937& rng);
        bool CanPlaceStructure(const std::string& structureName,
                              int worldX, int worldY, int worldZ) const;

        // Tree generation
        void GenerateTree(Chunk& chunk, int worldX, int worldY, int worldZ,
                         BiomeType biome, std::mt19937& rng);

        // === ORE GENERATION ===

        void GenerateOreVein(Chunk& chunk, BlockID oreType, int worldX, int worldY, int worldZ,
                            int veinSize, std::mt19937& rng);
        bool ShouldGenerateOre(BlockID oreType, int worldY, float noiseValue) const;

        // === CAVE GENERATION ===

        bool IsCaveBlock(int worldX, int worldY, int worldZ) const;
        void CarveCave(Chunk& chunk, int worldX, int worldY, int worldZ);

        // === UTILITY METHODS ===

        // Coordinate conversion
        void WorldToLocal(int worldX, int worldY, int worldZ, Math::ChunkPos chunkPos,
                         int& localX, int& localY, int& localZ) const;
        bool IsWorldYValid(int worldY) const;

        // Noise utilities
        float GetNoise(FastNoiseLite& noise, int x, int z) const;
        float GetNoise3D(FastNoiseLite& noise, int x, int y, int z) const;

        // Height map utilities
        uint64_t GetChunkKey(Math::ChunkPos position) const;
        std::vector<int> GetCachedHeightMap(Math::ChunkPos position) const;
        void CacheHeightMap(Math::ChunkPos position, const std::vector<int>& heightMap) const;

        // Validation
        bool IsValidChunkPosition(Math::ChunkPos position) const;

        // Error handling
        void SetLastError(const std::string& error) const;
        void LogError(const std::string& operation, const std::string& error) const;

        // Statistics
        void UpdatePassStats(GenerationPass pass, float timeMs) const;

        // Initialization helpers
        void InitializeNoiseGenerators();
        void InitializeBiomeConfigs();
        void InitializeDefaultStructures();
        void InitializePassFlags();
    };

    // === UTILITY FUNCTIONS ===

    // Biome type conversion
    std::string BiomeTypeToString(BiomeType biome);
    BiomeType StringToBiomeType(const std::string& biomeStr);

    // Factory function for creating procedural generators
    std::unique_ptr<IChunkGenerator> CreateProceduralGenerator(const GenerationConfig& config = GenerationConfig{});

} // namespace Game