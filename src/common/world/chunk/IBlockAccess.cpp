// File: src/common/world/chunk/IBlockAccess.cpp
//
// Out-of-line only because the sky-exposure walk needs BlockRegistry, and
// pulling that into IBlockAccess.hpp would drag the whole model/mining header
// chain into every physics and mesher translation unit that only wants
// GetBlock.
#include "IBlockAccess.hpp"

#include "../block/BlockRegistry.hpp"

namespace Game {

    namespace {
        // Mirrors the question MC's light engine asks of each block on the way
        // up: does this cell stop sky light? MC uses per-state opacity, where
        // leaves and water cost one level rather than blocking outright. With
        // no light levels to spend, the closest honest reading is "opaque
        // blocks roof a column, everything else does not" — which keeps crops
        // growing under a tree and under glass, both of which match vanilla.
        bool BlocksSkyLight(BlockID id) {
            if (id == BlockID::Air) return false;
            // `opaque` is set from the block's render layer at registration
            // (RegisterModelBlock: `opaque = layer == Opaque`) and is what the
            // mesher culls faces against — i.e. exactly the blocks that would
            // cast a shadow. Cutout blocks (leaves, crops, glass panes) and
            // translucent ones (glass, water) let sky through, which is the
            // right answer for all three: MC's leaves cost one light level, not
            // fifteen, and a crop under glass grows.
            return BlockRegistry::Get(id).opaque;
        }

        // World::MAX_Y, restated to avoid a dependency cycle: World.hpp
        // includes this header transitively.
        constexpr int kMaxBuildY = 319;
    } // namespace

    int IBlockAccess::GetRawBrightness(int worldX, int worldY, int worldZ) const {
        // Anything above the build limit is sky by definition.
        for (int y = worldY + 1; y <= kMaxBuildY; ++y) {
            // An unloaded neighbour column is not evidence of a roof. Treating
            // it as one would make crops at a chunk border stall until the
            // neighbour streamed in, and then start again — a difference the
            // player would read as random.
            if (!IsPositionLoaded(worldX, y, worldZ)) break;
            if (BlocksSkyLight(GetBlock(worldX, y, worldZ))) return 0;
        }
        return 15;
    }

} // namespace Game
