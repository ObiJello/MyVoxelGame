// File: src/common/entity/projectile/ThrownTrident.cpp
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

namespace Game {

    void ThrownTrident::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC ThrownTrident.onHitEntity: 8.0 through the trident's
        // `minecraft:damage` effects (Impaling), dealtDamage set BEFORE the
        // hurt, then ProjectileDeflection.REVERSE plus the near-total
        // velocity kill (0.02, 0.2, 0.02) — the trident drops at the target's
        // feet instead of flying on. The trident's item is its weapon
        // (getWeaponItem = the pickup stack); a drowned's carries none here,
        // so its throws take the plain 8.
        Entity* const owner = GetOwner();
        const DamageSourceInfo source =
            DamageSourceInfo::Of(MobDamageSource::Projectile, owner ? owner : this, this);
        ItemStack* const weapon = GetWeaponItem();
        float damage = kTridentDamage;
        if (weapon && m_level && !m_level->IsClientSide()) {
            damage = EnchantmentHelper::ModifyDamage(*m_level, *weapon, target, source, damage);
        }
        m_dealtDamage = true;

        if (auto* livingOwner = dynamic_cast<LivingEntity*>(owner)) {
            livingOwner->SetLastHurtMob(&target);
        }

        // (MC skips the post-hurt effects for endermen because the trident
        // never actually lands on one — the enderman Hurt override's teleport
        // dodge covers that here.)
        if (DealHitDamage(target, hit, MobDamageSource::Projectile, damage, owner ? owner : this) &&
            m_level && !m_level->IsClientSide()) {
            // doPostAttackEffectsWithItemSource (Channeling's bolt), then
            // doKnockback.
            EnchantmentHelper::DoPostAttackEffectsWithItemSource(*m_level, target, source, weapon);
            DoKnockback(target, source);
        }

        // Deflection: REVERSE (-0.5) then the (0.02, 0.2, 0.02) multiply.
        velocity *= -0.5;
        velocity.x *= 0.02;
        velocity.y *= 0.2;
        velocity.z *= 0.02;
        yRot += 180.0f;
        yRotO += 180.0f;
        // MC: TRIDENT_HIT (Channeling's thunder is its own play_sound effect).
        PlaySound(SoundEvents::TRIDENT_HIT, 1.0f, 1.0f);
    }

} // namespace Game
