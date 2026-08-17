// File: src/common/entity/ai/brain/Brain.cpp
#include "common/entity/ai/brain/Brain.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Entity.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"

#include <algorithm>
#include <cmath>

namespace Game {

    // ── PositionTracker ────────────────────────────────────────────────────

    glm::dvec3 PositionTracker::CurrentPosition() const {
        if (entity) {
            return trackEyeHeight ? entity->GetEyePosition() : entity->position;
        }
        // MC BlockPosTracker uses the block's BOTTOM CENTRE, not its corner —
        // a mob told to walk to a block otherwise aims at its north-west edge.
        return glm::dvec3(static_cast<double>(blockPos.x) + 0.5,
                          static_cast<double>(blockPos.y),
                          static_cast<double>(blockPos.z) + 0.5);
    }

    glm::ivec3 PositionTracker::CurrentBlockPosition() const {
        if (!entity) return blockPos;
        return entity->BlockPosition();
    }

    bool PositionTracker::IsVisibleBy(const LivingEntity& viewer) const {
        (void)viewer;
        // MC EntityTracker checks the viewer's sensing; a block target is
        // always "visible". Line of sight for entity targets is applied by the
        // sensors that populate the memory, so re-testing here would double it.
        return true;
    }

    bool NearestVisibleLivingEntities::Contains(const LivingEntity* e) const {
        return std::find(entities.begin(), entities.end(), e) != entities.end();
    }

    // ── Sensor ─────────────────────────────────────────────────────────────

    void Sensor::Tick(EntityLevel& level, LivingEntity& body) {
        if (m_timeToTick < 0) {
            // MC seeds this in the constructor from a shared RANDOM. Doing it
            // on the first tick instead gives the same staggering without the
            // brain needing a random at construction time.
            m_timeToTick = m_scanRate > 0 ? level.Random().NextInt(m_scanRate) : 0;
        }
        if (--m_timeToTick <= 0) {
            m_timeToTick = m_scanRate;
            DoTick(level, body);
        }
    }

    // ── Behavior ───────────────────────────────────────────────────────────

    bool Behavior::HasRequiredMemories(const LivingEntity& body) const {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        for (const MemoryCondition& c : m_entryCondition) {
            if (!brain->CheckMemory(c.module, c.status)) return false;
        }
        return true;
    }

    bool Behavior::TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        if (!HasRequiredMemories(body)) return false;
        if (!CheckExtraStartConditions(level, body)) return false;

        m_status = BehaviorStatus::Running;
        // MC: minDuration + nextInt(maxDuration + 1 - minDuration). The +1 is
        // MC's, and it makes the range INCLUSIVE of maxDuration.
        const int span = m_maxDuration + 1 - m_minDuration;
        const int duration = m_minDuration + (span > 0 ? level.Random().NextInt(span) : 0);
        m_endTimestamp = timestamp + duration;
        Start(level, body, timestamp);
        return true;
    }

    void Behavior::TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        if (!TimedOut(timestamp) && CanStillUse(level, body, timestamp)) {
            Tick(level, body, timestamp);
        } else {
            DoStop(level, body, timestamp);
        }
    }

    void Behavior::DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        m_status = BehaviorStatus::Stopped;
        Stop(level, body, timestamp);
    }

    // ── GateBehavior ───────────────────────────────────────────────────────

    GateBehavior::GateBehavior(std::vector<MemoryCondition> entryCondition,
                               std::vector<MemoryModule> exitErasedMemories,
                               OrderPolicy orderPolicy, RunningPolicy runningPolicy,
                               std::vector<Entry> behaviors)
        : m_entryCondition(std::move(entryCondition)),
          m_exitErasedMemories(std::move(exitErasedMemories)),
          m_orderPolicy(orderPolicy), m_runningPolicy(runningPolicy),
          m_behaviors(std::move(behaviors)) {}

    bool GateBehavior::HasRequiredMemories(const LivingEntity& body) const {
        const Brain* brain = body.GetBrain();
        if (!brain) return false;
        for (const MemoryCondition& c : m_entryCondition) {
            if (!brain->CheckMemory(c.module, c.status)) return false;
        }
        return true;
    }

    void GateBehavior::Shuffle(EntityLevel& level) {
        // MC ShufflingList.shuffle: each entry draws `random.nextFloat()` and
        // the list is sorted by that, RAISED to the power 1/weight. MC writes
        // it as `-pow(rand, 1/weight)` on WeightedEntry and sorts ascending —
        // the standard trick for a weighted sample without replacement, and the
        // reason a weight-3 option is picked three times as often rather than
        // merely sorting first.
        for (Entry& e : m_behaviors) {
            const double r = static_cast<double>(level.Random().NextFloat());
            e.randWeight = -std::pow(r, 1.0 / std::max(1, e.weight));
        }
        std::sort(m_behaviors.begin(), m_behaviors.end(),
                  [](const Entry& a, const Entry& b) { return a.randWeight < b.randWeight; });
    }

    bool GateBehavior::TryStart(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        if (!HasRequiredMemories(body)) return false;

        m_status = BehaviorStatus::Running;
        if (m_orderPolicy == OrderPolicy::Shuffled) Shuffle(level);

        for (Entry& e : m_behaviors) {
            if (e.behavior->GetStatus() != BehaviorStatus::Stopped) continue;
            const bool started = e.behavior->TryStart(level, body, timestamp);
            // RunOne stops at the first child that actually starts; TryAll
            // offers the chance to every child.
            if (started && m_runningPolicy == RunningPolicy::RunOne) break;
        }
        return true;
    }

    void GateBehavior::TickOrStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        for (Entry& e : m_behaviors) {
            if (e.behavior->GetStatus() == BehaviorStatus::Running) {
                e.behavior->TickOrStop(level, body, timestamp);
            }
        }
        const bool anyRunning = std::any_of(
            m_behaviors.begin(), m_behaviors.end(),
            [](const Entry& e) { return e.behavior->GetStatus() == BehaviorStatus::Running; });
        if (!anyRunning) DoStop(level, body, timestamp);
    }

    void GateBehavior::DoStop(EntityLevel& level, LivingEntity& body, int64_t timestamp) {
        m_status = BehaviorStatus::Stopped;
        for (Entry& e : m_behaviors) {
            if (e.behavior->GetStatus() == BehaviorStatus::Running) {
                e.behavior->DoStop(level, body, timestamp);
            }
        }
        if (Brain* brain = body.GetBrain()) {
            for (MemoryModule m : m_exitErasedMemories) brain->EraseMemory(m);
        }
    }

    void GateBehavior::ClearReferenceTo(const Entity* entity) {
        for (Entry& e : m_behaviors) e.behavior->ClearReferenceTo(entity);
    }

    BehaviorPtr MakeRunOne(std::vector<GateBehavior::Entry> weighted) {
        return MakeRunOne({}, std::move(weighted));
    }

    BehaviorPtr MakeRunOne(std::vector<MemoryCondition> entryCondition,
                           std::vector<GateBehavior::Entry> weighted) {
        return std::make_unique<GateBehavior>(
            std::move(entryCondition), std::vector<MemoryModule>{},
            GateBehavior::OrderPolicy::Shuffled, GateBehavior::RunningPolicy::RunOne,
            std::move(weighted));
    }

    // ── Brain: memories ────────────────────────────────────────────────────

    void Brain::RegisterMemory(MemoryModule module) {
        m_memories[static_cast<size_t>(module)].registered = true;
    }

    bool Brain::IsRegistered(MemoryModule module) const {
        return m_memories[static_cast<size_t>(module)].registered;
    }

    bool Brain::CheckMemory(MemoryModule module, MemoryStatus status) const {
        const Slot& slot = m_memories[static_cast<size_t>(module)];
        // MC returns FALSE for an unregistered memory whatever the status is
        // asked for — including VALUE_ABSENT. A behaviour requiring a memory
        // the mob was never given must not fire.
        if (!slot.registered) return false;
        switch (status) {
            case MemoryStatus::Registered:   return true;
            case MemoryStatus::ValuePresent: return slot.value.has_value();
            case MemoryStatus::ValueAbsent:  return !slot.value.has_value();
        }
        return false;
    }

    void Brain::SetMemory(MemoryModule module, MemoryValue value) {
        SetMemoryWithExpiry(module, std::move(value), ExpirableValue::kNoExpiry);
    }

    void Brain::SetMemoryWithExpiry(MemoryModule module, MemoryValue value, int64_t ttl) {
        Slot& slot = m_memories[static_cast<size_t>(module)];
        if (!slot.registered) {
            // Writing an unregistered memory is a programming error, not a
            // runtime condition: the value would be stored and then fail every
            // CheckMemory, so the behaviour that wrote it would appear to do
            // nothing at all.
            Log::Warning("[Brain] write to unregistered memory '%s'",
                         std::string(MemoryModuleName(module)).c_str());
            return;
        }
        const size_t want = MemoryKindIndex(MemoryModuleKind(module));
        if (value.index() != want) {
            Log::Warning("[Brain] memory '%s' expects kind %zu, got %zu — dropped",
                         std::string(MemoryModuleName(module)).c_str(),
                         want, value.index());
            return;
        }
        slot.value = ExpirableValue{ std::move(value), ttl };
    }

    void Brain::EraseMemory(MemoryModule module) {
        m_memories[static_cast<size_t>(module)].value.reset();
    }

    void Brain::ClearMemories() {
        for (Slot& s : m_memories) s.value.reset();
    }

    const MemoryValue* Brain::GetMemory(MemoryModule module) const {
        const Slot& slot = m_memories[static_cast<size_t>(module)];
        if (!slot.registered || !slot.value.has_value()) return nullptr;
        return &slot.value->value;
    }

    int64_t Brain::GetTimeUntilExpiry(MemoryModule module) const {
        const Slot& slot = m_memories[static_cast<size_t>(module)];
        if (!slot.registered || !slot.value.has_value()) return 0;
        return slot.value->timeToLive;
    }

    namespace {
        template <typename T>
        std::optional<T> ReadAs(const MemoryValue* v) {
            if (!v) return std::nullopt;
            if (const T* t = std::get_if<T>(v)) return *t;
            return std::nullopt;
        }
        template <typename T>
        const T* PointAt(const MemoryValue* v) {
            return v ? std::get_if<T>(v) : nullptr;
        }
    }

    std::optional<bool>    Brain::GetBool(MemoryModule m) const { return ReadAs<bool>(GetMemory(m)); }
    std::optional<int>     Brain::GetInt(MemoryModule m) const { return ReadAs<int>(GetMemory(m)); }
    std::optional<int64_t> Brain::GetLong(MemoryModule m) const { return ReadAs<int64_t>(GetMemory(m)); }
    std::optional<glm::ivec3> Brain::GetBlockPos(MemoryModule m) const {
        return ReadAs<glm::ivec3>(GetMemory(m));
    }
    std::optional<glm::dvec3> Brain::GetVec3(MemoryModule m) const {
        return ReadAs<glm::dvec3>(GetMemory(m));
    }
    const WalkTarget* Brain::GetWalkTarget(MemoryModule m) const {
        return PointAt<WalkTarget>(GetMemory(m));
    }
    const PositionTracker* Brain::GetPositionTracker(MemoryModule m) const {
        return PointAt<PositionTracker>(GetMemory(m));
    }
    Entity* Brain::GetEntity(MemoryModule m) const {
        const MemoryValue* v = GetMemory(m);
        if (!v) return nullptr;
        Entity* const* e = std::get_if<Entity*>(v);
        return e ? *e : nullptr;
    }
    const std::vector<Entity*>* Brain::GetEntityList(MemoryModule m) const {
        return PointAt<std::vector<Entity*>>(GetMemory(m));
    }
    const NearestVisibleLivingEntities* Brain::GetVisibleEntities(MemoryModule m) const {
        return PointAt<NearestVisibleLivingEntities>(GetMemory(m));
    }

    bool Brain::IsMemoryValue(MemoryModule module, const Entity* entity) const {
        return GetEntity(module) == entity;
    }

    // ── Brain: sensors, activities ─────────────────────────────────────────

    void Brain::AddSensor(SensorPtr sensor) {
        // A sensor's outputs are registered here rather than by the caller,
        // which is MC's arrangement — the sensor declares what it writes, so a
        // behaviour asking for VALUE_ABSENT on it works from the first tick.
        for (MemoryModule m : sensor->Requires()) RegisterMemory(m);
        m_sensors.push_back(std::move(sensor));
    }

    void Brain::SetCoreActivities(std::vector<Activity> activities) {
        m_coreActivities = std::move(activities);
    }

    void Brain::AddActivity(Activity activity, int priorityOfFirstBehavior,
                            std::vector<BehaviorPtr> behaviors) {
        AddActivityWithConditions(activity, priorityOfFirstBehavior,
                                  std::move(behaviors), {});
    }

    void Brain::AddActivityWithConditions(Activity activity, int priorityOfFirstBehavior,
                                          std::vector<BehaviorPtr> behaviors,
                                          std::vector<MemoryCondition> conditions) {
        // MC createPriorityPairs: consecutive priorities from the first.
        int priority = priorityOfFirstBehavior;
        for (BehaviorPtr& b : behaviors) {
            m_behaviorsByPriority[priority++].push_back(Entry{ activity, std::move(b) });
        }
        m_activityRequirements.emplace_back(activity, std::move(conditions));
    }

    void Brain::AddActivityAndRemoveMemoryWhenStopped(
            Activity activity, int priorityOfFirstBehavior,
            std::vector<BehaviorPtr> behaviors, MemoryModule memory) {
        AddActivityWithConditions(
            activity, priorityOfFirstBehavior, std::move(behaviors),
            { MemoryCondition{ memory, MemoryStatus::ValuePresent } });
        m_activityMemoriesToEraseWhenStopped.emplace_back(
            activity, std::vector<MemoryModule>{ memory });
    }

    bool Brain::IsActive(Activity activity) const {
        return std::find(m_activeActivities.begin(), m_activeActivities.end(), activity)
               != m_activeActivities.end();
    }

    std::optional<Activity> Brain::GetActiveNonCoreActivity() const {
        for (Activity a : m_activeActivities) {
            if (std::find(m_coreActivities.begin(), m_coreActivities.end(), a)
                == m_coreActivities.end()) {
                return a;
            }
        }
        return std::nullopt;
    }

    bool Brain::ActivityRequirementsAreMet(Activity activity) const {
        // MC returns FALSE for an activity that was never added — an activity
        // with no entry at all is not "unconditionally valid".
        bool found = false;
        for (const auto& [a, conditions] : m_activityRequirements) {
            if (a != activity) continue;
            found = true;
            for (const MemoryCondition& c : conditions) {
                if (!CheckMemory(c.module, c.status)) return false;
            }
        }
        return found;
    }

    void Brain::SetActiveActivityToFirstValid(const std::vector<Activity>& activities) {
        for (Activity a : activities) {
            if (ActivityRequirementsAreMet(a)) {
                SetActiveActivity(a);
                return;
            }
        }
    }

    void Brain::SetActiveActivity(Activity activity) {
        if (IsActive(activity)) return;
        EraseMemoriesForOtherActivitiesThan(activity);
        m_activeActivities.clear();
        m_activeActivities.insert(m_activeActivities.end(),
                                  m_coreActivities.begin(), m_coreActivities.end());
        m_activeActivities.push_back(activity);
    }

    void Brain::EraseMemoriesForOtherActivitiesThan(Activity activity) {
        for (Activity old : m_activeActivities) {
            if (old == activity) continue;
            for (const auto& [a, memories] : m_activityMemoriesToEraseWhenStopped) {
                if (a != old) continue;
                for (MemoryModule m : memories) EraseMemory(m);
            }
        }
    }

    // ── Brain: tick ────────────────────────────────────────────────────────

    void Brain::ForgetOutdatedMemories() {
        for (Slot& slot : m_memories) {
            if (!slot.value.has_value()) continue;
            // MC checks expiry BEFORE ticking, so a memory set with ttl 1 is
            // readable for exactly one tick.
            if (slot.value->HasExpired()) {
                slot.value.reset();
                continue;
            }
            slot.value->Tick();
        }
    }

    void Brain::Tick(EntityLevel& level, LivingEntity& body) {
        ForgetOutdatedMemories();

        for (SensorPtr& s : m_sensors) s->Tick(level, body);

        const int64_t time = level.GetGameTime();

        // startEachNonRunningBehavior: ascending priority, and only behaviours
        // whose activity is currently active.
        for (auto& [priority, entries] : m_behaviorsByPriority) {
            (void)priority;
            for (Entry& e : entries) {
                if (!IsActive(e.activity)) continue;
                if (e.behavior->GetStatus() != BehaviorStatus::Stopped) continue;
                e.behavior->TryStart(level, body, time);
            }
        }

        // tickEachRunningBehavior. Collected first because a behaviour may stop
        // itself and MC iterates a snapshot.
        for (auto& [priority, entries] : m_behaviorsByPriority) {
            (void)priority;
            for (Entry& e : entries) {
                if (e.behavior->GetStatus() == BehaviorStatus::Running) {
                    e.behavior->TickOrStop(level, body, time);
                }
            }
        }
    }

    void Brain::StopAll(EntityLevel& level, LivingEntity& body) {
        const int64_t time = level.GetGameTime();
        for (auto& [priority, entries] : m_behaviorsByPriority) {
            (void)priority;
            for (Entry& e : entries) {
                if (e.behavior->GetStatus() == BehaviorStatus::Running) {
                    e.behavior->DoStop(level, body, time);
                }
            }
        }
    }

    void Brain::ClearReferenceTo(const Entity* entity) {
        for (auto& [priority, entries] : m_behaviorsByPriority) {
            (void)priority;
            for (Entry& e : entries) e.behavior->ClearReferenceTo(entity);
        }
        for (SensorPtr& s : m_sensors) s->ClearReferenceTo(entity);

        // And the memories themselves: a memory holding a freed entity is the
        // same use-after-free a goal's cached pointer would be.
        for (Slot& slot : m_memories) {
            if (!slot.value.has_value()) continue;
            MemoryValue& v = slot.value->value;
            if (Entity** e = std::get_if<Entity*>(&v)) {
                if (*e == entity) slot.value.reset();
                continue;
            }
            if (auto* list = std::get_if<std::vector<Entity*>>(&v)) {
                list->erase(std::remove(list->begin(), list->end(), entity), list->end());
                continue;
            }
            if (auto* vis = std::get_if<NearestVisibleLivingEntities>(&v)) {
                auto& es = vis->entities;
                es.erase(std::remove_if(es.begin(), es.end(),
                                        [&](LivingEntity* l) {
                                            return static_cast<const Entity*>(l) == entity;
                                        }),
                         es.end());
                continue;
            }
            if (auto* wt = std::get_if<WalkTarget>(&v)) {
                if (wt->target.entity == entity) slot.value.reset();
                continue;
            }
            if (auto* pt = std::get_if<PositionTracker>(&v)) {
                if (pt->entity == entity) slot.value.reset();
            }
        }
    }

    std::vector<const char*> Brain::RunningBehaviourNames() const {
        std::vector<const char*> out;
        for (const auto& [priority, entries] : m_behaviorsByPriority) {
            (void)priority;
            for (const Entry& e : entries) {
                if (e.behavior->GetStatus() == BehaviorStatus::Running) {
                    out.push_back(e.behavior->DebugString());
                }
            }
        }
        return out;
    }

} // namespace Game
