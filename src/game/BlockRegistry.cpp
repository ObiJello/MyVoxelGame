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
        // 0: Air (transparent; no faces rendered)
        RegisterBlock(BlockID::Air,   "Air",   false, {0,0,0,0,0,0});

        // 1: Stone (opaque; same texture on all faces) - Try index 1 first, if that doesn't work try 0
        RegisterBlock(BlockID::Stone, "Stone", true,  {1,1,1,1,1,1});

        // 2: Dirt (opaque; same texture on all faces)
        RegisterBlock(BlockID::Dirt,  "Dirt",  true,  {2,2,2,2,2,2});

        // 3: Grass (opaque; top=3, bottom=2, sides=4)
        RegisterBlock(BlockID::Grass, "Grass", true,  {1,1,1,1,1,1});

        // … Add more blocks here as needed.
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