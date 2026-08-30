// File: src/common/entity/ai/goals/TurtleGoals.cpp
#include "common/entity/ai/goals/TurtleGoals.hpp"

#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/RandomPos.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Game {

    namespace {

        // Squared-distance-to-block-centre helper for MC's
        // BlockPos.closerToCenterThan(position, range).
        bool CloserToCenterThan(const glm::ivec3& pos, const glm::dvec3& p,
                                double range) {
            const double dx = (pos.x + 0.5) - p.x;
            const double dy = (pos.y + 0.5) - p.y;
            const double dz = (pos.z + 0.5) - p.z;
            return dx * dx + dy * dy + dz * dz < range * range;
        }

        // ── MC Turtle.TurtleMoveControl ────────────────────────────────────

        class TurtleMoveControl : public MoveControl {
        public:
            explicit TurtleMoveControl(Turtle* turtle)
                : MoveControl(turtle), m_turtle(turtle) {}

            void Tick() override {
                UpdateSpeed();
                if (m_operation == Operation::MoveTo
                    && !m_turtle->GetNavigation().IsDone()) {
                    const double xd = m_wantedX - m_turtle->position.x;
                    double yd = m_wantedY - m_turtle->position.y;
                    const double zd = m_wantedZ - m_turtle->position.z;
                    const double dd = std::sqrt(xd * xd + yd * yd + zd * zd);
                    if (dd < 1.0e-5) {
                        m_turtle->SetSpeed(0.0f);
                        return;
                    }
                    yd /= dd;
                    const float yRotD = static_cast<float>(
                        std::atan2(zd, xd) * (180.0 / Mth::kPi)) - 90.0f;
                    m_turtle->yRot = RotLerp(m_turtle->yRot, yRotD, 90.0f);
                    m_turtle->yBodyRot = m_turtle->yRot;
                    const float targetSpeed = static_cast<float>(
                        m_speedModifier
                        * m_turtle->GetAttributeValue(Attribute::MovementSpeed));
                    m_turtle->SetSpeed(Mth::Lerp(0.125f, m_turtle->GetSpeed(),
                                                 targetSpeed));
                    m_turtle->velocity.y +=
                        static_cast<double>(m_turtle->GetSpeed()) * yd * 0.1;
                } else {
                    m_turtle->SetSpeed(0.0f);
                }
            }

        private:
            void UpdateSpeed() {
                if (m_turtle->IsInWater()) {
                    // Buoyancy — turtles bob upward while swimming.
                    m_turtle->velocity.y += 0.005;
                    if (!CloserToCenterThan(m_turtle->HomePos(),
                                            m_turtle->position, 16.0)) {
                        m_turtle->SetSpeed(
                            std::max(m_turtle->GetSpeed() / 2.0f, 0.08f));
                    }
                    if (m_turtle->IsBaby()) {
                        m_turtle->SetSpeed(
                            std::max(m_turtle->GetSpeed() / 3.0f, 0.06f));
                    }
                } else if (m_turtle->onGround) {
                    m_turtle->SetSpeed(
                        std::max(m_turtle->GetSpeed() / 2.0f, 0.06f));
                }
            }

            Turtle* m_turtle;
        };

    } // namespace

    std::unique_ptr<MoveControl> MakeTurtleMoveControl(Turtle* turtle) {
        return std::make_unique<TurtleMoveControl>(turtle);
    }

    // ── TurtlePanicGoal ────────────────────────────────────────────────────

    TurtlePanicGoal::TurtlePanicGoal(Turtle* turtle, double speedModifier)
        : PanicGoal(turtle, speedModifier) {}

    bool TurtlePanicGoal::CanUse() {
        if (!ShouldPanic()) return false;

        // MC lookForWater(range 7) — wider than the base goal's fire search.
        EntityLevel* level = m_mob->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (blocks) {
            const glm::ivec3 origin = m_mob->BlockPosition();
            for (int dx = -7; dx <= 7; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -7; dz <= 7; ++dz) {
                        const int x = origin.x + dx, y = origin.y + dy,
                                  z = origin.z + dz;
                        if (blocks->IsBlockFluid(x, y, z)) {
                            m_posX = x + 0.5;
                            m_posY = y;
                            m_posZ = z + 0.5;
                            return true;
                        }
                    }
                }
            }
        }
        return FindRandomPosition();
    }

    // ── TurtleBreedGoal ────────────────────────────────────────────────────

    TurtleBreedGoal::TurtleBreedGoal(Turtle* turtle, double speedModifier)
        : BreedGoal(turtle, speedModifier), m_turtle(turtle) {}

    bool TurtleBreedGoal::CanUse() {
        return BreedGoal::CanUse() && !m_turtle->HasEgg();
    }

    // ── TurtleLayEggGoal ───────────────────────────────────────────────────

    TurtleLayEggGoal::TurtleLayEggGoal(Turtle* turtle, double speedModifier)
        : MoveToBlockGoal(turtle, speedModifier, 16), m_turtle(turtle) {}

    bool TurtleLayEggGoal::CanUse() {
        return m_turtle->HasEgg()
            && CloserToCenterThan(m_turtle->HomePos(), m_turtle->position, 9.0)
            && MoveToBlockGoal::CanUse();
    }

    bool TurtleLayEggGoal::CanContinueToUse() {
        return MoveToBlockGoal::CanContinueToUse() && m_turtle->HasEgg()
            && CloserToCenterThan(m_turtle->HomePos(), m_turtle->position, 9.0);
    }

    void TurtleLayEggGoal::Tick() {
        MoveToBlockGoal::Tick();
        if (!m_turtle->IsInWater() && IsReachedTarget()) {
            if (m_turtle->GetLayEggCounter() < 1) {
                m_turtle->SetLayingEgg(true);
            } else if (m_turtle->GetLayEggCounter() > AdjustedTickDelay(200)) {
                // MC: place the turtle_egg one above the dug sand. (MC rolls
                // 1-4 eggs into the block's EGGS state; block-state
                // properties do not reach the mob seam, so one egg block
                // stands for the clutch. The lay sound and the BLOCK_PLACE
                // game event wait on their systems.)
                EntityLevel* level = m_turtle->Level();
                if (level) {
                    const glm::ivec3 eggPos(m_blockPos.x, m_blockPos.y + 1,
                                            m_blockPos.z);
                    level->SetBlock(eggPos, BlockID::TurtleEgg);
                }
                m_turtle->SetHasEgg(false);
                m_turtle->SetLayingEgg(false);
                // MC setInLoveTime(600) — the pair courts again.
                m_turtle->SetInLove(nullptr);
            }

            if (m_turtle->IsLayingEgg()) {
                m_turtle->IncrementLayEggCounter();
            }
        }
    }

    bool TurtleLayEggGoal::IsValidTarget(const IBlockAccess& blocks,
                                         const glm::ivec3& pos) const {
        // MC: air above, sand below the spot.
        if (blocks.GetBlock(pos.x, pos.y + 1, pos.z) != BlockID::Air) {
            return false;
        }
        return Turtle::IsSandBlock(blocks.GetBlock(pos.x, pos.y, pos.z));
    }

    // ── TurtleGoToWaterGoal ────────────────────────────────────────────────

    TurtleGoToWaterGoal::TurtleGoToWaterGoal(Turtle* turtle, double speedModifier)
        : MoveToBlockGoal(turtle, turtle->IsBaby() ? 2.0 : speedModifier, 24),
          m_turtle(turtle) {
        m_verticalSearchStart = -1;
    }

    bool TurtleGoToWaterGoal::CanUse() {
        if (m_turtle->IsBaby() && !m_turtle->IsInWater()) {
            return MoveToBlockGoal::CanUse();
        }
        return !m_turtle->IsGoingHome() && !m_turtle->IsInWater()
            && !m_turtle->HasEgg() && MoveToBlockGoal::CanUse();
    }

    bool TurtleGoToWaterGoal::CanContinueToUse() {
        EntityLevel* level = m_turtle->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        return !m_turtle->IsInWater() && m_tryTicks <= 1200 && blocks
            && IsValidTarget(*blocks, m_blockPos);
    }

    bool TurtleGoToWaterGoal::IsValidTarget(const IBlockAccess& blocks,
                                            const glm::ivec3& pos) const {
        return blocks.GetBlock(pos.x, pos.y, pos.z) == BlockID::Water;
    }

    // ── TurtleGoHomeGoal ───────────────────────────────────────────────────

    TurtleGoHomeGoal::TurtleGoHomeGoal(Turtle* turtle, double speedModifier)
        : m_turtle(turtle), m_speedModifier(speedModifier) {}

    bool TurtleGoHomeGoal::CanUse() {
        if (m_turtle->IsBaby()) return false;
        if (m_turtle->HasEgg()) return true;
        if (m_turtle->Level()->Random().NextInt(ReducedTickDelay(700)) != 0) {
            return false;
        }
        return !CloserToCenterThan(m_turtle->HomePos(), m_turtle->position, 64.0);
    }

    void TurtleGoHomeGoal::Start() {
        m_turtle->SetGoingHome(true);
        m_stuck = false;
        m_closeToHomeTryTicks = 0;
    }

    void TurtleGoHomeGoal::Stop() { m_turtle->SetGoingHome(false); }

    bool TurtleGoHomeGoal::CanContinueToUse() {
        return !CloserToCenterThan(m_turtle->HomePos(), m_turtle->position, 7.0)
            && !m_stuck && m_closeToHomeTryTicks <= AdjustedTickDelay(600);
    }

    void TurtleGoHomeGoal::Tick() {
        const glm::ivec3 homePos = m_turtle->HomePos();
        const bool closeToHome =
            CloserToCenterThan(homePos, m_turtle->position, 16.0);
        if (closeToHome) ++m_closeToHomeTryTicks;

        if (m_turtle->GetNavigation().IsDone()) {
            const glm::dvec3 homeVec(homePos.x + 0.5, homePos.y, homePos.z + 0.5);
            auto nextPos = RandomPos::GetPosTowards(*m_turtle, 16, 3, homeVec,
                                                    Mth::kPi / 10.0);
            if (!nextPos) {
                nextPos = RandomPos::GetPosTowards(*m_turtle, 8, 7, homeVec,
                                                   Mth::kPi / 2.0);
            }

            if (nextPos && !closeToHome) {
                // MC: a far-from-home hop must land in water; re-roll wider
                // otherwise.
                const IBlockAccess* blocks = m_turtle->Level()->Blocks();
                const glm::ivec3 p(static_cast<int>(std::floor(nextPos->x)),
                                   static_cast<int>(std::floor(nextPos->y)),
                                   static_cast<int>(std::floor(nextPos->z)));
                if (blocks && blocks->GetBlock(p.x, p.y, p.z) != BlockID::Water) {
                    nextPos = RandomPos::GetPosTowards(*m_turtle, 16, 5, homeVec,
                                                       Mth::kPi / 2.0);
                }
            }

            if (!nextPos) {
                m_stuck = true;
                return;
            }

            m_turtle->GetNavigation().MoveTo(nextPos->x, nextPos->y, nextPos->z,
                                             m_speedModifier);
        }
    }

    // ── TurtleTravelGoal ───────────────────────────────────────────────────

    TurtleTravelGoal::TurtleTravelGoal(Turtle* turtle, double speedModifier)
        : m_turtle(turtle), m_speedModifier(speedModifier) {}

    bool TurtleTravelGoal::CanUse() {
        return !m_turtle->IsGoingHome() && !m_turtle->HasEgg()
            && m_turtle->IsInWater();
    }

    void TurtleTravelGoal::Start() {
        JavaRandom& random = m_turtle->Level()->Random();
        const int xt = random.NextInt(1025) - 512;
        int yt = random.NextInt(9) - 4;
        const int zt = random.NextInt(1025) - 512;
        // MC clamps the roam target to below sea level (63).
        if (static_cast<double>(yt) + m_turtle->position.y > 63.0 - 1.0) {
            yt = 0;
        }
        m_turtle->SetTravelPos(glm::ivec3(
            static_cast<int>(std::floor(xt + m_turtle->position.x)),
            static_cast<int>(std::floor(yt + m_turtle->position.y)),
            static_cast<int>(std::floor(zt + m_turtle->position.z))));
        m_stuck = false;
    }

    void TurtleTravelGoal::Tick() {
        if (!m_turtle->TravelPos()) {
            m_stuck = true;
            return;
        }
        if (m_turtle->GetNavigation().IsDone()) {
            const glm::ivec3 t = *m_turtle->TravelPos();
            const glm::dvec3 targetPos(t.x + 0.5, t.y, t.z + 0.5);
            auto nextPos = RandomPos::GetPosTowards(*m_turtle, 16, 3, targetPos,
                                                    Mth::kPi / 10.0);
            if (!nextPos) {
                nextPos = RandomPos::GetPosTowards(*m_turtle, 8, 7, targetPos,
                                                   Mth::kPi / 2.0);
            }

            if (nextPos) {
                // MC checks hasChunksAt over a 34-block radius so the swim
                // never walks into ungenerated terrain; the loaded-position
                // probe is this engine's equivalent.
                const IBlockAccess* blocks = m_turtle->Level()->Blocks();
                const int xc = static_cast<int>(std::floor(nextPos->x));
                const int zc = static_cast<int>(std::floor(nextPos->z));
                if (blocks && !blocks->IsPositionLoaded(
                                  xc, static_cast<int>(nextPos->y), zc)) {
                    nextPos.reset();
                }
            }

            if (!nextPos) {
                m_stuck = true;
                return;
            }

            m_turtle->GetNavigation().MoveTo(nextPos->x, nextPos->y, nextPos->z,
                                             m_speedModifier);
        }
    }

    bool TurtleTravelGoal::CanContinueToUse() {
        return !m_turtle->GetNavigation().IsDone() && !m_stuck
            && !m_turtle->IsGoingHome() && !m_turtle->IsInLove()
            && !m_turtle->HasEgg();
    }

    void TurtleTravelGoal::Stop() { m_turtle->SetTravelPos(std::nullopt); }

    // ── TurtleRandomStrollGoal ─────────────────────────────────────────────

    TurtleRandomStrollGoal::TurtleRandomStrollGoal(Turtle* turtle,
                                                   double speedModifier,
                                                   int interval)
        : RandomStrollGoal(turtle, speedModifier, interval), m_turtle(turtle) {}

    bool TurtleRandomStrollGoal::CanUse() {
        return !m_turtle->IsInWater() && !m_turtle->IsGoingHome()
            && !m_turtle->HasEgg() && RandomStrollGoal::CanUse();
    }

} // namespace Game
