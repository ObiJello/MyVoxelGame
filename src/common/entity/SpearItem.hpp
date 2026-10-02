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
#include "common/data/DataComponents.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string_view>
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

        // The component records (common/data/components/WeaponComponents.hpp)
        // under the names this module has always used.
        using KineticCondition = Game::KineticWeapon::Condition;
        using KineticWeapon    = Game::KineticWeapon;
        using PiercingWeapon   = Game::PiercingWeapon;
        using AttackRange      = Game::AttackRange;

        // What makes a stack spear-like: its KINETIC_WEAPON (the charge),
        // PIERCING_WEAPON (the jab), ATTACK_RANGE and ATTACK_ANIMATION
        // components (Item.Properties.spear puts all of them on the seven
        // spears; a component patch can give them to anything).
        struct SpearDefinition {
            ItemID                        item = Items::Air;
            std::optional<KineticWeapon>  kineticWeapon;
            std::optional<PiercingWeapon> piercingWeapon;
            AttackRange                   range;
            // MC ATTACK_ANIMATION's duration (SwingAnimation(STAB, …)).
            int                           stabDurationTicks = 6;
            bool                          stab = false;
        };

        // The stack's definition — nullopt when it has neither a
        // KINETIC_WEAPON nor a PIERCING_WEAPON.
        std::optional<SpearDefinition> ForStack(const ItemStack& stack);
        // The item prototype's (a holder known only by its item id: a
        // remote player's hand, a renderer's cached id). Null when the
        // prototype is not spear-like.
        const SpearDefinition* Find(ItemID id);
        inline bool IsSpear(ItemID id) { return Find(id) != nullptr; }
        // MC `stack.has(PIERCING_WEAPON)` — jabs instead of attacking.
        bool IsPiercing(const ItemStack& stack);
        // MC stack.get(KINETIC_WEAPON) / get(PIERCING_WEAPON).
        std::optional<KineticWeapon>  Kinetic(const ItemStack& stack);
        std::optional<PiercingWeapon> Piercing(const ItemStack& stack);

        // MC ItemStack.getAttackAnimation().duration() — the ATTACK_ANIMATION
        // component's, SwingAnimation.DEFAULT's 6-tick WHACK without one.
        int AttackAnimationDuration(ItemID id);
        int AttackAnimationDuration(const ItemStack& stack);
        // Whether the attack swing is a STAB (ATTACK_ANIMATION type).
        bool IsStabSwing(ItemID id);
        bool IsStabSwing(const ItemStack& stack);

        // MC Player.cannotAttackWithItem(stack, tolerance): a full charge is
        // required (MINIMUM_ATTACK_CHARGE), with `tolerance` ticks of slack
        // (0 on the client's click, 5 on the server's check).
        bool CannotAttackWithItem(ItemID id, int attackStrengthTicker, float attackStrengthDelay,
                                  int tolerance);
        // The same over a stack's own MINIMUM_ATTACK_CHARGE.
        bool CannotAttackWithItem(const ItemStack& stack, int attackStrengthTicker, float attackStrengthDelay,
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
        void PlayWeaponSound(LivingEntity& causer, std::string_view sound);

    } // namespace Spear
} // namespace Game
