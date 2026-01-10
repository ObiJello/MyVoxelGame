#include "levelgen/Heightmap.h"
#include "world/IBlockType.h"

namespace minecraft {
namespace levelgen {

// Reference: Heightmap.java lines 156-177 (Types enum predicates)
Heightmap::OpaquePredicate Heightmap::getOpaquePredicate(Types type) {
    switch (type) {
        case Types::WORLD_SURFACE_WG:
        case Types::WORLD_SURFACE:
            // NOT_AIR: Any non-air block
            return [](const world::IBlockType* block) -> bool {
                return block != nullptr && !block->isAir();
            };

        case Types::OCEAN_FLOOR_WG:
        case Types::OCEAN_FLOOR:
            // MATERIAL_MOTION_BLOCKING: Blocks that block motion
            return [](const world::IBlockType* block) -> bool {
                if (block == nullptr) return false;
                // Reference: BlockBehaviour.BlockStateBase::blocksMotion
                return block->blocksMotion();
            };

        case Types::MOTION_BLOCKING:
            // Motion-blocking or fluid
            return [](const world::IBlockType* block) -> bool {
                if (block == nullptr) return false;
                // Reference: (input) -> input.blocksMotion() || !input.getFluidState().isEmpty()
                return block->blocksMotion() || block->isFluid();
            };

        case Types::MOTION_BLOCKING_NO_LEAVES:
            // Motion-blocking or fluid, but not leaves
            return [](const world::IBlockType* block) -> bool {
                if (block == nullptr) return false;
                // Reference: (input) -> (input.blocksMotion() || !input.getFluidState().isEmpty())
                //                      && !(input.getBlock() instanceof LeavesBlock)
                bool isMotionBlockingOrFluid = block->blocksMotion() || block->isFluid();
                return isMotionBlockingOrFluid && !block->isLeaves();
            };
    }

    // Default: any non-air block
    return [](const world::IBlockType* block) -> bool {
        return block != nullptr && !block->isAir();
    };
}

} // namespace levelgen
} // namespace minecraft
