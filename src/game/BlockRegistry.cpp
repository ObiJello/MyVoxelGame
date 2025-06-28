#include "BlockRegistry.hpp"
#include "Log.hpp"

namespace Game {

    // Define (and default‐construct) the static array:
    std::array<Block, BlockRegistry::Size> BlockRegistry::blockDefinitions{};

    static void RegisterBlock(BlockID id, const std::string& name, bool opaque, std::array<uint16_t,6> texIndices) {
        size_t index = static_cast<size_t>(id);
        BlockRegistry::blockDefinitions[index] = Block{ name, opaque, texIndices };
        Log::Info("Registered block ID %u as \"%s\" (opaque=%s)",
                  static_cast<unsigned>(id),
                  name.c_str(),
                  opaque ? "true" : "false");
    }

    void BlockRegistry::Init() {
        // Texture indices: [Right, Left, Top, Bottom, Front, Back] = [+X, -X, +Y, -Y, +Z, -Z]

        // 0: Air (transparent; no faces rendered) - Uses texture index 1008
        RegisterBlock(BlockID::Air, "Air", false, {1008,1008,1008,1008,1008,1008});

        // 1: Stone (opaque; same texture on all faces) - Pixel (16,0) = Index 1
        RegisterBlock(BlockID::Stone, "Stone", true, {1,1,1,1,1,1});

        // 2: Dirt (opaque; same texture on all faces) - Pixel (0,0) = Index 0
        RegisterBlock(BlockID::Dirt, "Dirt", true, {0,0,0,0,0,0});

        // 3: Grass (opaque; different textures per face)
        // Top: grass_top (32,0) = Index 2, Bottom: dirt (0,0) = Index 0, Sides: grass_side (32,16) = Index 18
        RegisterBlock(BlockID::Grass, "Grass", true, {18,18,2,0,18,18});

        // 4: Sand (opaque; same texture on all faces) - Pixel (0,16) = Index 16
        RegisterBlock(BlockID::Sand, "Sand", true, {16,16,16,16,16,16});

        // 5: Sandstone (opaque; different textures)
        // Top: sandstone_top (0,64) = Index 64, Bottom: sandstone_bottom (0,48) = Index 48, Sides: sandstone_side (0,32) = Index 32
        RegisterBlock(BlockID::Sandstone, "Sandstone", true, {32,32,64,48,32,32});

        // 6: Oak Log (opaque; different textures)
        // Top/Bottom: wood_top (48,16) = Index 19, Sides: wood_side (48,0) = Index 3
        RegisterBlock(BlockID::OakLog, "Oak Log", true, {3,3,19,19,3,3});

        // 7: Snow (opaque; same texture on all faces) - Pixel (16,16) = Index 17
        RegisterBlock(BlockID::Snow, "Snow", true, {17,17,17,17,17,17});

        // 8: Snow Grass (opaque; different textures)
        // Top: snow (16,16) = Index 17, Bottom: dirt (0,0) = Index 0, Sides: snowgrass (16,32) = Index 33
        RegisterBlock(BlockID::SnowGrass, "Snow Grass", true, {33,33,17,0,33,33});

        // 9: Ice (opaque; same texture on all faces) - Pixel (16,48) = Index 49
        RegisterBlock(BlockID::Ice, "Ice", true, {49,49,49,49,49,49});

        // 10: Glass (transparent; same texture on all faces) - Pixel (64,0) = Index 4
        RegisterBlock(BlockID::Glass, "Glass", false, {4,4,4,4,4,4});

        // 11: Bedrock (opaque; same texture on all faces) - Pixel (80,0) = Index 5
        RegisterBlock(BlockID::Bedrock, "Bedrock", true, {5,5,5,5,5,5});

        // 12: Water (transparent; different textures)
        // Top/Bottom: water (96,0) = Index 6, Sides: water_side (112,0) = Index 7
        RegisterBlock(BlockID::Water, "Water", false, {7,7,6,6,7,7});

        // 13: Leaves (transparent; same texture on all faces) - Pixel (80,16) = Index 21
        RegisterBlock(BlockID::Leaves, "Leaves", false, {21,21,21,21,21,21});

        // 14: Cherry Log (opaque; different textures)
        // Top/Bottom: cherry_log_top (32,64) = Index 66, Sides: cherry_log_side (16,64) = Index 65
        RegisterBlock(BlockID::CherryLog, "Cherry Log", true, {65,65,66,66,65,65});

        // 15: Birch Log (opaque; different textures)
        // Top/Bottom: birch_log_top (48,48) = Index 51, Sides: birch_log_side (48,64) = Index 67
        RegisterBlock(BlockID::BirchLog, "Birch Log", true, {67,67,51,51,67,67});

        // 16: Acacia Log (opaque; different textures)
        // Top/Bottom: acacia_log_top (64,48) = Index 52, Sides: acacia_log_side (64,64) = Index 68
        RegisterBlock(BlockID::AcaciaLog, "Acacia Log", true, {68,68,52,52,68,68});

        // 17: Cherry Leaves (transparent; same texture on all faces) - Pixel (80,32) = Index 37
        RegisterBlock(BlockID::CherryLeaves, "Cherry Leaves", false, {37,37,37,37,37,37});

        // 18: Coal Ore (opaque; same texture on all faces) - Pixel (0,80) = Index 80
        RegisterBlock(BlockID::CoalOre, "Coal Ore", true, {80,80,80,80,80,80});

        // 19: Redstone Ore (opaque; same texture on all faces) - Pixel (16,80) = Index 81
        RegisterBlock(BlockID::RedstoneOre, "Redstone Ore", true, {81,81,81,81,81,81});

        // 20: Lapis Ore (opaque; same texture on all faces) - Pixel (32,80) = Index 82
        RegisterBlock(BlockID::LapisOre, "Lapis Ore", true, {82,82,82,82,82,82});

        // 21: Iron Ore (opaque; same texture on all faces) - Pixel (48,80) = Index 83
        RegisterBlock(BlockID::IronOre, "Iron Ore", true, {83,83,83,83,83,83});

        // 22: Gold Ore (opaque; same texture on all faces) - Pixel (64,80) = Index 84
        RegisterBlock(BlockID::GoldOre, "Gold Ore", true, {84,84,84,84,84,84});

        // 23: Emerald Ore (opaque; same texture on all faces) - Pixel (80,80) = Index 85
        RegisterBlock(BlockID::EmeraldOre, "Emerald Ore", true, {85,85,85,85,85,85});

        // 24: Diamond Ore (opaque; same texture on all faces) - Pixel (0,96) = Index 96
        RegisterBlock(BlockID::DiamondOre, "Diamond Ore", true, {96,96,96,96,96,96});

        // 25: Gravel (opaque; same texture on all faces) - Pixel (0,112) = Index 112
        RegisterBlock(BlockID::Gravel, "Gravel", true, {112,112,112,112,112,112});

        // 26: Mycelium (opaque; different textures)
        // Top: mycelium_top (80,48) = Index 53, Bottom: dirt (0,0) = Index 0, Sides: mycelium_side (80,64) = Index 69
        RegisterBlock(BlockID::Mycelium, "Mycelium", true, {69,69,53,0,69,69});
    }

    const Block& BlockRegistry::Get(BlockID id) {
        size_t idx = static_cast<size_t>(id);
#ifndef NDEBUG
        if (idx >= blockDefinitions.size()) {
            Log::Error("BlockRegistry::Get() - invalid BlockID %u", static_cast<unsigned>(id));
            std::abort();
        }
#endif
        return blockDefinitions[idx];
    }

} // namespace Game