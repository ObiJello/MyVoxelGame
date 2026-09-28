// File: src/common/world/block/entity/CopperGolemStatueBlockEntity.hpp
//
// Mirrors net.minecraft.world.level.block.entity.CopperGolemStatueBlockEntity
// — a copper golem turned to a statue. Its pose, facing and oxidation are
// the block's state (CopperGolemStatueBlock POSE / FACING, the weathering
// family); the entity carries the golem's name (its CUSTOM_NAME component),
// which the dropped statue keeps and a placed one takes back, and it is what
// CopperGolemStatueRenderer draws the golem from.
#pragma once

#include "BlockEntity.hpp"

#include <string>

namespace Game {

    class CopperGolemStatueBlockEntity : public BlockEntity {
    public:
        CopperGolemStatueBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        const std::string& GetCustomName() const { return m_customName; }
        void SetCustomName(std::string name) { m_customName = std::move(name); }

        void ApplyItemComponents(const DataComponentMap& components) override;
        void CollectComponents(DataComponentMap& out) const override;

    private:
        std::string m_customName;
    };

} // namespace Game
