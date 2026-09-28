// File: src/client/ClientTickRateManager.hpp
//
// Port of MC net.minecraft.world.TickRateManager — the CLIENT half.
//
// Vanilla splits this in two: `TickRateManager` is owned by every Level
// (including ClientLevel) and holds the rate/freeze state, while
// `ServerTickRateManager` extends it with sprinting and the packet broadcasts.
// This engine's server merged both into Server::ServerTickRateManager; this is
// the plain base, mirrored on the client from the two ticking packets.
//
// ── Why the client needs its own copy ──────────────────────────────────────
//
// Client-side mobs are real Game::Mob instances that tick on the client's own
// 20 Hz loop — that is what gives them smooth gravity, walk animation and arm
// swing between position packets instead of a stepped slide. The cost is that
// they keep animating when the SERVER stops, because nothing told the client.
// `/tick freeze` looked broken for exactly that reason: the world stopped and
// the skeletons carried on swinging.
//
// MC's answer is this class plus `isEntityFrozen`, checked in
// ClientLevel.tickEntities. Note what it does NOT freeze: the local player.
// You can still walk around a frozen world in vanilla, and the `!(entity
// instanceof Player)` clause below is the whole reason.
#pragma once

#include <algorithm>
#include <chrono>

namespace Client {

    class ClientTickRateManager {
    public:
        static constexpr float kMinTickRate = 1.0f;

        void SetTickRate(float rate) {
            m_tickRate = std::max(rate, kMinTickRate);
        }
        float TickRate() const { return m_tickRate; }

        void SetFrozen(bool frozen) { m_isFrozen = frozen; }
        bool IsFrozen() const       { return m_isFrozen; }

        void SetFrozenTicksToRun(int ticks) { m_frozenTicksToRun = ticks; }
        int  FrozenTicksToRun() const       { return m_frozenTicksToRun; }
        bool IsSteppingForward() const      { return m_frozenTicksToRun > 0; }

        // MC TickRateManager.tick — runs ONCE per client tick, BEFORE anything
        // reads runsNormally(). A step consumes one queued frozen tick, so
        // `/tick step 5` runs five real ticks and then re-freezes on its own
        // without another packet.
        void Tick() {
            m_runGameElements = !m_isFrozen || m_frozenTicksToRun > 0;
            if (m_frozenTicksToRun > 0) --m_frozenTicksToRun;
        }

        // MC Minecraft.pause folded in: while the WORLD is paused (the
        // server reports every player paused — ServerPausedS2C, or the
        // host's own IntegratedServer::IsPaused) nothing runs, the local
        // player included (Minecraft.java:1959 skips the whole level tick).
        // A pause menu open while somebody else is still playing pauses
        // nothing, exactly as a published vanilla world.
        void SetServerPaused(bool paused) { m_serverPaused = paused; }
        bool IsServerPaused() const       { return m_serverPaused; }
        void SetWorldPaused(bool paused) {
            // The world clock below stops and starts on the transitions.
            if (paused != m_worldPaused) {
                const auto now = std::chrono::steady_clock::now();
                if (paused) m_worldClockPausedAt = now;
                else        m_worldClockPausedTotal += now - m_worldClockPausedAt;
            }
            m_worldPaused = paused;
        }
        bool IsWorldPaused() const        { return m_worldPaused; }

        bool RunsNormally() const { return m_runGameElements && !m_worldPaused; }

        // MC TickRateManager.isEntityFrozen, minus the two clauses this engine
        // cannot express: the local player is ticked separately here rather
        // than through an entity list, and nothing can be ridden, so
        // `countPlayerPassengers()` is always 0. What remains is the whole
        // rule for mobs and dropped items.
        bool IsEntityFrozen() const { return !m_runGameElements || m_worldPaused; }

        // Seconds of WORLD time on the real-time clock: steady time since the
        // process started, minus every world pause (the pause menu, the join
        // transition's hold). For the animations that run on seconds rather
        // than ticks — the gun portals' breathing, open/close, static ping
        // and teleport flash — so they hold still with the rest of a paused
        // world and resume from the same instant, without a jump. Main thread
        // (it is advanced by SetWorldPaused, which the frame loop calls).
        double WorldClockSeconds() const {
            const auto now = std::chrono::steady_clock::now();
            const auto end = m_worldPaused ? m_worldClockPausedAt : now;
            return std::chrono::duration<double>(end - m_worldClockEpoch - m_worldClockPausedTotal).count();
        }

    private:
        float m_tickRate         = 20.0f;
        int   m_frozenTicksToRun = 0;
        bool  m_runGameElements  = true;
        bool  m_isFrozen         = false;
        bool  m_serverPaused     = false;   // what the server last reported
        bool  m_worldPaused      = false;   // serverPaused && (this client's menu is open ||
                                            //   the join transition holds the world — PlatformMain)
        // WorldClockSeconds' state.
        std::chrono::steady_clock::time_point m_worldClockEpoch    = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point m_worldClockPausedAt = m_worldClockEpoch;
        std::chrono::steady_clock::duration   m_worldClockPausedTotal{0};
    };

    // One per process — the client has exactly one level at a time, and MC
    // hangs its manager off that level.
    inline ClientTickRateManager g_clientTickRate;

} // namespace Client
