// File: src/common/entity/ai/goals/HorseGoals.cpp
#include "common/entity/ai/goals/HorseGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"

namespace Game {

    namespace {

        // MC AbstractHorse.isMobControlled — the first passenger is a Mob.
        bool IsMobControlled(const AbstractHorse& horse) {
            return dynamic_cast<Mob*>(horse.GetFirstPassenger()) != nullptr;
        }

    } // namespace

    // ── MountPanicGoal ─────────────────────────────────────────────────────

    MountPanicGoal::MountPanicGoal(AbstractHorse* horse, double speedModifier)
        : PanicGoal(horse, speedModifier), m_horse(horse) {}

    bool MountPanicGoal::ShouldPanic() const {
        return !IsMobControlled(*m_horse) && PanicGoal::ShouldPanic();
    }

    // ── RunAroundLikeCrazyGoal ─────────────────────────────────────────────

    RunAroundLikeCrazyGoal::RunAroundLikeCrazyGoal(AbstractHorse* horse,
                                                   double speedModifier)
        : m_horse(horse), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool RunAroundLikeCrazyGoal::CanUse() {
        // MC: !isMobControlled && !isTamed && isVehicle — taming does not
        // exist, so the tame term is always "untamed".
        if (IsMobControlled(*m_horse) || !m_horse->IsVehicle()) return false;
        auto pos = RandomPos::GetPos(*m_horse, 5, 4);
        if (!pos) return false;
        m_posX = pos->x;
        m_posY = pos->y;
        m_posZ = pos->z;
        return true;
    }

    void RunAroundLikeCrazyGoal::Start() {
        m_horse->GetNavigation().MoveTo(m_posX, m_posY, m_posZ, m_speedModifier);
    }

    bool RunAroundLikeCrazyGoal::CanContinueToUse() {
        return !m_horse->GetNavigation().IsDone() && m_horse->IsVehicle();
    }

    // MC's tick() rolls the 1-in-50 buck: dismount the player rider, or
    // tame at temper. Both halves ride the taming/riding-player systems —
    // and the goal cannot run without a player rider — so the base tick (a
    // no-op) is the honest port until they land.

    RandomStandGoal::RandomStandGoal(AbstractHorse* horse) : m_horse(horse) {
        ResetStandInterval();
    }

    bool RandomStandGoal::CanUse() {
        ++m_nextStand;
        if (m_nextStand > 0
            && m_horse->Level()->Random().NextInt(1000) < m_nextStand) {
            ResetStandInterval();
            return !m_horse->IsImmobile()
                && m_horse->Level()->Random().NextInt(10) == 0;
        }
        return false;
    }

    void RandomStandGoal::Start() {
        m_horse->StandIfPossible();
        // MC also plays getAmbientStandSound — sounds wait on the system.
    }

    void RandomStandGoal::ResetStandInterval() {
        m_nextStand = -m_horse->GetAmbientStandInterval();
    }

} // namespace Game
