// File: src/common/entity/projectile/Projectile.cpp
#include "common/entity/projectile/Projectile.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/world/block/TntBlock.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    Entity* Projectile::GetOwner() {
        return Level() ? m_ownerRef.Get(*Level()) : nullptr;
    }

    Projectile::Projectile(EntityTypeId type, EntityLevel* level)
        : Mob(type, level) {}

    bool Projectile::CollisionShapeContains(const IBlockAccess& blocks,
                                            const glm::ivec3& bp,
                                            const glm::dvec3& point) {
        const BlockID id = blocks.GetBlock(bp.x, bp.y, bp.z);
        if (id == BlockID::Air || !BlockRegistry::HasCollision(id)) return false;

        const auto set = BlockRegistry::GetBlockCollisionShapeSet(
            blocks.GetBlockState(bp.x, bp.y, bp.z));
        const glm::dvec3 local = point - glm::dvec3(bp);
        for (uint8_t i = 0; i < set.count; ++i) {
            const auto& box = set.boxes[i];
            if (local.x >= box.min.x && local.x <= box.max.x &&
                local.y >= box.min.y && local.y <= box.max.y &&
                local.z >= box.min.z && local.z <= box.max.z) {
                return true;
            }
        }
        return false;
    }

    double Projectile::RayAabb(const glm::dvec3& origin, const glm::dvec3& dir,
                               const AABB& box) {
        double tMin = 0.0, tMax = 1.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double o = origin[axis], d = dir[axis];
            const double lo = box.min[axis], hi = box.max[axis];
            if (std::abs(d) < 1.0e-12) {
                if (o < lo || o > hi) return -1.0;
                continue;
            }
            double t0 = (lo - o) / d, t1 = (hi - o) / d;
            if (t0 > t1) std::swap(t0, t1);
            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);
            if (tMin > tMax) return -1.0;
        }
        return tMin;
    }

    void Projectile::Shoot(double xd, double yd, double zd, float velocityScale,
                           float inaccuracy) {
        glm::dvec3 dir(xd, yd, zd);
        const double len = glm::length(dir);
        if (len > 1.0e-9) dir /= len;

        if (m_level) {
            JavaRandom& rng = m_level->Random();
            dir.x += rng.Triangle(0.0, 0.0172275 * inaccuracy);
            dir.y += rng.Triangle(0.0, 0.0172275 * inaccuracy);
            dir.z += rng.Triangle(0.0, 0.0172275 * inaccuracy);
        }
        velocity = dir * static_cast<double>(velocityScale);
        needsSync = true;

        const double horiz = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        yRot = static_cast<float>(std::atan2(velocity.x, velocity.z) * Mth::kRadToDeg);
        xRot = static_cast<float>(std::atan2(velocity.y, horiz) * Mth::kRadToDeg);
        yRotO = yRot;
        xRotO = xRot;
        yHeadRot = yBodyRot = yRot;
    }

    void Projectile::ShootFromRotation(const Entity& shooter, float xRot,
                                       float yRot, float yOffset,
                                       float velocityScale, float inaccuracy) {
        // MC Projectile.shootFromRotation, transcribed: view angles → unit
        // direction, shoot(), then the shooter's own known movement is added
        // (its y only while airborne — a jump-throw leads upward, a grounded
        // sprint only leads forward).
        const float xd = -std::sin(yRot * Mth::kDegToRad) *
                          std::cos(xRot * Mth::kDegToRad);
        const float yd = -std::sin((xRot + yOffset) * Mth::kDegToRad);
        const float zd =  std::cos(yRot * Mth::kDegToRad) *
                          std::cos(xRot * Mth::kDegToRad);
        Shoot(xd, yd, zd, velocityScale, inaccuracy);
        const glm::dvec3 known = shooter.GetKnownMovement();
        velocity += glm::dvec3(known.x, shooter.onGround ? 0.0 : known.y,
                               known.z);
        needsSync = true;
    }

    float Projectile::LerpRotation(float from, float to, float step) {
        while (to - from < -180.0f) from -= 360.0f;
        while (to - from >= 180.0f) from += 360.0f;
        return from + step * (to - from);
    }

    void Projectile::RotateTowardsMovement(float step) {
        const glm::dvec3& v = velocity;
        if (v.x * v.x + v.y * v.y + v.z * v.z < 1.0e-14) return;
        const double horiz = std::sqrt(v.x * v.x + v.z * v.z);
        const float targetYRot =
            static_cast<float>(std::atan2(v.x, v.z) * Mth::kRadToDeg);
        const float targetXRot =
            static_cast<float>(std::atan2(v.y, horiz) * Mth::kRadToDeg);
        xRot = LerpRotation(xRot, targetXRot, step);
        yRot = LerpRotation(yRot, targetYRot, step);
        yHeadRot = yBodyRot = yRot;
    }

    bool Projectile::CanHitEntity(const Entity& entity) const {
        // Identity compare, not a resolve: "am I allowed to hit this" must
        // answer the same whether or not my shooter is currently loaded, and
        // it keeps this method const.
        if (&entity == this) return false;
        // MC: the owner is protected only until leftOwner — after that a
        // shooter can be hit by their own projectile (MC also spares the
        // owner's co-passengers; no vehicle system here).
        if (!m_leftOwner && m_ownerRef.Matches(entity)) return false;
        return entity.IsAlive();
    }

    bool Projectile::CheckLeftOwner() {
        Entity* owner = GetOwner();
        if (!owner) return true;
        // MC Projectile.checkLeftOwner: this box expanded towards the
        // movement, inflated 1.0, against the owner's box.
        AABB box = GetAABB();
        const glm::vec3 v(velocity);
        box.min = glm::min(box.min, box.min + v);
        box.max = glm::max(box.max, box.max + v);
        box.min -= glm::vec3(1.0f);
        box.max += glm::vec3(1.0f);
        return !box.Intersects(owner->GetAABB());
    }

    Projectile::HitResult Projectile::Clip(const glm::dvec3& origin,
                                           const glm::dvec3& movement,
                                           bool checkEntities) {
        HitResult result;
        if (!m_level || !m_level->Blocks()) return result;
        const IBlockAccess& blocks = *m_level->Blocks();

        // MC Projectile.tick's `if (!leftOwner) leftOwner = checkLeftOwner()`.
        // Clip is the funnel every projectile tick passes through, and it runs
        // before this tick's movement is applied — the same ordering as MC.
        if (!m_leftOwner) m_leftOwner = CheckLeftOwner();

        const double moveLen = glm::length(movement);
        if (moveLen < 1.0e-9) return result;

        // ── Block clip: 0.05-block march (see the header) ──────────────────
        double tBlock = 1.0;
        glm::ivec3 hitBlockPos{0};
        {
            const int steps = std::max(1, static_cast<int>(std::ceil(moveLen / 0.05)));
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const glm::dvec3 p = origin + movement * t;
                const glm::ivec3 bp(static_cast<int>(std::floor(p.x)),
                                    static_cast<int>(std::floor(p.y)),
                                    static_cast<int>(std::floor(p.z)));
                if (CollisionShapeContains(blocks, bp, p)) {
                    tBlock = t;
                    hitBlockPos = bp;
                    break;
                }
            }
        }

        // ── Entity clip (MC findHitEntities + inflate(0.3)) ────────────────
        LivingEntity* hitEntity = nullptr;
        int hitDragonPart = -1;
        double tEntity = tBlock;
        if (checkEntities) {
            AABB sweep = GetAABB();
            sweep.min = glm::min(sweep.min, sweep.min + glm::vec3(movement));
            sweep.max = glm::max(sweep.max, sweep.max + glm::vec3(movement));
            sweep.min -= glm::vec3(1.0f);
            sweep.max += glm::vec3(1.0f);

            std::vector<Entity*> candidates;
            m_level->GetEntitiesInBox(sweep, this, candidates);
            for (Entity* e : candidates) {
                auto* living = dynamic_cast<LivingEntity*>(e);
                if (!living || !CanHitEntity(*living)) continue;

                // ── Ender dragon: clip against the eight PART boxes ────────
                //
                // MC's arrows collide with the EnderDragonPart entities, not
                // the dragon's 16x8 body box — an arrow through the gap
                // between a wing and the tail passes clean through. The part
                // index rides HitResult.dragonPart; DealHitDamage routes it
                // into HurtPart so a head shot takes full damage, exactly as
                // the part entity's hurt() would.
                if (auto* dragon = dynamic_cast<EnderDragon*>(living)) {
                    AABB parts[EnderDragon::kDragonPartCount];
                    dragon->ComputePartBoxes(parts);
                    for (int p = 0; p < EnderDragon::kDragonPartCount; ++p) {
                        AABB pbox = parts[p];
                        pbox.min -= glm::vec3(0.3f);
                        pbox.max += glm::vec3(0.3f);
                        const double t = RayAabb(origin, movement, pbox);
                        if (t >= 0.0 && t < tEntity) {
                            tEntity = t;
                            hitEntity = living;
                            hitDragonPart = p;
                        }
                    }
                    continue;
                }

                AABB box = living->GetAABB();
                box.min -= glm::vec3(0.3f);   // MC canHitEntity inflate
                box.max += glm::vec3(0.3f);
                const double t = RayAabb(origin, movement, box);
                if (t >= 0.0 && t < tEntity) {
                    tEntity = t;
                    hitEntity = living;
                    hitDragonPart = -1;
                }
            }
        }

        if (hitEntity) {
            result.type = HitResult::Type::Entity;
            result.t = tEntity;
            result.entity = hitEntity;
            result.dragonPart = hitDragonPart;
            result.location = origin + movement * tEntity;
        } else if (tBlock < 1.0) {
            result.type = HitResult::Type::Block;
            result.t = tBlock;
            result.blockPos = hitBlockPos;
            result.location = origin + movement * tBlock;
        }
        return result;
    }

    bool Projectile::DealHitDamage(LivingEntity& target, const HitResult& hit,
                                   MobDamageSource source, float amount,
                                   Entity* attacker) {
        if (auto* dragon = dynamic_cast<EnderDragon*>(&target)) {
            // MC: the ray hit an EnderDragonPart, whose hurt() routes
            // EnderDragon.hurt(part, source, damage) — the head takes full
            // damage, and the phase's onHurt sees this projectile as the
            // direct entity.
            const bool headHit = hit.dragonPart == EnderDragon::kDragonPartHead;
            return dragon->HurtPart(source, amount, attacker, headHit, this);
        }
        return target.Hurt(source, amount, attacker);
    }

    void Projectile::OnHit(const HitResult& hit) {
        if (hit.IsEntity() && hit.entity) {
            OnHitEntity(*hit.entity, hit);
        } else if (hit.IsBlock()) {
            // MC BlockBehaviour.onProjectileHit runs BEFORE the projectile's
            // own block handling — a flaming arrow lights the TNT it hit and
            // then still sticks (or, for TNT specifically, hits air, because
            // priming removed the block).
            NotifyBlockOfProjectileHit(hit);
            OnHitBlock(hit);
        }
    }

    void Projectile::NotifyBlockOfProjectileHit(const HitResult& hit) {
        if (!m_level || m_level->IsClientSide()) return;
        ILevelWrite* write = m_level->MutableBlocks();
        if (!write) return;

        const glm::ivec3 pos = hit.blockPos;
        if (write->GetBlock(pos.x, pos.y, pos.z) != BlockID::Tnt) return;

        // MC TntBlock.onProjectileHit: only a BURNING projectile lights it, and
        // the kill credit goes to whoever shot it.
        if (TntOnProjectileHit(*write, pos, this, GetOwner())) {
            // MC removeBlock(pos, false).
            write->SetBlock(pos.x, pos.y, pos.z, BlockID::Air,
                            World::UpdateFlags::All);
        }
    }


} // namespace Game
