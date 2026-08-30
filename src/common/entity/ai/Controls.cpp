// File: src/common/entity/ai/Controls.cpp
#include "common/entity/ai/Controls.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/entity/mobs/Animals.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/pathfinder/NodeEvaluator.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── MoveControl ────────────────────────────────────────────────────────

    void MoveControl::SetWantedPosition(double x, double y, double z, double speedModifier) {
        m_wantedX = x;
        m_wantedY = y;
        m_wantedZ = z;
        m_speedModifier = speedModifier;
        // A jump in progress is NOT interrupted by a new waypoint — otherwise a
        // mob re-targeted mid-hop would cancel the jump and clip the ledge.
        if (m_operation != Operation::Jumping) m_operation = Operation::MoveTo;
    }

    void MoveControl::Strafe(float forward, float right) {
        m_operation = Operation::Strafe;
        m_strafeForwards = forward;
        m_strafeRight = right;
        m_speedModifier = 0.25;
    }

    float MoveControl::RotLerp(float from, float to, float max) {
        float diff = Mth::WrapDegrees(to - from);
        diff = std::clamp(diff, -max, max);

        float result = from + diff;
        if (result < 0.0f)        result += 360.0f;
        else if (result > 360.0f) result -= 360.0f;
        return result;
    }

    bool MoveControl::IsWalkable(float dx, float dz) const {
        // MC MoveControl.isWalkable asks the navigation's node evaluator; the
        // port's evaluator is created lazily on the first CreatePath (see
        // PathNavigation::SetCanFloat), so this asks the shared static
        // classifier instead, the way RandomPos does — for the ground mobs
        // that strafe it is the same WalkNodeEvaluator.getPathType answer.
        // MC returns true when the pieces are missing, and so does this.
        EntityLevel* level = m_mob->Level();
        const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
        if (!blocks) return true;

        PathfindingContext ctx;
        ctx.blocks = blocks;
        ctx.mob = m_mob;
        const int x = static_cast<int>(std::floor(m_mob->position.x + static_cast<double>(dx)));
        const int y = m_mob->BlockPosition().y;
        const int z = static_cast<int>(std::floor(m_mob->position.z + static_cast<double>(dz)));
        return WalkNodeEvaluator::GetPathTypeStatic(ctx, x, y, z) == PathType::Walkable;
    }

    void MoveControl::Tick() {
        switch (m_operation) {
            case Operation::Strafe: {
                const float speed = static_cast<float>(m_mob->GetAttributeValue(Attribute::MovementSpeed));
                const float speedModified = static_cast<float>(m_speedModifier) * speed;

                float xa = m_strafeForwards;
                float za = m_strafeRight;
                float dist = std::sqrt(xa * xa + za * za);
                if (dist < 1.0f) dist = 1.0f;
                dist = speedModified / dist;
                xa *= dist;
                za *= dist;

                // MC MoveControl.tick: rotate the scaled strafe input into
                // world space and probe where it leads — when the destination
                // is not WALKABLE, the strafe is replaced with a plain
                // forward step (forward=1, strafe=0), which is what stops a
                // strafing skeleton backing off a ledge.
                const float sinYaw = std::sin(m_mob->yRot * Mth::kDegToRad);
                const float cosYaw = std::cos(m_mob->yRot * Mth::kDegToRad);
                const float dx = xa * cosYaw - za * sinYaw;
                const float dz = za * cosYaw + xa * sinYaw;
                if (!IsWalkable(dx, dz)) {
                    m_strafeForwards = 1.0f;
                    m_strafeRight = 0.0f;
                }

                m_mob->SetSpeed(speedModified);
                m_mob->SetZza(m_strafeForwards);
                m_mob->SetXxa(m_strafeRight);
                m_operation = Operation::Wait;
                break;
            }

            case Operation::MoveTo: {
                // One-shot: the navigation re-arms this every tick it wants
                // movement. A mob whose path ended simply stops being told to
                // move and falls through to the Wait branch below.
                m_operation = Operation::Wait;

                const double xd = m_wantedX - m_mob->position.x;
                const double zd = m_wantedZ - m_mob->position.z;
                const double yd = m_wantedY - m_mob->position.y;
                const double dd = xd * xd + yd * yd + zd * zd;

                if (dd < kMinSpeedSq) {
                    m_mob->SetZza(0.0f);
                    return;
                }

                const float wantedYaw =
                    static_cast<float>(std::atan2(zd, xd) * (180.0 / 3.14159265358979323846)) - 90.0f;
                m_mob->yRot = RotLerp(m_mob->yRot, wantedYaw, kMaxTurn);
                m_mob->SetSpeed(static_cast<float>(
                    m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed)));

                // ── Auto-jump ──────────────────────────────────────────────
                //
                // Two independent triggers, both from MC:
                //  1. the waypoint is higher than one step AND close in the
                //     horizontal plane — i.e. a wall right in front;
                //  2. the mob is standing inside a block whose collision top is
                //     above its feet, which is how it climbs out of a slab or
                //     off a partially-embedded position.
                // Doors and fences are excluded from (2) because jumping at
                // them accomplishes nothing.
                EntityLevel* level = m_mob->Level();
                const IBlockAccess* blocks = level ? level->Blocks() : nullptr;
                if (blocks) {
                    const glm::ivec3 p = m_mob->BlockPosition();
                    const BlockID id = blocks->GetBlock(p.x, p.y, p.z);
                    const bool solid = BlockRegistry::HasCollision(id);

                    double shapeTop = 0.0;
                    if (solid) {
                        const BlockRegistry::BlockShape& shape =
                            BlockRegistry::GetBlockShape(blocks->GetBlockState(p.x, p.y, p.z));
                        shapeTop = static_cast<double>(p.y) + static_cast<double>(shape.max.y);
                    }

                    // MC excludes only BlockTags.DOORS and BlockTags.FENCES
                    // from the stand-inside jump — NOT walls or fence gates,
                    // even though the evaluator lumps all of those into
                    // PathType.FENCE. A mob wedged against a wall or a closed
                    // gate therefore still hops, exactly as vanilla's does.
                    // Tag membership per the decompile's BlockItemTagsProvider:
                    // DOORS = every *_door (wood, copper, iron); FENCES = the
                    // wooden fences + nether_brick_fence, i.e. every *_fence
                    // (and never *_fence_gate, which ends differently).
                    const std::string& slug = BlockRegistry::Get(id).registrySlug;
                    const bool isDoorOrFence =
                        slug.ends_with("_door") || slug.ends_with("_fence");

                    const double horizSq = xd * xd + zd * zd;
                    // MC compares the SQUARED horizontal distance against an
                    // UNSQUARED max(1.0, bbWidth). That reads like a vanilla
                    // slip, but it is what MC does, and squaring it here made
                    // wide mobs (a 1.4 spider: 1.96 vs 1.4) jump at walls from
                    // noticeably further out than vanilla.
                    const double widthLimit = std::max(1.0f, m_mob->GetBbWidth());

                    if ((yd > static_cast<double>(m_mob->MaxUpStep()) && horizSq < widthLimit) ||
                        (solid && m_mob->position.y < shapeTop && !isDoorOrFence)) {
                        m_mob->GetJumpControl().Jump();
                        m_operation = Operation::Jumping;
                    }
                }
                break;
            }

            case Operation::Jumping:
                m_mob->SetSpeed(static_cast<float>(
                    m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed)));
                if (m_mob->onGround || m_mob->IsInLiquid()) m_operation = Operation::Wait;
                break;

            case Operation::Wait:
            default:
                m_mob->SetZza(0.0f);
                break;
        }
    }

    // ── LookControl ────────────────────────────────────────────────────────

    void LookControl::SetLookAt(const glm::dvec3& target) {
        SetLookAt(target.x, target.y, target.z);
    }

    void LookControl::SetLookAt(double x, double y, double z) {
        SetLookAt(x, y, z,
                  static_cast<float>(m_mob->GetHeadRotSpeed()),
                  static_cast<float>(m_mob->GetMaxHeadXRot()));
    }

    void LookControl::SetLookAt(double x, double y, double z,
                                float yMaxRotSpeed, float xMaxRotAngle) {
        m_wantedX = x;
        m_wantedY = y;
        m_wantedZ = z;
        m_yMaxRotSpeed = yMaxRotSpeed;
        m_xMaxRotAngle = xMaxRotAngle;
        // Two ticks, not one: goals only re-evaluate every OTHER tick, so a
        // one-tick cooldown would leave the head drifting back half the time.
        m_lookAtCooldown = 2;
    }

    bool LookControl::GetYRotD(float& out) const {
        const double dx = m_wantedX - m_mob->position.x;
        const double dz = m_wantedZ - m_mob->position.z;
        if (std::abs(dz) <= 1.0e-5 && std::abs(dx) <= 1.0e-5) return false;
        out = static_cast<float>(std::atan2(dz, dx) * (180.0 / 3.14159265358979323846)) - 90.0f;
        return true;
    }

    bool LookControl::GetXRotD(float& out) const {
        const double dx = m_wantedX - m_mob->position.x;
        const double dy = m_wantedY - m_mob->GetEyeY();
        const double dz = m_wantedZ - m_mob->position.z;
        const double sd = std::sqrt(dx * dx + dz * dz);
        if (std::abs(dy) <= 1.0e-5 && std::abs(sd) <= 1.0e-5) return false;
        // Negated: MC's pitch is positive DOWN.
        out = static_cast<float>(-(std::atan2(dy, sd) * (180.0 / 3.14159265358979323846)));
        return true;
    }

    void LookControl::Tick() {
        if (ResetXRotOnTick()) m_mob->xRot = 0.0f;

        if (m_lookAtCooldown > 0) {
            --m_lookAtCooldown;
            float yRotD = 0.0f, xRotD = 0.0f;
            if (GetYRotD(yRotD)) {
                m_mob->yHeadRot = Mth::ApproachDegrees(m_mob->yHeadRot, yRotD, m_yMaxRotSpeed);
            }
            if (GetXRotD(xRotD)) {
                m_mob->xRot = Mth::ApproachDegrees(m_mob->xRot, xRotD, m_xMaxRotAngle);
            }
        } else {
            // Nothing to look at: drift the head back to the body, slowly.
            m_mob->yHeadRot = Mth::ApproachDegrees(m_mob->yHeadRot, m_mob->yBodyRot, 10.0f);
        }

        ClampHeadRotationToBody();
    }

    void LookControl::ClampHeadRotationToBody() {
        // Only while walking. A standing mob may look right round behind
        // itself; a walking one keeps its head within its neck limit of the
        // direction it is travelling.
        if (!m_mob->GetNavigation().IsDone()) {
            m_mob->yHeadRot = Mth::RotateIfNecessary(
                m_mob->yHeadRot, m_mob->yBodyRot,
                static_cast<float>(m_mob->GetMaxHeadYRot()));
        }
    }

    // ── FlyingMoveControl ──────────────────────────────────────────────────

    void FlyingMoveControl::Tick() {
        if (m_operation == Operation::MoveTo) {
            // MC consumes the wanted position every tick — the navigation
            // re-arms it while a path is live.
            m_operation = Operation::Wait;
            m_mob->SetNoGravity(true);

            const double xd = m_wantedX - m_mob->position.x;
            const double yd = m_wantedY - m_mob->position.y;
            const double zd = m_wantedZ - m_mob->position.z;
            const double dd = xd * xd + yd * yd + zd * zd;
            if (dd < kMinSpeedSq) {
                m_mob->SetYya(0.0f);
                m_mob->SetZza(0.0f);
                return;
            }

            const float yRotD = static_cast<float>(
                std::atan2(zd, xd) * Mth::kRadToDeg) - 90.0f;
            m_mob->yRot = RotLerp(m_mob->yRot, yRotD, 90.0f);

            // MC: MOVEMENT_SPEED drives grounded movement, FLYING_SPEED the
            // air — a parrot walks slowly but flits quickly.
            const float speed = static_cast<float>(m_speedModifier *
                m_mob->GetAttributeValue(m_mob->onGround ? Attribute::MovementSpeed
                                                         : Attribute::FlyingSpeed));
            m_mob->SetSpeed(speed);

            const double sd = std::sqrt(xd * xd + zd * zd);
            if (std::abs(yd) > 1.0e-5 || std::abs(sd) > 1.0e-5) {
                const float xRotD = static_cast<float>(
                    -(std::atan2(yd, sd) * Mth::kRadToDeg));
                m_mob->xRot = RotLerp(m_mob->xRot, xRotD, static_cast<float>(m_maxTurn));
                m_mob->SetYya(yd > 0.0 ? speed : -speed);
            }
        } else {
            if (!m_hoversInPlace) m_mob->SetNoGravity(false);
            m_mob->SetYya(0.0f);
            m_mob->SetZza(0.0f);
        }
    }

    // ── GhastMoveControl ───────────────────────────────────────────────────

    bool GhastMoveControl::CanReach(const glm::dvec3& travel) const {
        EntityLevel* level = m_mob->Level();
        if (!level || !level->Blocks()) return false;
        PhysicsContext phys = level->Physics();

        const AABB box = m_mob->GetAABB();
        const double len = glm::length(travel);
        const int steps = std::max(1, static_cast<int>(std::ceil(len)));
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            AABB probe = box;
            probe.min += glm::vec3(travel * t);
            probe.max += glm::vec3(travel * t);
            if (CollidesAt(probe, phys)) return false;
        }
        return true;
    }

    void GhastMoveControl::Tick() {
        // MC Ghast.GhastMoveControl.tick (careful = false; shouldBeStopped is
        // the happy ghast's harness hook and always false for the plain one).
        if (m_operation != Operation::MoveTo) return;
        if (m_floatDuration-- > 0) return;

        EntityLevel* level = m_mob->Level();
        if (!level) return;
        m_floatDuration += level->Random().NextInt(5) + 2;

        const glm::dvec3 travel(m_wantedX - m_mob->position.x,
                                m_wantedY - m_mob->position.y,
                                m_wantedZ - m_mob->position.z);
        if (CanReach(travel)) {
            const double len = glm::length(travel);
            if (len > 1.0e-9) {
                const double speed =
                    m_mob->GetAttributeValue(Attribute::FlyingSpeed) * 5.0 / 3.0;
                m_mob->velocity += travel / len * speed;
                m_mob->needsSync = true;
            }
        } else {
            m_operation = Operation::Wait;
        }
    }

    // ── GuardianMoveControl ────────────────────────────────────────────────

    GuardianMoveControl::GuardianMoveControl(Guardian* guardian)
        : MoveControl(guardian), m_guardian(guardian) {}

    void GuardianMoveControl::Tick() {
        // MC GuardianMoveControl.tick. Unlike the base control this does NOT
        // consume the MOVE_TO operation each tick — MC's doesn't either; the
        // navigation going done is what parks the guardian.
        if (m_operation == Operation::MoveTo && !m_mob->GetNavigation().IsDone()) {
            const double xd = m_wantedX - m_mob->position.x;
            const double yd = m_wantedY - m_mob->position.y;
            const double zd = m_wantedZ - m_mob->position.z;
            const double length = std::sqrt(xd * xd + yd * yd + zd * zd);
            const double xn = xd / length;
            const double yn = yd / length;
            const double zn = zd / length;

            const float yRotD = static_cast<float>(
                std::atan2(zd, xd) * Mth::kRadToDeg) - 90.0f;
            m_mob->yRot = RotLerp(m_mob->yRot, yRotD, 90.0f);
            m_mob->yBodyRot = m_mob->yRot;

            const float targetSpeed = static_cast<float>(
                m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed));
            const float newSpeed = Mth::Lerp(0.125f, m_mob->GetSpeed(), targetSpeed);
            m_mob->SetSpeed(newSpeed);

            // The idle bob: two sinusoids phased by tickCount + id so a group
            // of guardians does not pulse in lockstep, plus the 0.1-scaled
            // vertical pull toward the waypoint.
            const double push = std::sin(
                static_cast<double>(m_mob->tickCount + m_mob->GetId()) * 0.5) * 0.05;
            const double cosY = std::cos(m_mob->yRot * Mth::kDegToRad);
            const double sinY = std::sin(m_mob->yRot * Mth::kDegToRad);
            const double yPush = std::sin(
                static_cast<double>(m_mob->tickCount + m_mob->GetId()) * 0.75) * 0.05;
            m_mob->velocity += glm::dvec3(
                push * cosY,
                yPush * (sinY + cosY) * 0.25 +
                    static_cast<double>(newSpeed) * yn * 0.1,
                push * sinY);

            // The look target trails the travel direction at an eighth per
            // tick, from wherever it last was (or from the new target outright
            // if nothing was being looked at).
            LookControl& control = m_mob->GetLookControl();
            const double newLookX = m_mob->position.x + xn * 2.0;
            const double newLookY = m_mob->GetEyeY() + yn / length;
            const double newLookZ = m_mob->position.z + zn * 2.0;
            double oldLookX = control.GetWantedX();
            double oldLookY = control.GetWantedY();
            double oldLookZ = control.GetWantedZ();
            if (!control.IsLookingAtTarget()) {
                oldLookX = newLookX;
                oldLookY = newLookY;
                oldLookZ = newLookZ;
            }
            control.SetLookAt(Mth::Lerp(0.125, oldLookX, newLookX),
                              Mth::Lerp(0.125, oldLookY, newLookY),
                              Mth::Lerp(0.125, oldLookZ, newLookZ),
                              10.0f, 40.0f);
            m_guardian->SetMoving(true);
        } else {
            m_mob->SetSpeed(0.0f);
            m_guardian->SetMoving(false);
        }
    }

    // ── FishMoveControl ────────────────────────────────────────────────────

    void FishMoveControl::Tick() {
        // MC gates the buoyancy on eye-in-water; the engine's coarser
        // IsInWater is the same answer for a half-block-tall fish.
        if (m_mob->IsInWater()) {
            m_mob->velocity.y += 0.005;
        }

        if (m_operation == Operation::MoveTo && !m_mob->GetNavigation().IsDone()) {
            const float targetSpeed = static_cast<float>(
                m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed));
            m_mob->SetSpeed(m_mob->GetSpeed() +
                            0.125f * (targetSpeed - m_mob->GetSpeed()));

            const double xd = m_wantedX - m_mob->position.x;
            const double yd = m_wantedY - m_mob->position.y;
            const double zd = m_wantedZ - m_mob->position.z;
            if (yd != 0.0) {
                const double dd = std::sqrt(xd * xd + yd * yd + zd * zd);
                m_mob->velocity.y += m_mob->GetSpeed() * (yd / dd) * 0.1;
            }
            if (xd != 0.0 || zd != 0.0) {
                const float yRotD = static_cast<float>(
                    std::atan2(zd, xd) * Mth::kRadToDeg) - 90.0f;
                m_mob->yRot = RotLerp(m_mob->yRot, yRotD, 90.0f);
                m_mob->yBodyRot = m_mob->yRot;
            }
        } else {
            m_mob->SetSpeed(0.0f);
        }
    }

    // ── SmoothSwimmingMoveControl ──────────────────────────────────────────

    float SmoothSwimmingMoveControl::GetTurningSpeedFactor(float leftToTurn) {
        return 1.0f - std::clamp((leftToTurn - 10.0f) / 50.0f, 0.0f, 1.0f);
    }

    void SmoothSwimmingMoveControl::Tick() {
        // The buoyancy nudge: +0.005/tick in water counters the water-branch
        // sink, so a fish holds depth instead of settling to the floor.
        if (m_applyGravity && m_mob->IsInWater()) {
            m_mob->velocity.y += 0.005;
        }

        if (m_operation == Operation::MoveTo && !m_mob->GetNavigation().IsDone()) {
            const double xd = m_wantedX - m_mob->position.x;
            const double yd = m_wantedY - m_mob->position.y;
            const double zd = m_wantedZ - m_mob->position.z;
            const double dd = xd * xd + yd * yd + zd * zd;
            if (dd < kMinSpeedSq) {
                m_mob->SetZza(0.0f);
                return;
            }

            const float yRotD = static_cast<float>(
                std::atan2(zd, xd) * Mth::kRadToDeg) - 90.0f;
            m_mob->yRot = RotLerp(m_mob->yRot, yRotD, static_cast<float>(m_maxTurnY));
            m_mob->yBodyRot = m_mob->yRot;
            m_mob->yHeadRot = m_mob->yRot;

            const float speed = static_cast<float>(
                m_speedModifier * m_mob->GetAttributeValue(Attribute::MovementSpeed));
            if (m_mob->IsInWater()) {
                m_mob->SetSpeed(speed * m_inWaterSpeedModifier);
                const double sd = std::sqrt(xd * xd + zd * zd);
                if (std::abs(yd) > 1.0e-5 || std::abs(sd) > 1.0e-5) {
                    float xRotD = static_cast<float>(
                        -(std::atan2(yd, sd) * Mth::kRadToDeg));
                    xRotD = std::clamp(Mth::WrapDegrees(xRotD),
                                       static_cast<float>(-m_maxTurnX),
                                       static_cast<float>(m_maxTurnX));
                    m_mob->xRot = Mth::ApproachDegrees(m_mob->xRot, xRotD, 5.0f);
                }

                // The pitch splits the speed between forward and vertical —
                // the mob literally swims along its nose.
                const float cosX = std::cos(m_mob->xRot * Mth::kDegToRad);
                const float sinX = std::sin(m_mob->xRot * Mth::kDegToRad);
                m_mob->SetZza(cosX * speed);
                m_mob->SetYya(-sinX * speed);
            } else {
                // Beached: crawl at the outside-water modifier, throttled to
                // zero while still facing the wrong way.
                const float leftToTurn =
                    std::abs(Mth::WrapDegrees(m_mob->yRot - yRotD));
                m_mob->SetSpeed(speed * m_outsideWaterSpeedModifier *
                                GetTurningSpeedFactor(leftToTurn));
            }
        } else {
            m_mob->SetSpeed(0.0f);
            m_mob->SetXxa(0.0f);
            m_mob->SetYya(0.0f);
            m_mob->SetZza(0.0f);
        }
    }

    // ── SmoothSwimmingLookControl ──────────────────────────────────────────

    void SmoothSwimmingLookControl::Tick() {
        if (m_lookAtCooldown > 0) {
            --m_lookAtCooldown;
            float yRotD = 0.0f, xRotD = 0.0f;
            // MC: the swimming head LEADS the look target by 20 degrees of yaw
            // and 10 of pitch.
            if (GetYRotD(yRotD)) {
                m_mob->yHeadRot = Mth::ApproachDegrees(m_mob->yHeadRot,
                                                       yRotD + 20.0f, m_yMaxRotSpeed);
            }
            if (GetXRotD(xRotD)) {
                m_mob->xRot = Mth::ApproachDegrees(m_mob->xRot,
                                                   xRotD + 10.0f, m_xMaxRotAngle);
            }
        } else {
            if (m_mob->GetNavigation().IsDone()) {
                m_mob->xRot = Mth::ApproachDegrees(m_mob->xRot, 0.0f, 5.0f);
            }
            m_mob->yHeadRot = Mth::ApproachDegrees(m_mob->yHeadRot,
                                                   m_mob->yBodyRot, m_yMaxRotSpeed);
        }

        const float headDiffBody = Mth::WrapDegrees(m_mob->yHeadRot - m_mob->yBodyRot);
        if (headDiffBody < static_cast<float>(-m_maxYRotFromCenter)) {
            m_mob->yBodyRot -= 4.0f;
        } else if (headDiffBody > static_cast<float>(m_maxYRotFromCenter)) {
            m_mob->yBodyRot += 4.0f;
        }
    }

    // ── JumpControl ────────────────────────────────────────────────────────

    void JumpControl::Tick() {
        // The latch lives exactly one tick. LivingEntity::AiStep reads
        // `jumping` in its jump phase later in the SAME tick, which is why this
        // runs at the end of Mob::ServerAiStep rather than at the start.
        m_mob->jumping = m_jump;
        m_jump = false;
    }

    // ── RabbitJumpControl ──────────────────────────────────────────────────

    RabbitJumpControl::RabbitJumpControl(Rabbit* rabbit)
        : JumpControl(rabbit), m_rabbit(rabbit) {}

    void RabbitJumpControl::Tick() {
        // MC RabbitJumpControl.tick: the latch fires StartJumping (which sets
        // `jumping` AND arms the 10-tick animation clock) instead of the base
        // hand-off. Note the base's `jumping = m_jump` does NOT run — a rabbit
        // only ever jumps through StartJumping, exactly as in MC.
        if (m_jump) {
            m_rabbit->StartJumping();
            m_jump = false;
        }
    }

    // ── RabbitMoveControl ──────────────────────────────────────────────────

    RabbitMoveControl::RabbitMoveControl(Rabbit* rabbit)
        : MoveControl(rabbit), m_rabbit(rabbit) {}

    void RabbitMoveControl::Tick() {
        // MC RabbitMoveControl.tick, verbatim: parked at 0 while grounded and
        // idle, restored to the remembered hop speed while moving or airborne.
        if (m_rabbit->onGround && !m_rabbit->jumping &&
            !static_cast<RabbitJumpControl&>(m_rabbit->GetJumpControl()).WantJump()) {
            m_rabbit->SetSpeedModifier(0.0);
        } else if (HasWanted() || m_operation == Operation::Jumping) {
            m_rabbit->SetSpeedModifier(m_nextJumpSpeed);
        }
        MoveControl::Tick();
    }

    void RabbitMoveControl::SetWantedPosition(double x, double y, double z,
                                              double speedModifier) {
        // MC: a swimming rabbit always paddles at 1.5, and any positive speed
        // is remembered as the speed of the NEXT hop.
        if (m_rabbit->IsInWater()) {
            speedModifier = 1.5;
        }
        MoveControl::SetWantedPosition(x, y, z, speedModifier);
        if (speedModifier > 0.0) {
            m_nextJumpSpeed = speedModifier;
        }
    }

    // ── BodyRotationControl ────────────────────────────────────────────────

    bool BodyRotationControl::IsMoving() const {
        const double dx = m_mob->position.x - m_mob->oldPosition.x;
        const double dz = m_mob->position.z - m_mob->oldPosition.z;
        return dx * dx + dz * dz > MoveControl::kMinSpeedSq;
    }

    void BodyRotationControl::ClientTick() {
        if (IsMoving()) {
            // Walking: the body IS the facing, and the head is pulled into its
            // neck limit around it.
            m_mob->yBodyRot = m_mob->yRot;
            m_mob->yHeadRot = Mth::RotateIfNecessary(
                m_mob->yHeadRot, m_mob->yBodyRot,
                static_cast<float>(m_mob->GetMaxHeadYRot()));
            m_lastStableYHeadRot = m_mob->yHeadRot;
            m_headStableTime = 0;
            return;
        }

        // MC notCarryingMobPassengers — a standing mob ridden BY another mob
        // (a chicken under a jockey) leaves its body alone entirely: the
        // rider's steering owns the facing. Player riders do not count.
        if (dynamic_cast<Mob*>(m_mob->GetFirstPassenger()) != nullptr) return;

        // Standing still. A head that keeps moving resets the timer; a head
        // that holds still for 10 ticks starts dragging the body round to meet
        // it over the following 10. This is the whole reason MC mobs turn their
        // heads before their bodies.
        if (std::abs(m_mob->yHeadRot - m_lastStableYHeadRot) > kHeadStableAngle) {
            m_headStableTime = 0;
            m_lastStableYHeadRot = m_mob->yHeadRot;
            m_mob->yBodyRot = Mth::RotateIfNecessary(
                m_mob->yBodyRot, m_mob->yHeadRot,
                static_cast<float>(m_mob->GetMaxHeadYRot()));
        } else if (++m_headStableTime > kDelayUntilFacingForward) {
            const float frac = std::clamp(
                static_cast<float>(m_headStableTime - kDelayUntilFacingForward) /
                    static_cast<float>(kTicksToFaceForward),
                0.0f, 1.0f);
            // The allowed offset shrinks to zero, so the body ends up exactly
            // under the head rather than merely near it.
            const float remaining = static_cast<float>(m_mob->GetMaxHeadYRot()) * (1.0f - frac);
            m_mob->yBodyRot = Mth::RotateIfNecessary(m_mob->yBodyRot, m_mob->yHeadRot, remaining);
        }
    }

} // namespace Game
