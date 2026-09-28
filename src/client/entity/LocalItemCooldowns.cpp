// File: src/client/entity/LocalItemCooldowns.cpp
#include "client/entity/LocalItemCooldowns.hpp"

#include "common/entity/Item.hpp"
#include "common/entity/ItemCooldowns.hpp"
#include "common/network/packets/game/CooldownS2CPacket.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace Client::LocalItemCooldowns {

    namespace {
        using Clock = std::chrono::steady_clock;

        struct State {
            std::mutex           mutex;
            Game::ItemCooldowns  cooldowns;
            Clock::time_point    lastTick = Clock::now();
        };

        State& Get() {
            static State s;
            return s;
        }

        // The fraction of a 50 ms tick elapsed since the last Tick — MC's
        // getGameTimeDeltaPartialTick(true), clamped so a stalled frame does
        // not run the sweep ahead of the tick that has not happened yet.
        float PartialTick(const State& s) {
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s.lastTick).count();
            return static_cast<float>(std::clamp(ms / 50.0, 0.0, 1.0));
        }
    } // namespace

    void OnPacket(const Network::CooldownS2CPacket& packet) {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (packet.duration == 0) {
            s.cooldowns.RemoveCooldown(packet.cooldownGroup);
        } else {
            s.cooldowns.AddCooldown(packet.cooldownGroup, packet.duration);
        }
    }

    void Tick() {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.cooldowns.Tick();
        s.lastTick = Clock::now();
    }

    float GetCooldownPercent(const Game::ItemStack& item) {
        if (item.IsEmpty()) return 0.0f;
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.cooldowns.GetCooldownPercent(item, PartialTick(s));
    }

    bool IsOnCooldown(const Game::ItemStack& item) {
        if (item.IsEmpty()) return false;
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.cooldowns.IsOnCooldown(item);
    }

    void Clear() {
        State& s = Get();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.cooldowns.Clear();
    }

} // namespace Client::LocalItemCooldowns
