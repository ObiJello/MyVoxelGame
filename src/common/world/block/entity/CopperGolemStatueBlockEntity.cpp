// File: src/common/world/block/entity/CopperGolemStatueBlockEntity.cpp
#include "CopperGolemStatueBlockEntity.hpp"

#include "common/data/DataComponents.hpp"

namespace Game {

    void CopperGolemStatueBlockEntity::ApplyItemComponents(const DataComponentMap& components) {
        if (auto name = components.get(DataComponents::CUSTOM_NAME)) m_customName = *name;
        MarkDirty();
    }

    void CopperGolemStatueBlockEntity::CollectComponents(DataComponentMap& out) const {
        if (!m_customName.empty()) out.set(DataComponents::CUSTOM_NAME, m_customName);
    }

} // namespace Game
