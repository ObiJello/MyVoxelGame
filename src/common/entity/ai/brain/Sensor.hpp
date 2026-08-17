// File: src/common/entity/ai/brain/Sensor.hpp
//
// MC net.minecraft.world.entity.ai.sensing.Sensor.
//
// Sensors are the only things that WRITE the world into a brain. They run on a
// fixed period — 20 ticks by default — and a behaviour never scans for itself;
// it reads whatever memory a sensor last wrote. That indirection is what keeps
// the cost of a crowd of brain mobs bounded: twelve behaviours reading
// NEAREST_VISIBLE_LIVING_ENTITIES cost one scan between them, not twelve.
//
// MC seeds the first tick with `random.nextInt(scanRate)` so that mobs spawned
// on the same tick do not all scan on the same tick forever — the same
// staggering Mob::ServerAiStep does with the entity id.
#pragma once

#include "common/entity/ai/brain/Memory.hpp"

#include <memory>
#include <vector>

namespace Game {

    class LivingEntity;
    struct EntityLevel;

    class Sensor {
    public:
        static constexpr int kDefaultScanRate = 20;

        explicit Sensor(int scanRate = kDefaultScanRate) : m_scanRate(scanRate) {}
        virtual ~Sensor() = default;

        // Seeded out of line: EntityLevel is only forward-declared here.
        void Tick(EntityLevel& level, LivingEntity& body);

        // The memories this sensor writes. The brain registers them, which is
        // what makes a VALUE_ABSENT requirement on them meaningful.
        virtual std::vector<MemoryModule> Requires() const = 0;

        virtual void ClearReferenceTo(const Entity* entity) { (void)entity; }

    protected:
        virtual void DoTick(EntityLevel& level, LivingEntity& body) = 0;

    private:
        int     m_scanRate;
        int64_t m_timeToTick = -1;   // -1 = seed on first tick
    };

    using SensorPtr = std::unique_ptr<Sensor>;

} // namespace Game
