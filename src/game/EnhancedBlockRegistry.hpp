// File: src/game/EnhancedBlockRegistry.hpp
#pragma once

#include "Blocks.hpp"
#include "BlockModel.hpp"
#include <array>
#include <string>

namespace Game {

    struct EnhancedBlock {
        std::string name;
        bool opaque;
        std::string modelName;  // Reference to BlockModel instead of texture indices

        // Optional override for blocks that don't use standard models
        std::array<uint16_t, 6> legacyTexIdx{0, 0, 0, 0, 0, 0};
        bool useLegacyTextures = false;

        // Rendering hints
        bool enableBiomeTinting = false;  // Whether this block uses biome coloring
        bool isTransparent = false;       // Whether this block has transparent parts
    };

    class EnhancedBlockRegistry {
    public:
        static constexpr size_t Size = static_cast<size_t>(BlockID::Count);

        // Initialize the enhanced block registry
        static void Init();

        // Get enhanced block definition by ID
        static const EnhancedBlock& Get(BlockID id);

        // Check if a block uses model-based rendering
        static bool UsesModelRendering(BlockID id);

        // Get model for a block (returns default if not found)
        static const BlockModel& GetBlockModel(BlockID id);

        // Backing storage for all enhanced blocks
        static std::array<EnhancedBlock, Size> blockDefinitions;

    private:
        EnhancedBlockRegistry() = delete;

        // Helper to register a block with model-based rendering
        static void RegisterModelBlock(BlockID id, const std::string& name, bool opaque,
                                     const std::string& modelName);

        // Helper to register a block with legacy texture indices
        static void RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                      const std::array<uint16_t, 6>& texIndices);
    };

} // namespace Game