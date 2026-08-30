// File: src/common/entity/projectile/ShulkerBullet.cpp
#include "common/entity/projectile/ShulkerBullet.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/block/BlockRegistry.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    namespace {

        // MC Direction ordinals: DOWN, UP, NORTH, SOUTH, WEST, EAST.
        constexpr glm::ivec3 kDirStep[6] = {
            { 0, -1,  0 }, { 0, 1, 0 }, { 0, 0, -1 },
            { 0,  0,  1 }, { -1, 0, 0 }, { 1, 0, 0 },
        };
        // Direction -> axis (0 = X, 1 = Y, 2 = Z).
        constexpr int kDirAxis[6] = { 1, 1, 2, 2, 0, 0 };

        bool IsEmptyBlock(const IBlockAccess& blocks, const glm::ivec3& p) {
            return blocks.GetBlock(p.x, p.y, p.z) == BlockID::Air;
        }

    } // namespace

    void ShulkerBullet::InitShot(LivingEntity& owner, LivingEntity& target,
                                 int excludeAxis) {
        SetOwner(&owner);
        // MC: the owner's bounding-box centre.
        position = glm::dvec3(owner.position.x,
                              owner.position.y + owner.GetBbHeight() * 0.5,
                              owner.position.z);
        m_finalTarget = &target;
        m_currentMoveDirection = 1;   // MC starts at Direction.UP
        SelectNextMoveDirection(excludeAxis);
    }

    void ShulkerBullet::SelectNextMoveDirection(int avoidAxis) {
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        JavaRandom& rng = m_level->Random();

        double yOffset = 0.5;
        glm::ivec3 targetPos;
        if (!m_finalTarget) {
            targetPos = BlockPosition() + glm::ivec3(0, -1, 0);
        } else {
            yOffset = static_cast<double>(m_finalTarget->GetBbHeight()) * 0.5;
            targetPos = glm::ivec3(
                static_cast<int>(std::floor(m_finalTarget->position.x)),
                static_cast<int>(std::floor(m_finalTarget->position.y + yOffset)),
                static_cast<int>(std::floor(m_finalTarget->position.z)));
        }

        double tx = static_cast<double>(targetPos.x) + 0.5;
        double ty = static_cast<double>(targetPos.y) + yOffset;
        double tz = static_cast<double>(targetPos.z) + 0.5;

        int selection = -1;
        // MC BlockPos.closerToCenterThan(position, 2.0).
        const double cx = targetPos.x + 0.5 - position.x;
        const double cy = targetPos.y + 0.5 - position.y;
        const double cz = targetPos.z + 0.5 - position.z;
        if (cx * cx + cy * cy + cz * cz >= 4.0) {
            const glm::ivec3 current = BlockPosition();
            int options[4];
            int optionCount = 0;

            if (avoidAxis != 0) {   // X
                if (current.x < targetPos.x &&
                    IsEmptyBlock(blocks, current + kDirStep[5])) {
                    options[optionCount++] = 5;   // EAST
                } else if (current.x > targetPos.x &&
                           IsEmptyBlock(blocks, current + kDirStep[4])) {
                    options[optionCount++] = 4;   // WEST
                }
            }
            if (avoidAxis != 1) {   // Y
                if (current.y < targetPos.y &&
                    IsEmptyBlock(blocks, current + kDirStep[1])) {
                    options[optionCount++] = 1;   // UP
                } else if (current.y > targetPos.y &&
                           IsEmptyBlock(blocks, current + kDirStep[0])) {
                    options[optionCount++] = 0;   // DOWN
                }
            }
            if (avoidAxis != 2) {   // Z
                if (current.z < targetPos.z &&
                    IsEmptyBlock(blocks, current + kDirStep[3])) {
                    options[optionCount++] = 3;   // SOUTH
                } else if (current.z > targetPos.z &&
                           IsEmptyBlock(blocks, current + kDirStep[2])) {
                    options[optionCount++] = 2;   // NORTH
                }
            }

            selection = rng.NextInt(6);   // MC Direction.getRandom
            if (optionCount == 0) {
                for (int attempts = 5;
                     !IsEmptyBlock(blocks, current + kDirStep[selection]) &&
                     attempts > 0;
                     --attempts) {
                    selection = rng.NextInt(6);
                }
            } else {
                selection = options[rng.NextInt(optionCount)];
            }

            tx = position.x + kDirStep[selection].x;
            ty = position.y + kDirStep[selection].y;
            tz = position.z + kDirStep[selection].z;
        }

        m_currentMoveDirection = selection;
        const double xa = tx - position.x;
        const double ya = ty - position.y;
        const double za = tz - position.z;
        const double distance = std::sqrt(xa * xa + ya * ya + za * za);
        if (distance == 0.0) {
            m_targetDeltaX = 0.0;
            m_targetDeltaY = 0.0;
            m_targetDeltaZ = 0.0;
        } else {
            m_targetDeltaX = xa / distance * kSpeed;
            m_targetDeltaY = ya / distance * kSpeed;
            m_targetDeltaZ = za / distance * kSpeed;
        }

        needsSync = true;
        m_flightSteps = 10 + m_level->Random().NextInt(5) * 10;
    }

    void ShulkerBullet::CheckDespawn() {
        if (m_level && m_level->GetDifficulty() == Difficulty::Peaceful) {
            Discard();
        }
    }

    bool ShulkerBullet::CanHitEntity(const Entity& entity) const {
        // MC: never an entity with noPhysics — the only such entities in this
        // port are other projectiles.
        if (dynamic_cast<const Projectile*>(&entity)) return false;
        return Projectile::CanHitEntity(entity);
    }

    bool ShulkerBullet::Hurt(MobDamageSource source, float amount, Entity* attacker) {
        (void)source; (void)amount; (void)attacker;
        if (m_level && !m_level->IsClientSide()) Destroy();
        return true;   // MC hurtServer: any hit pops the bullet
    }

    void ShulkerBullet::Destroy() {
        Discard();
    }

    void ShulkerBullet::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;

        auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner());
        const bool wasHurt = target.Hurt(MobDamageSource::Projectile, 4.0f,
                                         livingOwner ? GetOwner() : this);
        // MC ShulkerBullet.onHitEntity: a landed hit applies LEVITATION for
        // 200 ticks (10 s), attributed to firstNonNull(owner, this). On a
        // player the stored effect ticks down but cannot move them — player
        // motion is client-authoritative and no effect sync exists yet (the
        // documented follow-up).
        if (wasHurt) {
            target.AddEffect(MobEffectInstance(MobEffectId::Levitation, 200),
                             GetOwner() ? GetOwner() : this);
        }
    }

    void ShulkerBullet::OnHit(const HitResult& hit) {
        Projectile::OnHit(hit);
        Destroy();
    }

    void ShulkerBullet::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const bool serverSide = !m_level->IsClientSide();

        Entity::BaseTick();   // MC super.tick() runs first

        LivingEntity* target = nullptr;
        HitResult hit;
        if (serverSide) {
            if (m_finalTarget &&
                (!m_finalTarget->IsAlive() || m_finalTarget->IsSpectator())) {
                m_finalTarget = nullptr;
            }
            target = m_finalTarget;

            if (!target) {
                // MC applyGravity — the bullet's 0.04.
                velocity.y -= 0.04;
            } else {
                m_targetDeltaX = std::clamp(m_targetDeltaX * 1.025, -1.0, 1.0);
                m_targetDeltaY = std::clamp(m_targetDeltaY * 1.025, -1.0, 1.0);
                m_targetDeltaZ = std::clamp(m_targetDeltaZ * 1.025, -1.0, 1.0);
                velocity += glm::dvec3((m_targetDeltaX - velocity.x) * 0.2,
                                       (m_targetDeltaY - velocity.y) * 0.2,
                                       (m_targetDeltaZ - velocity.z) * 0.2);
            }

            hit = Clip(position, velocity, true);
        }

        // MC: the bullet always advances its FULL movement; the hit is
        // resolved afterwards.
        position += velocity;

        if (serverSide && hit.IsHit() && IsAlive()) {
            OnHit(hit);
            if (IsRemoved()) return;
        }

        RotateTowardsMovement(0.5f);

        if (serverSide && target) {
            if (m_flightSteps > 0) {
                --m_flightSteps;
                if (m_flightSteps == 0) {
                    SelectNextMoveDirection(m_currentMoveDirection == -1
                                                ? -1
                                                : kDirAxis[m_currentMoveDirection]);
                }
            }

            if (m_currentMoveDirection != -1) {
                const glm::ivec3 current = BlockPosition();
                const int axis = kDirAxis[m_currentMoveDirection];
                const glm::ivec3 ahead = current + kDirStep[m_currentMoveDirection];
                // MC loadedAndEntityCanStandOn — approximated with the
                // collision table: a solid block ahead means re-steer.
                if (BlockRegistry::HasCollision(
                        blocks.GetBlock(ahead.x, ahead.y, ahead.z))) {
                    SelectNextMoveDirection(axis);
                } else {
                    const glm::ivec3 targetBp = target->BlockPosition();
                    if ((axis == 0 && current.x == targetBp.x) ||
                        (axis == 1 && current.y == targetBp.y) ||
                        (axis == 2 && current.z == targetBp.z)) {
                        SelectNextMoveDirection(axis);
                    }
                }
            }
        }
    }

} // namespace Game
