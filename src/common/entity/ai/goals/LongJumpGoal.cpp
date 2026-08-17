// File: src/common/entity/ai/goals/LongJumpGoal.cpp
#include "common/entity/ai/goals/LongJumpGoal.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // MC LongJumpToRandomPos's constants.
        constexpr int kFindJumpTries       = 20;
        constexpr int kPrepareJumpDuration = 40;
        constexpr int kMinPathfindDistance = 8;   // maxVisitedNodesMultiplier
        constexpr int kTimeOutDuration     = 200;
        constexpr int kMidJumpTimeOut      = 100; // LongJumpMidJump

        // MC ALLOWED_ANGLES, in degrees above horizontal. Shuffled per attempt,
        // so a frog does not always pick the same arc to the same spot.
        constexpr int kAllowedAngles[] = { 65, 70, 75, 80 };

        // MC Entity.level().noCollision(entity, box) reduced to what matters
        // here: does this box overlap terrain. Entity-vs-entity is excluded
        // deliberately — MC's noCollision(Entity, AABB) skips the entity itself
        // and the mobs it could push through anyway.
        bool NoCollision(const EntityLevel& level, const AABB& box) {
            return !CollidesAt(box, level.Physics());
        }

        AABB BoxAt(const glm::dvec3& centreBottom, float width, float height) {
            const float half = width * 0.5f;
            AABB box;
            box.min = glm::vec3(centreBottom.x - half, centreBottom.y, centreBottom.z - half);
            box.max = glm::vec3(centreBottom.x + half, centreBottom.y + height,
                                centreBottom.z + half);
            return box;
        }

        // MC LongJumpUtil.isClearTransition — sample the segment between two
        // arc points at least once per (min dimension), and require every
        // sample's bounding box to be free.
        bool IsClearTransition(const EntityLevel& level, float width, float height,
                               const glm::dvec3& from, const glm::dvec3& to) {
            const glm::dvec3 direction = to - from;
            const double minDimension = static_cast<double>(std::min(width, height));
            if (minDimension <= 0.0) return true;

            const double length = glm::length(direction);
            const int checks = static_cast<int>(std::ceil(length / minDimension));
            if (checks <= 0) return true;

            const glm::dvec3 step = (length > 0.0 ? direction / length : glm::dvec3(0.0))
                                    * (minDimension * 0.9);
            glm::dvec3 point = from;
            for (int i = 0; i < checks; ++i) {
                point = (i == checks - 1) ? to : point + step;
                if (!NoCollision(level, BoxAt(point, width, height))) return false;
            }
            return true;
        }

    } // namespace

    std::optional<glm::dvec3> CalculateJumpVectorForAngle(
            Mob& mob, const glm::dvec3& targetPos, float maxJumpVelocity,
            int angleDegrees, bool checkCollision) {
        EntityLevel* level = mob.Level();
        if (!level) return std::nullopt;

        const glm::dvec3 mobPos = mob.position;

        // MC pulls the aim point half a block back toward the mob along the
        // horizontal, so the arc lands ON the block rather than at its far
        // edge — without it a frog clips the block it is aiming for.
        glm::dvec3 plane(targetPos.x - mobPos.x, 0.0, targetPos.z - mobPos.z);
        const double planeLen = glm::length(plane);
        if (planeLen > 1.0e-9) plane = plane / planeLen * 0.5;
        else                   plane = glm::dvec3(0.0);

        const glm::dvec3 aim = targetPos - plane;
        const glm::dvec3 direction = aim - mobPos;

        const double angrad = static_cast<double>(angleDegrees) * Mth::kPi / 180.0;
        const double xzAng  = std::atan2(direction.z, direction.x);

        const glm::dvec3 horizontal(direction.x, 0.0, direction.z);
        const double r2 = glm::dot(horizontal, horizontal);
        const double r  = std::sqrt(r2);
        const double y  = direction.y;
        const double g  = mob.GetGravity();

        const double sin2ang    = std::sin(2.0 * angrad);
        const double cosangsqr  = std::cos(angrad) * std::cos(angrad);
        const double sinangrad  = std::sin(angrad);
        const double cosangrad  = std::cos(angrad);
        const double sinxzAng   = std::sin(xzAng);
        const double cosxzAng   = std::cos(xzAng);

        const double denom = r * sin2ang - 2.0 * y * cosangsqr;
        if (denom == 0.0) return std::nullopt;

        // The launch speed that satisfies the ballistic equation for this
        // angle. Negative means no real solution — the target is behind the
        // arc's reach at this angle, which is why MC tries four of them.
        const double v0sqr = r2 * g / denom;
        if (v0sqr < 0.0) return std::nullopt;

        const double v0 = std::sqrt(v0sqr);
        if (v0 > static_cast<double>(maxJumpVelocity)) return std::nullopt;

        const double v0r = v0 * cosangrad;
        const double v0y = v0 * sinangrad;

        if (checkCollision && v0r > 0.0) {
            const int samples = static_cast<int>(std::ceil(r / v0r)) * 2;
            const float width  = mob.GetBbWidth();
            const float height = mob.GetBbHeight();

            double ri = 0.0;
            bool havePrev = false;
            glm::dvec3 previous(0.0);

            for (int i = 0; i < samples - 1; ++i) {
                ri += r / static_cast<double>(samples);
                const double yi = sinangrad / cosangrad * ri
                                - (ri * ri) * g / (2.0 * v0sqr * cosangrad * cosangrad);
                const glm::dvec3 sample(mobPos.x + ri * cosxzAng,
                                        mobPos.y + yi,
                                        mobPos.z + ri * sinxzAng);
                if (havePrev
                    && !IsClearTransition(*level, width, height, previous, sample)) {
                    return std::nullopt;
                }
                previous = sample;
                havePrev = true;
            }
        }

        // MC's final 0.95 — the arc is solved exactly and then deliberately
        // undershot, so a mob lands ON the block rather than skidding past it.
        return glm::dvec3(v0r * cosxzAng, v0y, v0r * sinxzAng) * 0.95;
    }

    // ── LongJumpGoal ───────────────────────────────────────────────────────

    LongJumpGoal::LongJumpGoal(Mob* mob, const Config& config)
        : m_mob(mob), m_cfg(config) {
        // MC's behaviour claims the look target and drives the body, and
        // nothing else may steer while it is crouching or airborne.
        SetFlags(GoalFlag::Move | GoalFlag::Look | GoalFlag::Jump);
    }

    int LongJumpGoal::SampleCooldown() const {
        EntityLevel* level = m_mob->Level();
        if (!level) return m_cfg.cooldownMin;
        return level->Random().NextInt(m_cfg.cooldownMin, m_cfg.cooldownMax);
    }

    bool LongJumpGoal::DefaultAcceptableLandingSpot(Mob& mob, const glm::ivec3& target) {
        EntityLevel* level = mob.Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;

        // MC: `isSolidRender` on the block below. IsBlockSolid is this engine's
        // nearest equivalent — it is the same question the pathfinder asks
        // about standable ground.
        if (!blocks->IsBlockSolid(target.x, target.y - 1, target.z)) return false;

        PathfindingContext ctx;
        ctx.blocks = blocks;
        ctx.mob = &mob;
        const PathType type =
            WalkNodeEvaluator::GetPathTypeStatic(ctx, target.x, target.y, target.z);
        // Malus 0 means "no penalty" — MC requires the landing square to be one
        // the mob would happily walk on, not merely one it can survive.
        return mob.GetPathfindingMalus(type) == 0.0f;
    }

    bool LongJumpGoal::CanUse() {
        if (m_cfg.ownsCooldown && m_cooldownUntilTick >= 0
            && m_mob->tickCount < m_cooldownUntilTick) {
            return false;
        }

        // MC checkExtraStartConditions. The honey-block clause is dropped: this
        // engine has no honey block, so there is nothing to test.
        const bool canStart = m_mob->onGround && !m_mob->IsInWater() && !m_mob->IsInLava();
        if (!canStart) {
            // MC halves the cooldown on a failed start, so a frog that lands in
            // water retries sooner than one that has just jumped.
            if (m_cfg.ownsCooldown) {
                m_cooldownUntilTick = m_mob->tickCount + SampleCooldown() / 2;
            }
            return false;
        }
        return true;
    }

    void LongJumpGoal::Start() {
        m_phase = Phase::Searching;
        m_chosenJump.reset();
        m_findJumpTries = kFindJumpTries;
        m_prepareTicks = 0;
        m_startTick = m_mob->tickCount;
        m_initialPosition = m_mob->position;

        // MC's LONG_JUMP activity does not contain MoveToTargetSink, so taking
        // the activity runs that behaviour's stop(), which calls
        // navigation.stop(). Without the equivalent here the stroll goal's path
        // survives losing the MOVE flag and PathNavigation::Tick keeps steering
        // — the frog never stops moving, canStillUse's position check fails
        // every tick, and the jump can never get past its 40-tick crouch.
        m_mob->GetNavigation().Stop();

        m_candidates.clear();
        m_notPreferred.clear();

        const glm::ivec3 mobPos = m_mob->BlockPosition();
        for (int x = mobPos.x - m_cfg.maxWidth; x <= mobPos.x + m_cfg.maxWidth; ++x) {
            for (int y = mobPos.y - m_cfg.maxHeight; y <= mobPos.y + m_cfg.maxHeight; ++y) {
                for (int z = mobPos.z - m_cfg.maxWidth; z <= mobPos.z + m_cfg.maxWidth; ++z) {
                    const glm::ivec3 p(x, y, z);
                    if (p == mobPos) continue;
                    // MC's weight is ceil(distSqr) — the FURTHEST candidates are
                    // the most likely, which is what makes a frog commit to a
                    // real leap instead of hopping one block.
                    const glm::ivec3 d = p - mobPos;
                    const int weight = d.x * d.x + d.y * d.y + d.z * d.z;
                    m_candidates.push_back(PossibleJump{ p, weight });
                }
            }
        }

        EntityLevel* level = m_mob->Level();
        m_wantingPreferred = m_cfg.preferredBlockCount > 0 && level
                          && level->Random().NextFloat() < m_cfg.preferredBlocksChance;
    }

    bool LongJumpGoal::CanContinueToUse() {
        // Elapsed ticks, not a counter: CanContinueToUse is polled on the
        // 2-tick goal cadence while Tick runs every tick, so counting here
        // would make every MC duration twice as long.
        const int elapsed = static_cast<int>(m_mob->tickCount - m_startTick);

        if (m_phase == Phase::MidJump) {
            // MC LongJumpMidJump.canStillUse, plus its 100-tick timeout so a
            // mob wedged in the air cannot hold the goal forever.
            return !m_mob->onGround && elapsed < kTimeOutDuration + kMidJumpTimeOut;
        }

        if (elapsed >= kTimeOutDuration) {
            if (m_cfg.ownsCooldown) {
                m_cooldownUntilTick = m_mob->tickCount + SampleCooldown() / 2;
            }
            return false;
        }

        // MC's canStillUse: the mob must not have MOVED while searching. If it
        // has, the candidate weights and the solved arcs are all stale.
        const bool isValid = m_initialPosition == m_mob->position
                          && m_findJumpTries > 0
                          && !m_mob->IsInWater()
                          && (m_chosenJump.has_value() || !m_candidates.empty()
                              || !m_notPreferred.empty());
        if (!isValid && m_cfg.ownsCooldown) {
            m_cooldownUntilTick = m_mob->tickCount + SampleCooldown() / 2;
        }
        return isValid;
    }

    void LongJumpGoal::Tick() {
        if (m_phase == Phase::MidJump) return;

        if (m_chosenJump.has_value()) {
            // MC pickCandidate sets the LOOK_TARGET memory, and the CORE
            // activity's LookAtTargetSink re-applies it EVERY tick. Both halves
            // matter, and dropping them is why the frog used to launch sideways:
            //
            //   look control  turns yHeadRot toward the block, 10 deg/tick
            //   body control  sees a head that has held still for 10 ticks and
            //                 then closes the body-to-head gap from 75 deg to 0
            //                 over the next 10 (rotateHeadTowardsFront)
            //   launch        setYRot(yBodyRot) locks the facing in
            //
            // That chain needs a look target that PERSISTS. Setting it once
            // leaves the head drifting back to the body two ticks later, the
            // body never turns, and the frog flies at a target it is not facing
            // — the velocity was always right, only the facing was wrong.
            // MC's 40-tick crouch is exactly long enough for the chain to
            // finish, which is why the number is not a stylistic choice.
            m_mob->GetLookControl().SetLookAt(m_lookTarget);

            if (++m_prepareTicks < kPrepareJumpDuration) return;

            // MC: face the way the body is already pointing, drop friction, and
            // launch. `needsSync` gets the velocity to the client immediately
            // rather than on the next periodic send — without it the client
            // sees the frog teleport along the arc instead of flying it.
            m_mob->yRot = m_mob->yBodyRot;
            m_mob->SetDiscardFriction(true);
            m_mob->velocity = *m_chosenJump;
            m_mob->needsSync = true;
            m_mob->hurtMarked = true;
            m_mob->SetPose(Pose::LongJumping);
            m_phase = Phase::MidJump;
            return;
        }

        --m_findJumpTries;
        PickCandidate();
    }

    void LongJumpGoal::Stop() {
        if (m_phase == Phase::MidJump) {
            // MC LongJumpMidJump.stop — kill the horizontal carry on landing so
            // the frog stops where it lands instead of sliding on.
            if (m_mob->onGround) {
                m_mob->velocity.x *= 0.1;
                m_mob->velocity.z *= 0.1;
            }
            if (m_cfg.ownsCooldown) {
                m_cooldownUntilTick = m_mob->tickCount + SampleCooldown();
            }
        }
        m_mob->SetDiscardFriction(false);
        m_mob->SetPose(Pose::Standing);
        m_phase = Phase::Searching;
        m_chosenJump.reset();
        m_candidates.clear();
        m_notPreferred.clear();
    }

    bool LongJumpGoal::IsPreferredBlockBelow(const glm::ivec3& target) const {
        EntityLevel* level = m_mob->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return false;
        const BlockID below = blocks->GetBlock(target.x, target.y - 1, target.z);
        for (int i = 0; i < m_cfg.preferredBlockCount; ++i) {
            if (m_cfg.preferredBlocks[i] == below) return true;
        }
        return false;
    }

    std::optional<LongJumpGoal::PossibleJump> LongJumpGoal::GetJumpCandidate() {
        EntityLevel* level = m_mob->Level();
        if (!level) return std::nullopt;

        // MC WeightedRandom.getRandomItem, then removes the winner so a
        // candidate is never considered twice.
        const auto takeWeighted = [&]() -> std::optional<PossibleJump> {
            if (m_candidates.empty()) return std::nullopt;
            int total = 0;
            for (const PossibleJump& c : m_candidates) total += c.weight;
            if (total <= 0) return std::nullopt;
            int roll = level->Random().NextInt(total);
            for (size_t i = 0; i < m_candidates.size(); ++i) {
                roll -= m_candidates[i].weight;
                if (roll < 0) {
                    const PossibleJump picked = m_candidates[i];
                    m_candidates.erase(m_candidates.begin() + static_cast<long>(i));
                    return picked;
                }
            }
            return std::nullopt;
        };

        if (!m_wantingPreferred) return takeWeighted();

        // MC LongJumpToPreferredBlock: drain the whole list looking for a
        // preferred block, keeping the rejects, then fall back to them.
        while (!m_candidates.empty()) {
            std::optional<PossibleJump> candidate = takeWeighted();
            if (!candidate) break;
            if (IsPreferredBlockBelow(candidate->targetPos)) return candidate;
            m_notPreferred.push_back(*candidate);
        }
        if (!m_notPreferred.empty()) {
            const PossibleJump front = m_notPreferred.front();
            m_notPreferred.erase(m_notPreferred.begin());
            return front;
        }
        return std::nullopt;
    }

    bool LongJumpGoal::IsAcceptableLandingPosition(const glm::ivec3& target) const {
        const glm::ivec3 mobPos = m_mob->BlockPosition();
        // MC rejects a target in the mob's own column — that is a hop in place.
        if (mobPos.x == target.x && mobPos.z == target.z) return false;
        return m_cfg.acceptableLandingSpot ? m_cfg.acceptableLandingSpot(*m_mob, target)
                                           : DefaultAcceptableLandingSpot(*m_mob, target);
    }

    std::optional<glm::dvec3> LongJumpGoal::CalculateOptimalJumpVector(
            const glm::dvec3& targetPos) {
        EntityLevel* level = m_mob->Level();
        if (!level) return std::nullopt;

        int angles[4] = { kAllowedAngles[0], kAllowedAngles[1],
                          kAllowedAngles[2], kAllowedAngles[3] };
        // MC Collections.shuffle — Fisher-Yates downward, which is what
        // java.util.Collections.shuffle does.
        for (int i = 3; i > 0; --i) {
            const int j = level->Random().NextInt(i + 1);
            std::swap(angles[i], angles[j]);
        }

        const float maxJumpVelocity = static_cast<float>(
            m_mob->GetAttributeValue(Attribute::JumpStrength)
            * static_cast<double>(m_cfg.maxJumpVelocityMultiplier));

        for (const int angle : angles) {
            std::optional<glm::dvec3> v = CalculateJumpVectorForAngle(
                *m_mob, targetPos, maxJumpVelocity, angle, true);
            if (v) return v;
        }
        return std::nullopt;
    }

    void LongJumpGoal::PickCandidate() {
        while (!m_candidates.empty() || !m_notPreferred.empty()) {
            std::optional<PossibleJump> candidate = GetJumpCandidate();
            if (!candidate) return;

            const glm::ivec3 target = candidate->targetPos;
            if (!IsAcceptableLandingPosition(target)) continue;

            const glm::dvec3 targetCentre(static_cast<double>(target.x) + 0.5,
                                          static_cast<double>(target.y),
                                          static_cast<double>(target.z) + 0.5);
            std::optional<glm::dvec3> jump = CalculateOptimalJumpVector(targetCentre);
            if (!jump) continue;

            // MC's last filter, and the one that makes the behaviour read as
            // intelligent: if the mob could simply WALK there, it walks. Only
            // an unreachable spot is worth jumping to.
            std::optional<Path> path = m_mob->GetNavigation().CreatePath(
                target, 0, static_cast<float>(kMinPathfindDistance));
            if (path && path->CanReach()) continue;

            m_chosenJump = jump;
            m_lookTarget = targetCentre;
            m_prepareTicks = 0;
            m_phase = Phase::Preparing;
            return;
        }
    }

} // namespace Game
