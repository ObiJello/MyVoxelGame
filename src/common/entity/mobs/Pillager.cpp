// File: src/common/entity/mobs/Pillager.cpp
//
// See Pillager.hpp. References (minecraft_code_26.3-pre-2/decompiled_net/
// minecraft/): world/entity/monster/illager/Pillager.java, world/entity/ai/
// goal/RangedCrossbowAttackGoal.java, world/entity/raid/Raider.java.
#include "common/entity/mobs/Pillager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/GeneratedMobDefs.hpp"
#include "common/entity/ai/Controls.hpp"
#include "common/entity/ai/Sensing.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RaiderGoals.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/ai/navigation/PathNavigation.hpp"
#include "common/inventory/SimpleContainerOps.hpp"
#include "common/world/enchantment/Enchantment.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"

#include <string_view>

namespace Game {

    namespace {
        // File-scope: the goals keep pointers into these.
        constexpr EntityTypeId kCreakingAvoid[]   = { EntityTypeId::Creaking };
        constexpr EntityTypeId kVillagerTargets[] = { EntityTypeId::Villager,
                                                      EntityTypeId::WanderingTrader };
        constexpr EntityTypeId kIronGolemTargets[] = { EntityTypeId::IronGolem };

        // MC `itemStack.getItem() instanceof BannerItem` — the sixteen
        // colours' banners (standing; the wall banner is no item).
        bool IsBannerItem(ItemID id) {
            const std::string_view slug = ItemRegistry::Slug(id);
            return slug.size() > 7 && slug.substr(slug.size() - 7) == "_banner";
        }
    }

    // ── RangedCrossbowAttackGoal ───────────────────────────────────────────

    RangedCrossbowAttackGoal::RangedCrossbowAttackGoal(Mob* mob, CrossbowAttackMob* shooter,
                                                       double speedModifier, float attackRadius)
        : m_mob(mob), m_shooter(shooter), m_speedModifier(speedModifier),
          m_attackRadiusSqr(attackRadius * attackRadius) {
        SetFlags(GoalFlag::Move | GoalFlag::Look);
    }

    bool RangedCrossbowAttackGoal::IsValidTarget() const {
        const LivingEntity* target = m_mob->GetTarget();
        return target && target->IsAlive();
    }

    bool RangedCrossbowAttackGoal::IsHoldingCrossbow() const {
        return m_mob->IsHoldingItem(Items::Crossbow);
    }

    bool RangedCrossbowAttackGoal::CanUse() {
        return IsValidTarget() && IsHoldingCrossbow();
    }

    bool RangedCrossbowAttackGoal::CanContinueToUse() {
        return IsValidTarget() && (CanUse() || !m_mob->GetNavigation().IsDone()) && IsHoldingCrossbow();
    }

    void RangedCrossbowAttackGoal::Stop() {
        m_mob->SetAggressive(false);
        m_mob->SetTarget(nullptr);
        m_seeTime = 0;
        if (m_use.usingItem) {
            MobCrossbow::StopUsingItem(m_use);
            m_shooter->SetChargingCrossbow(false);
            // MC then sets getUseItem()'s CHARGED_PROJECTILES to EMPTY — but
            // stopUsingItem has already emptied useItem, so the write lands
            // on ItemStack.EMPTY: a crossbow loaded before the stop stays
            // loaded (and fires on the next engagement).
        }
    }

    void RangedCrossbowAttackGoal::Tick() {
        // LivingEntity.tick's updatingUsingItem runs ahead of aiStep: the
        // draw advances before the goal reads it.
        MobCrossbow::TickUsingItem(*m_mob, m_use);

        LivingEntity* target = m_mob->GetTarget();
        if (!target || !m_mob->Level()) return;
        JavaRandom& random = m_mob->Level()->Random();

        const bool hasLineOfSight = m_mob->GetSensing().HasLineOfSight(*target);
        const bool hadLineOfSight = m_seeTime > 0;
        if (hasLineOfSight != hadLineOfSight) m_seeTime = 0;
        if (hasLineOfSight) ++m_seeTime; else --m_seeTime;

        const double distanceToSqr = m_mob->DistanceToSqr(*target);
        const bool needsToMove =
            (distanceToSqr > static_cast<double>(m_attackRadiusSqr) || m_seeTime < 5) && m_attackDelay == 0;
        if (needsToMove) {
            --m_updatePathDelay;
            if (m_updatePathDelay <= 0) {
                m_mob->GetNavigation().MoveTo(*target, CanRun() ? m_speedModifier : m_speedModifier * 0.5);
                // PATHFINDING_DELAY_RANGE = TimeUtil.rangeOfSeconds(1, 2).
                m_updatePathDelay = random.NextInt(20, 40);
            }
        } else {
            m_updatePathDelay = 0;
            m_mob->GetNavigation().Stop();
        }

        m_mob->GetLookControl().SetLookAt(target->position.x, target->GetEyeY(), target->position.z,
                                          30.0f, 30.0f);
        switch (m_crossbowState) {
            case CrossbowState::Uncharged:
                if (!needsToMove) {
                    MobCrossbow::StartUsingItem(*m_mob, m_use,
                                                MobCrossbow::WeaponHoldingHand(*m_mob, Items::Crossbow));
                    m_crossbowState = CrossbowState::Charging;
                    m_shooter->SetChargingCrossbow(true);
                }
                break;
            case CrossbowState::Charging: {
                if (!m_use.usingItem) m_crossbowState = CrossbowState::Uncharged;
                const int pullTime = m_use.TicksUsingItem();
                const ItemStack& useItem = m_mob->GetEquipment(m_use.hand);
                if (pullTime >= MobCrossbow::ChargeDuration(useItem)) {
                    MobCrossbow::ReleaseUsingItem(*m_mob, m_use);
                    m_crossbowState = CrossbowState::Charged;
                    m_attackDelay = 20 + random.NextInt(20);
                    m_shooter->SetChargingCrossbow(false);
                }
                break;
            }
            case CrossbowState::Charged:
                --m_attackDelay;
                if (m_attackDelay == 0) m_crossbowState = CrossbowState::ReadyToAttack;
                break;
            case CrossbowState::ReadyToAttack:
                if (hasLineOfSight) {
                    m_shooter->PerformRangedAttack(*target, 1.0f);
                    m_crossbowState = CrossbowState::Uncharged;
                }
                break;
        }
    }

    // ── Pillager ───────────────────────────────────────────────────────────

    void Pillager::CreateAttributes(AttributeMap& out) {
        CreateMonsterAttributes(out);
        out.Register(Attribute::MovementSpeed, 0.3499999940395355);
        out.Register(Attribute::MaxHealth,    24.0);
        out.Register(Attribute::AttackDamage,  5.0);
        out.Register(Attribute::FollowRange,  32.0);
    }

    Pillager::Pillager(EntityLevel* level) : AbstractIllager(EntityTypeId::Pillager, level) {
        CreateAttributes(m_attributes);
        m_health = GetMaxHealth();
        // The generated def's locomotion numbers, as the generic path this
        // class was promoted out of applied them.
        if (const MobDef* def = FindMobDef(EntityTypeId::Pillager)) {
            SetLandSpeedFactor(def->landSpeedFactor);
            SetWalkAnimParams(def->walkAnimScale, def->walkAnimCap, def->walkAnimFactor, def->walkAnimBabyScale);
        }
        RegisterGoals();
    }

    void Pillager::RegisterGoals() {
        // MC Pillager.registerGoals, priority for priority, after super's
        // (AbstractIllager → Raider → PatrollingMonster: the patrol goal).
        AbstractIllager::RegisterGoals();
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(1, std::make_unique<AvoidEntityGoal>(this, kCreakingAvoid, 1, 8.0f, 1.0, 1.2));
        m_goalSelector.AddGoal(2, std::make_unique<HoldGroundAttackGoal>(this, 10.0f));
        m_goalSelector.AddGoal(3, std::make_unique<RangedCrossbowAttackGoal>(this, this, 1.0, 8.0f));
        m_goalSelector.AddGoal(8, std::make_unique<RandomStrollGoal>(this, 0.6));
        m_goalSelector.AddGoal(9, std::make_unique<LookAtPlayerGoal>(this, 15.0f, 1.0f));
        // MC 10 LookAtPlayerGoal(Mob.class, 15.0F) — the any-mob glance is
        // not modelled (LookAtPlayerGoal targets players only).

        // MC 1 (new HurtByTargetGoal(this, Raider.class)).setAlertOthers():
        // a raider's stray bolt is never answered; the alert wakes the other
        // pillagers (mob.getClass()).
        auto hurtBy = std::make_unique<HurtByTargetGoal>(this);
        hurtBy->SetIgnoreDamageFrom(&Raiders::IsRaider).SetAlertOthers();
        m_targetSelector.AddGoal(1, std::move(hurtBy));
        m_targetSelector.AddGoal(2, std::make_unique<NearestAttackableTargetGoal>(this, /*mustSee=*/true));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kVillagerTargets, 2, /*mustSee=*/false));
        m_targetSelector.AddGoal(3, std::make_unique<NearestAttackableTargetGoal>(
                                        this, kIronGolemTargets, 1, /*mustSee=*/true));
    }

    void Pillager::PerformRangedAttack(LivingEntity& target, float power) {
        (void)target; (void)power;
        // MC performRangedAttack → performCrossbowAttack(this, 1.6F), aimed
        // at getTarget().
        MobCrossbow::PerformCrossbowAttack(*this, *this, GetTarget(), MobCrossbow::kMobArrowPower);
    }

    bool Pillager::CanUseNonMeleeWeapon(const ItemStack& stack) const {
        return stack.itemId == Items::Crossbow;
    }

    std::shared_ptr<SpawnGroupData>
    Pillager::FinalizeSpawn(SpawnReason reason, std::shared_ptr<SpawnGroupData> groupData) {
        if (m_level) {
            JavaRandom& random = m_level->Random();
            const DifficultyInstance difficulty = CurrentDifficulty();
            PopulateDefaultEquipmentSlots(random, difficulty);
            PopulateDefaultEquipmentEnchantments(random, difficulty);
        }
        // Then Raider's (canJoinRaid) and PatrollingMonster's (the 6% leader
        // roll and its ominous banner, the PATROL flag), then Mob's.
        return AbstractIllager::FinalizeSpawn(reason, std::move(groupData));
    }

    void Pillager::PopulateDefaultEquipmentSlots(JavaRandom& random, const DifficultyInstance& difficulty) {
        (void)random; (void)difficulty;
        SetEquipment(EquipmentSlot::MAINHAND, ItemStack(Items::Crossbow, 1));
    }

    void Pillager::EnchantSpawnedWeapon(JavaRandom& random, const DifficultyInstance& difficulty) {
        AbstractIllager::EnchantSpawnedWeapon(random, difficulty);
        if (random.NextInt(300) == 0) {
            ItemStack weapon = GetMainHandEquipment();
            if (weapon.itemId == Items::Crossbow) {
                // enchantment_provider/pillager_spawn_crossbow.json: single
                // minecraft:piercing level 1 (upgraded onto the stack).
                EnchantmentHelper::Enchant(weapon, Enchantments::Piercing, 1);
                ReplaceEquipmentStack(EquipmentSlot::MAINHAND, weapon);
            }
        }
    }

    void Pillager::PickUpItem(int32_t itemEntityId, const ItemStack& stack) {
        // MC Pillager.pickUpItem: a banner → Raider / Mob.pickUpItem (the
        // raid-leader banner branch needs an active raid); wantsItem (the
        // ominous banner during a raid) → the inventory. No raid exists, so
        // only the banner branch can run.
        if (IsBannerItem(stack.itemId)) {
            AbstractIllager::PickUpItem(itemEntityId, stack);
        }
    }

    void Pillager::Tick() {
        AbstractIllager::Tick();
        // The client's use clock for the charge pose / pulling sprite.
        if (m_level && m_level->IsClientSide()) {
            m_clientChargeTicks = m_chargingCrossbow ? m_clientChargeTicks + 1 : -1;
        }
    }

    int Pillager::GetIllagerArmPose() const {
        if (IsChargingCrossbow()) return 5;                       // CROSSBOW_CHARGE
        if (IsHoldingItem(Items::Crossbow)) return 4;             // CROSSBOW_HOLD
        return IsAggressive() ? 1 : 7;                            // ATTACKING : NEUTRAL
    }

} // namespace Game
