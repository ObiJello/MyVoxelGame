// File: src/common/world/block/entity/DaylightDetectorBlockEntity.hpp
//
// MC DaylightDetectorBlockEntity carries no data at all; it exists so the
// block has a ticker. Vanilla's DaylightDetectorBlock.getTicker re-measures
// the sky every 20 game ticks, and only in a dimension with sky light.
#pragma once

#include "BlockEntity.hpp"

namespace Game {

    class DaylightDetectorBlockEntity : public BlockEntity {
    public:
        DaylightDetectorBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        bool NeedsTicking() const override { return true; }
        void Tick(World* world, float deltaTime) override;   // RedstoneComponents.cpp
    };

} // namespace Game
