// File: src/common/entity/ai/brain/CoreBehaviors.hpp
//
// The two behaviours every brain mob has, plus the sensor that feeds them.
//
// MC puts LookAtTargetSink and MoveToTargetSink in the CORE activity of every
// single brain mob, and they are what makes a brain do anything at all: no
// other behaviour ever calls the look control or the navigation directly. A
// behaviour that wants the mob somewhere writes WALK_TARGET and stops caring;
// MoveToTargetSink is the one thing that reads it, paths to it, and clears it.
//
// That is why porting the brain without these two would produce a mob whose
// memories fill up correctly and which never moves.
#pragma once

#include "common/entity/ai/brain/Behavior.hpp"
#include "common/entity/ai/brain/Sensor.hpp"
#include "common/world/pathfinder/Path.hpp"

#include <optional>

namespace Game {

    class Mob;

    // MC LookAtTargetSink — turn the head at whatever LOOK_TARGET holds.
    class LookAtTargetSink : public Behavior {
    public:
        LookAtTargetSink(int minDuration, int maxDuration);
        const char* DebugString() const override { return "LookAtTargetSink"; }

    protected:
        bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void Stop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
    };

    // MC MoveToTargetSink — path to whatever WALK_TARGET holds.
    //
    // The default 150..250 tick duration is not a timeout on the walk; it is MC
    // forcing a re-path periodically so a mob cannot follow a stale path
    // forever. The 40-tick cooldown after a STUCK navigation is what stops a
    // mob that cannot reach its target from re-pathing every tick.
    class MoveToTargetSink : public Behavior {
    public:
        MoveToTargetSink() : MoveToTargetSink(150, 250) {}
        MoveToTargetSink(int minTimeout, int maxTimeout);
        const char* DebugString() const override { return "MoveToTargetSink"; }

    protected:
        bool CheckExtraStartConditions(EntityLevel& level, LivingEntity& body) override;
        bool CanStillUse(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void Start(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void Tick(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;
        void Stop(EntityLevel& level, LivingEntity& body, int64_t timestamp) override;

    private:
        bool TryComputePath(Mob& body, const WalkTarget& target, int64_t timestamp);
        static bool ReachedTarget(const Mob& body, const WalkTarget& target);

        int  m_remainingCooldown = 0;
        std::optional<Path> m_path;
        std::optional<glm::ivec3> m_lastTargetPos;
        float m_speedModifier = 1.0f;
    };

    // MC NearestLivingEntitySensor — the scan every other sensor and behaviour
    // reads from. Writes both the raw list and the visible subset, sorted by
    // distance, which is what lets NearestVisibleLivingEntities::FindClosest
    // stop at the first match.
    class NearestLivingEntitySensor : public Sensor {
    public:
        std::vector<MemoryModule> Requires() const override {
            return { MemoryModule::NearestLivingEntities,
                     MemoryModule::NearestVisibleLivingEntities };
        }

    protected:
        void DoTick(EntityLevel& level, LivingEntity& body) override;
    };

} // namespace Game
