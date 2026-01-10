// File: src/server/world/MyTerrainGenerator.hpp
#pragma once

#include "common/world/gen/IChunkGenerator.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Log.hpp"

// Your terrain library includes
#include "levelgen/NoiseRegistry.h"
#include "levelgen/DensityFunctionRegistry.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/NoiseRouterData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Beardifier.h"
#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRuleData.h"
#include "levelgen/ChunkGenerationRunner.h"
#include "world/ProtoChunk.h"
#include "world/MinecraftBlockType.h"
#include "world/biome/MultiNoiseBiomeSource.h"

namespace Game {

    /**
     * Wrapper generator that integrates your custom terrain library
     * with the game's IChunkGenerator interface.
     */
    class MyTerrainGenerator : public IChunkGenerator {
    public:
        explicit MyTerrainGenerator(const GenerationConfig& config);
        ~MyTerrainGenerator() override;

        // IChunkGenerator interface implementation
        bool Initialize() override;
        void Shutdown() override;
        ChunkGenerationResult GenerateChunk(Math::ChunkPos position) override;
        std::future<ChunkGenerationResult> GenerateChunkAsync(Math::ChunkPos position) override;
        std::vector<int> GenerateHeightMap(Math::ChunkPos position) override;
        std::string GenerateBiome(Math::ChunkPos position) override;

        void SetConfig(const GenerationConfig& config) override;
        GenerationConfig GetConfig() const override;
        void SetSeed(int32_t seed) override;
        int32_t GetSeed() const override;
        void SetWorldType(const std::string& worldType) override;
        std::string GetWorldType() const override;

        void SetPassEnabled(GenerationPass pass, bool enabled) override;
        bool IsPassEnabled(GenerationPass pass) const override;
        ChunkGenerationResult GenerateWithPasses(Math::ChunkPos position, const std::vector<GenerationPass>& passes) override;

        bool IsReady() const override;

        GeneratorStats GetStats() const override;
        void ResetStats() override;
        void SetMaxGenerationTime(float maxTimeMs) override;
        float GetMaxGenerationTime() const override;

        void RegisterTerrainFunction(const std::string& name, TerrainFunction func) override;
        void RegisterFeatureFunction(const std::string& name, FeatureFunction func) override;
        void SetTerrainFunction(const std::string& name) override;
        void AddFeatureFunction(const std::string& name) override;

        DebugInfo GetDebugInfo(Math::ChunkPos position) override;
        void SetDebugMode(bool enabled) override;
        bool IsDebugMode() const override;

        std::string GetLastError() const override;
        void ClearErrors() override;

    private:
        GenerationConfig m_config;
        GeneratorStats m_stats;
        bool m_initialized = false;

        // Your terrain library components (initialized once)
        minecraft::levelgen::NoiseBasedChunkGenerator* m_generator = nullptr;
        minecraft::levelgen::RandomState* m_randomState = nullptr;
        minecraft::levelgen::NoiseGeneratorSettings* m_settings = nullptr;
        minecraft::levelgen::FluidPicker* m_fluidPicker = nullptr;
        minecraft::levelgen::Beardifier* m_beardifier = nullptr;
        minecraft::levelgen::SurfaceSystem* m_surfaceSystem = nullptr;
        minecraft::levelgen::RuleSource* m_surfaceRules = nullptr;
        minecraft::levelgen::ChunkGenerationRunner* m_runner = nullptr;
        minecraft::world::biome::MultiNoiseBiomeSource* m_biomeSource = nullptr;
        minecraft::world::BlockRegistry* m_blockRegistry = nullptr;

        // Helper to map your library's block types to game BlockIDs
        BlockID MapBlockType(world::IBlockType* blockType) const;
    };

} // namespace Game