// File: src/common/entity/ai/Controls.cpp
#include "common/entity/ai/Controls.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/pathfinder/PathTypeTable.hpp"

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
                            BlockRegistry::GetBlockShape(id, blocks->GetBlockState(p.x, p.y, p.z));
                        shapeTop = static_cast<double>(p.y) + static_cast<double>(shape.max.y);
                    }

                    const PathType blockPathType = GetPathTypeFromBlock(id);
                    const bool isDoorOrFence = blockPathType == PathType::Fence ||
                                               blockPathType == PathType::DoorWoodClosed ||
                                               blockPathType == PathType::DoorIronClosed ||
                                               blockPathType == PathType::DoorOpen;

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

    // ── JumpControl ────────────────────────────────────────────────────────

    void JumpControl::Tick() {
        // The latch lives exactly one tick. LivingEntity::AiStep reads
        // `jumping` in its jump phase later in the SAME tick, which is why this
        // runs at the end of Mob::ServerAiStep rather than at the start.
        m_mob->jumping = m_jump;
        m_jump = false;
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
