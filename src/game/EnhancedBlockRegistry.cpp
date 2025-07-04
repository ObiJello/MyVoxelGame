// File: src/game/EnhancedBlockRegistry.cpp
#include "EnhancedBlockRegistry.hpp"
#include "BlockRegistry.hpp"  // For backward compatibility
#include "../core/Log.hpp"

namespace Game {

    // Define the static array
    std::array<EnhancedBlock, EnhancedBlockRegistry::Size> EnhancedBlockRegistry::blockDefinitions{};

    void EnhancedBlockRegistry::RegisterModelBlock(BlockID id, const std::string& name, bool opaque,
                                                  const std::string& modelName, bool enableBiomeTinting) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterModelBlock", static_cast<unsigned>(id));
            return;
        }

        blockDefinitions[index] = EnhancedBlock{
            .name = name,
            .opaque = opaque,
            .modelName = modelName,
            .legacyTexIdx = {0, 0, 0, 0, 0, 0},
            .useLegacyTextures = false,
            .enableBiomeTinting = enableBiomeTinting,
            .isTransparent = !opaque
        };

        Log::Info("Registered model-based block ID %u as \"%s\" (model=%s, opaque=%s, biome_tint=%s)",
                  static_cast<unsigned>(id), name.c_str(), modelName.c_str(),
                  opaque ? "true" : "false", enableBiomeTinting ? "true" : "false");
    }

    void EnhancedBlockRegistry::RegisterLegacyBlock(BlockID id, const std::string& name, bool opaque,
                                                   const std::array<uint16_t, 6>& texIndices) {
        size_t index = static_cast<size_t>(id);
        if (index >= blockDefinitions.size()) {
            Log::Error("Invalid BlockID %u in RegisterLegacyBlock", static_cast<unsigned>(id));
            return;
        }

        blockDefinitions[index] = EnhancedBlock{
            .name = name,
            .opaque = opaque,
            .modelName = "",
            .legacyTexIdx = texIndices,
            .useLegacyTextures = true,
            .enableBiomeTinting = false,
            .isTransparent = !opaque
        };

        Log::Info("Registered legacy block ID %u as \"%s\" (legacy_textures=%s)",
                  static_cast<unsigned>(id), name.c_str(), opaque ? "opaque" : "transparent");
    }

    void EnhancedBlockRegistry::Init() {
        Log::Info("Initializing Enhanced Block Registry...");

        // Start with some model-based blocks
        RegisterModelBlock(BlockID::Stone, "Stone", true, "stone");
        RegisterModelBlock(BlockID::Dirt, "Dirt", true, "dirt");
        RegisterModelBlock(BlockID::Grass, "Grass", true, "grass_block", true);  // Enable biome tinting
        RegisterModelBlock(BlockID::Sand, "Sand", true, "sand");
        RegisterModelBlock(BlockID::Sandstone, "Sandstone", true, "sandstone");
        RegisterModelBlock(BlockID::OakLog, "Oak Log", true, "oak_log");
        RegisterModelBlock(BlockID::Snow, "Snow", true, "snow");
        RegisterModelBlock(BlockID::SnowGrass, "Snow Grass", true, "grass_block_snow");
        RegisterModelBlock(BlockID::Ice, "Ice", true, "ice");
        RegisterModelBlock(BlockID::Glass, "Glass", false, "glass");  // Transparent
        RegisterModelBlock(BlockID::Bedrock, "Bedrock", true, "bedrock");
        RegisterModelBlock(BlockID::Water, "Water", false, "water");  // Transparent
        RegisterModelBlock(BlockID::Leaves, "Leaves", false, "oak_leaves", true);  // Transparent + biome tinting
        RegisterModelBlock(BlockID::CherryLog, "Cherry Log", true, "cherry_log");
        RegisterModelBlock(BlockID::BirchLog, "Birch Log", true, "birch_log");
        RegisterModelBlock(BlockID::AcaciaLog, "Acacia Log", true, "acacia_log");
        RegisterModelBlock(BlockID::CherryLeaves, "Cherry Leaves", false, "cherry_leaves", true);
        RegisterModelBlock(BlockID::CoalOre, "Coal Ore", true, "coal_ore");
        RegisterModelBlock(BlockID::RedstoneOre, "Redstone Ore", true, "redstone_ore");
        RegisterModelBlock(BlockID::LapisOre, "Lapis Ore", true, "lapis_ore");
        RegisterModelBlock(BlockID::IronOre, "Iron Ore", true, "iron_ore");
        RegisterModelBlock(BlockID::GoldOre, "Gold Ore", true, "gold_ore");
        RegisterModelBlock(BlockID::EmeraldOre, "Emerald Ore", true, "emerald_ore");
        RegisterModelBlock(BlockID::DiamondOre, "Diamond Ore", true, "diamond_ore");
        RegisterModelBlock(BlockID::Gravel, "Gravel", true, "gravel");
        RegisterModelBlock(BlockID::Mycelium, "Mycelium", true, "mycelium", true);  // Biome tinting

        // Air is special case - always use legacy (transparent, no model)
        RegisterLegacyBlock(BlockID::Air, "Air", false, {1008, 1008, 1008, 1008, 1008, 1008});

        Log::Info("Enhanced Block Registry initialization complete - %zu blocks registered",
                 static_cast<size_t>(BlockID::Count));
    }

    const EnhancedBlock& EnhancedBlockRegistry::Get(BlockID id) {
        size_t idx = static_cast<size_t>(id);
        if (idx >= blockDefinitions.size()) {
            Log::Error("EnhancedBlockRegistry::Get() - invalid BlockID %u", static_cast<unsigned>(id));
            // Return air block as fallback
            return blockDefinitions[0];
        }
        return blockDefinitions[idx];
    }

    bool EnhancedBlockRegistry::UsesModelRendering(BlockID id) {
        const EnhancedBlock& block = Get(id);
        return !block.useLegacyTextures;
    }

    const BlockModel& EnhancedBlockRegistry::GetBlockModel(BlockID id) {
        const EnhancedBlock& block = Get(id);
        if (block.useLegacyTextures) {
            Log::Warning("Attempted to get model for legacy block: %s", block.name.c_str());
            return BlockModelRegistry::GetModel(""); // Returns default model
        }
        return BlockModelRegistry::GetModel(block.modelName);
    }

} // namespace Game