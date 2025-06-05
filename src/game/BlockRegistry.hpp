#pragma once

#include "Blocks.hpp"
#include <array>

namespace Game {

    class BlockRegistry {
    public:
        // Number of registered block types (BlockID::Count).
        static constexpr size_t Size = static_cast<size_t>(BlockID::Count);

        // Call once at startup to fill in the static blockDefinitions[] array.
        static void Init();

        // Retrieve a Block by its ID. Assumes Init() was called.
        static const Block& Get(BlockID id);

        // Backing storage for all blocks, indexed by (uint16_t) BlockID.
        // Made public so Init() can populate it.
        static std::array<Block, Size> blockDefinitions;

    private:
        // Prevent instantiation.
        BlockRegistry() = delete;
    };

} // namespace Game