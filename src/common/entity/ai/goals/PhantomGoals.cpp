// File: src/common/entity/ai/goals/PhantomGoals.cpp
#include "common/entity/ai/goals/PhantomGoals.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Game {

    namespace {

        // MC Mth.approach — step `value` toward `target` by at most `delta`.
        float Approach(float value, float target, float delta) {
            return value < target ? std::min(value + delta, target)
                                  : std::max(value - delta, target);
        }

        // MC Level.getSeaLevel — the overworld noise settings value, same
        // constant the amphibious navigation and spawn placements use.
        constexpr int kSeaLevel = 63;

        // MC Level.getHeightmapPos(MOTION_BLOCKING, pos).y — the first free
        // cell above the highest motion-blocking block in the column. No
        // heightmap exists on IBlockAccess, so the column is walked; the
        // call is rare (once per strategy-goal stop).
        int MotionBlockingY(const IBlockAccess& blocks, int x, int z) {
            for (int y = 319; y >= -64; --y) {
                if (BlockRegistry::HasCollision(blocks.GetBlock(x, y, z))) {
                    return y + 1;
                }
            }
            return -64;
        }

    } // namespace

    // ── PhantomMoveControl ─────────────────────────────────────────────────

    PhantomMoveControl::PhantomMoveControl(Phantom* phantom)
        : MoveControl(phantom), m_phantom(phantom) {}

    void PhantomMoveControl::Tick() {
        // MC PhantomMoveControl.tick, transcribed. It ignores the wanted-
        // position plumbing entirely and steers on the phantom's own
        // moveTargetPoint.
        if (m_phantom->horizontalCollision) {
            m_phantom->yRot += 180.0f;
            m_speed = 0.1f;
        }

        const glm::dvec3& target = m_phantom->GetMoveTargetPoint();
        double tdx = target.x - m_phantom->position.x;
        double tdy = target.y - m_phantom->position.y;
        double tdz = target.z - m_phantom->position.z;
        double sd = std::sqrt(tdx * tdx + tdz * tdz);
        if (std::abs(sd) <= 1.0e-5) return;

        const double yRelativeScale = 1.0 - std::abs(tdy * 0.7) / sd;
        tdx *= yRelativeScale;
        tdz *= yRelativeScale;
        sd = std::sqrt(tdx * tdx + tdz * tdz);
        const double sd2 = std::sqrt(tdx * tdx + tdz * tdz + tdy * tdy);

        const float prevYRot = m_phantom->yRot;
        const float angle = static_cast<float>(std::atan2(tdz, tdx));
        const float a = Mth::WrapDegrees(m_phantom->yRot + 90.0f);
        const float b = Mth::WrapDegrees(angle * Mth::kRadToDeg);
        m_phantom->yRot = Mth::ApproachDegrees(a, b, 4.0f) - 90.0f;
        m_phantom->yBodyRot = m_phantom->yRot;

        if (std::abs(Mth::DegreesDifference(prevYRot, m_phantom->yRot)) < 3.0f) {
            m_speed = Approach(m_speed, 1.8f, 0.005f * (1.8f / m_speed));
        } else {
            m_speed = Approach(m_speed, 0.2f, 0.025f);
        }

        const float xRotD = static_cast<float>(
            -(std::atan2(-tdy, sd) * static_cast<double>(Mth::kRadToDeg)));
        m_phantom->xRot = xRotD;

        const float moveAngle = m_phantom->yRot + 90.0f;
        const double txd = static_cast<double>(
                               m_speed * std::cos(moveAngle * Mth::kDegToRad)) *
                           std::abs(tdx / sd2);
        const double tzd = static_cast<double>(
                               m_speed * std::sin(moveAngle * Mth::kDegToRad)) *
                           std::abs(tdz / sd2);
        const double tyd = static_cast<double>(
                               m_speed * std::sin(xRotD * Mth::kDegToRad)) *
                           std::abs(tdy / sd2);

        const glm::dvec3 desired(txd, tyd, tzd);
        m_phantom->velocity += (desired - m_phantom->velocity) * 0.2;
        m_phantom->needsSync = true;
    }

    // ── PhantomLookControl / PhantomBodyRotationControl ────────────────────

    PhantomLookControl::PhantomLookControl(Phantom* phantom)
        : LookControl(phantom) {}

    PhantomBodyRotationControl::PhantomBodyRotationControl(Phantom* phantom)
        : BodyRotationControl(phantom), m_phantom(phantom) {}

    void PhantomBodyRotationControl::ClientTick() {
        // MC: head snapped to the body, body snapped to the yaw.
        m_phantom->yHeadRot = m_phantom->yBodyRot;
        m_phantom->yBodyRot = m_phantom->yRot;
    }

    // ── PhantomMoveTargetGoal ──────────────────────────────────────────────

    PhantomMoveTargetGoal::PhantomMoveTargetGoal(Phantom* phantom)
        : m_phantom(phantom) {
        SetFlags(static_cast<uint8_t>(GoalFlag::Move));
    }

    bool PhantomMoveTargetGoal::TouchingTarget() const {
        const glm::dvec3 d = m_phantom->GetMoveTargetPoint() - m_phantom->position;
        return glm::dot(d, d) < 4.0;
    }

    // ── PhantomCircleAroundAnchorGoal ──────────────────────────────────────

    bool PhantomCircleAroundAnchorGoal::CanUse() {
        return m_phantom->GetTarget() == nullptr ||
               m_phantom->GetAttackPhase() == PhantomAttackPhase::Circle;
    }

    void PhantomCircleAroundAnchorGoal::Start() {
        JavaRandom& rng = m_phantom->Level()->Random();
        m_distance = 5.0f + rng.NextFloat() * 10.0f;
        m_height = -4.0f + rng.NextFloat() * 9.0f;
        // MC random.nextBoolean() — JavaRandom here has no boolean draw;
        // NextInt(2) consumes the same single next() call.
        m_clockwise = rng.NextInt(2) == 0 ? 1.0f : -1.0f;
        SelectNext();
    }

    void PhantomCircleAroundAnchorGoal::Tick() {
        EntityLevel* level = m_phantom->Level();
        if (!level) return;
        JavaRandom& rng = level->Random();

        if (rng.NextInt(AdjustedTickDelay(350)) == 0) {
            m_height = -4.0f + rng.NextFloat() * 9.0f;
        }

        if (rng.NextInt(AdjustedTickDelay(250)) == 0) {
            ++m_distance;
            if (m_distance > 15.0f) {
                m_distance = 5.0f;
                m_clockwise = -m_clockwise;
            }
        }

        if (rng.NextInt(AdjustedTickDelay(450)) == 0) {
            m_angle = rng.NextFloat() * 2.0f * Mth::kPi;
            SelectNext();
        }

        if (TouchingTarget()) {
            SelectNext();
        }

        // MC: an orbit that would descend into (or climb into) a solid block
        // clamps the height offset and re-picks.
        const IBlockAccess* blocks = level->Blocks();
        if (blocks) {
            const glm::ivec3 p = m_phantom->BlockPosition();
            if (m_phantom->GetMoveTargetPoint().y < m_phantom->position.y &&
                blocks->GetBlock(p.x, p.y - 1, p.z) != BlockID::Air) {
                m_height = std::max(1.0f, m_height);
                SelectNext();
            }
            if (m_phantom->GetMoveTargetPoint().y > m_phantom->position.y &&
                blocks->GetBlock(p.x, p.y + 1, p.z) != BlockID::Air) {
                m_height = std::min(-1.0f, m_height);
                SelectNext();
            }
        }
    }

    void PhantomCircleAroundAnchorGoal::SelectNext() {
        if (!m_phantom->HasAnchorPoint()) {
            m_phantom->SetAnchorPoint(m_phantom->BlockPosition());
        }

        m_angle += m_clockwise * 15.0f * Mth::kDegToRad;
        const glm::ivec3& anchor = m_phantom->GetAnchorPoint();
        m_phantom->SetMoveTargetPoint(glm::dvec3(
            static_cast<double>(anchor.x) +
                static_cast<double>(m_distance * std::cos(m_angle)),
            static_cast<double>(anchor.y) + static_cast<double>(-4.0f + m_height),
            static_cast<double>(anchor.z) +
                static_cast<double>(m_distance * std::sin(m_angle))));
    }

    // ── PhantomSweepAttackGoal ─────────────────────────────────────────────

    bool PhantomSweepAttackGoal::CanUse() {
        return m_phantom->GetTarget() != nullptr &&
               m_phantom->GetAttackPhase() == PhantomAttackPhase::Swoop;
    }

    bool PhantomSweepAttackGoal::CanContinueToUse() {
        LivingEntity* target = m_phantom->GetTarget();
        if (target == nullptr) return false;
        if (!target->IsAlive()) return false;

        // MC: spectators and creative players end the dive.
        if (target->IsPlayer() &&
            (target->IsSpectator() || target->IsCreative())) {
            return false;
        }

        if (!CanUse()) return false;

        // MC: every 20 ticks, scan for living cats within 16 blocks. Each
        // found cat hisses (sound — no sound system); ANY cat scares the
        // phantom off its dive.
        if (m_phantom->tickCount > m_catSearchTick) {
            m_catSearchTick = m_phantom->tickCount + 20;
            EntityLevel* level = m_phantom->Level();
            bool anyCat = false;
            if (level) {
                AABB box = m_phantom->GetAABB();
                box.min -= glm::vec3(16.0f);
                box.max += glm::vec3(16.0f);
                std::vector<Entity*> nearby;
                level->GetEntitiesInBox(box, m_phantom, nearby);
                for (Entity* e : nearby) {
                    if (e->GetType() == EntityTypeId::Cat && e->IsAlive()) {
                        anyCat = true;
                    }
                }
            }
            m_isScaredOfCat = anyCat;
        }

        return !m_isScaredOfCat;
    }

    void PhantomSweepAttackGoal::Stop() {
        m_phantom->SetTarget(nullptr);
        m_phantom->SetAttackPhase(PhantomAttackPhase::Circle);
    }

    void PhantomSweepAttackGoal::Tick() {
        LivingEntity* target = m_phantom->GetTarget();
        if (target == nullptr) return;

        m_phantom->SetMoveTargetPoint(glm::dvec3(
            target->position.x,
            target->position.y + static_cast<double>(target->GetBbHeight()) * 0.5,
            target->position.z));

        AABB reach = m_phantom->GetAABB();
        reach.min -= glm::vec3(0.2f);
        reach.max += glm::vec3(0.2f);
        if (reach.Intersects(target->GetAABB())) {
            m_phantom->DoHurtTarget(*target);
            m_phantom->SetAttackPhase(PhantomAttackPhase::Circle);
            // MC level event 1039 (phantom bite sound) — no sound system.
        } else if (m_phantom->horizontalCollision || m_phantom->hurtTime > 0) {
            m_phantom->SetAttackPhase(PhantomAttackPhase::Circle);
        }
    }

    // ── PhantomAttackStrategyGoal ──────────────────────────────────────────

    bool PhantomAttackStrategyGoal::CanUse() {
        LivingEntity* target = m_phantom->GetTarget();
        if (target == nullptr) return false;
        // MC: canAttack(level, target, TargetingConditions.DEFAULT).
        return TargetingConditions::ForCombat().Test(m_phantom, *target);
    }

    void PhantomAttackStrategyGoal::Start() {
        m_nextSweepTick = AdjustedTickDelay(10);
        m_phantom->SetAttackPhase(PhantomAttackPhase::Circle);
        SetAnchorAboveTarget();
    }

    void PhantomAttackStrategyGoal::Stop() {
        // MC: park the anchor 10-30 blocks above the surface under it, so
        // the idle orbit resumes at a sane height.
        if (m_phantom->HasAnchorPoint() && m_phantom->Level() &&
            m_phantom->Level()->Blocks()) {
            const glm::ivec3& anchor = m_phantom->GetAnchorPoint();
            const int surfaceY = MotionBlockingY(*m_phantom->Level()->Blocks(),
                                                 anchor.x, anchor.z);
            m_phantom->SetAnchorPoint(glm::ivec3(
                anchor.x,
                surfaceY + 10 + m_phantom->Level()->Random().NextInt(20),
                anchor.z));
        }
    }

    void PhantomAttackStrategyGoal::Tick() {
        if (m_phantom->GetAttackPhase() == PhantomAttackPhase::Circle) {
            --m_nextSweepTick;
            if (m_nextSweepTick <= 0) {
                m_phantom->SetAttackPhase(PhantomAttackPhase::Swoop);
                SetAnchorAboveTarget();
                m_nextSweepTick = AdjustedTickDelay(
                    (8 + m_phantom->Level()->Random().NextInt(4)) * 20);
                // MC plays PHANTOM_SWOOP here — no sound system.
            }
        }
    }

    void PhantomAttackStrategyGoal::SetAnchorAboveTarget() {
        // MC reads getTarget() unguarded (canUse guarantees one); the guard
        // here is C++ pointer hygiene, not behaviour.
        LivingEntity* target = m_phantom->GetTarget();
        if (!m_phantom->HasAnchorPoint() || target == nullptr) return;

        glm::ivec3 anchor = target->BlockPosition();
        anchor.y += 20 + m_phantom->Level()->Random().NextInt(20);
        if (anchor.y < kSeaLevel) {
            anchor.y = kSeaLevel + 1;
        }
        m_phantom->SetAnchorPoint(anchor);
    }

    // ── PhantomAttackPlayerTargetGoal ──────────────────────────────────────

    PhantomAttackPlayerTargetGoal::PhantomAttackPlayerTargetGoal(Phantom* phantom)
        : m_phantom(phantom),
          m_attackTargeting(TargetingConditions::ForCombat().Range(64.0)),
          m_nextScanTick(ReducedTickDelay(20)) {}

    bool PhantomAttackPlayerTargetGoal::CanUse() {
        if (m_nextScanTick > 0) {
            --m_nextScanTick;
            return false;
        }

        m_nextScanTick = ReducedTickDelay(60);
        EntityLevel* level = m_phantom->Level();
        if (!level) return false;

        // MC: players inside the box inflated (16, 64, 16), highest first.
        AABB box = m_phantom->GetAABB();
        box.min -= glm::vec3(16.0f, 64.0f, 16.0f);
        box.max += glm::vec3(16.0f, 64.0f, 16.0f);

        std::vector<LivingEntity*> players;
        level->GetPlayers(players);
        std::vector<LivingEntity*> candidates;
        for (LivingEntity* player : players) {
            if (!player) continue;
            if (!player->GetAABB().Intersects(box)) continue;
            if (!m_attackTargeting.Test(m_phantom, *player)) continue;
            candidates.push_back(player);
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const LivingEntity* a, const LivingEntity* b) {
                      return a->position.y > b->position.y;
                  });

        for (LivingEntity* player : candidates) {
            if (TargetingConditions::ForCombat().Test(m_phantom, *player)) {
                m_phantom->SetTarget(player);
                return true;
            }
        }
        return false;
    }

    bool PhantomAttackPlayerTargetGoal::CanContinueToUse() {
        LivingEntity* target = m_phantom->GetTarget();
        return target != nullptr &&
               TargetingConditions::ForCombat().Test(m_phantom, *target);
    }

} // namespace Game
