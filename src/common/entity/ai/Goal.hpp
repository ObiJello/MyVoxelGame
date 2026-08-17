// File: src/common/entity/ai/Goal.hpp
//
// MC net.minecraft.world.entity.ai.goal.Goal / WrappedGoal / GoalSelector.
//
// Three details decide whether ported mobs behave like MC ones, and all three
// are easy to lose in translation:
//
//  1. LOWER PRIORITY NUMBER WINS. `canBeReplacedBy` is
//     `isInterruptable() && other.priority < this.priority`. A zombie's attack
//     goal is priority 3 and its stroll goal is 7, so attacking pre-empts
//     strolling. Sorting the list ascending and taking the first match is NOT
//     the same algorithm and produces different behaviour under flag contention.
//
//  2. GOALS ARE EVALUATED IN INSERTION ORDER, not priority order. MC uses an
//     ObjectLinkedOpenHashSet and iterates it directly; priority only enters
//     through the flag-replacement test. This container is therefore a vector.
//
//  3. FLAGS ARE THE ARBITRATION MECHANISM. Two goals that declare no flags
//     never conflict and can both run (FollowParentGoal is deliberately
//     flagless). Two that both want MOVE cannot.
//
// The 2-tick evaluation cadence lives in Mob::ServerAiStep, not here: full
// tick() every other tick, tickRunningGoals(false) on the off tick. That is
// also why `reducedTickDelay` halves durations — a "40 tick" timer only gets
// 20 evaluations.
#pragma once

#include "common/core/Mth.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Game {

    class Mob;
    class Entity;

    // MC Goal.Flag. A bitmask rather than an EnumSet — there are four of them
    // and the set operations are all single instructions this way.
    enum class GoalFlag : uint8_t {
        Move   = 1 << 0,
        Look   = 1 << 1,
        Jump   = 1 << 2,
        Target = 1 << 3,
    };

    inline constexpr uint8_t operator|(GoalFlag a, GoalFlag b) {
        return static_cast<uint8_t>(a) | static_cast<uint8_t>(b);
    }
    inline constexpr uint8_t operator|(uint8_t a, GoalFlag b) {
        return a | static_cast<uint8_t>(b);
    }

    class Goal {
    public:
        virtual ~Goal() = default;

        virtual bool CanUse() = 0;
        virtual bool CanContinueToUse() { return CanUse(); }
        virtual bool IsInterruptable() const { return true; }
        virtual void Start() {}
        virtual void Stop() {}
        virtual void Tick() {}

        // Goals that need sub-2-tick resolution (melee attack timing, creeper
        // fuse, look-around) opt in here and are ticked on the off tick too.
        virtual bool RequiresUpdateEveryTick() const { return false; }

        // Diagnostic only — surfaced in the debug entity panel.
        virtual const char* Name() const = 0;

        // Drop any cached pointer to `entity`, which is about to be destroyed.
        //
        // Goals cache entity pointers across ticks (a breeding partner, a
        // followed parent, the thing being looked at) and re-validate them with
        // IsAlive() on the next tick. In MC that is safe because the JVM keeps
        // a removed entity's object alive as long as anything references it —
        // isRemoved() on a dead entity is a legal read. C++ has no such
        // guarantee: once MobManager frees the mob, that IsAlive() call is a
        // virtual dispatch through a freed vptr.
        //
        // So the manager calls this on every surviving mob for every mob it is
        // about to erase. Any goal that caches an entity pointer MUST override
        // it; forgetting one is a use-after-free that will not reproduce
        // reliably.
        virtual void ClearReferenceTo(const class Entity* entity) {}

        uint8_t GetFlags() const { return m_flags; }
        void    SetFlags(uint8_t flags) { m_flags = flags; }

    protected:
        // MC Goal.adjustedTickDelay / reducedTickDelay. A goal that is only
        // evaluated every other tick must halve its timers or everything it
        // does takes twice as long as in vanilla.
        int AdjustedTickDelay(int ticks) const {
            return RequiresUpdateEveryTick() ? ticks : ReducedTickDelay(ticks);
        }
        static int ReducedTickDelay(int ticks) { return Mth::PositiveCeilDiv(ticks, 2); }

    private:
        uint8_t m_flags = 0;
    };

    // MC WrappedGoal — pairs a goal with its priority and running state.
    class WrappedGoal {
    public:
        WrappedGoal(int priority, std::unique_ptr<Goal> goal)
            : m_goal(std::move(goal)), m_priority(priority) {}

        bool CanBeReplacedBy(const WrappedGoal& other) const {
            return m_goal->IsInterruptable() && other.m_priority < m_priority;
        }

        void Start() {
            if (!m_running) { m_running = true; m_goal->Start(); }
        }
        void Stop() {
            if (m_running) { m_running = false; m_goal->Stop(); }
        }

        bool IsRunning() const { return m_running; }
        int  GetPriority() const { return m_priority; }
        Goal*       Get()       { return m_goal.get(); }
        const Goal* Get() const { return m_goal.get(); }

    private:
        std::unique_ptr<Goal> m_goal;
        int  m_priority;
        bool m_running = false;
    };

    class GoalSelector {
    public:
        void AddGoal(int priority, std::unique_ptr<Goal> goal);

        // MC GoalSelector.tick — cleanup pass, then start pass, then tick all
        // running goals.
        void Tick();

        // MC tickRunningGoals(force). With force=false only goals that asked
        // for it are ticked; this is the off-tick path.
        void TickRunningGoals(bool forceTickAll);

        void DisableControlFlag(GoalFlag flag);
        void EnableControlFlag(GoalFlag flag);
        void SetControlFlag(GoalFlag flag, bool enabled);

        const std::vector<WrappedGoal>& All() const { return m_goals; }

        // Drop every goal and release the flag locks.
        //
        // MC has no equivalent because a mob's registerGoals is called exactly
        // once, from the right class. Here a mob promoted out of the generic
        // path inherits Generic*'s goal set from a BASE constructor, which has
        // already run by the time the subclass's constructor body can object —
        // so a bat, which MC gives no goals at all, needs a way to say so.
        void Clear() {
            m_goals.clear();
            for (int& owner : m_lockedFlags) owner = -1;
        }

        // Forwarded to every goal — see Goal::ClearReferenceTo.
        void ClearReferenceTo(const Entity* entity);

        // Is any running goal of the named kind active? Used by
        // PathfinderMob::IsPanicking, which MC implements the same way.
        bool IsRunning(const char* goalName) const;

    private:
        bool ContainsDisabledFlag(const WrappedGoal& goal) const;
        bool CanBeReplacedForAllFlags(const WrappedGoal& goal) const;

        // Insertion-ordered, deliberately — see the header note.
        std::vector<WrappedGoal> m_goals;

        // Which goal currently owns each flag. Index is the flag's bit
        // position; -1 means unowned. MC uses an EnumMap with a NO_GOAL
        // sentinel of priority MAX_VALUE, which behaves identically.
        int     m_lockedFlags[4] = {-1, -1, -1, -1};
        uint8_t m_disabledFlags = 0;
    };

} // namespace Game
