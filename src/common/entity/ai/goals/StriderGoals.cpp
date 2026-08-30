// File: src/common/entity/ai/goals/StriderGoals.cpp
#include "common/entity/ai/goals/StriderGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

namespace Game {

    StriderGoToLavaGoal::StriderGoToLavaGoal(Strider* strider,
                                             double speedModifier)
        : MoveToBlockGoal(strider, speedModifier, /*searchRange=*/8,
                          /*verticalSearchRange=*/2),
          m_strider(strider) {}

    bool StriderGoToLavaGoal::CanUse() {
        return !m_strider->IsInLava() && MoveToBlockGoal::CanUse();
    }

    bool StriderGoToLavaGoal::CanContinueToUse() {
        if (m_strider->IsInLava()) return false;
        const IBlockAccess* blocks =
            m_strider->Level() ? m_strider->Level()->Blocks() : nullptr;
        return blocks != nullptr && IsValidTarget(*blocks, m_blockPos);
    }

    bool StriderGoToLavaGoal::IsValidTarget(const IBlockAccess& blocks,
                                            const glm::ivec3& pos) const {
        // MC: the block is lava and the one above is land-pathable — no
        // collision (isPathfindable(LAND) reduced to the collision table).
        return blocks.GetBlock(pos.x, pos.y, pos.z) == BlockID::Lava &&
               !BlockRegistry::HasCollision(
                   blocks.GetBlock(pos.x, pos.y + 1, pos.z));
    }

} // namespace Game
