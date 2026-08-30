// File: src/common/entity/ai/goals/DolphinGoals.hpp
//
// MC animal/dolphin/Dolphin.java's goal set — the top-level DolphinJumpGoal
// and TryFindWaterGoal ported whole, plus the nested goals whose systems
// (air supply, structures-as-treasure, boats, mob-held items, the
// DOLPHINS_GRACE effect) this port does not have, each declared with the
// gate that keeps it honest.
#pragma once

#include "common/entity/ai/goals/BasicGoals.hpp"

#include <glm/glm.hpp>

namespace Game {

    class Dolphin;

    // MC ai/goal/DolphinJumpGoal — the breach: 1-in-10 polls, only when the
    // water ahead is clear for the sampled steps {0,1,4,5,6,7} with two air
    // blocks above each, then a (dir*0.6, 0.7) leap with the body pitched
    // along the arc and levelled on re-entry.
    class DolphinJumpGoal : public Goal {
    public:
        DolphinJumpGoal(Dolphin* dolphin, int interval);
        bool CanUse() override;
        bool CanContinueToUse() override;
        bool IsInterruptable() const override { return false; }
        void Start() override;
        void Stop() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "DolphinJumpGoal"; }

    private:
        bool WaterIsClear(const glm::ivec3& pos, int stepX, int stepZ, int step) const;
        bool SurfaceIsClear(const glm::ivec3& pos, int stepX, int stepZ, int step) const;

        Dolphin* m_dolphin;
        int      m_interval;
        bool     m_breached = false;
    };

    // MC ai/goal/TryFindWaterGoal — a grounded, dry dolphin paths to the
    // closest water surface within 2 blocks around its feet.
    class TryFindWaterGoal : public Goal {
    public:
        explicit TryFindWaterGoal(Dolphin* dolphin);
        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "TryFindWaterGoal"; }

    private:
        Dolphin* m_dolphin;
    };

    // MC ai/goal/BreathAirGoal — surface when air runs low (below 140 of the
    // dolphin's 4800), un-interruptable, steering straight up at the nearest
    // breathable block while also swimming there under its own power.
    class BreathAirGoal : public Goal {
    public:
        explicit BreathAirGoal(Dolphin* dolphin) : m_dolphin(dolphin) {
            SetFlags(GoalFlag::Move | GoalFlag::Look);
        }
        bool CanUse() override;
        bool CanContinueToUse() override { return CanUse(); }
        bool IsInterruptable() const override { return false; }
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "BreathAirGoal"; }

    private:
        void FindAirPosition();
        bool GivesAir(const glm::ivec3& pos) const;

        Dolphin* m_dolphin;
    };

    // MC Dolphin.DolphinSwimToTreasureGoal — gated on gotFish (a fed
    // dolphin), then findNearestMapStructure(DOLPHIN_LOCATED). No structure
    // registry exists; MC's own start() marks the goal stuck when no
    // structure is found, and stop() then clears gotFish — which is exactly
    // what this port runs, so a fed dolphin shrugs once and moves on.
    class DolphinSwimToTreasureGoal : public Goal {
    public:
        explicit DolphinSwimToTreasureGoal(Dolphin* dolphin);
        bool CanUse() override;
        bool CanContinueToUse() override { return false; }
        bool IsInterruptable() const override { return false; }
        void Start() override;
        void Stop() override;
        const char* Name() const override { return "DolphinSwimToTreasureGoal"; }

    private:
        Dolphin* m_dolphin;
    };

    // MC Dolphin.DolphinSwimWithPlayerGoal — escort a SWIMMING player and
    // grant DOLPHINS_GRACE. Neither the player swim pose nor the effect
    // exists in this port; MC's own `player.isSwimming()` gate never opens.
    class DolphinSwimWithPlayerGoal : public Goal {
    public:
        DolphinSwimWithPlayerGoal(Dolphin* dolphin, double speedModifier)
            : m_dolphin(dolphin) { (void)speedModifier; }
        bool CanUse() override { return false; }
        const char* Name() const override { return "DolphinSwimWithPlayerGoal"; }

    private:
        Dolphin* m_dolphin;
    };

    // MC Dolphin.PlayWithItemsGoal — toss floating ItemEntities around. Mob
    // item pickup does not exist in this port, so the goal is inert with it.
    class PlayWithItemsGoal : public Goal {
    public:
        explicit PlayWithItemsGoal(Dolphin* dolphin) : m_dolphin(dolphin) {}
        bool CanUse() override { return false; }
        const char* Name() const override { return "PlayWithItemsGoal"; }

    private:
        Dolphin* m_dolphin;
    };

    // MC ai/goal/FollowPlayerRiddenEntityGoal — swim alongside a
    // player-ridden boat (or nautilus). No rideable vehicles exist here.
    class FollowPlayerRiddenEntityGoal : public Goal {
    public:
        explicit FollowPlayerRiddenEntityGoal(Dolphin* dolphin) : m_dolphin(dolphin) {}
        bool CanUse() override { return false; }
        const char* Name() const override { return "FollowPlayerRiddenEntityGoal"; }

    private:
        Dolphin* m_dolphin;
    };

} // namespace Game
