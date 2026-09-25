// File: src/common/entity/decoration/HangingEntity.cpp
#include "common/entity/decoration/HangingEntity.hpp"

#include "common/entity/ArmorStand.hpp"
#include "common/entity/EndCrystal.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/entity/PrimedTnt.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/LegacySolid.hpp"
#include "common/world/block/RedstoneFamilies.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>
#include <vector>

namespace Game {

    HangingEntity::HangingEntity(EntityTypeId type, EntityLevel* level)
        : Mob(type, level, NoAiTag{}) {
        // No health to speak of; the living attributes exist only because the
        // Mob pipeline reads them.
        CreateLivingAttributes(m_attributes);
        m_health = GetMaxHealth();
        ClearHoldsEntityRefs();
    }

    // ── Geometry ───────────────────────────────────────────────────────────

    void HangingEntity::RecalculateBoundingBox() {
        const AABBd box = CalculateBoundingBox(m_pos, m_direction);
        position = (box.min + box.max) * 0.5;
        oldPosition = position;
        SetFixedBoundingBox(box);
    }

    void HangingEntity::SetDirection(Direction direction) {
        if (!IsHorizontal(direction)) return;   // MC Validate.isTrue(horizontal)
        m_direction = direction;
        xRot = xRotO = 0.0f;
        yRot = yRotO = ToYRot(direction);
        yBodyRot = yBodyRotO = yRot;
        yHeadRot = yHeadRotO = yRot;
        RecalculateBoundingBox();
    }

    void HangingEntity::SetHangingPos(const glm::ivec3& pos) {
        m_pos = pos;
        RecalculateBoundingBox();
    }

    void HangingEntity::SyncFromNetwork() {
        // The facing: MC's setDirection writes xRot = -90 × step for a
        // ceiling-facing (up) frame and +90 for a floor-facing (down) one,
        // yRot for a wall.
        if (xRot <= -45.0f)     m_direction = Direction::Up;
        else if (xRot >= 45.0f) m_direction = Direction::Down;
        else                    m_direction = FromYRot(yRot);
        // The cell: the centre the server sent, minus the box's offset from
        // its cell's centre (the same offset for every cell).
        const AABBd probe = CalculateBoundingBox(glm::ivec3(0), m_direction);
        const glm::dvec3 offset = (probe.min + probe.max) * 0.5 - glm::dvec3(0.5);
        const glm::dvec3 cellCenter = position - offset;
        m_pos = glm::ivec3(static_cast<int>(std::floor(cellCenter.x)),
                           static_cast<int>(std::floor(cellCenter.y)),
                           static_cast<int>(std::floor(cellCenter.z)));
        RecalculateBoundingBox();
    }

    // ── Survival ───────────────────────────────────────────────────────────

    bool HangingEntity::HasLevelCollision(const AABBd& box) const {
        // The box lies flush on the wall's face, so the test box is pulled
        // in by a hair — touching is not overlapping. Kept in double: a
        // float box far from the origin cannot hold a 1e-5 inset.
        if (!m_level || !m_level->Blocks()) return true;
        constexpr double kEps = 1.0e-5;
        const AABBd pop = AABBd::FromMinMax(box.min + glm::dvec3(kEps), box.max - glm::dvec3(kEps));
        PhysicsContext phys;
        phys.blockAccess = m_level->Blocks();
        return CollidesAt(pop, phys);
    }

    bool HangingEntity::CanCoexist(bool allowIntersectingSameType) const {
        if (!m_level) return false;
        const AABBd pop = PopBox();
        std::vector<Entity*> others;
        // The level buckets entities by the chunk of their POSITION, and a
        // canvas up to 16 wide reaches 8 blocks past its centre into the
        // next chunk — so the query is widened by that much (MC's section
        // walk pads its box the same way) and the real test is the
        // Intersects(pop) below.
        constexpr float kReach = 8.0f;
        AABB query;
        query.min = glm::vec3(pop.min) - glm::vec3(kReach, 0.0f, kReach);
        query.max = glm::vec3(pop.max) + glm::vec3(kReach, 0.0f, kReach);
        m_level->GetEntitiesInBox(query, this, others);
        for (const Entity* other : others) {
            const auto* hanging = dynamic_cast<const HangingEntity*>(other);
            if (!hanging || hanging == this || hanging->IsRemoved()) continue;
            const bool intersectsSameType = !allowIntersectingSameType && hanging->GetType() == GetType();
            const bool isSameDirection = hanging->GetDirection() == m_direction;
            if ((intersectsSameType || isSameDirection) && hanging->GetAABBd().Intersects(pop)) return false;
        }
        return true;
    }

    bool HangingEntity::Survives() const {
        if (!m_level || !m_level->Blocks()) return false;
        if (HasLevelCollision(PopBox())) return false;

        // calculateSupportBox: the box pushed half a block into the wall and
        // deflated by 1e-7; every cell it touches must be isSolid() or a diode.
        const IBlockAccess& blocks = *m_level->Blocks();
        const AABBd box = GetAABBd();
        const glm::dvec3 shift = -Step(m_direction) * 0.5;
        const glm::dvec3 lo = box.min + shift + glm::dvec3(1.0e-7);
        const glm::dvec3 hi = box.max + shift - glm::dvec3(1.0e-7);
        for (int x = static_cast<int>(std::floor(lo.x)); x <= static_cast<int>(std::floor(hi.x)); ++x) {
            for (int y = static_cast<int>(std::floor(lo.y)); y <= static_cast<int>(std::floor(hi.y)); ++y) {
                for (int z = static_cast<int>(std::floor(lo.z)); z <= static_cast<int>(std::floor(hi.z)); ++z) {
                    const BlockState state = blocks.GetBlockState(x, y, z);
                    if (!IsLegacySolid(state) && !IsDiodeBlock(state.Block())) return false;
                }
            }
        }
        return CanCoexist(false);
    }

    // ── Ticking and breaking ───────────────────────────────────────────────

    void HangingEntity::Tick() {
        // MC BlockAttachedEntity.tick — the server's alone.
        if (!m_level || m_level->IsClientSide()) return;
        // checkBelowWorld.
        if (position.y < static_cast<double>(m_level->GetMinY() - 64)) {
            Remove(RemovalReason::Discarded);
            return;
        }
        if (m_ticksSinceLastCheck++ >= kCheckInterval) {
            m_ticksSinceLastCheck = 0;
            if (!IsRemoved() && !Survives()) {
                Remove(RemovalReason::Discarded);
                DropItem(nullptr);
            }
        }
    }

    void HangingEntity::Kill() {
        // The ENTITY_DIE game event has no listener here.
        Remove(RemovalReason::Killed);
    }

    bool HangingEntity::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        (void)amount;
        if (!m_level || m_level->IsClientSide()) return false;
        // isInvulnerableToBase: the invulnerable flag holds against all but
        // the void and a creative player (source.isCreativePlayer()).
        const bool creativePlayer = attacker && attacker->IsPlayer() && attacker->IsCreative();
        if (IsInvulnerable() && source != MobDamageSource::Void && !creativePlayer) return false;
        // Mobs break hanging entities only while mob griefing is on. MC
        // tests source.getEntity() — the CAUSING entity: a projectile's
        // shooter, primed TNT's igniter — for `instanceof Mob`. The engine's
        // non-mob entities ride the Mob class (Painting.hpp's note), so they
        // are resolved to their cause, or passed over, first.
        if (!m_level->MobGriefing() && attacker && !attacker->IsPlayer()) {
            Entity* cause = attacker;
            if (auto* projectile = dynamic_cast<Projectile*>(attacker)) cause = projectile->GetOwner();
            else if (auto* tnt = dynamic_cast<PrimedTnt*>(attacker))   cause = tnt->GetOwner();
            const bool causeIsMob = cause && !cause->IsPlayer() && dynamic_cast<Mob*>(cause) &&
                                    !dynamic_cast<Projectile*>(cause) && !dynamic_cast<PrimedTnt*>(cause) &&
                                    !dynamic_cast<FallingBlockEntity*>(cause) && !dynamic_cast<EndCrystal*>(cause) &&
                                    !dynamic_cast<HangingEntity*>(cause) && !dynamic_cast<ArmorStand*>(cause);
            if (causeIsMob) return false;
        }
        if (!IsRemoved()) {
            Kill();
            DropItem(attacker);
        }
        return true;
    }

    bool HangingEntity::SkipAttackInteraction(Entity& source) {
        // (mayInteract — spawn protection — has no counterpart here.)
        if (!source.IsPlayer()) return false;
        return Hurt(MobDamageSource::PlayerAttack, 0.0f, &source);
    }

    void HangingEntity::SpawnAtLocation(const ItemStack& stack, float yOffs) {
        if (!m_level || stack.IsEmpty()) return;
        const glm::dvec3 at(position.x + StepX(m_direction) * 0.15, position.y + yOffs,
                            position.z + StepZ(m_direction) * 0.15);
        m_level->SpawnItemStackDrop(at, stack);
    }

} // namespace Game
