// File: src/common/entity/ai/brain/Brain.hpp
//
// MC net.minecraft.world.entity.ai.Brain.
//
// The second of MC's two AI systems. Twenty-one mob types in this port use it in
// vanilla — frog, sniffer, warden, breeze, camel, armadillo, copper golem, goat,
// allay, axolotl, piglin, hoglin, villager and the rest — and until now they ran
// the goal system's generic set instead, which is why their animations were
// baked and never played.
//
// FOUR THINGS RUN THE MOB, in this order every tick:
//
//   1. forgetOutdatedMemories   decay every memory's TTL, erase what expired
//   2. tickSensors              periodic world scans that WRITE memories
//   3. startEachNonRunningBehavior  by priority, for each ACTIVE activity
//   4. tickEachRunningBehavior  tick or stop what is running
//
// Behaviours never reference each other. A sensor writes NEAREST_LIVING_ENTITIES,
// one behaviour turns that into ATTACK_TARGET, another walks toward whatever
// ATTACK_TARGET holds. That indirection is the whole design, and it is why an
// activity can be swapped wholesale without anything knowing.
//
// ACTIVITIES are the coarse mode — IDLE, PANIC, FIGHT, LONG_JUMP. A behaviour
// belongs to one, and only behaviours in an ACTIVE activity are started. Core
// activities are always active; exactly one non-core activity is active at a
// time, chosen by setActiveActivityToFirstValid against each activity's memory
// requirements.
#pragma once

#include "common/entity/ai/brain/Activity.hpp"
#include "common/entity/ai/brain/Behavior.hpp"
#include "common/entity/ai/brain/Memory.hpp"
#include "common/entity/ai/brain/Sensor.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Game {

    class LivingEntity;
    class EntityLevel;

    class Brain {
    public:
        Brain() = default;
        Brain(const Brain&) = delete;
        Brain& operator=(const Brain&) = delete;

        // ── Memories ───────────────────────────────────────────────────────
        //
        // MC distinguishes a memory that is REGISTERED (this mob has the slot)
        // from one that HOLDS a value. A behaviour requiring VALUE_ABSENT only
        // passes when the slot exists and is empty — an unregistered memory
        // fails every status including VALUE_ABSENT, which is MC's checkMemory
        // returning false on a null map entry and is load-bearing: it stops a
        // behaviour firing on a mob that was never given that memory at all.
        void RegisterMemory(MemoryModule module);
        bool IsRegistered(MemoryModule module) const;

        bool CheckMemory(MemoryModule module, MemoryStatus status) const;
        bool HasMemoryValue(MemoryModule module) const {
            return CheckMemory(module, MemoryStatus::ValuePresent);
        }

        void SetMemory(MemoryModule module, MemoryValue value);
        void SetMemoryWithExpiry(MemoryModule module, MemoryValue value, int64_t ttl);
        void EraseMemory(MemoryModule module);
        void ClearMemories();

        const MemoryValue* GetMemory(MemoryModule module) const;

        // Remaining TTL, or 0 when absent. MC's getTimeUntilExpiry — the
        // armadillo reads it to decide when to unroll, so it is a real input
        // and not just bookkeeping.
        int64_t GetTimeUntilExpiry(MemoryModule module) const;

        // Typed reads. They return nullopt both when the memory is absent and
        // when it holds another type, so a behaviour cannot act on a value the
        // writer never put there.
        std::optional<bool>       GetBool(MemoryModule module) const;
        std::optional<int>        GetInt(MemoryModule module) const;
        std::optional<int64_t>    GetLong(MemoryModule module) const;
        std::optional<glm::ivec3> GetBlockPos(MemoryModule module) const;
        std::optional<glm::dvec3> GetVec3(MemoryModule module) const;
        const WalkTarget*      GetWalkTarget(MemoryModule module) const;
        const PositionTracker* GetPositionTracker(MemoryModule module) const;
        Entity*                GetEntity(MemoryModule module) const;
        const std::vector<Entity*>* GetEntityList(MemoryModule module) const;
        const NearestVisibleLivingEntities* GetVisibleEntities(MemoryModule module) const;

        // MC isMemoryValue — "does this memory hold exactly this entity".
        bool IsMemoryValue(MemoryModule module, const Entity* entity) const;

        // ── Sensors ────────────────────────────────────────────────────────
        void AddSensor(SensorPtr sensor);

        // ── Activities and behaviours ──────────────────────────────────────
        void SetCoreActivities(std::vector<Activity> activities);
        void SetDefaultActivity(Activity activity) { m_defaultActivity = activity; }

        // MC addActivity — behaviours take consecutive priorities starting at
        // `priorityOfFirstBehavior`.
        void AddActivity(Activity activity, int priorityOfFirstBehavior,
                         std::vector<BehaviorPtr> behaviors);
        // MC addActivityWithConditions — the same, plus the memory conditions
        // that decide whether the activity may become active at all.
        void AddActivityWithConditions(Activity activity, int priorityOfFirstBehavior,
                                       std::vector<BehaviorPtr> behaviors,
                                       std::vector<MemoryCondition> conditions);
        // MC addActivityAndRemoveMemoryWhenStopped — the activity requires the
        // memory to be present and erases it when it stops, which is how MC
        // makes an activity self-terminating (TONGUE erases ATTACK_TARGET).
        void AddActivityAndRemoveMemoryWhenStopped(
            Activity activity, int priorityOfFirstBehavior,
            std::vector<BehaviorPtr> behaviors, MemoryModule memory);

        // MC's activity switch. The FIRST activity whose requirements are met
        // wins; nothing changes when none do.
        void SetActiveActivityToFirstValid(const std::vector<Activity>& activities);
        void UseDefaultActivity() { SetActiveActivity(m_defaultActivity); }
        bool IsActive(Activity activity) const;
        std::optional<Activity> GetActiveNonCoreActivity() const;

        // ── Tick ───────────────────────────────────────────────────────────
        void Tick(EntityLevel& level, LivingEntity& body);
        void StopAll(EntityLevel& level, LivingEntity& body);

        // Forwarded to every behaviour AND scrubbed from every memory.
        void ClearReferenceTo(const Entity* entity);

        // Diagnostic: the behaviours currently running, for the debug panel.
        std::vector<const char*> RunningBehaviourNames() const;

    private:
        struct Slot {
            bool registered = false;
            std::optional<ExpirableValue> value;
        };

        void SetActiveActivity(Activity activity);
        void EraseMemoriesForOtherActivitiesThan(Activity activity);
        bool ActivityRequirementsAreMet(Activity activity) const;
        void ForgetOutdatedMemories();

        std::array<Slot, kMemoryModuleCount> m_memories{};
        std::vector<SensorPtr> m_sensors;

        // MC keys this by priority then activity, and iterates the priority map
        // in ASCENDING order — a std::map, not a hash map, because the
        // iteration order IS the priority order.
        struct Entry {
            Activity    activity;
            BehaviorPtr behavior;
        };
        std::map<int, std::vector<Entry>> m_behaviorsByPriority;

        std::vector<std::pair<Activity, std::vector<MemoryCondition>>> m_activityRequirements;
        std::vector<std::pair<Activity, std::vector<MemoryModule>>> m_activityMemoriesToEraseWhenStopped;

        std::vector<Activity> m_coreActivities;
        std::vector<Activity> m_activeActivities;
        Activity m_defaultActivity = Activity::Idle;
    };

} // namespace Game
