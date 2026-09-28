// File: src/common/entity/ai/goals/LlamaGoals.hpp
//
// MC ai/goal/LlamaFollowCaravanGoal — the llama caravan. A llama that is
// neither leashed nor already in a caravan looks for the nearest llama
// (either kind) within 9/4/9 that heads a caravan without a tail yet, else
// the nearest leashed one without a tail, and falls in behind it
// (Llama.joinCaravan) — as long as the chain ahead leads to a leashed llama
// within 8 links. It then paths to two blocks behind its head at 2.1×,
// speeding up by 1.2× (up to three times) whenever it falls more than 26
// blocks behind, and drops out (Llama.leaveCaravan) when its head dies, the
// chain loses its lead, or it cannot catch up. A chain whose lead is tied to
// a fence knot stands still.
#pragma once

#include "common/entity/ai/Goal.hpp"

namespace Game {

    class Llama;

    class LlamaFollowCaravanGoal : public Goal {
    public:
        // MC CARAVAN_LIMIT.
        static constexpr int kCaravanLimit = 8;

        LlamaFollowCaravanGoal(Llama* llama, double speedModifier);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "LlamaFollowCaravanGoal"; }

    private:
        // MC firstIsLeashed: walking the chain of heads from `current`, is
        // one of them (within kCaravanLimit links) on a lead?
        bool FirstIsLeashed(const Llama& current, int counter) const;

        Llama* m_llama;
        double m_speedModifier;
        int    m_distCheckCounter = 0;
    };

} // namespace Game
