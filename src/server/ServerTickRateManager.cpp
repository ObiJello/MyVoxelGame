// File: src/server/ServerTickRateManager.cpp
#include "ServerTickRateManager.hpp"

#include "IntegratedServer.hpp"
#include "network/NetworkServer.hpp"
#include "network/ServerConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace Server {

    namespace {
        int64_t NowNanos() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    } // namespace

    // ── MC ServerTickRateManager's two broadcasts ───────────────────────────

    void ServerTickRateManager::updateStateToClients() const {
        NetworkServer* net = m_server.GetNetworkServer();
        if (!net) return;   // pre-startup / shutdown; nothing is listening

        const Network::TickingStateS2CPacket packet(m_tickrate, m_isFrozen);
        net->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::TickingStateS2C),
                             Network::Serialization::Serialize(packet));
    }

    void ServerTickRateManager::updateStepTicks() const {
        NetworkServer* net = m_server.GetNetworkServer();
        if (!net) return;

        const Network::TickingStepS2CPacket packet(m_frozenTicksToRun);
        net->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::TickingStepS2C),
                             Network::Serialization::Serialize(packet));
    }

    void ServerTickRateManager::updateJoiningPlayer(ServerConnection& connection) const {
        const Network::TickingStateS2CPacket state(m_tickrate, m_isFrozen);
        connection.SendPacket(static_cast<uint8_t>(Network::PacketId::TickingStateS2C),
                              Network::Serialization::Serialize(state));

        const Network::TickingStepS2CPacket step(m_frozenTicksToRun);
        connection.SendPacket(static_cast<uint8_t>(Network::PacketId::TickingStepS2C),
                              Network::Serialization::Serialize(step));
    }

    void ServerTickRateManager::setFrozen(bool frozen) {
        m_isFrozen = frozen;
        updateStateToClients();
    }

    // MC TickRateManager.setTickRate:
    //     this.tickrate = Math.max(rate, 1.0F);
    //     this.nanosecondsPerTick = (long)(NANOSECONDS_PER_SECOND / this.tickrate);
    void ServerTickRateManager::setTickRate(float rate) {
        m_tickrate = std::max(rate, kMinTickRate);
        m_nanosecondsPerTick = static_cast<int64_t>(
            static_cast<double>(kNanosPerSecond) / static_cast<double>(m_tickrate));
        updateStateToClients();
        // MC also calls server.onTickRateChanged() here, whose only job is to
        // pull the next autosave forward (MinecraftServer.computeNextAutosaveInterval
        // scales with the tick rate). This engine's chunk saving is not on a
        // tick-count schedule, so there is nothing to recompute.
    }

    bool ServerTickRateManager::stepGameIfPaused(int ticks) {
        if (!isFrozen()) return false;   // MC: stepping requires a frozen game
        m_frozenTicksToRun = ticks;
        updateStepTicks();
        return true;
    }

    bool ServerTickRateManager::stopStepping() {
        if (m_frozenTicksToRun > 0) {
            m_frozenTicksToRun = 0;
            updateStepTicks();
            return true;
        }
        return false;
    }

    bool ServerTickRateManager::requestGameToSprint(int ticks) {
        const bool interrupted = m_remainingSprintTicks > 0;
        m_sprintTimeSpend = 0;
        m_scheduledCurrentSprintTicks = ticks;
        m_remainingSprintTicks = ticks;
        m_previousIsFrozen = isFrozen();
        setFrozen(false);   // a sprint always runs, even from a frozen game
        return interrupted;
    }

    bool ServerTickRateManager::stopSprinting() {
        if (m_remainingSprintTicks > 0) {
            finishTickSprint();
            return true;
        }
        return false;
    }

    // MC ServerTickRateManager.checkShouldSprintThisTick. Note the order: a
    // sprint that has run out finishes HERE, on the tick after its last, which
    // is what makes the report land once rather than every tick.
    bool ServerTickRateManager::checkShouldSprintThisTick() {
        if (!m_runGameElements) return false;
        if (m_remainingSprintTicks > 0) {
            m_sprintTickStartTime = NowNanos();
            --m_remainingSprintTicks;
            return true;
        }
        finishTickSprint();
        return false;
    }

    void ServerTickRateManager::endTickWork() {
        m_sprintTimeSpend += NowNanos() - m_sprintTickStartTime;
    }

    void ServerTickRateManager::finishTickSprint() {
        const int64_t completedTicks = m_scheduledCurrentSprintTicks - m_remainingSprintTicks;
        const double millisecondsToComplete =
            std::max(1.0, static_cast<double>(m_sprintTimeSpend)) /
            static_cast<double>(kNanosPerMillisecond);
        const int ticksPerSecond = static_cast<int>(
            static_cast<double>(kMillisPerSecond * completedTicks) / millisecondsToComplete);
        const double msPerTick = (completedTicks == 0)
            ? static_cast<double>(millisecondsPerTick())
            : millisecondsToComplete / static_cast<double>(completedTicks);

        m_scheduledCurrentSprintTicks = 0;
        m_sprintTimeSpend = 0;
        m_remainingSprintTicks = 0;

        // MC sends commands.tick.sprint.report through the server's own command
        // source, which broadcasts to ops and the console. A sprint finishes
        // asynchronously — the player who started it may have disconnected — so
        // there is no connection to answer here; the log is the analogue, and
        // it reaches both latest.log and the in-game console.
        char msPerTickStr[32];
        std::snprintf(msPerTickStr, sizeof(msPerTickStr), "%.2f", msPerTick);
        Log::Info("Sprint completed with %d ticks per second, or %s ms per tick",
                  ticksPerSecond, msPerTickStr);

        setFrozen(m_previousIsFrozen);
    }

    void ServerTickRateManager::recordTickTime(int64_t nanos) {
        m_tickTimes[m_tickTimeIndex] = nanos;
        m_tickTimeIndex = (m_tickTimeIndex + 1) % kSampleCount;
        if (m_tickTimeFilled < kSampleCount) ++m_tickTimeFilled;
    }

    int64_t ServerTickRateManager::averageTickTimeNanos() const {
        if (m_tickTimeFilled == 0) return 0;
        int64_t total = 0;
        for (int i = 0; i < m_tickTimeFilled; ++i) total += m_tickTimes[i];
        return total / m_tickTimeFilled;
    }

    void ServerTickRateManager::copyTickTimes(int64_t (&out)[kSampleCount]) const {
        // Unfilled slots stay 0, matching MC — its array is allocated at full
        // size and starts zeroed, so an early /tick query reports optimistic
        // percentiles there too.
        for (int i = 0; i < kSampleCount; ++i) out[i] = m_tickTimes[i];
    }

} // namespace Server
