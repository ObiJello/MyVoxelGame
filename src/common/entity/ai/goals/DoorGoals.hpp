// File: src/common/entity/ai/goals/DoorGoals.hpp
//
// MC DoorInteractGoal + BreakDoorGoal, plus the Vindicator's two nested
// goals (its BreakDoorGoal specialisation and the Johnny target goal).
//
// The door-breaking loop end to end: the pathfinder treats a closed wooden
// door as walkable once the navigation's canOpenDoors flag is set (the
// evaluator's DoorWoodClosed branch), so the mob paths straight at it and
// collides; DoorInteractGoal notices the collision plus a door node within
// the next two path nodes; BreakDoorGoal then counts 240 ticks of pounding
// (swing every ~20 ticks) and removes the door — BOTH halves, because this
// engine has no double-block linkage to drop the orphaned upper half the way
// MC's neighbour update does.
//
// Not modelled, named at their sites: OpenDoorGoal / DoorBlock.setOpen
// (villager machinery — no door use interaction exists), the crack overlay
// (destroyBlockProgress — no block-damage render pipeline), and the level
// events 1019/1021/2001 (door-pound and break sounds/particles).
#pragma once

#include "common/entity/ai/Goal.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/entity/EntityLevel.hpp"

#include <glm/glm.hpp>

namespace Game {

    class Mob;
    class Vindicator;

    // MC DoorInteractGoal (abstract).
    class DoorInteractGoal : public Goal {
    public:
        explicit DoorInteractGoal(Mob* mob);

        bool CanUse() override;
        bool CanContinueToUse() override;
        void Start() override;
        void Tick() override;
        bool RequiresUpdateEveryTick() const override { return true; }
        const char* Name() const override { return "DoorInteractGoal"; }

        // MC DoorBlock.isWoodenDoor — a door whose type canOpenByHand (every
        // door but iron). Static so BreakDoorGoal's world edit shares it.
        static bool IsWoodenDoorAt(const EntityLevel& level, const glm::ivec3& pos);

    protected:
        // MC isOpen() — reads the OPEN blockstate property (and, like MC,
        // drops m_hasDoor when the door has vanished — hence non-const).
        bool IsOpen();
        // MC setOpen() is OpenDoorGoal's half (villagers) — unported: no
        // door-use interaction exists in this engine yet.

        Mob*       m_mob;
        glm::ivec3 m_doorPos{0};
        bool       m_hasDoor = false;

    private:
        bool  m_passed = false;
        float m_doorOpenDirX = 0.0f;
        float m_doorOpenDirZ = 0.0f;
    };

    // MC BreakDoorGoal. `validDifficulty` is MC's Predicate<Difficulty> —
    // the zombie passes HARD-only, the vindicator NORMAL-or-HARD.
    class BreakDoorGoal : public DoorInteractGoal {
    public:
        using DifficultyPredicate = bool (*)(Difficulty);

        BreakDoorGoal(Mob* mob, DifficultyPredicate validDifficulty);
        BreakDoorGoal(Mob* mob, int doorBreakTime, DifficultyPredicate validDifficulty);

        bool CanUse() override;
        void Start() override;
        bool CanContinueToUse() override;
        void Stop() override;
        void Tick() override;
        const char* Name() const override { return "BreakDoorGoal"; }

        // MC Zombie.DOOR_BREAKING_PREDICATE / Vindicator.DOOR_BREAKING_PREDICATE.
        static bool HardOnly(Difficulty d) { return d == Difficulty::Hard; }
        static bool NormalOrHard(Difficulty d) {
            return d == Difficulty::Normal || d == Difficulty::Hard;
        }

    protected:
        // MC getDoorBreakTime: Math.max(240, doorBreakTime) — yes, the
        // vindicator's constructor argument of 6 is swallowed by the max in
        // vanilla too; transcribed as-is.
        int GetDoorBreakTime() const { return m_doorBreakTime > 240 ? m_doorBreakTime : 240; }

        int m_breakTime = 0;
        int m_lastBreakProgress = -1;
        int m_doorBreakTime = -1;

    private:
        bool IsValidDifficulty() const;

        DifficultyPredicate m_validDifficulty;
    };

    // MC Vindicator.VindicatorBreakDoorGoal — BreakDoorGoal(6, NORMAL|HARD)
    // with a 1-in-10 (reduced-tick) start roll. MC additionally gates BOTH
    // canUse and canContinueToUse on hasActiveRaid(); no raid system exists,
    // and keeping the gate would leave the goal permanently dead — so it is
    // treated as satisfied, DEVIATION documented here: a vanilla vindicator
    // only breaks doors during a raid, ours whenever one blocks its path on
    // NORMAL/HARD (which is exactly its in-raid behaviour).
    class VindicatorBreakDoorGoal : public BreakDoorGoal {
    public:
        explicit VindicatorBreakDoorGoal(Mob* mob);

        bool CanUse() override;
        void Start() override;
        const char* Name() const override { return "VindicatorBreakDoorGoal"; }
    };

} // namespace Game
