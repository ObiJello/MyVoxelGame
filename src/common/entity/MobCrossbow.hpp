// File: src/common/entity/MobCrossbow.hpp
//
// A mob firing a crossbow — MC CrossbowAttackMob (the pillager, the piglin)
// and the pieces of LivingEntity / CrossbowItem / ProjectileWeaponItem /
// ProjectileUtil / BehaviorUtils it drives:
//
//   * the use-item clock (LivingEntity.startUsingItem / updatingUsingItem /
//     releaseUsingItem / stopUsingItem) for the crossbow's draw — CrossbowItem
//     .onUseTick plays the loading sounds and, at full charge, loads the
//     projectiles into the stack's CHARGED_PROJECTILES (tryLoadProjectiles →
//     ProjectileWeaponItem.draw over Monster.getProjectile: a held arrow or
//     rocket, else a plain arrow);
//   * CrossbowAttackMob.performCrossbowAttack → CrossbowItem.performShooting:
//     the loaded projectiles fly as MC's fan (Multishot's spread), aimed at
//     the target through getProjectileShotVector, with Piercing, Flame's
//     projectile_spawned effects and the crossbow's wear;
//   * BehaviorUtils.isWithinAttackRange — a projectile weapon's range instead
//     of the melee reach for a mob that can fire it.
//
// The clock lives in the goal / behaviour that runs the draw (the only reader
// of it, as MC's RangedCrossbowAttackGoal and CrossbowAttack are), ticked at
// the head of their tick — the same order as LivingEntity.tick's
// updatingUsingItem ahead of aiStep. Everything here is server-side; the
// client sees the charging flag (CrossbowAttackMob::IsChargingCrossbow, on
// each mob's synced anim byte) and the loaded crossbow through the equipment
// sync.
//
// References (minecraft_code_26.3-pre-2/decompiled_net/minecraft/):
// world/entity/monster/CrossbowAttackMob.java, world/item/CrossbowItem.java,
// world/item/ProjectileWeaponItem.java, world/entity/projectile/
// ProjectileUtil.java, world/entity/ai/behavior/BehaviorUtils.java,
// world/entity/monster/Monster.java (getProjectile), world/entity/
// LivingEntity.java (the use-item clock).
#pragma once

#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/RangedAttackMob.hpp"

#include <cstdint>
#include <string>

namespace Game {

    class LivingEntity;
    class Mob;
    struct ItemStack;
    using ItemID = uint32_t;

    // MC CrossbowAttackMob — a RangedAttackMob whose ranged attack is a
    // crossbow shot. Pillager and Piglin implement it.
    class CrossbowAttackMob : public RangedAttackMob {
    public:
        // MC setChargingCrossbow — the synced IS_CHARGING_CROSSBOW flag the
        // client's arm pose (CROSSBOW_CHARGE) reads.
        virtual void SetChargingCrossbow(bool charging) = 0;
        virtual bool IsChargingCrossbow() const = 0;
        // MC onCrossbowAttackPerformed — both implementors reset noActionTime.
        virtual void OnCrossbowAttackPerformed() = 0;
        // The client's draw clock (MC's client-side getTicksUsingItem while
        // the using-item flag is set): ticks since the synced charging flag
        // came on, -1 while not charging. The CROSSBOW_CHARGE arm pose and
        // the pulling sprite read it.
        virtual int GetClientChargeTicks() const = 0;
    };

    namespace MobCrossbow {

        // MC CrossbowItem.DEFAULT_RANGE / BowItem.DEFAULT_RANGE — the
        // ProjectileWeaponItem.getDefaultProjectileRange values.
        inline constexpr int   kCrossbowRange = 8;
        inline constexpr int   kBowRange      = 15;
        // MC CrossbowItem.MOB_ARROW_POWER — performCrossbowAttack's power.
        inline constexpr float kMobArrowPower = 1.6f;
        // MC CrossbowItem.getUseDuration.
        inline constexpr int   kUseDuration = 72000;

        // MC ProjectileUtil.getWeaponHoldingHand: the main hand when it holds
        // `weapon`, else the off hand.
        EquipmentSlot WeaponHoldingHand(const Mob& mob, ItemID weapon);

        // MC ProjectileWeaponItem.getDefaultProjectileRange for the held main
        // hand item (0 when it is not a projectile weapon).
        int DefaultProjectileRange(ItemID id);

        // MC BehaviorUtils.isWithinAttackRange(body, target, margin): a main
        // hand projectile weapon the mob can fire → closer than its range
        // minus the margin; otherwise the melee reach.
        bool IsWithinAttackRange(const Mob& body, const LivingEntity& target, int projectileAttackRangeMargin);

        // MC CrossbowItem.getChargeDuration(crossbow, user): Quick Charge's
        // modified 1.25 s, in ticks.
        int ChargeDuration(const ItemStack& crossbow);

        // The mob's use of its crossbow — MC LivingEntity.useItem /
        // useItemRemaining / usedItemHand, plus CrossbowItem's
        // startSoundPlayed / midLoadSoundPlayed latches (fields of the item
        // singleton in MC; per shooter here).
        struct UseState {
            bool          usingItem = false;
            EquipmentSlot hand = EquipmentSlot::MAINHAND;
            int           remaining = 0;   // MC useItemRemaining
            bool          startSoundPlayed = false;
            bool          midLoadSoundPlayed = false;

            // MC getTicksUsingItem.
            int TicksUsingItem() const { return usingItem ? kUseDuration - remaining : 0; }
        };

        // MC LivingEntity.startUsingItem(hand): nothing when the hand is
        // empty or the mob is already using an item.
        void StartUsingItem(Mob& mob, UseState& use, EquipmentSlot hand);
        // MC LivingEntity.updatingUsingItem → updateUsingItem: the item's
        // onUseTick at the current remaining count, then one tick off it.
        // Stops the use if the hand no longer holds the item.
        void TickUsingItem(Mob& mob, UseState& use);
        // MC LivingEntity.releaseUsingItem: the crossbow's releaseUsing and —
        // useOnRelease — one more onUseTick (the draw that loads it at full
        // charge), then stopUsingItem.
        void ReleaseUsingItem(Mob& mob, UseState& use);
        // MC LivingEntity.stopUsingItem.
        void StopUsingItem(UseState& use);

        // MC CrossbowAttackMob.performCrossbowAttack(body, power) →
        // CrossbowItem.performShooting(level, body, hand, crossbow, power,
        // rangedAttackUncertainty, getTarget()), then
        // onCrossbowAttackPerformed. `target` may be null (the shot then
        // follows the view vector).
        void PerformCrossbowAttack(Mob& body, CrossbowAttackMob& shooter, LivingEntity* target, float power);

        // MC items/crossbow.json for a mob holding `crossbow`: the charge
        // type's sprite when loaded, the pulling frame while drawing
        // (`useTicks` >= 0 ticks into the draw), else crossbow_standby.
        std::string SpriteFor(const ItemStack& crossbow, int useTicks);

    } // namespace MobCrossbow

} // namespace Game
