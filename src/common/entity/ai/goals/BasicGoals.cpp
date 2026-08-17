// File: src/common/entity/ai/goals/BasicGoals.cpp
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Game {

    // ── FloatGoal ──────────────────────────────────────────────────────────

    FloatGoal::FloatGoal(Mob* mob) : m_mob(mob) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Jump));
        // MC sets this in the constructor, not on use: a mob that CAN float
        // must path across water surfaces too, or it swims in circles refusing
        // to path to the shore it is floating next to.
        mob->GetNavigation().SetCanFloat(true);
    }

    bool FloatGoal::CanUse() {
        return m_mob->IsInWater() || m_mob->IsInLava();
    }

    void FloatGoal::Tick() {
        // 80% per tick rather than every tick: the mob bobs at the surface
        // instead of launching out of the water.
        if (m_mob->Level()->Random().NextFloat() < 0.8f) {
            m_mob->GetJumpControl().Jump();
        }
    }

    // ── RandomStrollGoal ───────────────────────────────────────────────────

    RandomStrollGoal::RandomStrollGoal(PathfinderMob* mob, double speedModifier,
                                       int interval, bool checkNoActionTime)
        : m_mob(mob), m_speedModifier(speedModifier), m_interval(interval),
          m_checkNoActionTime(checkNoActionTime) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool RandomStrollGoal::GetPosition(glm::dvec3& out) {
        auto pos = RandomPos::GetPos(*m_mob, 10, 7);
        if (!pos) return false;
        out = *pos;
        return true;
    }

    bool RandomStrollGoal::CanUse() {
        if (!m_forceTrigger) {
            // A mob nobody has been near for 5 seconds stops wandering. This is
            // what keeps distant, unobserved mobs cheap — and it pairs with the
            // despawn rule, which also keys on noActionTime.
            if (m_checkNoActionTime && m_mob->GetNoActionTime() >= 100) return false;

            // Roughly a 1-in-60 chance per evaluation (the interval is halved
            // because this goal is only evaluated every other tick).
            if (m_mob->Level()->Random().NextInt(ReducedTickDelay(m_interval)) != 0) return false;
        }

        glm::dvec3 pos;
        if (!GetPosition(pos)) return false;

        m_wantedX = pos.x;
        m_wantedY = pos.y;
        m_wantedZ = pos.z;
        m_forceTrigger = false;
        return true;
    }

    bool RandomStrollGoal::CanContinueToUse() {
        return !m_mob->GetNavigation().IsDone();
    }

    void RandomStrollGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_wantedX, m_wantedY, m_wantedZ, m_speedModifier);
    }

    void RandomStrollGoal::Stop() {
        m_mob->GetNavigation().Stop();
    }

    // ── WaterAvoidingRandomStrollGoal ──────────────────────────────────────

    WaterAvoidingRandomStrollGoal::WaterAvoidingRandomStrollGoal(PathfinderMob* mob,
                                                                 double speedModifier,
                                                                 float probability)
        : RandomStrollGoal(mob, speedModifier), m_probability(probability) {}

    bool WaterAvoidingRandomStrollGoal::GetPosition(glm::dvec3& out) {
        // Already in water: always aim for land.
        if (m_mob->IsInWater()) {
            if (auto land = RandomPos::GetLandPos(*m_mob, 15, 7)) { out = *land; return true; }
            return RandomStrollGoal::GetPosition(out);
        }

        // On land: prefer a land target, but with probability 0.001 fall
        // through to an unconstrained roll — which is the only way a land mob
        // ever wades in.
        if (m_mob->Level()->Random().NextFloat() >= m_probability) {
            if (auto land = RandomPos::GetLandPos(*m_mob, 10, 7)) { out = *land; return true; }
            return false;
        }
        return RandomStrollGoal::GetPosition(out);
    }

    // ── LookAtPlayerGoal ───────────────────────────────────────────────────

    LookAtPlayerGoal::LookAtPlayerGoal(Mob* mob, float lookDistance, float probability,
                                       bool onlyHorizontal)
        : m_mob(mob), m_lookDistance(lookDistance), m_probability(probability),
          m_onlyHorizontal(onlyHorizontal) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Look));
        m_conditions = TargetingConditions::ForNonCombat().Range(lookDistance);
    }

    void LookAtPlayerGoal::UseInterval(int intervalMin, int intervalMax,
                                       int holdMin, int holdMax) {
        m_intervalMin = intervalMin;
        m_intervalMax = intervalMax;
        m_holdMin = holdMin;
        m_holdMax = holdMax;
    }

    bool LookAtPlayerGoal::CanUse() {
        if (m_intervalMin > 0) {
            // An absolute deadline rather than MC's per-evaluation countdown:
            // the brain ticks its behaviours every tick and this goal is polled
            // every other one, so counting polls would double every interval.
            if (m_nextLookTick < 0) {
                m_nextLookTick = m_mob->tickCount
                               + m_mob->Level()->Random().NextInt(m_intervalMin, m_intervalMax);
            }
            if (m_mob->tickCount < m_nextLookTick) return false;
        } else if (m_mob->Level()->Random().NextFloat() >= m_probability) {
            return false;
        }

        // An aggressive mob looks at what it is attacking; otherwise the
        // nearest player. MC assigns the target first and then OVERWRITES it
        // with the player search, so the target only wins when no player is in
        // range — preserved here rather than "fixed".
        if (m_mob->GetTarget()) m_lookAt = m_mob->GetTarget();

        LivingEntity* nearest = m_mob->Level()->GetNearestPlayer(
            m_mob->position.x, m_mob->GetEyeY(), m_mob->position.z, m_lookDistance);
        if (nearest && m_conditions.Test(m_mob, *nearest)) m_lookAt = nearest;

        return m_lookAt != nullptr;
    }

    bool LookAtPlayerGoal::CanContinueToUse() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return false;
        if (m_mob->DistanceToSqr(*m_lookAt) > static_cast<double>(m_lookDistance) * m_lookDistance) {
            return false;
        }
        return m_lookTime > 0;
    }

    void LookAtPlayerGoal::Start() {
        if (m_intervalMin > 0) {
            // MC LookAtTargetSink's duration, counted in POLLS because Tick
            // runs on the 2-tick goal cadence — AdjustedTickDelay is exactly
            // that halving, so the real-time hold is holdMin..holdMax ticks.
            m_lookTime = AdjustedTickDelay(
                m_mob->Level()->Random().NextInt(m_holdMin, m_holdMax));
            return;
        }
        m_lookTime = AdjustedTickDelay(40 + m_mob->Level()->Random().NextInt(40));
    }

    void LookAtPlayerGoal::Stop() {
        m_lookAt = nullptr;
        if (m_intervalMin > 0) {
            m_nextLookTick = m_mob->tickCount
                           + m_mob->Level()->Random().NextInt(m_intervalMin, m_intervalMax);
        }
    }

    void LookAtPlayerGoal::Tick() {
        if (!m_lookAt || !m_lookAt->IsAlive()) return;

        const double targetY = m_onlyHorizontal ? m_mob->GetEyeY() : m_lookAt->GetEyeY();
        m_mob->GetLookControl().SetLookAt(m_lookAt->position.x, targetY, m_lookAt->position.z);
        --m_lookTime;
    }

    // ── RandomLookAroundGoal ───────────────────────────────────────────────

    RandomLookAroundGoal::RandomLookAroundGoal(Mob* mob) : m_mob(mob) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool RandomLookAroundGoal::CanUse() {
        return m_mob->Level()->Random().NextFloat() < 0.02f;
    }

    bool RandomLookAroundGoal::CanContinueToUse() {
        return m_lookTime >= 0;
    }

    void RandomLookAroundGoal::Start() {
        const double angle = 2.0 * 3.14159265358979323846 * m_mob->Level()->Random().NextDouble();
        m_relX = std::cos(angle);
        m_relZ = std::sin(angle);
        m_lookTime = 20 + m_mob->Level()->Random().NextInt(20);
    }

    void RandomLookAroundGoal::Tick() {
        --m_lookTime;
        // A point one block away in a fixed direction — so the head turns to a
        // heading and holds it, rather than tracking anything.
        m_mob->GetLookControl().SetLookAt(m_mob->position.x + m_relX,
                                          m_mob->GetEyeY(),
                                          m_mob->position.z + m_relZ);
    }

    // ── RestrictSunGoal / FleeSunGoal ──────────────────────────────────────

    namespace {
        // MC Level.isBrightOutside — skyDarken below 4. NOT the same as IsDay:
        // the window closes during dusk well before night, which is what stops
        // a skeleton bothering to seek shade it no longer needs.
        bool IsBrightOutside(const EntityLevel* level) {
            return level && level->GetSkyDarken() < 4;
        }
    }

    bool RestrictSunGoal::CanUse() {
        // MC also requires an empty helmet slot and a ground navigation. Mobs
        // cannot wear anything here, so the first is trivially satisfied; the
        // second is real and checked.
        return IsBrightOutside(m_mob->Level())
            && dynamic_cast<GroundPathNavigation*>(&m_mob->GetNavigation()) != nullptr;
    }

    void RestrictSunGoal::Start() { SetAvoidSun(true); }
    void RestrictSunGoal::Stop()  { SetAvoidSun(false); }

    // MC GoalUtils.hasGroundPathNavigation — the flag lives on the ground
    // navigation, and a swimming or flying mob simply has nowhere to put it.
    void RestrictSunGoal::SetAvoidSun(bool v) {
        if (auto* ground = dynamic_cast<GroundPathNavigation*>(&m_mob->GetNavigation())) {
            ground->SetAvoidSun(v);
        }
    }

    bool FleeSunGoal::CanUse() {
        EntityLevel* level = m_mob->Level();
        if (m_mob->GetTarget() != nullptr) return false;   // busy fighting
        if (!IsBrightOutside(level)) return false;
        if (!m_mob->IsOnFire()) return false;
        const glm::ivec3 p = m_mob->BlockPosition();
        if (!level->CanSeeSky(p.x, p.y, p.z)) return false;
        return FindHidePos();
    }

    bool FleeSunGoal::FindHidePos() {
        EntityLevel* level = m_mob->Level();
        if (!level) return false;
        const glm::ivec3 origin = m_mob->BlockPosition();

        // MC's ten tries in a 20x6x20 box. GetWalkTargetValue < 0 is Monster's
        // negated light cost, so "shaded" and "somewhere this mob wants to be"
        // are the same test — which is why the goal needs no light check.
        for (int i = 0; i < 10; ++i) {
            const glm::ivec3 candidate(
                origin.x + level->Random().NextInt(20) - 10,
                origin.y + level->Random().NextInt(6) - 3,
                origin.z + level->Random().NextInt(20) - 10);
            if (!level->CanSeeSky(candidate.x, candidate.y, candidate.z)
                && m_mob->GetWalkTargetValue(candidate) < 0.0f) {
                m_wantedX = static_cast<double>(candidate.x) + 0.5;
                m_wantedY = static_cast<double>(candidate.y);
                m_wantedZ = static_cast<double>(candidate.z) + 0.5;
                return true;
            }
        }
        return false;
    }

    bool FleeSunGoal::CanContinueToUse() {
        return !m_mob->GetNavigation().IsDone();
    }

    void FleeSunGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_wantedX, m_wantedY, m_wantedZ, m_speedModifier);
    }

    // ── PanicGoal ──────────────────────────────────────────────────────────

    PanicGoal::PanicGoal(PathfinderMob* mob, double speedModifier)
        : m_mob(mob), m_speedModifier(speedModifier) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool PanicGoal::ShouldPanic() const {
        // MC filters on the DamageTypeTags.PANIC_CAUSES tag. Every damage type
        // this engine produces is in that tag except starvation and drowning,
        // neither of which mobs can suffer here — so "was hurt recently" is the
        // same predicate.
        return m_mob->HasLastDamageSource() && m_mob->GetLastHurtByMob() != nullptr;
    }

    bool PanicGoal::LookForWater() {
        EntityLevel* level = m_mob->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;

        // MC searches a 5-wide, 1-tall box for water. A burning mob that finds
        // some runs for it instead of running at random.
        const glm::ivec3 origin = m_mob->BlockPosition();
        for (int dx = -5; dx <= 5; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -5; dz <= 5; ++dz) {
                    const int x = origin.x + dx, y = origin.y + dy, z = origin.z + dz;
                    if (blocks->IsBlockFluid(x, y, z)) {
                        m_posX = x + 0.5; m_posY = y; m_posZ = z + 0.5;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool PanicGoal::FindRandomPosition() {
        // Deliberately a SHORT hop (5x4, not the stroll goal's 10x7): panic is
        // a burst of movement re-rolled often, not one long flight.
        auto pos = RandomPos::GetPos(*m_mob, 5, 4);
        if (!pos) return false;
        m_posX = pos->x; m_posY = pos->y; m_posZ = pos->z;
        return true;
    }

    bool PanicGoal::CanUse() {
        if (!ShouldPanic()) return false;
        if (m_mob->IsOnFire() && LookForWater()) return true;
        return FindRandomPosition();
    }

    bool PanicGoal::CanContinueToUse() {
        return !m_mob->GetNavigation().IsDone();
    }

    void PanicGoal::Start() {
        m_mob->GetNavigation().MoveTo(m_posX, m_posY, m_posZ, m_speedModifier);
        m_isRunning = true;
    }

    void PanicGoal::Stop() {
        m_isRunning = false;
    }


    void LookAtPlayerGoal::ClearReferenceTo(const Entity* entity) {
        if (m_lookAt == entity) m_lookAt = nullptr;
    }

} // namespace Game
