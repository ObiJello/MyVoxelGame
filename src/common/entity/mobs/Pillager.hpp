// File: src/common/entity/mobs/Pillager.hpp
//
// MC monster/illager/Pillager (on AbstractIllager / Raider) and the goal that
// fires its crossbow, MC ai/goal/RangedCrossbowAttackGoal.
//
// The pillager spawns holding a crossbow (populateDefaultEquipmentSlots, with
// the 1-in-300 Piercing I of VanillaEnchantmentProviders.PILLAGER_SPAWN_
// CROSSBOW on top of the ordinary spawn enchantment roll), draws it over the
// crossbow's charge time, holds the loaded shot for 20..39 ticks and fires
// with MobCrossbow::PerformCrossbowAttack. A pillager that loses its
// crossbow has no melee goal — as in MC, it only stands and stares.
//
// It sits on the raider chain (common/entity/raid/Raider.hpp): patrols —
// LongDistancePatrolGoal from PatrollingMonster, HoldGroundAttackGoal at 2,
// the 6% patrol-leader roll with the ominous banner, the captain's ominous
// bottle (loot_table/entities/pillager.json, MobManager), PatrolSpawner's
// patrols and the pillager outpost's spawn override. The raid half is
// absent with raids (Raider.hpp): the raid goals, applyRaidBuffs crossbows
// and the raid banner the inventory wants (wantsItem). The five-slot
// inventory itself is kept and saved ("Inventory").
#pragma once

#include "common/entity/raid/Raider.hpp"
#include "common/entity/MobCrossbow.hpp"
#include "common/entity/ai/Goal.hpp"
#include "common/inventory/SimpleContainer.hpp"

namespace Game {

    // MC RangedCrossbowAttackGoal<T extends Monster & RangedAttackMob &
    // CrossbowAttackMob>: close in until within the attack radius with 5
    // ticks of sight, draw, hold 20..39 ticks, fire when in sight.
    class RangedCrossbowAttackGoal : public Goal {
    public:
        RangedCrossbowAttackGoal(Mob* mob, CrossbowAttackMob* shooter, double speedModifier,
                                 float attackRadius);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "RangedCrossbowAttackGoal"; }

    private:
        enum class CrossbowState : uint8_t { Uncharged, Charging, Charged, ReadyToAttack };

        bool IsValidTarget() const;
        bool IsHoldingCrossbow() const;
        bool CanRun() const { return m_crossbowState == CrossbowState::Uncharged; }

        Mob*               m_mob;
        CrossbowAttackMob* m_shooter;
        double m_speedModifier;
        float  m_attackRadiusSqr;
        CrossbowState m_crossbowState = CrossbowState::Uncharged;
        int m_seeTime = 0;
        int m_attackDelay = 0;
        int m_updatePathDelay = 0;
        MobCrossbow::UseState m_use;
    };

    class Pillager : public AbstractIllager, public CrossbowAttackMob {
    public:
        static constexpr int kInventorySize = 5;   // MC Pillager.INVENTORY_SIZE

        explicit Pillager(EntityLevel* level);

        // MC Pillager.createAttributes: MOVEMENT_SPEED 0.35, MAX_HEALTH 24,
        // ATTACK_DAMAGE 5, FOLLOW_RANGE 32 on the monster base.
        static void CreateAttributes(AttributeMap& out);

        // ── CrossbowAttackMob ──────────────────────────────────────────────
        void SetChargingCrossbow(bool charging) override { m_chargingCrossbow = charging; }
        bool IsChargingCrossbow() const override { return m_chargingCrossbow; }
        // MC onCrossbowAttackPerformed: noActionTime = 0.
        void OnCrossbowAttackPerformed() override { ResetNoActionTime(); }
        // MC performRangedAttack → performCrossbowAttack(this, 1.6).
        void PerformRangedAttack(LivingEntity& target, float power) override;

        // MC Pillager.canUseNonMeleeWeapon: the crossbow.
        bool CanUseNonMeleeWeapon(const ItemStack& stack) const override;
        // MC ItemTags.PILLAGER_PREFERRED_WEAPONS.
        const char* GetPreferredWeaponType() const override { return "minecraft:pillager_preferred_weapons"; }

        // MC getMaxSpawnClusterSize: a lone pillager outside patrols.
        int GetMaxSpawnClusterSize() const override { return 1; }
        // MC Pillager.getWalkTargetValue: 0 everywhere — it has no light
        // preference to stroll toward.
        float GetWalkTargetValue(const glm::ivec3& pos) const override { (void)pos; return 0.0f; }

        // MC Pillager.finalizeSpawn: the crossbow and its enchantment roll,
        // then Raider / Mob.
        std::shared_ptr<SpawnGroupData>
        FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) override;

        // MC Pillager.pickUpItem: a banner goes through Mob's equip; the raid
        // banner it wants (wantsItem: only during an active raid — none
        // here) would go to the inventory; anything else is left lying.
        void PickUpItem(int32_t itemEntityId, const ItemStack& stack) override;

        // MC InventoryCarrier — the pillager's five pockets ("Inventory").
        SimpleContainer&       GetInventory()       { return m_inventory; }
        const SimpleContainer& GetInventory() const { return m_inventory; }

        // MC IS_CHARGING_CROSSBOW rides bit 0 of the anim byte.
        uint8_t GetAnimStateByte() const override { return m_chargingCrossbow ? 1 : 0; }
        void    SetAnimStateByte(uint8_t v) override { m_chargingCrossbow = (v & 1) != 0; }

        // The client's crossbow-draw clock (CrossbowAttackMob).
        int GetClientChargeTicks() const override { return m_clientChargeTicks; }

        void Tick() override;

        // MC Pillager.getArmPose ordinals (AbstractIllager.IllagerArmPose):
        // CROSSBOW_CHARGE while charging, CROSSBOW_HOLD holding a crossbow,
        // ATTACKING while aggressive, else NEUTRAL.
        int GetIllagerArmPose() const;

    protected:
        void RegisterGoals() override;
        // MC Pillager.populateDefaultEquipmentSlots: the crossbow.
        void PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) override;
        // MC Pillager.enchantSpawnedWeapon: Mob's roll, then 1 in 300 a
        // crossbow gets PILLAGER_SPAWN_CROSSBOW (Piercing I).
        void EnchantSpawnedWeapon(JavaRandom& random, const DifficultyInstance& difficulty) override;

    private:
        SimpleContainer m_inventory{ kInventorySize };
        bool m_chargingCrossbow = false;
        int  m_clientChargeTicks = -1;
    };

} // namespace Game
