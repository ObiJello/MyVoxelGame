// File: src/common/entity/ai/Goal.cpp
#include "common/entity/ai/Goal.hpp"

#include <cstring>

namespace Game {

    namespace {
        // Bit position of a flag, for indexing m_lockedFlags.
        int FlagIndex(GoalFlag f) {
            switch (f) {
                case GoalFlag::Move:   return 0;
                case GoalFlag::Look:   return 1;
                case GoalFlag::Jump:   return 2;
                case GoalFlag::Target: return 3;
            }
            return 0;
        }
        constexpr GoalFlag kAllFlags[4] = {
            GoalFlag::Move, GoalFlag::Look, GoalFlag::Jump, GoalFlag::Target
        };
    }

    void GoalSelector::AddGoal(int priority, std::unique_ptr<Goal> goal) {
        m_goals.emplace_back(priority, std::move(goal));
    }

    bool GoalSelector::ContainsDisabledFlag(const WrappedGoal& goal) const {
        return (goal.Get()->GetFlags() & m_disabledFlags) != 0;
    }

    bool GoalSelector::CanBeReplacedForAllFlags(const WrappedGoal& goal) const {
        const uint8_t flags = goal.Get()->GetFlags();
        for (GoalFlag f : kAllFlags) {
            if ((flags & static_cast<uint8_t>(f)) == 0) continue;
            const int owner = m_lockedFlags[FlagIndex(f)];
            if (owner < 0) continue;                     // unowned — free to take
            if (!m_goals[owner].CanBeReplacedBy(goal)) return false;
        }
        return true;
    }

    void GoalSelector::Tick() {
        // ── Cleanup: stop goals that can no longer run ─────────────────────
        for (WrappedGoal& g : m_goals) {
            if (g.IsRunning() && (ContainsDisabledFlag(g) || !g.Get()->CanContinueToUse())) {
                g.Stop();
            }
        }

        // Release any flag whose owner stopped. MC does this by removing dead
        // entries from lockedFlags; the effect is the same.
        for (int i = 0; i < 4; ++i) {
            if (m_lockedFlags[i] >= 0 && !m_goals[m_lockedFlags[i]].IsRunning()) {
                m_lockedFlags[i] = -1;
            }
        }

        // ── Start: first goal (in INSERTION order) that can claim its flags ─
        for (size_t i = 0; i < m_goals.size(); ++i) {
            WrappedGoal& g = m_goals[i];
            if (g.IsRunning()) continue;
            if (ContainsDisabledFlag(g)) continue;
            if (!CanBeReplacedForAllFlags(g)) continue;
            if (!g.Get()->CanUse()) continue;

            const uint8_t flags = g.Get()->GetFlags();
            for (GoalFlag f : kAllFlags) {
                if ((flags & static_cast<uint8_t>(f)) == 0) continue;
                const int idx = FlagIndex(f);
                if (m_lockedFlags[idx] >= 0) m_goals[m_lockedFlags[idx]].Stop();
                m_lockedFlags[idx] = static_cast<int>(i);
            }
            g.Start();
        }

        TickRunningGoals(true);
    }

    void GoalSelector::TickRunningGoals(bool forceTickAll) {
        for (WrappedGoal& g : m_goals) {
            if (g.IsRunning() && (forceTickAll || g.Get()->RequiresUpdateEveryTick())) {
                g.Get()->Tick();
            }
        }
    }

    void GoalSelector::DisableControlFlag(GoalFlag flag) {
        m_disabledFlags |= static_cast<uint8_t>(flag);
    }

    void GoalSelector::EnableControlFlag(GoalFlag flag) {
        m_disabledFlags &= static_cast<uint8_t>(~static_cast<uint8_t>(flag));
    }

    void GoalSelector::SetControlFlag(GoalFlag flag, bool enabled) {
        if (enabled) EnableControlFlag(flag); else DisableControlFlag(flag);
    }

    void GoalSelector::ClearReferenceTo(const Entity* entity) {
        for (WrappedGoal& g : m_goals) g.Get()->ClearReferenceTo(entity);
    }

    bool GoalSelector::IsRunning(const char* goalName) const {
        for (const WrappedGoal& g : m_goals) {
            if (g.IsRunning() && std::strcmp(g.Get()->Name(), goalName) == 0) return true;
        }
        return false;
    }

} // namespace Game
