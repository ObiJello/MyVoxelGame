// File: src/common/entity/npc/WanderingTrader.cpp
#include "common/entity/npc/WanderingTrader.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/alchemy/Potions.hpp"
#include "common/entity/ai/goals/AttackGoals.hpp"
#include "common/entity/ai/goals/BasicGoals.hpp"
#include "common/entity/ai/goals/RestrictionGoals.hpp"
#include "common/entity/ai/goals/TraderGoals.hpp"
#include "common/sound/SoundEvents.hpp"
#include "common/world/level/DimensionId.hpp"

#include <iterator>
#include <memory>

namespace Game {

    namespace {

        // The Class<T> each AvoidEntityGoal names, flattened through MC's
        // hierarchy (getEntitiesOfClass matches subclasses): Zombie.class is
        // the zombie, the husk, the drowned, the zombie villager and the
        // zombified piglin; the others have no subclasses.
        constexpr EntityTypeId kAvoidZombie[] = {
            EntityTypeId::Zombie, EntityTypeId::Husk, EntityTypeId::Drowned,
            EntityTypeId::ZombieVillager, EntityTypeId::ZombifiedPiglin,
        };
        constexpr EntityTypeId kAvoidEvoker[]     = { EntityTypeId::Evoker };
        constexpr EntityTypeId kAvoidVindicator[] = { EntityTypeId::Vindicator };
        constexpr EntityTypeId kAvoidVex[]        = { EntityTypeId::Vex };
        constexpr EntityTypeId kAvoidPillager[]   = { EntityTypeId::Pillager };
        constexpr EntityTypeId kAvoidIllusioner[] = { EntityTypeId::Illusioner };
        constexpr EntityTypeId kAvoidZoglin[]     = { EntityTypeId::Zoglin };

        template <size_t N>
        std::unique_ptr<AvoidEntityGoal> Avoid(PathfinderMob* mob, const EntityTypeId (&types)[N],
                                               float maxDistance) {
            // MC AvoidEntityGoal(this, X.class, maxDist, 0.5, 0.5).
            return std::make_unique<AvoidEntityGoal>(mob, types, static_cast<int>(N),
                                                     maxDistance, 0.5, 0.5);
        }

        // MC DimensionType.hasFixedTime: the Nether and the End in vanilla,
        // plus the engine's dimensions that pin their clock.
        bool HasFixedTime(DimensionId d) {
            return d == DimensionId::Nether || d == DimensionId::End ||
                   DimensionFixedTime(d).has_value();
        }

        // The UseItemGoal selectors (MC's lambdas in registerGoals).
        bool WantsInvisibility(const WanderingTrader& t) {
            return t.IsDarkOutside() && !t.IsInvisibleNow();
        }
        bool WantsMilk(const WanderingTrader& t) {
            return t.IsBrightOutside() && t.IsInvisibleNow();
        }

    } // namespace

    WanderingTrader::WanderingTrader(EntityLevel* level)
        : AbstractVillager(EntityTypeId::WanderingTrader, level) {
        // The AbstractVillager constructor cleared the generic set; the
        // trader's own goals are registered HERE, from the most-derived
        // constructor, where the virtual resolves to this class.
        RegisterGoals();
    }

    void WanderingTrader::RegisterGoals() {
        // MC WanderingTrader.registerGoals, priority for priority.
        m_goalSelector.AddGoal(0, std::make_unique<FloatGoal>(this));
        m_goalSelector.AddGoal(0, std::make_unique<TraderUseItemGoal>(
                                      this, HeldItem::InvisibilityPotion,
                                      SoundEvents::WANDERING_TRADER_DISAPPEARED, &WantsInvisibility));
        m_goalSelector.AddGoal(0, std::make_unique<TraderUseItemGoal>(
                                      this, HeldItem::MilkBucket,
                                      SoundEvents::WANDERING_TRADER_REAPPEARED, &WantsMilk));
        m_goalSelector.AddGoal(1, std::make_unique<TradeWithPlayerGoal>(this));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidZombie, 8.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidEvoker, 12.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidVindicator, 8.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidVex, 8.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidPillager, 15.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidIllusioner, 12.0f));
        m_goalSelector.AddGoal(1, Avoid(this, kAvoidZoglin, 10.0f));
        m_goalSelector.AddGoal(1, std::make_unique<PanicGoal>(this, 0.5));
        m_goalSelector.AddGoal(1, std::make_unique<LookAtTradingPlayerGoal>(this));
        m_goalSelector.AddGoal(2, std::make_unique<WanderToPositionGoal>(this, 2.0, 0.35));
        m_goalSelector.AddGoal(4, std::make_unique<MoveTowardsRestrictionGoal>(this, 0.35));
        m_goalSelector.AddGoal(8, std::make_unique<WaterAvoidingRandomStrollGoal>(this, 0.35));
        // MC InteractGoal(this, Player.class, 3.0F, 1.0F): LookAtPlayerGoal at
        // probability 1 that also claims MOVE — the trader stops to face a
        // player standing within three blocks.
        {
            auto interact = std::make_unique<LookAtPlayerGoal>(this, 3.0f, 1.0f);
            interact->SetFlags(GoalFlag::Look | GoalFlag::Move);
            m_goalSelector.AddGoal(9, std::move(interact));
        }
        m_goalSelector.AddGoal(10, std::make_unique<LookAtMobGoal>(this, 8.0f));
    }

    // ── Time of day ──────────────────────────────────────────────────────

    bool WanderingTrader::IsBrightOutside() const {
        if (!m_level || HasFixedTime(m_level->Dimension())) return false;
        return m_level->GetSkyDarken() < 4;
    }

    bool WanderingTrader::IsDarkOutside() const {
        if (!m_level || HasFixedTime(m_level->Dimension())) return false;
        return !IsBrightOutside();
    }

    // ── Interaction ──────────────────────────────────────────────────────

    UseResult WanderingTrader::MobInteract(LivingEntity& player, ItemStack& held) {
        // MC WanderingTrader.mobInteract. The spawn-egg test names the
        // VILLAGER egg, as MC's does. (Stats.TALKED_TO_VILLAGER — no
        // statistics.)
        if (held.itemId != Items::VillagerSpawnEgg && IsAlive() && !IsTrading() && !IsBaby()) {
            if (m_level && !m_level->IsClientSide()) {
                // No offers: the click is consumed and nothing opens — the
                // trader has no head-shake (that is Villager.setUnhappy).
                if (GetOffers().empty()) return UseResult::Consume;
                SetTradingPlayer(&player);
                m_level->OpenMerchantMenu(player, *this);
            }
            return UseResult::Success;
        }
        return AbstractVillager::MobInteract(player, held);
    }

    // ── Trading ──────────────────────────────────────────────────────────

    void WanderingTrader::UpdateTrades() {
        // MC updateTrades: TradeSets.WANDERING_TRADER_BUYING, _UNCOMMON,
        // _COMMON — two, two and five offers, in that order on the screen.
        if (!m_offers) return;
        AddOffersFromTradeSet("minecraft:wandering_trader/buying", *m_offers, std::nullopt);
        AddOffersFromTradeSet("minecraft:wandering_trader/uncommon", *m_offers, std::nullopt);
        AddOffersFromTradeSet("minecraft:wandering_trader/common", *m_offers, std::nullopt);
        BumpOffersRevision();
    }

    void WanderingTrader::RewardTradeXp(const MerchantOffer& offer) {
        // MC rewardTradeXp: 3..6 XP at the trader's feet + 0.5, only for an
        // offer that rewards XP. The trader itself gains nothing.
        if (!m_level || m_level->IsClientSide() || !offer.ShouldRewardExp()) return;
        const int popXp = 3 + m_level->Random().NextInt(4);
        m_level->AwardExperience(position + glm::dvec3(0.0, 0.5, 0.0), popXp, m_tradingPlayerId);
    }

    // ── Sounds ───────────────────────────────────────────────────────────

    const char* WanderingTrader::GetAmbientSound() const {
        return IsTrading() ? SoundEvents::WANDERING_TRADER_TRADE : SoundEvents::WANDERING_TRADER_AMBIENT;
    }

    void WanderingTrader::EmitDrinkSound() {
        if (!m_level) return;
        // MC Consumable.emitParticlesAndSounds: a DRINK plays at volume 0.5,
        // pitch Mth.randomBetween(random, 0.9, 1.0); the trader overrides the
        // sound itself (getConsumeSound).
        JavaRandom& random = m_level->Random();
        const float pitch = 0.9f + random.NextFloat() * (1.0f - 0.9f);
        PlaySound(m_heldItem == HeldItem::MilkBucket ? SoundEvents::WANDERING_TRADER_DRINK_MILK
                                                     : SoundEvents::WANDERING_TRADER_DRINK_POTION,
                  0.5f, pitch);
    }

    // ── Using an item ────────────────────────────────────────────────────

    void WanderingTrader::StartUsingItem() {
        // MC LivingEntity.startUsingItem: the use clock is the item's use
        // duration (Consumable.consumeTicks — 32 for both drinks).
        if (m_heldItem == HeldItem::None) return;
        m_useItemRemaining = kDrinkDuration;
    }

    void WanderingTrader::UpdateUsingItem() {
        if (m_useItemRemaining <= 0) return;
        if (m_heldItem == HeldItem::None) {   // MC: the hand changed → stopUsingItem
            m_useItemRemaining = 0;
            return;
        }
        // MC Consumable.shouldEmitParticlesAndSounds(remaining): past the
        // first 21.875% of the drink, every fourth tick of the countdown.
        const int usedFor = kDrinkDuration - m_useItemRemaining;
        const int waitTicks = static_cast<int>(static_cast<float>(kDrinkDuration) * 0.21875f);
        if (usedFor > waitTicks && m_useItemRemaining % 4 == 0) EmitDrinkSound();
        if (--m_useItemRemaining == 0) CompleteUsingItem();
    }

    void WanderingTrader::CompleteUsingItem() {
        // MC Consumable.onConsume: the sound (particles off for drinks), then
        // the potion's ConsumableListener / the milk's onConsumeEffects.
        EmitDrinkSound();
        if (m_heldItem == HeldItem::InvisibilityPotion) {
            // Potions.INVISIBILITY: invisibility, 3600 ticks.
            PotionContents(PotionId::Invisibility).ApplyToLivingEntity(*this, 1.0f);
        } else if (m_heldItem == HeldItem::MilkBucket) {
            // ClearAllStatusEffectsConsumeEffect.
            RemoveAllEffects();
        }
        // MC finishUsingItem leaves the bottle / bucket in the hand and
        // stopUsingItem ends the use; UseItemGoal.stop empties the hand.
        m_useItemRemaining = 0;
    }

    // ── Life ─────────────────────────────────────────────────────────────

    void WanderingTrader::AiStep() {
        // MC LivingEntity.tick's updatingUsingItem runs before aiStep (and so
        // before this tick's goals) — server-side here: the sounds and the
        // effects are the server's, the client only sees the held item.
        if (m_level && !m_level->IsClientSide()) UpdateUsingItem();
        AbstractVillager::AiStep();
        if (m_level && !m_level->IsClientSide()) MaybeDespawn();
    }

    void WanderingTrader::MaybeDespawn() {
        // MC maybeDespawn: a timer that pauses while trading; 0 = never.
        if (m_despawnDelay > 0 && !IsTrading() && --m_despawnDelay == 0) Discard();
    }

    ItemStack WanderingTrader::HeldItemStack(HeldItem item) {
        switch (item) {
            case HeldItem::MilkBucket:         return ItemStack(Items::MilkBucket, 1);
            case HeldItem::InvisibilityPotion: return CreatePotionItemStack(Items::Potion, PotionId::Invisibility);
            default:                           return ItemStack{};
        }
    }

    void WanderingTrader::SetHeldItem(HeldItem item) {
        m_heldItem = item;
        if (item == HeldItem::None) m_useItemRemaining = 0;
        // MC setItemSlot(MAINHAND, ...) — the equipment the tracker syncs and
        // Mob.dropCustomDeathLoot drops at the default 8.5% hand chance.
        if (m_level && !m_level->IsClientSide()) SetEquipment(EquipmentSlot::MAINHAND, HeldItemStack(item));
    }

} // namespace Game
