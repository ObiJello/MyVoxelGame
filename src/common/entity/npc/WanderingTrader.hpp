// File: src/common/entity/npc/WanderingTrader.hpp
//
// MC net.minecraft.world.entity.npc.wanderingtrader.WanderingTrader.
//
// ── What a wandering trader is ───────────────────────────────────────────────
// An AbstractVillager with a GOAL set instead of a brain (MC registerGoals):
// it drinks a potion of invisibility at dusk and a bucket of milk at dawn
// (UseItemGoal ×2), stops to trade (TradeWithPlayerGoal), runs from zombies,
// illagers, vexes and zoglins (AvoidEntityGoal ×7), panics when hurt, walks
// toward the spot it was sent to (WanderToPositionGoal, `wander_target`),
// stays near its home (MoveTowardsRestrictionGoal, the 16-block home the
// spawner gives it) and otherwise strolls and looks around.
//
// Its offers are rolled once from the three 26.3 trade sets — two BUYING
// offers, two UNCOMMON and five COMMON (data/minecraft/trade_set/
// wandering_trader/) — and never restock. It pays 3..6 XP per trade to the
// player and has no XP bar or level of its own.
//
// The spawner (server/level/WanderingTraderSpawner) hands it a DespawnDelay
// of 48000 ticks; it counts down while the trader is not trading and the
// trader vanishes at zero. A /summon'd or spawn-egg trader has 0 — never.
//
// ── What the client sees ─────────────────────────────────────────────────────
// MC shows the main-hand item across the crossed arms (CrossedArmsItemLayer).
// The only items a trader ever holds are the two UseItemGoal ones: they sit
// in the Mob MAINHAND equipment slot (synced, saved and dropped like any
// mob's), and the drink state also rides the anim-state byte (HeldItem
// below) for the drinking clock — visible even while the trader itself is
// invisible, as in MC (the famous floating milk bucket).
#pragma once

#include "common/entity/npc/Villager.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Game {

    class WanderingTrader : public AbstractVillager {
    public:
        // MC WanderingTraderSpawner.spawn's setDespawnDelay(48000).
        static constexpr int kSpawnedDespawnDelay = 48000;
        // MC Consumables.defaultDrink: consumeSeconds 1.6 → 32 ticks.
        static constexpr int kDrinkDuration = 32;

        // The main-hand item (MC setItemSlot(MAINHAND) by UseItemGoal). Also
        // the anim-state byte on the wire.
        enum class HeldItem : uint8_t { None = 0, InvisibilityPotion = 1, MilkBucket = 2 };

        explicit WanderingTrader(EntityLevel* level);

        // ── Despawn timer (MC despawnDelay, saved as "DespawnDelay") ─────
        int  GetDespawnDelay() const { return m_despawnDelay; }
        void SetDespawnDelay(int ticks) { m_despawnDelay = ticks; }

        // ── Wander target (MC wanderTarget, saved as "wander_target") ────
        const std::optional<glm::ivec3>& GetWanderTarget() const { return m_wanderTarget; }
        void SetWanderTarget(std::optional<glm::ivec3> pos) { m_wanderTarget = pos; }

        // ── Using an item (MC LivingEntity.startUsingItem / updatingUsingItem
        //    / completeUsingItem, for the two drinks) ──────────────────────
        HeldItem GetHeldItem() const { return m_heldItem; }
        // UseItemGoal's setItemSlot(MAINHAND, item) / (MAINHAND, EMPTY):
        // the drink state and, server side, the MAINHAND equipment stack.
        void     SetHeldItem(HeldItem item);
        // The stack a HeldItem stands for (empty for None).
        static ItemStack HeldItemStack(HeldItem item);
        // MC startUsingItem(MAIN_HAND) — the drink's 32-tick clock starts.
        void StartUsingItem();
        bool IsUsingItem() const { return m_useItemRemaining > 0; }
        int  GetUseItemRemainingTicks() const { return m_useItemRemaining; }

        // MC Level.isDarkOutside / isBrightOutside — skyDarken against 4, and
        // never in a dimension with a fixed clock.
        bool IsDarkOutside() const;
        bool IsBrightOutside() const;
        // MC Entity.isInvisible — the INVISIBILITY effect's shared flag.
        bool IsInvisibleNow() const { return IsEffectInvisible(); }

        // ── Merchant ─────────────────────────────────────────────────────
        bool ShowProgressBar() const override { return false; }
        const char* GetNotifyTradeSound() const override { return "entity.wandering_trader.yes"; }
        const char* GetTradeUpdatedSound(bool validTrade) const override {
            return validTrade ? "entity.wandering_trader.yes" : "entity.wandering_trader.no";
        }

        // ── Mob hooks ────────────────────────────────────────────────────
        UseResult MobInteract(LivingEntity& player, ItemStack& held) override;
        void AiStep() override;
        bool RemoveWhenFarAway(double) const override { return false; }
        const char* GetAmbientSound() const override;
        const char* GetHurtSound(MobDamageSource) const override { return "entity.wandering_trader.hurt"; }
        const char* GetDeathSound() const override { return "entity.wandering_trader.death"; }
        uint8_t GetAnimStateByte() const override { return static_cast<uint8_t>(m_heldItem); }
        void    SetAnimStateByte(uint8_t v) override {
            m_heldItem = v <= 2 ? static_cast<HeldItem>(v) : HeldItem::None;
        }

    protected:
        void RegisterGoals() override;
        void RewardTradeXp(const MerchantOffer& offer) override;
        void UpdateTrades() override;

    private:
        // MC updatingUsingItem → updateUsingItem (server).
        void UpdateUsingItem();
        // MC completeUsingItem → Consumable.onConsume: the last drink sound,
        // then the potion's effects / milk's ClearAllStatusEffects.
        void CompleteUsingItem();
        // MC Consumable.emitParticlesAndSounds for a DRINK (no particles):
        // the trader's own drink sound (Consumable.OverrideConsumeSound),
        // volume 0.5, pitch 0.9..1.0.
        void EmitDrinkSound();
        // MC maybeDespawn.
        void MaybeDespawn();

        std::optional<glm::ivec3> m_wanderTarget;
        int      m_despawnDelay = 0;        // MC DEFAULT_DESPAWN_DELAY
        HeldItem m_heldItem = HeldItem::None;
        int      m_useItemRemaining = 0;
    };

} // namespace Game
