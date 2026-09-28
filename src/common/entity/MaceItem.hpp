// File: src/common/entity/MaceItem.hpp
//
// MC net.minecraft.world.item.MaceItem — the smash attack.
//
// A mace hit taken while falling more than 1.5 blocks (and not gliding) is a
// SMASH: it deals a "mace_smash" damage type instead of player_attack, adds a
// fall-scaled bonus (4 per block for the first 3, 2 per block for the next 5,
// 1 per block after — plus Density's per-block bonus), bounces the attacker
// (vertical velocity 0.01, fall damage cancelled through the impulse
// context, fall distance reset after the hit), throws everything else
// within 3.5 blocks of the victim up and away, and — when the victim is on
// the ground — raises a pillar of dust from the block under it
// (level event 2013).
//
// The hooks are MC's own, in MC's order inside Player.attack:
//
//   CanSmashAttack        MaceItem.canSmashAttack (the damage source choice
//                         at createAttackSource, and every gate below)
//   AttackDamageBonus     Item.getAttackDamageBonus — added to the base damage
//                         BEFORE the crit multiplier, so a smash crit scales it
//   HurtEnemy             Item.hurtEnemy (itemAttackInteraction, after the hit
//                         landed; before the weapon's post-attack enchantment
//                         effects — Wind Burst reads the fall distance)
//   PostHurtEnemy         Item.postHurtEnemy (after the post-attack effects)
//
// Server side only: every function is a no-op on a client level.
#pragma once

#include "common/entity/GeneratedItemList.hpp"

namespace Game {

    class Entity;
    class LivingEntity;
    struct EntityLevel;
    struct DamageSourceInfo;

    namespace MaceItem {

        // MaceItem constants.
        inline constexpr float kSmashAttackFallThreshold  = 1.5f;
        inline constexpr float kSmashAttackHeavyThreshold = 5.0f;
        inline constexpr float kSmashAttackKnockbackRadius = 3.5f;
        inline constexpr float kSmashAttackKnockbackPower  = 0.7f;

        inline bool IsMace(ItemID item) { return item == Items::Mace; }

        // MaceItem.canSmashAttack: fallDistance > 1.5 && !isFallFlying.
        // (No elytra flight exists in this engine, so the glide term is
        // always false.)
        bool CanSmashAttack(const LivingEntity& attacker);

        // MaceItem.getAttackDamageBonus(victim, damage, source): 0 unless the
        // DIRECT entity of `source` can smash, else the fall curve plus
        // EnchantmentHelper.modifyFallBasedDamage (Density) × fall distance.
        float AttackDamageBonus(EntityLevel& level, Entity& victim, const DamageSourceInfo& source);

        // MaceItem.hurtEnemy(stack, mob, attacker).
        void HurtEnemy(EntityLevel& level, LivingEntity& mob, LivingEntity& attacker);

        // MaceItem.postHurtEnemy: a smash's fall distance is spent.
        void PostHurtEnemy(LivingEntity& attacker);

        // ENGINE RULE (deliberate deviation from MC): is `e` a tamed animal
        // owned by `owner` — a wolf, cat, parrot or nautilus (TamableAnimal),
        // or a horse, donkey, mule, camel or llama (HorseTaming)? The smash
        // shockwave and Wind Burst's blast spare the attacker's own pets.
        bool IsTamedPetOf(const Entity& e, const Entity& owner);

    } // namespace MaceItem

} // namespace Game
