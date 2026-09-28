// File: src/common/entity/MaceItem.cpp
//
// Reference: minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/item/MaceItem.java.
#include "common/entity/MaceItem.hpp"

#include "common/entity/ArmorStand.hpp"
#include "common/entity/Attributes.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/TamableAnimal.hpp"
#include "common/entity/HorseTaming.hpp"
#include "common/entity/projectile/Projectile.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/damagesource/DamageSourceInfo.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <cmath>
#include <string_view>
#include <vector>

namespace Game::MaceItem {

    namespace {

        // (double)0.7F, as MaceItem writes both the knockback power and the
        // push's vertical term.
        constexpr double kPower07 = static_cast<double>(0.7f);

        // MC Entity.getOnPos() = getOnPos(1.0E-5F): the block under the
        // entity's feet. The supporting-block refinement MC applies for an
        // entity standing on a block edge is not tracked here; the column
        // straight below is the same block for every entity standing on it.
        glm::ivec3 OnPos(const Entity& e) {
            return glm::ivec3(static_cast<int>(std::floor(e.position.x)),
                              static_cast<int>(std::floor(e.position.y - static_cast<double>(1.0e-5f))),
                              static_cast<int>(std::floor(e.position.z)));
        }

        // MC getEntitiesOfClass(LivingEntity.class, …) over an engine where
        // several non-living things (projectiles, primed TNT, falling blocks,
        // paintings, item frames, end crystals, vehicles) also ride the
        // LivingEntity/Mob pipeline. The NoAiTag-built entities are exactly
        // those non-living shells, except the armor stand, which IS a
        // LivingEntity in MC.
        bool IsVanillaLiving(Entity& e) {
            LivingEntity* living = e.AsLiving();
            if (!living) return false;
            if (dynamic_cast<Projectile*>(&e)) return false;
            if (auto* mob = dynamic_cast<Mob*>(&e); mob && !mob->HasAiControls()) {
                return e.GetType() == EntityTypeId::ArmorStand;
            }
            const std::string_view slug = e.TypeInfo().slug;
            const auto endsWith = [slug](std::string_view tail) {
                return slug.size() >= tail.size() && slug.substr(slug.size() - tail.size()) == tail;
            };
            if (endsWith("boat") || endsWith("raft") || endsWith("minecart")) return false;
            if (slug == "area_effect_cloud" || slug == "evoker_fangs" || slug == "lightning_bolt" ||
                slug == "eye_of_ender" || slug == "firework_rocket" || slug == "fishing_bobber" ||
                slug == "leash_knot" || slug == "marker" || slug == "interaction") {
                return false;
            }
            return true;
        }

        // MaceItem.knockbackPredicate(attacker, entity) — `entity` is the
        // mob that was HIT. Every term is MC's, including the tamed-animal
        // one, which (as vanilla writes it) spares the pets owned by the
        // victim, not by the attacker.
        bool KnockbackPredicate(const LivingEntity& attacker, const LivingEntity& entity,
                                LivingEntity& nearby) {
            const bool notSpectator = !nearby.IsSpectator();
            const bool notPlayer = &nearby != &attacker && &nearby != &entity;
            // attacker.isAlliedTo(nearby): scoreboard teams — none exist.
            const bool notAlliedToPlayer = true;
            bool tamedByVictim = false;
            if (const auto* animal = dynamic_cast<const TamableAnimal*>(&nearby)) {
                tamedByVictim = animal->IsTame() && animal->IsOwnedBy(entity);
            }
            // DELIBERATE DEVIATION (engine rule, user decision): the
            // attacker's own tamed pets are spared as well — MC spares only
            // the victim's.
            const bool tamedByAttacker = IsTamedPetOf(nearby, attacker);
            const bool notTamedByPlayer = !tamedByVictim && !tamedByAttacker;
            bool markerStand = false;
            if (const auto* stand = dynamic_cast<const ArmorStand*>(&nearby)) {
                markerStand = stand->IsMarker();
            }
            const bool notArmorStand = !markerStand;
            const double r = static_cast<double>(kSmashAttackKnockbackRadius);
            const bool withinRange = entity.DistanceToSqr(nearby) <= r * r;
            const bool flyingInCreative =
                nearby.IsPlayer() && nearby.IsCreative() && nearby.IsAbilityFlying();
            return notSpectator && notPlayer && notAlliedToPlayer && notTamedByPlayer &&
                   notArmorStand && withinRange && !flyingInCreative;
        }

        // MaceItem.getKnockbackPower.
        double KnockbackPower(const LivingEntity& attacker, const LivingEntity& nearby,
                              const glm::dvec3& direction) {
            return (static_cast<double>(kSmashAttackKnockbackRadius) - glm::length(direction)) *
                   kPower07 *
                   static_cast<double>(attacker.fallDistance > kSmashAttackHeavyThreshold ? 2 : 1) *
                   (1.0 - nearby.GetAttributeValue(Attribute::KnockbackResistance));
        }

        // MaceItem.knockback(level, attacker, entity).
        void Knockback(EntityLevel& level, LivingEntity& attacker, LivingEntity& entity) {
            // levelEvent(2013, entity.getOnPos(), 750): the dust pillar and
            // ring (ParticleUtils.spawnSmashAttackParticles) on every client.
            level.PlayLevelEvent(nullptr, LevelEvent::PARTICLES_SMASH_ATTACK, OnPos(entity), 750);

            const float inflate = kSmashAttackKnockbackRadius;
            AABB box = entity.GetAABB();
            box.min -= glm::vec3(inflate);
            box.max += glm::vec3(inflate);
            std::vector<Entity*> candidates;
            level.GetEntitiesInBox(box, nullptr, candidates);

            for (Entity* e : candidates) {
                if (!e || e->IsRemoved() || !IsVanillaLiving(*e)) continue;
                LivingEntity& nearby = *e->AsLiving();
                if (!KnockbackPredicate(attacker, entity, nearby)) continue;

                const glm::dvec3 direction = nearby.position - entity.position;
                const double knockbackPower = KnockbackPower(attacker, nearby, direction);
                // Vec3.normalize: a direction shorter than 1.0E-5F is ZERO.
                const double len = glm::length(direction);
                const glm::dvec3 unit = len < static_cast<double>(1.0e-5f) ? glm::dvec3(0.0) : direction / len;
                const glm::dvec3 knockbackVector = unit * knockbackPower;
                if (knockbackPower > 0.0) {
                    // nearby.push(x, 0.7, z) — for a player, MC follows with
                    // a ClientboundSetEntityMotionPacket; the view's
                    // AddDeltaMovement queues exactly that.
                    nearby.AddDeltaMovement(glm::dvec3(knockbackVector.x, kPower07, knockbackVector.z));
                }
            }
        }

    } // namespace

    bool IsTamedPetOf(const Entity& e, const Entity& owner) {
        if (&e == &owner) return false;
        if (const auto* animal = dynamic_cast<const TamableAnimal*>(&e)) {
            if (const LivingEntity* living = owner.AsLiving()) {
                return animal->IsTame() && animal->IsOwnedBy(*living);
            }
            return false;
        }
        if (const auto* horse = dynamic_cast<const HorseTaming*>(&e)) {
            return horse->IsTamed() && horse->OwnerRef().Matches(owner);
        }
        return false;
    }

    bool CanSmashAttack(const LivingEntity& attacker) {
        return attacker.fallDistance > kSmashAttackFallThreshold;
    }

    float AttackDamageBonus(EntityLevel& level, Entity& victim, const DamageSourceInfo& source) {
        LivingEntity* attacker = source.direct ? source.direct->AsLiving() : nullptr;
        if (!attacker || !CanSmashAttack(*attacker)) return 0.0f;

        constexpr double kFallHeightThreshold1 = 3.0;
        constexpr double kFallHeightThreshold2 = 8.0;
        const double fallDistance = static_cast<double>(attacker->fallDistance);
        double damage;
        if (fallDistance <= kFallHeightThreshold1) {
            damage = 4.0 * fallDistance;
        } else if (fallDistance <= kFallHeightThreshold2) {
            damage = 12.0 + 2.0 * (fallDistance - kFallHeightThreshold1);
        } else {
            damage = 22.0 + fallDistance - kFallHeightThreshold2;
        }

        if (level.IsClientSide()) return static_cast<float>(damage);
        // EnchantmentHelper.modifyFallBasedDamage(level, getWeaponItem(),
        // victim, source, 0) × fall distance — Density's per-block bonus.
        float perBlock = 0.0f;
        if (const ItemStack* weapon = attacker->GetWeaponItem(); weapon && !weapon->IsEmpty()) {
            perBlock = EnchantmentHelper::ModifyFallBasedDamage(level, *weapon, victim, source, 0.0f);
        }
        return static_cast<float>(damage + static_cast<double>(perBlock) * fallDistance);
    }

    void HurtEnemy(EntityLevel& level, LivingEntity& mob, LivingEntity& attacker) {
        if (level.IsClientSide() || !CanSmashAttack(attacker)) return;

        // attacker.setDeltaMovement(deltaMovement.with(Y, 0.01F)), then — for
        // a player — the motion packet at once, so the bounce reaches the
        // client ahead of anything else this tick pushes (Wind Burst).
        glm::dvec3 bounced = attacker.velocity;
        bounced.y = static_cast<double>(0.01f);
        attacker.SetDeltaMovementAndSync(bounced);

        // setIgnoreFallDamageFromCurrentImpulse(true, calculateImpactPosition):
        // an earlier impulse's impact point is kept when it is not above
        // where the attacker is now, else the attacker's own position.
        ImpulseContext& impulse = attacker.GetImpulseContext();
        const glm::dvec3 impactPos =
            impulse.IsIgnoringFallDamage() && impulse.impactPos.y <= attacker.position.y
                ? impulse.impactPos : attacker.position;
        impulse.SetIgnoreFallDamage(true, impactPos);

        const SoundSource source = attacker.GetSoundSource();
        if (mob.onGround) {
            // ServerPlayer.setSpawnExtraParticlesOnFall(true): the landing
            // after the bounce throws up a cloud of the block landed on.
            if (attacker.IsPlayer()) impulse.spawnExtraParticlesOnFall = true;
            const char* sound = attacker.fallDistance > kSmashAttackHeavyThreshold
                                    ? SoundEvents::MACE_SMASH_GROUND_HEAVY
                                    : SoundEvents::MACE_SMASH_GROUND;
            level.PlaySound(nullptr, attacker.position, sound, source, 1.0f, 1.0f);
        } else {
            level.PlaySound(nullptr, attacker.position, SoundEvents::MACE_SMASH_AIR, source, 1.0f, 1.0f);
        }

        Knockback(level, attacker, mob);
    }

    void PostHurtEnemy(LivingEntity& attacker) {
        if (CanSmashAttack(attacker)) attacker.ResetFallDistance();
    }

} // namespace Game::MaceItem
