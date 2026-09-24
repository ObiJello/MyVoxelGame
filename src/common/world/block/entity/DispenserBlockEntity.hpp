// File: src/common/world/block/entity/DispenserBlockEntity.hpp
//
// MC DispenserBlockEntity / DropperBlockEntity — nine slots, a random
// occupied-slot pick, and the insert that dispense behaviours use to hand
// back a remainder (an empty bucket).
#pragma once

#include "BaseContainerBlockEntity.hpp"

namespace Game {

    class JavaRandom;

    class DispenserBlockEntity : public BaseContainerBlockEntity {
    public:
        static constexpr int kContainerSize = 9;

        DispenserBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BaseContainerBlockEntity(type, worldPos, blockId, kContainerSize) {}

        // MC getRandomSlot: reservoir-sample one occupied slot, -1 when empty.
        int GetRandomSlot(JavaRandom& random) const;

        // MC insertItem: merge into matching or empty slots; returns the rest.
        ItemStack InsertItem(ItemStack stack);
    };

} // namespace Game
