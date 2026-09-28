// File: src/common/entity/WardenSpawnTracker.hpp
//
// Port of net.minecraft.world.entity.monster.warden.WardenSpawnTracker (26.3)
// — each player's sculk-shrieker warning level. A shrieker a player sets off
// raises the level (shared with every player within 16 blocks), four
// warnings summon a warden, and the level decays by one every ten minutes.
//
// One per player, held by Server::ServerPlayer (ticked from its tick, saved in
// playerdata as "warden_spawn_tracker", carried across a respawn); common
// code reaches a player's through EntityLevel::GetWardenSpawnTracker.
#pragma once

#include <glm/glm.hpp>

#include <optional>

namespace Game {

    struct EntityLevel;
    class LivingEntity;

    class WardenSpawnTracker {
    public:
        // MC WardenSpawnTracker constants.
        static constexpr int    kMaxWarningLevel                  = 4;
        static constexpr double kPlayerSearchRadius               = 16.0;
        static constexpr int    kWarningCheckDiameter             = 48;
        static constexpr int    kDecreaseWarningLevelEveryInterval = 12000;
        static constexpr int    kWarningLevelIncreaseCooldown     = 200;

        WardenSpawnTracker() = default;
        WardenSpawnTracker(int ticksSinceLastWarning, int warningLevel, int cooldownTicks)
            : m_ticksSinceLastWarning(ticksSinceLastWarning < 0 ? 0 : ticksSinceLastWarning),
              m_warningLevel(warningLevel < 0 ? 0 : warningLevel),
              m_cooldownTicks(cooldownTicks < 0 ? 0 : cooldownTicks) {}

        // MC tick: every 12000 ticks without a warning the level drops by one;
        // the 200-tick increase cooldown counts down.
        void Tick();
        void Reset();

        // MC tryWarn(level, pos, triggerPlayer): the new warning level, or
        // none when a warden is already within 24 blocks or any nearby
        // player's tracker is cooling down.
        static std::optional<int> TryWarn(EntityLevel& level, const glm::ivec3& pos, LivingEntity& triggerPlayer);

        void SetWarningLevel(int warningLevel);
        int  GetWarningLevel() const { return m_warningLevel; }
        int  GetTicksSinceLastWarning() const { return m_ticksSinceLastWarning; }
        int  GetCooldownTicks() const { return m_cooldownTicks; }

    private:
        bool OnCooldown() const { return m_cooldownTicks > 0; }
        void IncreaseWarningLevel();
        void DecreaseWarningLevel() { SetWarningLevel(m_warningLevel - 1); }
        void CopyData(const WardenSpawnTracker& from) {
            m_warningLevel = from.m_warningLevel;
            m_cooldownTicks = from.m_cooldownTicks;
            m_ticksSinceLastWarning = from.m_ticksSinceLastWarning;
        }

        int m_ticksSinceLastWarning = 0;
        int m_warningLevel = 0;
        int m_cooldownTicks = 0;
    };

} // namespace Game
