// File: src/common/entity/ai/brain/Behavior.hpp
//
// MC net.minecraft.world.entity.ai.behavior.{BehaviorControl, Behavior,
// GateBehavior, RunOne}.
//
// A Behavior is NOT a Goal, and the difference is the whole reason this exists
// alongside the goal system:
//
//   * A goal arbitrates through FLAGS — two goals wanting MOVE cannot both run.
//     A behaviour arbitrates through MEMORIES: it declares the memories it needs
//     present or absent, and the mob's state decides. That is why MC can run
//     "look at the player" and "walk somewhere" simultaneously without either
//     knowing about the other, and why a frog croaks only when it has no
//     WALK_TARGET.
//   * A goal is polled every other tick and lives forever. A behaviour is
//     started once, given a RANDOM duration between minDuration and maxDuration,
//     and force-stopped when it times out.
//   * Goals live in one flat priority list. Behaviours live inside ACTIVITIES,
//     and switching activity stops everything in the old one.
//
// The lifecycle is exact: tryStart checks memories and extra conditions, rolls
// the duration, and calls start; tickOrStop stops on timeout or when
// canStillUse goes false, otherwise ticks; doStop always calls stop.
#pragma once

#include "common/entity/ai/brain/Memory.hpp"

#include <memory>
#include <vector>

namespace Game {

    class LivingEntity;
    class EntityLevel;
    class Brain;

    enum class BehaviorStatus : uint8_t { Stopped, Running };

    // MC BehaviorControl — the interface the brain and the gates talk to.
    class BehaviorControl {
    public:
        virtual ~BehaviorControl() = default;

        virtual BehaviorStatus GetStatus() const = 0;
        virtual bool TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) = 0;
        virtual void TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) = 0;
        virtual void DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) = 0;

        // Diagnostic only, and the reason a brain is debuggable at all.
        virtual const char* DebugString() const = 0;

        // Drop any cached entity pointer, for the same use-after-free reason
        // Goal::ClearReferenceTo exists — MC relies on the GC keeping removed
        // entities readable and this port cannot.
        virtual void ClearReferenceTo(const Entity* entity) { (void)entity; }
    };

    using BehaviorPtr = std::unique_ptr<BehaviorControl>;

    // One entry of a behaviour's entry condition.
    struct MemoryCondition {
        MemoryModule module;
        MemoryStatus status;
    };

    // MC Behavior.
    class Behavior : public BehaviorControl {
    public:
        static constexpr int kDefaultDuration = 60;   // MC Behavior.DEFAULT_DURATION

        explicit Behavior(std::vector<MemoryCondition> entryCondition,
                          int minDuration = kDefaultDuration,
                          int maxDuration = -1)
            : m_entryCondition(std::move(entryCondition)),
              m_minDuration(minDuration),
              m_maxDuration(maxDuration < 0 ? minDuration : maxDuration) {}

        BehaviorStatus GetStatus() const final { return m_status; }

        bool TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) final;
        void TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) final;
        void DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) final;

        const char* DebugString() const override { return "Behavior"; }

    protected:
        virtual bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) {
            (void)level; (void)body; return true;
        }
        virtual void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
            (void)level; (void)body; (void)timestamp;
        }
        virtual void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
            (void)level; (void)body; (void)timestamp;
        }
        virtual void Stop(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
            (void)level; (void)body; (void)timestamp;
        }
        // MC's default is FALSE: a behaviour runs for exactly one tick unless it
        // says otherwise. Getting this backwards makes every behaviour sticky.
        virtual bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
            (void)level; (void)body; (void)timestamp; return false;
        }

        bool HasRequiredMemories(const LivingEntity& body) const;

    private:
        bool TimedOut(int64_t timestamp) const { return timestamp > m_endTimestamp; }

        std::vector<MemoryCondition> m_entryCondition;
        BehaviorStatus m_status = BehaviorStatus::Stopped;
        int64_t m_endTimestamp = 0;
        int     m_minDuration;
        int     m_maxDuration;
    };

    // MC GateBehavior — a behaviour that runs OTHER behaviours.
    //
    // The two policies are independent and both matter:
    //   OrderPolicy    Ordered keeps the declared order; Shuffled re-sorts the
    //                  list by weight * random each time the gate starts, which
    //                  is how MC turns a weighted list into a weighted pick.
    //   RunningPolicy  RunOne starts the FIRST child that will start; TryAll
    //                  starts every child that will.
    class GateBehavior : public BehaviorControl {
    public:
        enum class OrderPolicy : uint8_t { Ordered, Shuffled };
        enum class RunningPolicy : uint8_t { RunOne, TryAll };

        struct Entry {
            BehaviorPtr behavior;
            int         weight = 1;
            // Scratch for the shuffle — MC's ShufflingList keeps the same field
            // on its entries rather than sorting a separate array.
            double      randWeight = 0.0;
        };

        GateBehavior(std::vector<MemoryCondition> entryCondition,
                     std::vector<MemoryModule> exitErasedMemories,
                     OrderPolicy orderPolicy, RunningPolicy runningPolicy,
                     std::vector<Entry> behaviors);

        BehaviorStatus GetStatus() const override { return m_status; }
        bool TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        const char* DebugString() const override { return "GateBehavior"; }
        void ClearReferenceTo(const Entity* entity) override;

    private:
        bool HasRequiredMemories(const LivingEntity& body) const;
        void Shuffle(EntityLevel& level);

        std::vector<MemoryCondition> m_entryCondition;
        std::vector<MemoryModule>    m_exitErasedMemories;
        OrderPolicy    m_orderPolicy;
        RunningPolicy  m_runningPolicy;
        std::vector<Entry> m_behaviors;
        BehaviorStatus m_status = BehaviorStatus::Stopped;
    };

    // MC RunOne — a shuffled, run-one gate. The overwhelmingly common shape,
    // and the one that gives idle mobs their variety.
    BehaviorPtr MakeRunOne(std::vector<GateBehavior::Entry> weighted);
    BehaviorPtr MakeRunOne(std::vector<MemoryCondition> entryCondition,
                           std::vector<GateBehavior::Entry> weighted);

} // namespace Game
