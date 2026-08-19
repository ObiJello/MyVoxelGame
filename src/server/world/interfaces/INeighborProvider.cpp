// File: src/server/world/interfaces/INeighborProvider.cpp
//
// Out-of-line only because the fluid-state test needs BlockRegistry, and
// pulling that into INeighborProvider.hpp would drag the whole model/mining
// header chain into every server translation unit that only wants GetBlock.
// Same reasoning, same shape, as IBlockAccess.cpp on the common side.
#include "INeighborProvider.hpp"

#include "common/world/block/BlockRegistry.hpp"

namespace Game {

    bool INeighborProvider::ContainsWater(int worldX, int worldY, int worldZ) const {
        const BlockID id = GetBlock(worldX, worldY, worldZ);
        // Air is most of the world and can never hold water; skipping the
        // state read for it keeps the common case to a single block lookup.
        if (id == BlockID::Air) return false;
        return BlockRegistry::ContainsWater(GetBlockState(worldX, worldY, worldZ));
    }

} // namespace Game
