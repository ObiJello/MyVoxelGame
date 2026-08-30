// File: src/common/entity/ai/goals/MoveToBlockGoal.hpp
//
// MC MoveToBlockGoal + RemoveBlockGoal + Zombie.ZombieAttackTurtleEggGoal.
//
// MoveToBlockGoal is MC's "walk to a block that satisfies a predicate" base:
// an expanding-ring scan around the mob (nearest-first, which is why mobs
// pick the CLOSEST valid block, not a random one), a 200-400 tick retry
// timer between scans, and a 1200-tick give-up budget once walking.
//
// RemoveBlockGoal turns "reached it" into a 3-second stomp-and-break, gated
// on the mobGriefing rule. The zombie's turtle-egg attack is that with egg
// sounds and a slightly longer reach.
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/world/block/Blocks.hpp"

#include <glm/glm.hpp>

namespace Game {

    class PathfinderMob;
    class Rabbit;
    struct IBlockAccess;

    class MoveToBlockGoal : public Goal {
    public:
        static constexpr int kGiveUpTicks   = 1200;
        static constexpr int kStayTicks     = 1200;
        static constexpr int kIntervalTicks = 200;

        MoveToBlockGoal(PathfinderMob* mob, double speedModifier, int searchRange,
                        int verticalSearchRange = 1);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "MoveToBlockGoal"; }

    protected:
        // MC nextStartTick: reducedTickDelay(200 + rand(200)).
        virtual int NextStartTick() const;

        // The position the mob actually walks to — one above the found block.
        virtual glm::ivec3 GetMoveToTarget() const;

        // MC acceptedDistance — how close counts as arrived.
        virtual double AcceptedDistance() const { return 1.0; }

        virtual bool ShouldRecalculatePath() const { return m_tryTicks % 40 == 0; }

        // The predicate: is `pos` a block this goal wants?
        virtual bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const = 0;

        bool FindNearestBlock();
        bool IsReachedTarget() const { return m_reachedTarget; }
        void MoveMobToBlock();

        PathfinderMob* m_mob;
        double m_speedModifier;
        int    m_nextStartTick = 0;
        int    m_tryTicks = 0;
        int    m_maxStayTicks = 0;
        glm::ivec3 m_blockPos{0};
        bool   m_reachedTarget = false;
        int    m_searchRange;
        int    m_verticalSearchRange;
        int    m_verticalSearchStart = 0;
    };

    // MC RemoveBlockGoal: walk to the nearest block of one kind and break it
    // over ~3 seconds of stomping. Gated on mobGriefing.
    class RemoveBlockGoal : public MoveToBlockGoal {
    public:
        RemoveBlockGoal(BlockID blockToRemove, PathfinderMob* mob,
                        double speedModifier, int verticalSearchRange);

        bool CanUse() override;
        void Start() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "RemoveBlockGoal"; }

    protected:
        bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const override;

        // The block may be beside or below where the mob stands — MC probes
        // the position, its four sides, below and two-below.
        bool GetPosWithBlock(const IBlockAccess& blocks, const glm::ivec3& mobPos,
                             glm::ivec3& out) const;

        BlockID m_blockToRemove;
        int     m_ticksSinceReachedGoal = 0;
    };

    // MC Rabbit.RaidGardenGoal — MoveToBlockGoal(0.7, 16) hunting FARMLAND
    // carrying a max-age carrot crop: on arrival the crop loses one age (or,
    // at age 0, is broken), moreCarrotTicks gates the appetite, and the whole
    // goal is mobGriefing-gated. Lives here beside RemoveBlockGoal because it
    // is the same MoveToBlockGoal family (the ZombieAttackTurtleEggGoal
    // precedent).
    class RaidGardenGoal : public MoveToBlockGoal {
    public:
        explicit RaidGardenGoal(Rabbit* rabbit);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Tick() override;
        const char* Name() const override { return "RaidGardenGoal"; }

    protected:
        bool IsValidTarget(const IBlockAccess& blocks, const glm::ivec3& pos) const override;

    private:
        Rabbit* m_rabbit;
        // MC's wantsToRaid/canRaid — mutable because MC's isValidTarget
        // writes canRaid and our IsValidTarget hook is const.
        mutable bool m_wantsToRaid = false;
        mutable bool m_canRaid = false;
    };

    // MC Zombie.ZombieAttackTurtleEggGoal — RemoveBlockGoal(TURTLE_EGG) with a
    // 1.14-block accepted distance.
    class ZombieAttackTurtleEggGoal : public RemoveBlockGoal {
    public:
        ZombieAttackTurtleEggGoal(PathfinderMob* mob, double speedModifier,
                                  int verticalSearchRange)
            : RemoveBlockGoal(BlockID::TurtleEgg, mob, speedModifier, verticalSearchRange) {}

        const char* Name() const override { return "ZombieAttackTurtleEggGoal"; }

    protected:
        double AcceptedDistance() const override { return 1.14; }
    };

} // namespace Game
