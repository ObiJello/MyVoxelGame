// File: src/common/world/gen/IChunkGenerator.hpp
#pragma once

#include "../chunk/Chunk.hpp"
#include "../math/WorldMath.hpp"
#include "../math/WorldCoordinates.hpp"
#include <memory>
#include <string>
#include <vector>
#include <future>
#include <map>
#include <random>

namespace Game {

    // Configuration for chunk generation
    struct GenerationConfig {
        // World generation settings
        int32_t seed = 12345;
        std::string worldType = "default";  // e.g., "default", "superflat", "amplified"

        // Terrain settings
        int seaLevel = 64;
        int bedrockLevel = Math::WorldCoordinates::MIN_WORLD_Y;
        int maxTerrainHeight = 128;
        int minTerrainHeight = 32;

        // Feature generation flags
        bool generateOres = true;
        bool generateCaves = true;
        bool generateStructures = true;
        bool generateBiomes = true;
        bool generateVegetation = true;

        // Performance settings
        bool enableThreading = true;
        int maxGenerationThreads = 4;
        float maxGenerationTimeMs = 10.0f;  // Max time per chunk

        // Validation
        bool IsValid() const {
            return maxTerrainHeight > minTerrainHeight &&
                   seaLevel >= Math::WorldCoordinates::MIN_WORLD_Y &&
                   seaLevel <= Math::WorldCoordinates::MAX_WORLD_Y;
        }
    };

    // Result of a chunk generation operation
    struct ChunkGenerationResult {
        std::shared_ptr<Chunk> chunk;
        bool success = false;
        std::string errorMessage;

        // Generation metadata
        float generationTimeMs = 0.0f;
        size_t blocksGenerated = 0;
        size_t featuresGenerated = 0;
        std::string biome = "unknown";

        ChunkGenerationResult() = default;

        static ChunkGenerationResult Success(std::shared_ptr<Chunk> chunk) {
            ChunkGenerationResult result;
            result.chunk = chunk;
            result.success = true;
            return result;
        }

        static ChunkGenerationResult Failure(const std::string& error) {
            ChunkGenerationResult result;
            result.success = false;
            result.errorMessage = error;
            return result;
        }
    };

    // Abstract interface for chunk generation strategies
    class IChunkGenerator {
    public:
        virtual ~IChunkGenerator() = default;

        // === CORE GENERATION INTERFACE ===

        // Generate a single chunk synchronously
        virtual ChunkGenerationResult GenerateChunk(Math::ChunkPos position) = 0;

        // Generate a chunk asynchronously
        virtual std::future<ChunkGenerationResult> GenerateChunkAsync(Math::ChunkPos position) = 0;

        // Pre-generate terrain data without creating full chunk (for preview/planning)
        virtual std::vector<int> GenerateHeightMap(Math::ChunkPos position) = 0;

        // Generate biome data for a chunk
        virtual std::string GenerateBiome(Math::ChunkPos position) = 0;

        // === BATCH GENERATION ===

        // Generate multiple chunks (more efficient for some generators)
        virtual std::vector<ChunkGenerationResult> GenerateChunks(const std::vector<Math::ChunkPos>& positions) {
            std::vector<ChunkGenerationResult> results;
            results.reserve(positions.size());

            for (const auto& pos : positions) {
                results.push_back(GenerateChunk(pos));
            }

            return results;
        }

        // Generate chunks in a rectangular area
        virtual std::vector<ChunkGenerationResult> GenerateArea(Math::ChunkPos topLeft, Math::ChunkPos bottomRight) {
            std::vector<Math::ChunkPos> positions;

            for (int x = topLeft.x; x <= bottomRight.x; ++x) {
                for (int z = topLeft.z; z <= bottomRight.z; ++z) {
                    positions.push_back({x, z});
                }
            }

            return GenerateChunks(positions);
        }

        // === CONFIGURATION ===

        // Set generation configuration
        virtual void SetConfig(const GenerationConfig& config) = 0;
        virtual GenerationConfig GetConfig() const = 0;

        // Set world seed
        virtual void SetSeed(int32_t seed) = 0;
        virtual int32_t GetSeed() const = 0;

        // Set world type (affects generation algorithm)
        virtual void SetWorldType(const std::string& worldType) = 0;
        virtual std::string GetWorldType() const = 0;

        // === GENERATION LAYERS ===

        // Generation happens in multiple passes/layers
        enum class GenerationPass {
            Terrain,     // Basic terrain shape
            Ores,        // Ore distribution
            Caves,       // Cave systems
            Structures,  // Buildings, dungeons
            Vegetation,  // Trees, grass, flowers
            Fluids,      // Water, lava lakes
            Final        // Final cleanup pass
        };

        // Enable/disable specific generation passes
        virtual void SetPassEnabled(GenerationPass pass, bool enabled) = 0;
        virtual bool IsPassEnabled(GenerationPass pass) const = 0;

        // Generate only specific passes (for debugging/testing)
        virtual ChunkGenerationResult GenerateWithPasses(Math::ChunkPos position,
                                                        const std::vector<GenerationPass>& passes) = 0;

        // === LIFECYCLE ===

        // Initialize the generator
        virtual bool Initialize() = 0;

        // Shutdown the generator
        virtual void Shutdown() = 0;

        // Check if ready to generate
        virtual bool IsReady() const = 0;

        // === PERFORMANCE ===

        // Generation statistics
        struct GeneratorStats {
            size_t chunksGenerated = 0;
            float totalGenerationTimeMs = 0.0f;
            float averageGenerationTimeMs = 0.0f;
            size_t totalBlocksGenerated = 0;
            size_t totalFeaturesGenerated = 0;

            // Per-pass statistics
            std::map<GenerationPass, float> passTimeMs;

            void Reset() {
                chunksGenerated = 0;
                totalGenerationTimeMs = averageGenerationTimeMs = 0.0f;
                totalBlocksGenerated = totalFeaturesGenerated = 0;
                passTimeMs.clear();
            }
        };

        virtual GeneratorStats GetStats() const = 0;
        virtual void ResetStats() = 0;

        // Set performance limits
        virtual void SetMaxGenerationTime(float maxTimeMs) = 0;
        virtual float GetMaxGenerationTime() const = 0;

        // === VALIDATION ===

        // Validate generated chunk meets quality standards
        virtual bool ValidateGeneratedChunk(const Chunk& chunk) const {
            // Default validation
            if (chunk.IsEmpty()) {
                return false;
            }

            // Check for reasonable block distribution
            size_t nonAirBlocks = chunk.GetNonAirBlockCount();
            size_t totalBlocks = chunk.GetBlockCount();

            // Chunk should have some blocks but not be completely solid
            return nonAirBlocks > 0 && nonAirBlocks < totalBlocks;
        }

        // === CUSTOMIZATION ===

        // Register custom generation functions
        using TerrainFunction = std::function<void(Chunk&, Math::ChunkPos)>;
        using FeatureFunction = std::function<void(Chunk&, Math::ChunkPos, std::mt19937&)>;

        virtual void RegisterTerrainFunction(const std::string& name, TerrainFunction func) = 0;
        virtual void RegisterFeatureFunction(const std::string& name, FeatureFunction func) = 0;

        // Use custom functions
        virtual void SetTerrainFunction(const std::string& name) = 0;
        virtual void AddFeatureFunction(const std::string& name) = 0;

        // === DEBUGGING ===

        // Generate debug information about a chunk
        struct DebugInfo {
            std::string biome;
            std::vector<int> heightMap;
            std::map<std::string, int> blockCounts;
            std::vector<std::string> featuresGenerated;
            float generationTimePerPass[7]; // One for each GenerationPass
        };

        virtual DebugInfo GetDebugInfo(Math::ChunkPos position) = 0;

        // Enable/disable debug mode (slower but more detailed)
        virtual void SetDebugMode(bool enabled) = 0;
        virtual bool IsDebugMode() const = 0;

        // === ERROR HANDLING ===

        virtual std::string GetLastError() const = 0;
        virtual void ClearErrors() = 0;

    protected:
        // Helper for implementations to update statistics
        virtual void UpdateStats(const ChunkGenerationResult& result) {
            // Override in derived classes
        }
    };

    // Factory function type for creating chunk generators
    using ChunkGeneratorFactory = std::function<std::unique_ptr<IChunkGenerator>()>;

} // namespace Game