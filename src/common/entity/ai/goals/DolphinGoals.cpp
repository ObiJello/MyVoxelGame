// File: src/common/entity/ai/goals/DolphinGoals.cpp
#include "common/entity/ai/goals/DolphinGoals.hpp"

#include "common/entity/mobs/Fish.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <cmath>

namespace Game {

    namespace {

        // MC Entity.getMotionDirection, reduced to the horizontal step the
        // jump goal samples along: the dominant axis of the velocity, or of
        // the facing when the dolphin is coasting.
        void MotionStep(const Dolphin& dolphin, int& stepX, int& stepZ) {
            double x = dolphin.velocity.x;
            double z = dolphin.velocity.z;
            if (x * x + z * z < 1.0e-8) {
                const float yaw = dolphin.yRot * Mth::kDegToRad;
                x = -std::sin(yaw);
                z = std::cos(yaw);
            }
            if (std::abs(x) > std::abs(z)) {
                stepX = x > 0.0 ? 1 : -1;
                stepZ = 0;
            } else {
                stepX = 0;
                stepZ = z > 0.0 ? 1 : -1;
            }
        }

    } // namespace

    // ── DolphinJumpGoal ────────────────────────────────────────────────────

    DolphinJumpGoal::DolphinJumpGoal(Dolphin* dolphin, int interval)
        : m_dolphin(dolphin), m_interval(ReducedTickDelay(interval)) {
        // MC JumpGoal's flags.
        SetFlags(GoalFlag::Move | GoalFlag::Jump);
    }

    bool DolphinJumpGoal::CanUse() {
        EntityLevel* level = m_dolphin->Level();
        if (!level) return false;
        if (level->Random().NextInt(m_interval) != 0) return false;

        int stepX = 0, stepZ = 0;
        MotionStep(*m_dolphin, stepX, stepZ);
        const glm::ivec3 pos = m_dolphin->BlockPosition();

        // MC STEPS_TO_CHECK — water ahead at these steps, air two high above.
        static constexpr int kSteps[] = { 0, 1, 4, 5, 6, 7 };
        for (int step : kSteps) {
            if (!WaterIsClear(pos, stepX, stepZ, step)
                || !SurfaceIsClear(pos, stepX, stepZ, step)) {
                return false;
            }
        }
        return true;
    }

    bool DolphinJumpGoal::WaterIsClear(const glm::ivec3& pos, int stepX,
                                       int stepZ, int step) const {
        const IBlockAccess* blocks = m_dolphin->Level()->Blocks();
        if (!blocks) return false;
        const int x = pos.x + stepX * step, z = pos.z + stepZ * step;
        // MC: water AND not motion-blocking (waterlogged solids fail).
        return blocks->GetBlock(x, pos.y, z) == BlockID::Water;
    }

    bool DolphinJumpGoal::SurfaceIsClear(const glm::ivec3& pos, int stepX,
                                         int stepZ, int step) const {
        const IBlockAccess* blocks = m_dolphin->Level()->Blocks();
        if (!blocks) return false;
        const int x = pos.x + stepX * step, z = pos.z + stepZ * step;
        return blocks->GetBlock(x, pos.y + 1, z) == BlockID::Air
            && blocks->GetBlock(x, pos.y + 2, z) == BlockID::Air;
    }

    bool DolphinJumpGoal::CanContinueToUse() {
        const double yd = m_dolphin->velocity.y;
        return (!(yd * yd < 0.03) || m_dolphin->xRot == 0.0f
                || !(std::abs(m_dolphin->xRot) < 10.0f)
                || !m_dolphin->IsInWater())
            && !m_dolphin->onGround;
    }

    void DolphinJumpGoal::Start() {
        int stepX = 0, stepZ = 0;
        MotionStep(*m_dolphin, stepX, stepZ);
        m_dolphin->velocity += glm::dvec3(stepX * 0.6, 0.7, stepZ * 0.6);
        m_dolphin->needsSync = true;
        m_dolphin->GetNavigation().Stop();
        m_breached = false;
    }

    void DolphinJumpGoal::Stop() { m_dolphin->xRot = 0.0f; }

    void DolphinJumpGoal::Tick() {
        const bool alreadyBreached = m_breached;
        if (!alreadyBreached) {
            const IBlockAccess* blocks = m_dolphin->Level()->Blocks();
            const glm::ivec3 pos = m_dolphin->BlockPosition();
            m_breached =
                blocks && blocks->GetBlock(pos.x, pos.y, pos.z) == BlockID::Water;
        }
        // MC plays DOLPHIN_JUMP on the breach edge — sounds wait on the
        // sound system.

        const glm::dvec3 v = m_dolphin->velocity;
        if (v.y * v.y < 0.03 && m_dolphin->xRot != 0.0f) {
            m_dolphin->xRot = Mth::RotLerp(0.2f, m_dolphin->xRot, 0.0f);
        } else if (glm::length(v) > 1.0e-5) {
            const double horizontal = std::sqrt(v.x * v.x + v.z * v.z);
            const double rotation =
                std::atan2(-v.y, horizontal) * (180.0 / Mth::kPi);
            m_dolphin->xRot = static_cast<float>(rotation);
        }
    }

    // ── BreathAirGoal ──────────────────────────────────────────────────────

    bool BreathAirGoal::CanUse() {
        // MC: air below 140 (of 4800) — about seven seconds of breath left.
        return m_dolphin->GetAirSupply() < 140;
    }

    void BreathAirGoal::Start() { FindAirPosition(); }

    bool BreathAirGoal::GivesAir(const glm::ivec3& pos) const {
        // MC givesAir: the fluid state is empty (or a bubble column — none
        // here) AND the block is pathfindable as LAND. "No water and no
        // collision" is that test against this engine's block data.
        EntityLevel* level = m_dolphin->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;
        return !blocks->ContainsWater(pos.x, pos.y, pos.z) &&
               !BlockRegistry::HasCollision(blocks->GetBlock(pos.x, pos.y, pos.z));
    }

    void BreathAirGoal::FindAirPosition() {
        // MC findAirPosition: scan the 3x3 column from the dolphin's feet to
        // 8 blocks up for the first breathable block; failing that, aim 8
        // blocks straight up anyway. Then path to one above it at speed 1.
        const glm::ivec3 origin = m_dolphin->BlockPosition();
        const int x0 = static_cast<int>(std::floor(m_dolphin->position.x - 1.0));
        const int x1 = static_cast<int>(std::floor(m_dolphin->position.x + 1.0));
        const int z0 = static_cast<int>(std::floor(m_dolphin->position.z - 1.0));
        const int z1 = static_cast<int>(std::floor(m_dolphin->position.z + 1.0));
        const int y1 = static_cast<int>(std::floor(m_dolphin->position.y + 8.0));

        bool found = false;
        glm::ivec3 destination{0};
        for (int x = x0; x <= x1 && !found; ++x) {
            for (int y = origin.y; y <= y1 && !found; ++y) {
                for (int z = z0; z <= z1 && !found; ++z) {
                    const glm::ivec3 p(x, y, z);
                    if (GivesAir(p)) {
                        destination = p;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            destination = glm::ivec3(
                static_cast<int>(std::floor(m_dolphin->position.x)),
                static_cast<int>(std::floor(m_dolphin->position.y + 8.0)),
                static_cast<int>(std::floor(m_dolphin->position.z)));
        }

        m_dolphin->GetNavigation().MoveTo(destination.x, destination.y + 1,
                                          destination.z, 1.0);
    }

    void BreathAirGoal::Tick() {
        // MC tick: re-aim every tick and ALSO push directly — moveRelative
        // (0.02) + move(SELF, deltaMovement) — so a drowning dolphin rises
        // even where the navigator has no path.
        FindAirPosition();
        m_dolphin->MoveRelative(0.02f, glm::dvec3(m_dolphin->xxa,
                                                  m_dolphin->yya,
                                                  m_dolphin->zza));
        m_dolphin->Move(m_dolphin->velocity);
    }

    // ── TryFindWaterGoal ───────────────────────────────────────────────────

    TryFindWaterGoal::TryFindWaterGoal(Dolphin* dolphin) : m_dolphin(dolphin) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool TryFindWaterGoal::CanUse() {
        // MC: on solid ground and not in water.
        return m_dolphin->onGround && !m_dolphin->IsInWater();
    }

    void TryFindWaterGoal::Start() {
        // MC: scan the ±2 box around the feet for the closest block whose
        // top face is water, and path to it.
        EntityLevel* level = m_dolphin->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return;
        const glm::ivec3 origin = m_dolphin->BlockPosition();

        bool found = false;
        glm::ivec3 best{0};
        double bestDistSq = 0.0;
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dz = -2; dz <= 2; ++dz) {
                    const glm::ivec3 p = origin + glm::ivec3(dx, dy, dz);
                    if (blocks->GetBlock(p.x, p.y, p.z) != BlockID::Water) {
                        continue;
                    }
                    const double dd = static_cast<double>(dx * dx + dy * dy
                                                          + dz * dz);
                    if (!found || dd < bestDistSq) {
                        found = true;
                        best = p;
                        bestDistSq = dd;
                    }
                }
            }
        }
        if (found) {
            m_dolphin->GetNavigation().MoveTo(best.x + 0.5, best.y + 0.5,
                                              best.z + 0.5, 1.0);
        }
    }

    // ── DolphinSwimToTreasureGoal ──────────────────────────────────────────

    DolphinSwimToTreasureGoal::DolphinSwimToTreasureGoal(Dolphin* dolphin)
        : m_dolphin(dolphin) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool DolphinSwimToTreasureGoal::CanUse() {
        // MC gates on gotFish && air >= 100; feeding rides the item layer,
        // so gotFish is never raised and the hunt never starts. (When it
        // does, start() below already runs MC's no-structure path.)
        return false;
    }

    void DolphinSwimToTreasureGoal::Start() {
        // MC start(): findNearestMapStructure(DOLPHIN_LOCATED) — no
        // structure registry exists, which is MC's own "stuck" branch.
        m_dolphin->GetNavigation().Stop();
    }

    void DolphinSwimToTreasureGoal::Stop() {}

} // namespace Game
