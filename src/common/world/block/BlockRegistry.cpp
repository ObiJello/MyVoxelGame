// File: src/common/world/block/BlockRegistry.cpp
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
            .isTransparent = !opaque
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
            .isTransparent = !opaque
        };

        Log::Info("Registered legacy block ID %u as \"%s\" (legacy_textures=%s)",
                  static_cast<unsigned>(id), name.c_str(), opaque ? "opaque" : "transparent");
    }

    void BlockRegistry::Init() {
        Log::Info("Initializing Block Registry...");

        // SPECIAL: Air block - always transparent, no model
        RegisterLegacyBlock(BlockID::Air, "Air", false, {1008, 1008, 1008, 1008, 1008, 1008});

        // SOLID BLOCKS (Opaque layer)
        RegisterModelBlock(BlockID::Stone, "Stone", true, "stone");
        RegisterModelBlock(BlockID::Dirt, "Dirt", true, "dirt");
        RegisterModelBlock(BlockID::Grass, "Grass", true, "grass_block");
        RegisterModelBlock(BlockID::Sand, "Sand", true, "sand");
        RegisterModelBlock(BlockID::Sandstone, "Sandstone", true, "sandstone");
        RegisterModelBlock(BlockID::OakLog, "Oak Log", true, "oak_log");
        RegisterModelBlock(BlockID::Snow, "Snow", true, "snow_block");
        RegisterModelBlock(BlockID::SnowGrass, "Snow Grass", true, "grass_block_snow");
        RegisterModelBlock(BlockID::Bedrock, "Bedrock", true, "bedrock");
        RegisterModelBlock(BlockID::CherryLog, "Cherry Log", true, "cherry_log");
        RegisterModelBlock(BlockID::BirchLog, "Birch Log", true, "birch_log");
        RegisterModelBlock(BlockID::AcaciaLog, "Acacia Log", true, "acacia_log");
        RegisterModelBlock(BlockID::CoalOre, "Coal Ore", true, "coal_ore");
        RegisterModelBlock(BlockID::RedstoneOre, "Redstone Ore", true, "redstone_ore");
        RegisterModelBlock(BlockID::LapisOre, "Lapis Ore", true, "lapis_ore");
        RegisterModelBlock(BlockID::IronOre, "Iron Ore", true, "iron_ore");
        RegisterModelBlock(BlockID::GoldOre, "Gold Ore", true, "gold_ore");
        RegisterModelBlock(BlockID::EmeraldOre, "Emerald Ore", true, "emerald_ore");
        RegisterModelBlock(BlockID::DiamondOre, "Diamond Ore", true, "diamond_ore");
        RegisterModelBlock(BlockID::Gravel, "Gravel", true, "gravel");
        RegisterModelBlock(BlockID::Mycelium, "Mycelium", true, "mycelium");
        RegisterModelBlock(BlockID::Deepslate, "Deepslate", true, "deepslate");
        RegisterModelBlock(BlockID::Diorite,                       "Diorite",                        true,  "diorite");
        RegisterModelBlock(BlockID::Andesite,                      "Andesite",                       true,  "andesite");
        RegisterModelBlock(BlockID::Granite,                       "Granite",                        true,  "granite");
        RegisterModelBlock(BlockID::CopperOre,                     "Copper Ore",                     true,  "copper_ore");
        RegisterModelBlock(BlockID::DeepslateLapisOre,             "Deepslate Lapis Ore",            true,  "deepslate_lapis_ore");
        RegisterModelBlock(BlockID::DeepslateIronOre,              "Deepslate Iron Ore",             true,  "deepslate_iron_ore");
        RegisterModelBlock(BlockID::DeepslateGoldOre,              "Deepslate Gold Ore",             true,  "deepslate_gold_ore");
        RegisterModelBlock(BlockID::DeepslateDiamondOre,           "Deepslate Diamond Ore",          true,  "deepslate_diamond_ore");
        RegisterModelBlock(BlockID::DeepslateCopperOre,            "Deepslate Copper Ore",           true,  "deepslate_copper_ore");
        RegisterModelBlock(BlockID::Tuff,                          "Tuff",                           true,  "tuff");
        RegisterModelBlock(BlockID::ChiseledTuffBricks,            "Chiseled Tuff Bricks",           true,  "chiseled_tuff_bricks");
        RegisterModelBlock(BlockID::TuffBricks,                    "Tuff Bricks",                    true,  "tuff_bricks");
        RegisterModelBlock(BlockID::WaxedCutCopper,                "Waxed Cut Copper",               true,  "waxed_cut_copper");
        RegisterModelBlock(BlockID::waxed_chiseled_copper,         "Waxed Chiseled Copper",          true,  "waxed_chiseled_copper");
        RegisterModelBlock(BlockID::Cobblestone,                   "Cobblestone",                    true,  "cobblestone");
        RegisterModelBlock(BlockID::MossyCobblestone,              "Mossy Cobblestone",              true,  "mossy_cobblestone");
        RegisterModelBlock(BlockID::polished_tuff,                 "Polished Tuff",                  true,  "polished_tuff");
        RegisterModelBlock(BlockID::waxed_oxidized_chiseled_copper,"Waxed Oxidized Chiseled Copper", true,  "waxed_oxidized_chiseled_copper");

        // CUTOUT BLOCKS (Alpha-test layer)
        RegisterModelBlock(BlockID::OakLeaves, "Oak Leaves", false, "oak_leaves");
        RegisterModelBlock(BlockID::BirchLeaves, "Birch Leaves", false, "birch_leaves");
        RegisterModelBlock(BlockID::CherryLeaves, "Cherry Leaves", false, "cherry_leaves");
        RegisterModelBlock(BlockID::Spawner,                       "Spawner",                        false,  "spawner");

        // TRANSLUCENT BLOCKS (Blended layer)
        RegisterModelBlock(BlockID::Ice, "Ice", false, "ice");
        RegisterModelBlock(BlockID::Glass, "Glass", false, "glass");

        // FLUID BLOCKS (Special translucent layer)
        RegisterModelBlock(BlockID::Water, "Water", false, "water_still");
        RegisterModelBlock(BlockID::Lava, "Lava", true, "lava_still");


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