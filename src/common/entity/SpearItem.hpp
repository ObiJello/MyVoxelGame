// File: src/common/entity/SpearItem.hpp
//
// The 26.x spears — MC Item.Properties.spear and the three data components
// it attaches, with the combat each one drives:
//
//   KINETIC_WEAPON   (world/item/component/KineticWeapon)  — the charge: hold
//       use and the spear levels; every tick of the hold (ItemStack.onUseTick
//       → damageEntities) whatever the tip runs into takes a hit scaled by
//       the closing speed, gated per effect on how long the charge has run
//       and how fast (dismount / knockback / damage conditions), with a
//       per-target contact cooldown (recentKineticEnemies).
//   PIERCING_WEAPON  (world/item/component/PiercingWeapon) — the jab: a left
//       click stabs every entity along the reach ray (ServerboundPlayerAction
//       STAB), needs a full attack charge (MINIMUM_ATTACK_CHARGE 1.0), and
//       runs the post_piercing_attack enchantment effects (Lunge).
//   ATTACK_RANGE     (world/item/component/AttackRange)    — 2..4.5 blocks
//       (2..6.5 in creative), a 0.125 hitbox margin, half range for mobs.
//
// The components are fixed per item in vanilla (nothing in the game rewrites
// them), so they live here as the item's constant properties rather than as
// serialised DataComponents — every number is Items.java's spear(...) call.
//
// SpearAnimations' UseParams (the charge's raise / sway / lower curve) is here
// too: the first-person hand, the third-person arm and the held item all read
// it, on the client, from the same KineticWeapon.
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace Game {

    class Entity;
    class LivingEntity;
    struct EntityLevel;

    namespace Spear {

        // MC Item.getUseDuration for a KINETIC_WEAPON (and BLOCKS_ATTACKS):
        // APPROXIMATELY_INFINITE_USE_DURATION.
        constexpr int   kUseDuration = 72000;
        // MC KineticWeapon.HIT_FEEDBACK_TICKS.
        constexpr int   kHitFeedbackTicks = 10;
        // MC DataComponents.MINIMUM_ATTACK_CHARGE on every spear.
        constexpr float kMinimumAttackCharge = 1.0f;
        // MC entity event 2: LivingEntity.onKineticHit (the charge struck
        // something — the hit sound and the hand's recoil).
        constexpr uint8_t kEntityEventKineticHit = 2;

        // MC KineticWeapon.Condition.
        struct KineticCondition {
            int   maxDurationTicks = 0;
            float minSpeed = 0.0f;
            float minRelativeSpeed = 0.0f;

            bool Test(int ticksUsed, double attackerSpeed, double relativeSpeed,
                      double entityFactor) const {
                return ticksUsed <= maxDurationTicks &&
                       attackerSpeed >= static_cast<double>(minSpeed) * entityFactor &&
                       relativeSpeed >= static_cast<double>(minRelativeSpeed) * entityFactor;
            }
        };

        // MC KineticWeapon.
        struct KineticWeapon {
            int   contactCooldownTicks = 10;
            int   delayTicks = 0;
            std::optional<KineticCondition> dismountConditions;
            std::optional<KineticCondition> knockbackConditions;
            std::optional<KineticCondition> damageConditions;
            float forwardMovement = 0.0f;
            float damageMultiplier = 1.0f;
            const char* sound = nullptr;      // makeSound: at the start of the hold
            const char* hitSound = nullptr;   // makeLocalHitSound: on a connecting charge

            // MC computeDamageUseDuration: how long a mob holds the charge.
            int ComputeDamageUseDuration() const {
                return delayTicks + (damageConditions ? damageConditions->maxDurationTicks : 0);
            }
        };

        // MC PiercingWeapon.
        struct PiercingWeapon {
            bool dealsKnockback = true;
            bool dismounts = false;
            const char* sound = nullptr;      // makeSound: every jab
            const char* hitSound = nullptr;   // makeHitSound: a jab that struck
        };

        // MC AttackRange.
        struct AttackRange {
            float minReach = 0.0f, maxReach = 3.0f;
            float minCreativeReach = 0.0f, maxCreativeReach = 5.0f;
            float hitboxMargin = 0.3f;
            float mobFactor = 1.0f;

            float EffectiveMinRange(const Entity& entity) const;
            float EffectiveMaxRange(const Entity& entity) const;
        };

        struct SpearDefinition {
            ItemID         item = Items::Air;
            KineticWeapon  kinetic;
            PiercingWeapon piercing;
            AttackRange    range;
            // MC ATTACK_ANIMATION: SwingAnimation(STAB, attackDuration * 20).
            int            stabDurationTicks = 6;
        };

        // The spear's definition, or null for anything that is not a spear.
        const SpearDefinition* Find(ItemID id);
        inline bool IsSpear(ItemID id) { return Find(id) != nullptr; }
        inline const KineticWeapon* Kinetic(const ItemStack& stack) {
            const SpearDefinition* d = stack.IsEmpty() ? nullptr : Find(stack.itemId);
            return d ? &d->kinetic : nullptr;
        }
        inline const PiercingWeapon* Piercing(const ItemStack& stack) {
            const SpearDefinition* d = stack.IsEmpty() ? nullptr : Find(stack.itemId);
            return d ? &d->piercing : nullptr;
        }

        // MC ItemStack.getAttackAnimation().duration() — the spear's STAB
        // length, SwingAnimation.DEFAULT's 6-tick WHACK for everything else.
        int AttackAnimationDuration(ItemID id);

        // MC Player.cannotAttackWithItem(stack, tolerance): a full charge is
        // required (MINIMUM_ATTACK_CHARGE), with `tolerance` ticks of slack
        // (0 on the client's click, 5 on the server's check).
        bool CannotAttackWithItem(ItemID id, int attackStrengthTicker, float attackStrengthDelay,
                                  int tolerance);

        // ── SpearAnimations.UseParams ─────────────────────────────────────
        struct UseParams {
            float raiseProgress = 0.0f, raiseProgressStart = 0.0f, raiseProgressMiddle = 0.0f,
                  raiseProgressEnd = 0.0f, swayProgress = 0.0f, lowerProgress = 0.0f,
                  raiseBackProgress = 0.0f, swayIntensity = 0.0f, swayScaleSlow = 0.0f,
                  swayScaleFast = 0.0f;
            static UseParams FromKineticWeapon(const KineticWeapon& weapon, float time);
        };
        // MC SpearAnimations.hitFeedbackAmount.
        float HitFeedbackAmount(float ticksSinceFeedbackStart);

        // ── Combat (server) ───────────────────────────────────────────────

        // MC KineticWeapon.getMotion: the entity's (or, for a mob, its
        // mount's) known speed in blocks per second.
        glm::dvec3 GetMotion(Entity& entity);

        // MC PiercingWeapon.canHitEntity.
        bool CanHitEntity(Entity& jabber, Entity& target);

        struct EntityHit {
            Entity*    entity = nullptr;
            glm::dvec3 location{0.0};
            // An ender dragon struck on one of its part boxes: the part
            // index (EnderDragon::kDragonPart*), -1 otherwise.
            int        dragonPart = -1;
        };
        // MC ProjectileUtil.getHitEntitiesAlong(attacker, range, matching,
        // COLLIDER): every entity along the attacker's head-look ray between
        // the effective min and max reach (the max lengthened by the forward
        // component of its known movement), unless a block stops the ray
        // short of the min reach. Order is the level's entity order.
        void GetHitEntitiesAlong(LivingEntity& attacker, const AttackRange& range,
                                 const std::function<bool(Entity&)>& matching,
                                 std::vector<EntityHit>& out);

        // MC KineticWeapon.damageEntities: one tick of a charge. `useDuration`
        // is the stack's getUseDuration (72000), `ticksRemaining` the user's
        // countdown; the user remembers what it stabbed (contact cooldown).
        void DamageEntities(const KineticWeapon& weapon, int useDuration, int ticksRemaining,
                            LivingEntity& user, EquipmentSlot slot);

        // MC PiercingWeapon.attack: the jab, server side — every entity along
        // the reach takes stabAttack(damage, dealsDamage = true), then
        // onAttack, postPiercingAttack (Lunge), the hit sound when something
        // was struck, the jab sound, and the STAB swing.
        void PiercingAttack(const PiercingWeapon& weapon, LivingEntity& attacker, EquipmentSlot hand);

        // MC KineticWeapon.makeSound / PiercingWeapon.makeSound —
        // level.playSound(causer, ...): everyone but the causer, whose
        // client plays it itself.
        void PlayWeaponSound(LivingEntity& causer, const char* sound);

    } // namespace Spear
} // namespace Game
