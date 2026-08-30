// File: src/common/entity/ai/goals/EndermanGoals.hpp
//
// MC EnderMan's three nested goals. They live here rather than inside the
// Enderman class because every goal class in this port does; the behaviour is
// EnderMan.java's, verbatim:
//
//   EndermanFreezeWhenLookedAt   stop dead while a player within 16 blocks is
//                                staring (claims MOVE+JUMP, so nothing else
//                                moves the mob), and stare back.
//   EndermanLookForPlayerGoal    the aggro machine: a stare within follow
//                                range arms a 5-tick fuse, then the player
//                                becomes the target. While targeted: a stare
//                                inside 4 blocks teleports the enderman away;
//                                a target beyond 16 blocks for 30 ticks pulls
//                                the enderman TOWARD it by teleport.
//   EndermanLeaveBlockGoal /     the griefing pair, gated on mobGriefing:
//   EndermanTakeBlockGoal        1-in-20 per tick to grab an ENDERMAN_HOLDABLE
//                                block in a 4x3x4 box, 1-in-2000 to put the
//                                carried one down where it can survive.
#pragma once

#include "common/entity/ai/Goal.hpp"

namespace Game {

    class Enderman;
    class LivingEntity;

    class EndermanFreezeWhenLookedAt : public Goal {
    public:
        explicit EndermanFreezeWhenLookedAt(Enderman* enderman);

        bool CanUse() override;
        void Start() override;
        void Tick() override;
        const char* Name() const override { return "EndermanFreezeWhenLookedAt"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Enderman*     m_enderman;
        LivingEntity* m_target = nullptr;
    };

    class EndermanLookForPlayerGoal : public Goal {
    public:
        explicit EndermanLookForPlayerGoal(Enderman* enderman);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "EndermanLookForPlayerGoal"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        Enderman*     m_enderman;
        LivingEntity* m_pendingTarget = nullptr;
        LivingEntity* m_target = nullptr;
        int m_aggroTime = 0;
        int m_teleportTime = 0;
    };

    class EndermanLeaveBlockGoal : public Goal {
    public:
        explicit EndermanLeaveBlockGoal(Enderman* enderman) : m_enderman(enderman) {}

        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "EndermanLeaveBlockGoal"; }

    private:
        Enderman* m_enderman;
    };

    class EndermanTakeBlockGoal : public Goal {
    public:
        explicit EndermanTakeBlockGoal(Enderman* enderman) : m_enderman(enderman) {}

        bool CanUse() override;
        void Tick() override;
        const char* Name() const override { return "EndermanTakeBlockGoal"; }

    private:
        Enderman* m_enderman;
    };

} // namespace Game
