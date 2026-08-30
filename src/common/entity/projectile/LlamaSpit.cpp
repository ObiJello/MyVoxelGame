// File: src/common/entity/projectile/LlamaSpit.cpp
#include "common/entity/projectile/LlamaSpit.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/core/Mth.hpp"
#include "common/world/chunk/IBlockAccess.hpp"

#include <cmath>

namespace Game {

    void LlamaSpit::InitFromLlama(LivingEntity& llama) {
        SetOwner(&llama);
        const double side = static_cast<double>(llama.GetBbWidth() + 1.0f) * 0.5;
        const double yawRad = static_cast<double>(llama.yBodyRot) * Mth::kDegToRad;
        position = glm::dvec3(llama.position.x - side * std::sin(yawRad),
                              llama.GetEyeY() - 0.1,
                              llama.position.z + side * std::cos(yawRad));
    }

    void LlamaSpit::OnHitEntity(LivingEntity& target) {
        if (!m_level || m_level->IsClientSide()) return;
        // MC: the spit only damages when its owner is a living entity.
        auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner());
        if (!livingOwner) return;
        livingOwner->SetLastHurtMob(&target);
        target.Hurt(MobDamageSource::Projectile, 1.0f, livingOwner);
    }

    void LlamaSpit::OnHitBlock(const HitResult& hit) {
        (void)hit;
        if (m_level && !m_level->IsClientSide()) Discard();
    }

    void LlamaSpit::Tick() {
        if (!m_level || !m_level->Blocks()) return;
        const IBlockAccess& blocks = *m_level->Blocks();
        const bool serverSide = !m_level->IsClientSide();

        Entity::BaseTick();   // MC super.tick() first

        // MC: the hit is resolved BEFORE the position update.
        const glm::dvec3 movement = velocity;
        const HitResult hit = Clip(position, movement, serverSide);
        if (hit.IsHit() && IsAlive()) {
            OnHit(hit);
            if (IsRemoved()) return;
        }

        const glm::dvec3 newPos = position + movement;
        RotateTowardsMovement(0.2f);

        // MC: dissolves when its bounding box is entirely inside non-air
        // blocks, or in water.
        {
            const AABB box = GetAABB();
            bool anyAir = false;
            for (int x = static_cast<int>(std::floor(box.min.x));
                 x <= static_cast<int>(std::floor(box.max.x)) && !anyAir; ++x) {
                for (int y = static_cast<int>(std::floor(box.min.y));
                     y <= static_cast<int>(std::floor(box.max.y)) && !anyAir; ++y) {
                    for (int z = static_cast<int>(std::floor(box.min.z));
                         z <= static_cast<int>(std::floor(box.max.z)); ++z) {
                        if (blocks.GetBlock(x, y, z) == BlockID::Air) {
                            anyAir = true;
                            break;
                        }
                    }
                }
            }
            if (!anyAir) {
                Discard();
                return;
            }
        }
        if (IsInWater()) {
            Discard();
            return;
        }

        velocity = movement * 0.99;
        velocity.y -= kGravity;
        position = newPos;
    }

} // namespace Game
