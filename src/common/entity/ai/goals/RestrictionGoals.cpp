// File: src/common/entity/ai/goals/RestrictionGoals.cpp
#include "common/entity/ai/goals/RestrictionGoals.hpp"

#include "common/entity/Mob.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/Mth.hpp"

namespace Game {

    MoveTowardsRestrictionGoal::MoveTowardsRestrictionGoal(PathfinderMob* mob,
                                                           double speedModifier)
        : m_mob(mob), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool MoveTowardsRestrictionGoal::CanUse() {
        // MC: a mob inside its home (or with no home at all — isWithinHome is
        // vacuously true then) never starts.
        if (m_mob->IsWithinHome()) return false;

        // MC: DefaultRandomPos.getPosTowards(mob, 16, 7,
        // Vec3.atBottomCenterOf(getHomePosition()), PI/2).
        const glm::ivec3 home = m_mob->GetHomePosition();
        const glm::dvec3 towards(home.x + 0.5, home.y, home.z + 0.5);
        const std::optional<glm::dvec3> pos = RandomPos::GetPosTowards(
            *m_mob, 16, 7, towards, static_cast<double>(Mth::kPi) / 2.0);
        if (!pos) return false;

        m_wantedX = pos->x;
        m_wantedY = pos->y;
        m_wantedZ = pos->z;
        return true;
    }

    bool MoveTowardsRestrictionGoal::CanContinueToUse() {
        return !m_mob->GetNavigation().IsDone();
    }

    void MoveTowardsRestrictionGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_wantedX, m_wantedY, m_wantedZ,
                                      m_speedModifier);
    }

} // namespace Game
