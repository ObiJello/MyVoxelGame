// File: src/common/data/components/WeaponComponents.hpp
//
// MC 26.3 item components (weapons): piercing_weapon, kinetic_weapon,
// attack_range, minimum_attack_charge, damage_type, attack_animation,
// interact_animation (+ the WEAPON / BLOCKS_ATTACKS helpers the combat paths
// share). Wire ids 270-289 (DataComponents.hpp's id table). The NBT codecs
// live in server/world/storage/anvil/components/WeaponNbt.cpp.
//
// Every one of these is read from the stack at the point MC reads it
// (Item.use / getUseAnimation / getUseDuration for KINETIC_WEAPON,
// Minecraft.startAttack and handleInteract for PIERCING_WEAPON /
// ATTACK_RANGE / MINIMUM_ATTACK_CHARGE, ItemStack.getDamageSource for
// DAMAGE_TYPE, getAttackAnimation / getInteractAnimation for the swings), so
// any item that carries them behaves like the spear that defines them —
// `stick[kinetic_weapon={…},piercing_weapon={}]` charges and jabs.
#pragma once

#include "../DataComponents.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    struct AABBd;
    enum class MobDamageSource : uint8_t;

    // MC SwingAnimationType (world/item/SwingAnimationType.java); ordinals
    // are MC's wire ids.
    enum class SwingAnimationType : uint8_t { None = 0, Whack = 1, Stab = 2 };
    std::string_view SwingAnimationTypeName(SwingAnimationType type);
    bool SwingAnimationTypeFromName(std::string_view name, SwingAnimationType& out);

    // MC SwingAnimation (world/item/component/SwingAnimation.java).
    struct SwingAnimation {
        SwingAnimationType type = SwingAnimationType::Whack;
        int                duration = 6;   // ticks, NON_NEGATIVE_INT
        // SwingAnimation.DEFAULT — every item's attack_animation and
        // interact_animation unless it says otherwise.
        static SwingAnimation Default() { return SwingAnimation{}; }
        bool operator==(const SwingAnimation& o) const { return type == o.type && duration == o.duration; }
    };

    // MC KineticWeapon (world/item/component/KineticWeapon.java) — the
    // spear's charge.
    struct KineticWeapon {
        // KineticWeapon.Condition.
        struct Condition {
            int   maxDurationTicks = 0;
            float minSpeed = 0.0f;
            float minRelativeSpeed = 0.0f;

            bool Test(int ticksUsed, double attackerSpeed, double relativeSpeed, double entityFactor) const {
                return ticksUsed <= maxDurationTicks &&
                       attackerSpeed >= static_cast<double>(minSpeed) * entityFactor &&
                       relativeSpeed >= static_cast<double>(minRelativeSpeed) * entityFactor;
            }
        };

        int                      contactCooldownTicks = 10;
        int                      delayTicks = 0;
        std::optional<Condition> dismountConditions;
        std::optional<Condition> knockbackConditions;
        std::optional<Condition> damageConditions;
        float                    forwardMovement = 0.0f;
        float                    damageMultiplier = 1.0f;
        std::string              sound;      // makeSound, "" = none
        std::string              hitSound;   // makeLocalHitSound, "" = none

        // MC computeDamageUseDuration: how long a mob holds the charge.
        int ComputeDamageUseDuration() const {
            return delayTicks + (damageConditions ? damageConditions->maxDurationTicks : 0);
        }
    };

    // MC PiercingWeapon (world/item/component/PiercingWeapon.java) — the jab.
    struct PiercingWeapon {
        bool        dealsKnockback = true;
        bool        dismounts = false;
        std::string sound;      // makeSound, every jab; "" = none
        std::string hitSound;   // makeHitSound, a jab that struck; "" = none
    };

    // MC AttackRange (world/item/component/AttackRange.java).
    struct AttackRange {
        float minReach = 0.0f, maxReach = 3.0f;
        float minCreativeReach = 0.0f, maxCreativeReach = 5.0f;
        float hitboxMargin = 0.3f;
        float mobFactor = 1.0f;

        // AttackRange.defaultFor(living): the entity's interaction range,
        // no margin.
        static AttackRange DefaultFor(double entityInteractionRange) {
            const float r = static_cast<float>(entityInteractionRange);
            return AttackRange{0.0f, r, 0.0f, r, 0.0f, 1.0f};
        }

        float EffectiveMinRange(const Entity& entity) const;
        float EffectiveMaxRange(const Entity& entity) const;
        // isInRange(attacker, location) — the eye's distance to a point.
        bool IsInRange(const LivingEntity& attacker, const glm::dvec3& location) const;
        // isInRange(attacker, box, extraBuffer) — to the nearest point of a box.
        bool IsInRange(const LivingEntity& attacker, const AABBd& box, double extraBuffer) const;
    };

    // ── Readers (MC ItemStack's getters) ────────────────────────────────

    // ItemStack.getAttackAnimation / getInteractAnimation.
    SwingAnimation GetAttackAnimation(const ItemStack& stack);
    SwingAnimation GetInteractAnimation(const ItemStack& stack);
    // The item prototype's (for a holder known only by item id).
    SwingAnimation GetAttackAnimation(ItemID item);

    // MC MINIMUM_ATTACK_CHARGE (getOrDefault 0).
    float GetMinimumAttackCharge(const ItemStack& stack);
    // MC Player.cannotAttackWithItem(stack, tolerance): `strengthWithTolerance`
    // is getAttackStrengthScale(tolerance) — (ticker + tolerance) / delay.
    bool CannotAttackWithItem(const ItemStack& stack, float strengthWithTolerance);

    // MC DAMAGE_TYPE — the damage type id ("minecraft:spear") the item's
    // melee hits carry, if it names one.
    std::optional<std::string> GetItemDamageType(const ItemStack& stack);
    // The engine's damage source for a damage type id; `fallback` for a
    // type the engine has no source of its own for (the DamageSourceInfo
    // built for the hit still carries the exact id).
    MobDamageSource MobDamageSourceForType(std::string_view damageTypeId, MobDamageSource fallback);
    // ItemStack.getDamageSource's first step for a melee hit: DAMAGE_TYPE
    // when present, else `fallback` (the item's own / the attacker's).
    MobDamageSource ItemMeleeDamageSource(const ItemStack& weapon, MobDamageSource fallback);

    // A HolderSet<DamageType> (a "#tag", ids, or a list of both) contains
    // the damage type `typeId`.
    bool DamageTypeHolderSetContains(const std::vector<std::string>& set, std::string_view typeId);

    // ── BLOCKS_ATTACKS math (BlocksAttacks.java) ─────────────────────────

    // DamageReduction.resolve / BlocksAttacks.resolveBlockedDamage: the
    // share of `dealtDamage` a block absorbs, for a hit arriving `angle`
    // radians off the blocker's facing.
    float ResolveBlockedDamage(const BlocksAttacks& blocks, std::string_view damageTypeId,
                               float dealtDamage, double angle);
    // ItemDamageFunction.apply.
    int BlockingItemDamage(const BlocksAttacks& blocks, float blockedDamage);
    // disableBlockingForTicks(baseSeconds).
    int DisableBlockingForTicks(const BlocksAttacks& blocks, float baseSeconds);
    // `bypassed_by` holds the damage type.
    bool BlocksAttacksBypassedBy(const BlocksAttacks& blocks, std::string_view damageTypeId);

    // The spears' KINETIC_WEAPON / PIERCING_WEAPON / ATTACK_RANGE /
    // MINIMUM_ATTACK_CHARGE / ATTACK_ANIMATION / DAMAGE_TYPE, exactly as
    // Item.Properties.spear builds them for Items.java's seven spear(...)
    // registrations. Called by ItemRegistry::Initialize.
    void ItemRegistry_RegisterWeaponComponents(std::unordered_map<ItemID, Item>& pureItems);

} // namespace Game

namespace Game::DataComponents {

    // MC DataComponents.PIERCING_WEAPON (id 270).
    extern const DataComponentType<PiercingWeapon> PIERCING_WEAPON;
    // MC DataComponents.KINETIC_WEAPON (id 271).
    extern const DataComponentType<KineticWeapon> KINETIC_WEAPON;
    // MC DataComponents.ATTACK_RANGE (id 272).
    extern const DataComponentType<AttackRange> ATTACK_RANGE;
    // MC DataComponents.MINIMUM_ATTACK_CHARGE (id 273), 0..1.
    extern const DataComponentType<float> MINIMUM_ATTACK_CHARGE;
    // MC DataComponents.DAMAGE_TYPE (id 274) — the damage type's id, with
    // its namespace.
    extern const DataComponentType<std::string> DAMAGE_TYPE;
    // MC DataComponents.ATTACK_ANIMATION (id 275) / INTERACT_ANIMATION (276).
    extern const DataComponentType<SwingAnimation> ATTACK_ANIMATION;
    extern const DataComponentType<SwingAnimation> INTERACT_ANIMATION;

} // namespace Game::DataComponents
