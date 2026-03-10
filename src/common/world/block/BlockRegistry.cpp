// File: src/common/world/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "../../core/Log.hpp"

namespace Game {

    // Define the static array
    std::array<Block, BlockRegistry::Size> BlockRegistry::blockDefinitions{};

    void BlockRegistry::RegisterModelBlock(BlockID id, const std::string& name, RenderLayer layer,
                                              const std::string& modelName) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterModelBlock", static_cast<unsigned>(id));
            return;
        }

        bool opaque = (layer == RenderLayer::Opaque);
        blockDefinitions[index] = Block{
            .name = name,
            .opaque = opaque,
            .modelName = modelName,
            .legacyTexIdx = {0, 0, 0, 0, 0, 0},
            .useLegacyTextures = false,
            .isTransparent = !opaque,
            .renderLayer = layer
        };

        Log::Info("Registered model-based block ID %u as \"%s\" (model=%s, opaque=%s)",
                  static_cast<unsigned>(id), name.c_str(), modelName.c_str(),
                  opaque ? "true" : "false");
    }

    void BlockRegistry::RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                                   const std::array<uint16_t, 6>& texIndices) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterLegacyBlock", static_cast<unsigned>(id));
            return;
        }

        blockDefinitions[index] = Block{
            .name = name,
            .opaque = opaque,
            .modelName = "",
            .legacyTexIdx = texIndices,
            .useLegacyTextures = true,
            .enableBiomeTinting = false,
            .isTransparent = !opaque,
            .renderLayer = opaque ? RenderLayer::Opaque : RenderLayer::Cutout
        };

        Log::Info("Registered legacy block ID %u as \"%s\" (legacy_textures=%s)",
                  static_cast<unsigned>(id), name.c_str(), opaque ? "opaque" : "transparent");
    }

    void BlockRegistry::Init() {
        Log::Info("Initializing Block Registry...");

        // SPECIAL: Air block - always transparent, no model
        RegisterLegacyBlock(BlockID::Air, "Air", false, {1008, 1008, 1008, 1008, 1008, 1008});

        // All blocks from BlockDefs.inc (single source of truth)
        #define BLOCK_DEF(e, m, d, r) RegisterModelBlock(BlockID::e, d, Game::RenderLayer::r, m);
        #include "BlockDefs.inc"
        #undef BLOCK_DEF


        // Manual entries not in all_blocks.txt
        RegisterModelBlock(BlockID::SnowGrass, "Snow Grass", RenderLayer::Opaque, "grass_block_snow");

        // Override model names for blocks where minecraft ID != model file name
        RegisterModelBlock(BlockID::Water, "Water", RenderLayer::Translucent, "water_still");
        RegisterModelBlock(BlockID::Lava, "Lava", RenderLayer::Translucent, "lava_still");

        Log::Info("Block Registry initialization complete - %zu blocks registered",
                 static_cast<size_t>(BlockID::Count));
    }

    const Block& BlockRegistry::Get(BlockID id) {
        size_t idx = static_cast<size_t>(id);
        if (idx >= blockDefinitions.size()) {
            Log::Error("BlockRegistry::Get() - invalid BlockID %u", static_cast<unsigned>(id));
            // Return air block as fallback
            return blockDefinitions[0];
        }
        return blockDefinitions[idx];
    }

    bool BlockRegistry::UsesModelRendering(BlockID id) {
        const Block& block = Get(id);
        return !block.useLegacyTextures;
    }

    const BlockModel& BlockRegistry::GetBlockModel(BlockID id) {
        const Block& block = Get(id);
        return BlockModelRegistry::GetModel(block.modelName);
    }

} // namespace Game
