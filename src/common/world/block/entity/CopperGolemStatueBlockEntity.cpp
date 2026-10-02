// File: src/common/world/block/entity/CopperGolemStatueBlockEntity.cpp
#include "CopperGolemStatueBlockEntity.hpp"

#include "common/data/DataComponents.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"

namespace Game {

    void CopperGolemStatueBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        if (auto name = components.get(DataComponents::CUSTOM_NAME)) m_customName = *name;
        MarkDirty();
    }

    void CopperGolemStatueBlockEntity::CreateStatue(const CopperGolem& copperGolem) {
        m_customName = copperGolem.GetCustomName().value_or(std::string{});
        MarkDirty();
    }

    std::unique_ptr<CopperGolem> CopperGolemStatueBlockEntity::RemoveStatue(BlockState state,
                                                                            EntityLevel& level) const {
        auto copperGolem = std::make_unique<CopperGolem>(&level);
        // copperGolem.setCustomName(components().get(CUSTOM_NAME)).
        if (!m_customName.empty()) copperGolem->SetCustomName(m_customName);
        // initCopperGolem: snapTo(centre x, block y, centre z,
        // FACING.toYRot(), 0); yHeadRot = yBodyRot = yRot.
        const glm::ivec3 pos = GetWorldPos();
        copperGolem->position = glm::dvec3(static_cast<double>(pos.x) + 0.5, static_cast<double>(pos.y),
                                           static_cast<double>(pos.z) + 0.5);
        copperGolem->yRot = ToYRot(HorizontalFacingOf(state));
        copperGolem->xRot = 0.0f;
        copperGolem->SetYHeadRot(copperGolem->yRot);
        copperGolem->yBodyRot = copperGolem->yRot;
        copperGolem->SetOldPosAndRot();
        copperGolem->PlaySpawnSound();
        return copperGolem;
    }

    void CopperGolemStatueBlockEntity::CollectComponents(DataComponentMap& out) const {
        if (!m_customName.empty()) out.set(DataComponents::CUSTOM_NAME, m_customName);
    }

} // namespace Game
