// File: src/common/entity/ai/Sensing.hpp
//
// MC net.minecraft.world.entity.ai.sensing.Sensing — a one-tick line-of-sight
// memo.
//
// Every targeting goal asks "can I see my target" at least once per tick, and
// several ask repeatedly. The raycast is not free, so MC caches both answers
// (seen and unseen) per entity id and clears them at the top of each AI step.
// Two separate sets rather than a map because that is what MC does and it keeps
// "not asked yet" distinct from "asked, answer was false".
#pragma once

#include <cstdint>
#include <unordered_set>

namespace Game {

    class Mob;
    class Entity;

    class Sensing {
    public:
        explicit Sensing(Mob* mob) : m_mob(mob) {}

        // Cleared once per AI step, so a target that steps behind a wall is
        // noticed on the next tick and not sooner.
        void Tick();

        bool HasLineOfSight(const Entity& target);

    private:
        bool ComputeLineOfSight(const Entity& target) const;

        Mob* m_mob;
        std::unordered_set<int32_t> m_seen;
        std::unordered_set<int32_t> m_unseen;
    };

} // namespace Game
