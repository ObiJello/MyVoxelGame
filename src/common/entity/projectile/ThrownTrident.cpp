// File: src/common/entity/projectile/ThrownTrident.cpp
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/EntityLevel.hpp"

namespace Game {

    void ThrownTrident::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC ThrownTrident.onHitEntity: fixed 8.0 (enchantment damage
        // modifiers wait on the enchantment system), dealtDamage set BEFORE
        // the hurt, then ProjectileDeflection.REVERSE plus the near-total
        // velocity kill (0.02, 0.2, 0.02) — the trident drops at the target's
        // feet instead of flying on.
        m_dealtDamage = true;

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(GetOwner())) {
            livingOwner->SetLastHurtMob(&target);
        }

        // (MC skips the post-hurt effects for endermen because the trident
        // never actually lands on one — the enderman Hurt override's teleport
        // dodge covers that here.)
        DealHitDamage(target, hit, MobDamageSource::Projectile, kTridentDamage,
                    GetOwner() ? GetOwner() : this);

        // Deflection: REVERSE (-0.5) then the (0.02, 0.2, 0.02) multiply.
        velocity *= -0.5;
        velocity.x *= 0.02;
        velocity.y *= 0.2;
        velocity.z *= 0.02;
        yRot += 180.0f;
        yRotO += 180.0f;
    }

} // namespace Game
