// File: src/common/entity/projectile/ThrownTrident.cpp
#include "common/entity/projectile/ThrownTrident.hpp"
#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    void ThrownTrident::SetTridentItem(const ItemStack& trident) {
        SetPickupItemStack(trident);
        // ID_LOYALTY: getLoyaltyFromItem — the server level's
        // getTridentReturnToOwnerAcceleration, clamped to a byte; 0 on a
        // client. ID_FOIL: tridentItem.hasFoil().
        m_loyalty = 0;
        if (m_level && !m_level->IsClientSide() && !trident.IsEmpty()) {
            m_loyalty = static_cast<uint8_t>(std::clamp(
                EnchantmentHelper::GetTridentReturnToOwnerAcceleration(*m_level, trident, *this), 0, 31));
        }
        m_foil = !trident.IsEmpty() && trident.HasFoil();
        needsSync = true;
    }

    uint8_t ThrownTrident::GetVariantByte() const {
        return static_cast<uint8_t>(Arrow::GetVariantByte() | (m_foil ? 0x04 : 0) |
                                    ((m_loyalty & 0x1F) << 3));
    }

    void ThrownTrident::SetVariantByte(uint8_t v) {
        Arrow::SetVariantByte(v);
        m_foil = (v & 0x04) != 0;
        m_loyalty = static_cast<uint8_t>((v >> 3) & 0x1F);
    }

    ItemStack ThrownTrident::GetDefaultPickupItem() const {
        // MC ThrownTrident.getDefaultPickupItem.
        return ItemStack(Items::Trident, 1);
    }

    bool ThrownTrident::OwnedBy(const Entity& entity) {
        return OwnerRef().Matches(entity);
    }

    bool ThrownTrident::IsAcceptableReturnOwner() {
        // MC isAcceptibleReturnOwner: an owner alive, and not a spectating
        // player.
        Entity* owner = GetOwner();
        if (!owner || !owner->IsAlive()) return false;
        return !(owner->IsPlayer() && owner->IsSpectator());
    }

    void ThrownTrident::Tick() {
        // MC ThrownTrident.tick head: 4 ticks in the ground marks the damage
        // dealt (so a picked-loose trident no longer hits).
        if (m_inGroundTime > 4) m_dealtDamage = true;

        const bool clientSide = m_level && m_level->IsClientSide();
        // A client copy has no owner to steer by: a homing trident (the
        // no-physics bit its server copy syncs) rides the server's position
        // and rotation packets — simulating a ghost's straight flight here
        // would only fight them.
        if (clientSide && m_noPhysics) return;

        Entity* owner = clientSide ? nullptr : GetOwner();
        const int loyalty = m_loyalty;
        if (loyalty > 0 && (m_dealtDamage || m_noPhysics) && owner) {
            if (!IsAcceptableReturnOwner()) {
                // The owner is gone (or spectating): an ALLOWED trident
                // drops as its item (spawnAtLocation(level, item, 0.1)).
                if (m_pickup == Pickup::Allowed && m_level) {
                    m_level->SpawnItemStackDrop(position + glm::dvec3(0.0, 0.1, 0.0), GetPickupItem());
                }
                Discard();
            } else {
                // A mob owner takes it back once it is within reach (its
                // own width + 1 of the eyes): discarded, the drowned keeps
                // the one it holds.
                const glm::dvec3 eye = owner->GetEyePosition();
                if (!owner->IsPlayer() &&
                    glm::length(position - eye) < static_cast<double>(owner->GetBbWidth()) + 1.0) {
                    Discard();
                    return;
                }
                SetNoPhysics(true);
                const glm::dvec3 toOwner = eye - position;
                // setPosRaw(x, y + vec.y * 0.015 * loyalty, z).
                position.y += toOwner.y * 0.015 * static_cast<double>(loyalty);
                const double accel = 0.05 * static_cast<double>(loyalty);
                const double len = glm::length(toOwner);
                const glm::dvec3 dir = len > 1.0e-4 ? toOwner / len : glm::dvec3(0.0);
                velocity = velocity * 0.95 + dir * accel;
                needsSync = true;
                // playSound(TRIDENT_RETURN, 10, 1) on the first homing tick.
                if (m_returnTickCount == 0) PlaySound(SoundEvents::TRIDENT_RETURN, 10.0f, 1.0f);
                ++m_returnTickCount;
            }
        }
        if (IsRemoved()) return;
        Arrow::Tick();
    }

    void ThrownTrident::TickDespawn() {
        // MC: a loyal trident that can be collected never despawns.
        if (m_pickup != Pickup::Allowed || m_loyalty <= 0) Arrow::TickDespawn();
    }

    bool ThrownTrident::TryPickup(LivingEntity& player) {
        // MC: super.tryPickup, or — homing — straight into its owner's
        // inventory whatever the pickup rule.
        if (Arrow::TryPickup(player)) return true;
        return m_noPhysics && OwnedBy(player) && m_level &&
               m_level->TryAddItemToPlayer(player, GetPickupItem());
    }

    void ThrownTrident::PlayerTouch(LivingEntity& player) {
        // MC: only its owner, or anyone when it has none.
        if (OwnedBy(player) || !GetOwner()) Arrow::PlayerTouch(player);
    }

    glm::dvec3 ThrownTrident::HitBlockEffectsLocation(const glm::dvec3& hitPos,
                                                      const glm::ivec3& blockPos) const {
        // BlockPos.clampLocationWithin: into the struck block, 1e-5 inside
        // each face.
        const auto clampAxis = [](double v, int b) {
            return std::clamp(v, static_cast<double>(static_cast<float>(b) + 1.0e-5f),
                              static_cast<double>(static_cast<float>(b + 1) - 1.0e-5f));
        };
        return glm::dvec3(clampAxis(hitPos.x, blockPos.x), clampAxis(hitPos.y, blockPos.y),
                          clampAxis(hitPos.z, blockPos.z));
    }

    void ThrownTrident::OnHitEntity(LivingEntity& target, const HitResult& hit) {
        // MC ThrownTrident.onHitEntity: 8.0 through the trident's
        // `minecraft:damage` effects (Impaling against #sensitive_to_impaling),
        // dealtDamage set BEFORE the hurt; on a landed hit the post-attack
        // effects with the trident as the item source (Channeling's bolt in
        // a thunderstorm — a trident worn out by them is killed), the
        // knockback and the post-hurt effects; then — unless the target
        // shrugged it off by dodging (an enderman) — TRIDENT_HIT and the
        // REVERSE deflection scaled (0.02, 0.2, 0.02): the trident drops at
        // the target's feet instead of flying on.
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

        const bool wasHurt = DealHitDamage(target, hit, MobDamageSource::Projectile, damage,
                                           owner ? owner : this);
        if (wasHurt && m_level && !m_level->IsClientSide()) {
            // doPostAttackEffectsWithItemSourceOnBreak (the break kills the
            // trident — OnItemBreak), then doKnockback.
            EnchantmentHelper::DoPostAttackEffectsWithItemSource(*m_level, target, source, weapon);
            if (IsRemoved()) return;
            DoKnockback(target, source);
        }

        // Entity.projectileReceivesSideEffectsOnHit: always, but an
        // enderman's only when it was actually hurt.
        const bool sideEffects = wasHurt || target.GetType() != EntityTypeId::Enderman;
        if (!sideEffects) return;
        PlaySound(SoundEvents::TRIDENT_HIT, 1.0f, 1.0f);
        // ProjectileDeflection.REVERSE with power (0.02, 0.2, 0.02):
        // motion × (-power × 0.5), yRot += 170..190.
        velocity.x *= -0.02 * 0.5;
        velocity.y *= -0.2 * 0.5;
        velocity.z *= -0.02 * 0.5;
        if (m_level) {
            const float rotation = 170.0f + m_level->Random().NextFloat() * 20.0f;
            yRot += rotation;
            yRotO += rotation;
        }
        needsSync = true;
    }

} // namespace Game
