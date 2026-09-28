// File: src/common/entity/WardenSpawnTracker.cpp
#include "WardenSpawnTracker.hpp"

#include "common/entity/EntityLevel.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/mobs/AnimatedMobs.hpp"   // Warden

#include <algorithm>
#include <vector>

namespace Game {

    void WardenSpawnTracker::Tick() {
        if (m_ticksSinceLastWarning >= kDecreaseWarningLevelEveryInterval) {
            DecreaseWarningLevel();
            m_ticksSinceLastWarning = 0;
        } else {
            ++m_ticksSinceLastWarning;
        }
        if (m_cooldownTicks > 0) --m_cooldownTicks;
    }

    void WardenSpawnTracker::Reset() {
        m_ticksSinceLastWarning = 0;
        m_warningLevel = 0;
        m_cooldownTicks = 0;
    }

    void WardenSpawnTracker::SetWarningLevel(int warningLevel) {
        m_warningLevel = std::clamp(warningLevel, 0, kMaxWarningLevel);
    }

    void WardenSpawnTracker::IncreaseWarningLevel() {
        if (OnCooldown()) return;
        m_ticksSinceLastWarning = 0;
        m_cooldownTicks = kWarningLevelIncreaseCooldown;
        SetWarningLevel(m_warningLevel + 1);
    }

    std::optional<int> WardenSpawnTracker::TryWarn(EntityLevel& level, const glm::ivec3& pos,
                                                   LivingEntity& triggerPlayer) {
        const glm::dvec3 center(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5);

        // MC hasNearbyWarden: any Warden in the 48-block cube around the
        // shrieker (AABB.ofSize(center, 48, 48, 48)).
        {
            const float half = static_cast<float>(kWarningCheckDiameter) * 0.5f;
            AABB box;
            box.min = glm::vec3(center) - glm::vec3(half);
            box.max = glm::vec3(center) + glm::vec3(half);
            std::vector<Entity*> found;
            level.GetEntitiesInBox(box, nullptr, found);
            for (Entity* e : found) {
                if (dynamic_cast<Warden*>(e) && !e->IsRemoved()) return std::nullopt;
            }
        }

        // MC getNearbyPlayers: not a spectator, within 16 blocks, alive —
        // plus the player who set it off.
        std::vector<LivingEntity*> all;
        level.GetPlayers(all);
        std::vector<LivingEntity*> players;
        for (LivingEntity* p : all) {
            if (!p || p->IsSpectator() || !p->IsAlive()) continue;
            const glm::dvec3 d = p->position - center;
            if (glm::dot(d, d) < kPlayerSearchRadius * kPlayerSearchRadius) players.push_back(p);
        }
        if (std::find(players.begin(), players.end(), &triggerPlayer) == players.end()) {
            players.push_back(&triggerPlayer);
        }

        std::vector<WardenSpawnTracker*> trackers;
        for (LivingEntity* p : players) {
            WardenSpawnTracker* t = level.GetWardenSpawnTracker(*p);
            if (!t) continue;
            if (t->OnCooldown()) return std::nullopt;
            trackers.push_back(t);
        }
        if (trackers.empty()) return std::nullopt;

        // Stream.max(comparingInt(getWarningLevel)) — BinaryOperator.maxBy,
        // so the FIRST of equal maxima wins.
        WardenSpawnTracker* highest = trackers.front();
        for (WardenSpawnTracker* t : trackers) {
            if (t->GetWarningLevel() > highest->GetWarningLevel()) highest = t;
        }
        highest->IncreaseWarningLevel();
        const WardenSpawnTracker snapshot = *highest;
        for (WardenSpawnTracker* t : trackers) t->CopyData(snapshot);
        return snapshot.m_warningLevel;
    }

} // namespace Game
