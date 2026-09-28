// File: src/common/entity/ai/goals/HorseGoals.cpp
#include "common/entity/ai/goals/HorseGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"

namespace Game {

    namespace {

        // MC AbstractHorse.isMobControlled: false on the base — only the
        // zombie horse answers "a mob rides me" (its jockey); the llama keeps
        // the base answer.
        bool IsMobControlled(const Mob& horse) {
            const auto* equine = dynamic_cast<const AbstractHorse*>(&horse);
            return equine && equine->IsMobControlled();
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
        : RunAroundLikeCrazyGoal(horse, horse, speedModifier) {}

    RunAroundLikeCrazyGoal::RunAroundLikeCrazyGoal(PathfinderMob* mob, HorseTaming* taming,
                                                   double speedModifier)
        : m_horse(mob), m_taming(taming), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool RunAroundLikeCrazyGoal::CanUse() {
        // MC: !isMobControlled && !isTamed && isVehicle.
        if (IsMobControlled(*m_horse) || m_taming->IsTamed() || !m_taming->IsEquineVehicle()) return false;
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
        return !m_taming->IsTamed() && !m_horse->GetNavigation().IsDone() &&
               m_taming->IsEquineVehicle();
    }

    void RunAroundLikeCrazyGoal::Tick() {
        EntityLevel* level = m_horse->Level();
        if (!level || level->IsClientSide()) return;
        if (m_taming->IsTamed() || level->Random().NextInt(AdjustedTickDelay(50)) != 0) return;
        // getFirstPassenger: none → nothing; a player rolls the temper; any
        // other rider (a mob on an untamed horse) is simply thrown.
        if (!m_taming->IsEquineVehicle()) return;
        if (LivingEntity* player = m_taming->GetPlayerRider()) {
            const int temper = m_taming->GetTemper();
            const int maxTemper = m_taming->GetMaxTemper();
            if (maxTemper > 0 && level->Random().NextInt(maxTemper) < temper) {
                m_taming->TameWithName(*player);
                return;
            }
            m_taming->ModifyTemper(5);
        }
        // ejectPassengers: every passenger, the player seat included.
        m_taming->EjectPlayerRider();
        m_horse->EjectPassengers();
        m_taming->MakeMad();
        level->BroadcastEntityEvent(*m_horse, 6);
    }

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
        // MC playStandSound: horse.playSound(getAmbientStandSound()).
        m_horse->PlaySound(m_horse->GetAmbientStandSound(), 1.0f, 1.0f);
    }

    void RandomStandGoal::ResetStandInterval() {
        m_nextStand = -m_horse->GetAmbientStandInterval();
    }

} // namespace Game
