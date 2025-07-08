// File: src/engine/block/BlockRegistry.cpp
#include "BlockRegistry.hpp"
#include "BlockRegistry.hpp"
#include "../../core/Log.hpp"

namespace Game {

    // Define the static array
    std::array<Block, BlockRegistry::Size> BlockRegistry::blockDefinitions{};

    void BlockRegistry::RegisterModelBlock(BlockID id, const std::string& name, bool opaque,
                                              const std::string& modelName) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterModelBlock", static_cast<unsigned>(id));
            return;
        }

        blockDefinitions[index] = Block{
            .name = name,
            .opaque = opaque,
            .modelName = modelName,
            .legacyTexIdx = {0, 0, 0, 0, 0, 0},
            .useLegacyTextures = false,
            .isTransparent = !opaque  // FIXED: Set transparency correctly
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
            .isTransparent = !opaque  // FIXED: Set transparency correctly
        };

        Log::Info("Registered legacy block ID %u as \"%s\" (legacy_textures=%s)",
                  static_cast<unsigned>(id), name.c_str(), opaque ? "opaque" : "transparent");
    }

    void BlockRegistry::Init() {
        Log::Info("Initializing Block Registry...");

        // SPECIAL: Air block - always transparent, no model
        RegisterLegacyBlock(BlockID::Air, "Air", false, {1008, 1008, 1008, 1008, 1008, 1008});

        // FIXED: Set proper opacity flags based on Minecraft block properties
        RegisterModelBlock(BlockID::Stone, "Stone", true, "stone");                   // Opaque
        RegisterModelBlock(BlockID::Dirt, "Dirt", true, "dirt");                      // Opaque
        RegisterModelBlock(BlockID::Grass, "Grass", true, "grass_block");             // Opaque
        RegisterModelBlock(BlockID::Sand, "Sand", true, "sand");                      // Opaque
        RegisterModelBlock(BlockID::Sandstone, "Sandstone", true, "sandstone");       // Opaque
        RegisterModelBlock(BlockID::OakLog, "Oak Log", true, "oak_log");              // Opaque
        RegisterModelBlock(BlockID::Snow, "Snow", true, "snow_block");                // Opaque
        RegisterModelBlock(BlockID::SnowGrass, "Snow Grass", true, "grass_block_snow"); // Opaque
        RegisterModelBlock(BlockID::Ice, "Ice", false, "ice");                        // Transparent
        RegisterModelBlock(BlockID::Glass, "Glass", false, "glass");                  // Transparent
        RegisterModelBlock(BlockID::Bedrock, "Bedrock", true, "bedrock");             // Opaque
        RegisterModelBlock(BlockID::Water, "Water", false, "water");                  // Transparent
        RegisterModelBlock(BlockID::Leaves, "Leaves", false, "oak_leaves");           // Transparent
        RegisterModelBlock(BlockID::CherryLog, "Cherry Log", true, "cherry_log");     // Opaque
        RegisterModelBlock(BlockID::BirchLog, "Birch Log", true, "birch_log");        // Opaque
        RegisterModelBlock(BlockID::AcaciaLog, "Acacia Log", true, "acacia_log");     // Opaque
        RegisterModelBlock(BlockID::CherryLeaves, "Cherry Leaves", false, "cherry_leaves"); // Transparent
        RegisterModelBlock(BlockID::CoalOre, "Coal Ore", true, "coal_ore");           // Opaque
        RegisterModelBlock(BlockID::RedstoneOre, "Redstone Ore", true, "redstone_ore"); // Opaque
        RegisterModelBlock(BlockID::LapisOre, "Lapis Ore", true, "lapis_ore");        // Opaque
        RegisterModelBlock(BlockID::IronOre, "Iron Ore", true, "iron_ore");           // Opaque
        RegisterModelBlock(BlockID::GoldOre, "Gold Ore", true, "gold_ore");           // Opaque
        RegisterModelBlock(BlockID::EmeraldOre, "Emerald Ore", true, "emerald_ore");  // Opaque
        RegisterModelBlock(BlockID::DiamondOre, "Diamond Ore", true, "diamond_ore");  // Opaque
        RegisterModelBlock(BlockID::Gravel, "Gravel", true, "gravel");                // Opaque
        RegisterModelBlock(BlockID::Mycelium, "Mycelium", true, "mycelium");          // Opaque
        RegisterModelBlock(BlockID::Deepslate, "Deepslate", true, "deepslate");       // Opaque
        RegisterModelBlock(BlockID::Lava, "Lava", false, "lava");                     // Transparent



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