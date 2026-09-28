// File: src/common/entity/ai/goals/TraderGoals.hpp
//
// The goals the wandering trader and its llamas register that no other mob
// in this port uses:
//
//   TradeWithPlayerGoal       MC ai/goal/TradeWithPlayerGoal
//   LookAtTradingPlayerGoal   MC ai/goal/LookAtTradingPlayerGoal
//   LookAtMobGoal             MC LookAtPlayerGoal(mob, Mob.class, distance) —
//                             the engine's LookAtPlayerGoal is the Player.class
//                             form only
//   TraderUseItemGoal         MC ai/goal/UseItemGoal<WanderingTrader>
//   WanderToPositionGoal      MC WanderingTrader.WanderToPositionGoal
//   TraderLlamaDefendWanderingTraderGoal
//                             MC TraderLlama.TraderLlamaDefendWanderingTraderGoal
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/entity/ai/TargetingConditions.hpp"
#include "common/entity/ai/goals/TargetGoals.hpp"
#include "common/entity/npc/WanderingTrader.hpp"

#include <cstdint>

namespace Game {

    class AbstractVillager;
    class LivingEntity;
    class Llama;
    class Mob;

    // MC TradeWithPlayerGoal: while a player trades, stand still (JUMP|MOVE)
    // — until the partner walks more than 4 blocks off, the trader is hurt,
    // leaves the ground or gets wet; then the trade ends (setTradingPlayer
    // null, which closes the screen through MerchantMenu.stillValid).
    class TradeWithPlayerGoal : public Goal {
    public:
        explicit TradeWithPlayerGoal(AbstractVillager* mob);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "TradeWithPlayerGoal"; }

    private:
        AbstractVillager* m_mob;
    };

    // MC LookAtTradingPlayerGoal (a LookAtPlayerGoal at 8 blocks): while
    // trading, the look target IS the partner.
    class LookAtTradingPlayerGoal : public Goal {
    public:
        explicit LookAtTradingPlayerGoal(AbstractVillager* mob);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "LookAtTradingPlayerGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        AbstractVillager* m_mob;
        LivingEntity*     m_lookAt = nullptr;
        float             m_lookDistance = 8.0f;
        int               m_lookTime = 0;
    };

    // MC LookAtPlayerGoal with lookAtType = Mob.class: the nearest MOB in the
    // mob's box inflated (distance, 3, distance) that the non-combat
    // conditions (range, line of sight) accept, on MC's 2% roll.
    class LookAtMobGoal : public Goal {
    public:
        LookAtMobGoal(Mob* mob, float lookDistance, float probability = 0.02f);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "LookAtMobGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Mob*          m_mob;
        LivingEntity* m_lookAt = nullptr;
        float         m_lookDistance;
        float         m_probability;
        int           m_lookTime = 0;
        TargetingConditions m_conditions;
    };

    // MC UseItemGoal<WanderingTrader>(mob, item, finishUsingSound,
    // canUseSelector): put the item in the main hand and start using it; when
    // the use completes (MC canContinueToUse = isUsingItem), empty the hand
    // and play the finish sound at pitch 0.9..1.1.
    class TraderUseItemGoal : public Goal {
    public:
        using Selector = bool (*)(const WanderingTrader&);

        TraderUseItemGoal(WanderingTrader* mob, WanderingTrader::HeldItem item,
                          const char* finishUsingSound, Selector canUseSelector);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "UseItemGoal"; }

    private:
        WanderingTrader*          m_mob;
        WanderingTrader::HeldItem m_item;
        const char*               m_finishUsingSound;
        Selector                  m_canUseSelector;
    };

    // MC WanderingTrader.WanderToPositionGoal(trader, stopDistance 2,
    // speedModifier 0.35): head for the wander target in 10-block legs until
    // within `stopDistance` of its centre; the target is forgotten when the
    // goal stops (so a trader sent to a bell goes there once).
    class WanderToPositionGoal : public Goal {
    public:
        WanderToPositionGoal(WanderingTrader* trader, double stopDistance, double speedModifier);

        bool CanUse() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "WanderToPositionGoal"; }

    private:
        bool IsTooFarAway(const glm::ivec3& pos, double distance) const;

        WanderingTrader* m_trader;
        double           m_stopDistance;
        double           m_speedModifier;
    };

    // MC TraderLlama.TraderLlamaDefendWanderingTraderGoal: a llama on a
    // wandering trader's lead targets whatever last hurt the trader (once
    // per hurt timestamp) — and its ranged goal spits at it.
    class TraderLlamaDefendWanderingTraderGoal : public TargetGoal {
    public:
        explicit TraderLlamaDefendWanderingTraderGoal(Llama* llama);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "TraderLlamaDefendWanderingTraderGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Llama*        m_llama;
        LivingEntity* m_ownerLastHurtBy = nullptr;
        int64_t       m_timestamp = 0;
    };

} // namespace Game
