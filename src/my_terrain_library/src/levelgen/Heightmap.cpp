#include "levelgen/Heightmap.h"
#include "world/level/block/state/BlockState.h"
#include "world/IChunk.h"
#include "core/BlockPos.h"

namespace minecraft {
namespace levelgen {

// Reference: Heightmap.java lines 43-80
void Heightmap::primeHeightmaps(world::IChunk* chunk, const std::set<Types>& types) {
    if (types.empty() || chunk == nullptr) return;

    // Get heightmaps for all requested types
    std::vector<Heightmap*> heightmaps;
    heightmaps.reserve(types.size());
    for (Types type : types) {
        auto* hm = chunk->getOrCreateHeightmapUnprimed(static_cast<int>(type));
        if (hm) heightmaps.push_back(hm);
    }

    if (heightmaps.empty()) return;

    // Get the highest section position
    int32_t highestSectionPosition = chunk->getHighestSectionPosition() + 16;
    int32_t minY = chunk->getMinY();

    // Scan each column from top to bottom
    // Reference: Java iterates x then z (for x in 0..15, for z in 0..15)
    for (int32_t x = 0; x < 16; ++x) {
        for (int32_t z = 0; z < 16; ++z) {
            // Reset which heightmaps are still looking for their surface
            std::vector<Heightmap*> active = heightmaps;

            // Scan downward from highest section to minY
            for (int32_t y = highestSectionPosition - 1; y >= minY && !active.empty(); --y) {
                // Get block at this position
                BlockState* block = chunk->getBlockState(x, y, z);

                // If not air, check each heightmap
                if (block && !block->isAir()) {
                    auto it = active.begin();
                    while (it != active.end()) {
                        Heightmap* hm = *it;
                        // If this block matches the heightmap's opaque predicate
                        if (hm->getIsOpaque()(block)) {
                            // Set this heightmap's value for this column
                            hm->setHeight(x, z, y + 1);
                            // Remove from active list
                            it = active.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            }

            // Any heightmaps that didn't find an opaque block remain at their default (0 + minY)
        }
    }
}

// Reference: Heightmap.java lines 156-177 (Types enum predicates)
Heightmap::OpaquePredicate Heightmap::getOpaquePredicate(Types type) {
    switch (type) {
        case Types::WORLD_SURFACE_WG:
        case Types::WORLD_SURFACE:
            // NOT_AIR: Any non-air block
            return [](const BlockState* block) -> bool {
                return block != nullptr && !block->isAir();
            };

        case Types::OCEAN_FLOOR_WG:
        case Types::OCEAN_FLOOR:
            // MATERIAL_MOTION_BLOCKING: Blocks that block motion
            return [](const BlockState* block) -> bool {
                if (block == nullptr) return false;
                // Reference: BlockBehaviour.BlockStateBase::blocksMotion
                return block->blocksMotion();
            };

        case Types::MOTION_BLOCKING:
            // Motion-blocking or fluid
            return [](const BlockState* block) -> bool {
                if (block == nullptr) return false;
                // Reference: (input) -> input.blocksMotion() || !input.getFluidState().isEmpty()
                return block->blocksMotion() || block->isFluid();
            };

        case Types::MOTION_BLOCKING_NO_LEAVES:
            // Motion-blocking or fluid, but not leaves
            return [](const BlockState* block) -> bool {
                if (block == nullptr) return false;
                // Reference: (input) -> (input.blocksMotion() || !input.getFluidState().isEmpty())
                //                      && !(input.getBlock() instanceof LeavesBlock)
                bool isMotionBlockingOrFluid = block->blocksMotion() || block->isFluid();
                return isMotionBlockingOrFluid && !block->isLeaves();
            };
    }

    // Default: any non-air block
    return [](const BlockState* block) -> bool {
        return block != nullptr && !block->isAir();
    };
}

} // namespace levelgen
} // namespace minecraft
