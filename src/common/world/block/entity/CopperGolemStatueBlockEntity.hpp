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

#include <memory>
#include <string>

namespace Game {

    class CopperGolem;
    struct EntityLevel;

    class CopperGolemStatueBlockEntity : public BlockEntity {
    public:
        CopperGolemStatueBlockEntity(const BlockEntityType* type, glm::ivec3 worldPos, BlockID blockId)
            : BlockEntity(type, worldPos, blockId) {}

        const std::string& GetCustomName() const { return m_customName; }
        void SetCustomName(std::string name) { m_customName = std::move(name); }

        // MC CopperGolemStatueBlockEntity.createStatue: the golem that set
        // here leaves its name on the statue (CUSTOM_NAME — none clears it).
        // The caller marks the entity changed (setChanged).
        void CreateStatue(const CopperGolem& copperGolem);

        // MC removeStatue + initCopperGolem: a fresh copper golem (TRIGGERED)
        // with the statue's name, standing at the cell's bottom centre and
        // facing the statue's FACING (head and body too), the spawn sound
        // played. The caller adds it and removes the block. Null without a
        // level to create into.
        std::unique_ptr<CopperGolem> RemoveStatue(BlockState state, EntityLevel& level) const;

        void ApplyItemComponents(const DataComponentMap& components) override;
        void CollectComponents(DataComponentMap& out) const override;

    private:
        std::string m_customName;
    };

} // namespace Game
