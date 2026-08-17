// File: src/server/ServerTickRateManager.hpp
//
// Port of MC's TickRateManager + ServerTickRateManager
// (world/TickRateManager.java, server/ServerTickRateManager.java), merged into
// one class. Vanilla splits them because the CLIENT has a TickRateManager too
// and only the server half owns sprinting; this engine's client has no tick-rate
// concept at all (see the note at the bottom), so the split would buy nothing.
//
// This is what /tick drives, and what the server loop reads for its budget.
//
// Three states, and they compose:
//   normal    — nanosecondsPerTick apart, game elements run
//   frozen    — ticks still happen (packets, chunk streaming) but game elements
//               are skipped, so the world stops evolving. `step` runs a fixed
//               number of frozen ticks WITH game elements.
//   sprinting — tick budget drops to zero and the loop runs flat out for N
//               ticks, then reports how fast it managed.
#pragma once

#include <cstdint>

namespace Server {

    class IntegratedServer;
    class ServerConnection;

    class ServerTickRateManager {
    public:
        // MC TickRateManager.MIN_TICKRATE. The upper bound (10000) is NOT here:
        // vanilla enforces it in the command's `floatArg(1.0F, 10000.0F)`
        // argument type, not in the manager, so a programmatic caller can
        // exceed it. TickCommand reproduces that split.
        static constexpr float  kMinTickRate      = 1.0f;
        static constexpr float  kDefaultTickRate  = 20.0f;
        static constexpr int64_t kNanosPerSecond      = 1000000000LL;
        static constexpr int64_t kNanosPerMillisecond = 1000000LL;
        static constexpr int64_t kMillisPerSecond     = 1000LL;

        explicit ServerTickRateManager(IntegratedServer& server) : m_server(server) {}

        // ── Rate ────────────────────────────────────────────────────────────
        float   tickrate() const { return m_tickrate; }
        int64_t nanosecondsPerTick() const { return m_nanosecondsPerTick; }
        float   millisecondsPerTick() const {
            return static_cast<float>(m_nanosecondsPerTick) /
                   static_cast<float>(kNanosPerMillisecond);
        }
        void setTickRate(float rate);

        // ── Freeze / step ───────────────────────────────────────────────────
        bool isFrozen() const { return m_isFrozen; }
        // Out of line, and not a plain setter any more: MC's
        // ServerTickRateManager overrides setFrozen purely to broadcast the new
        // state, and every caller here (including the sprint path) goes through
        // it for that reason.
        void setFrozen(bool frozen);

        // True when game elements should run this tick. MC gates world
        // simulation on this while still ticking networking, which is why a
        // frozen server stays joinable and keeps streaming chunks.
        bool runsNormally() const { return m_runGameElements; }
        bool isSteppingForward() const { return m_frozenTicksToRun > 0; }
        int  frozenTicksToRun() const { return m_frozenTicksToRun; }

        // MC TickRateManager.tick — called once per tick, BEFORE the tick body,
        // and it is what makes `step` count down.
        void tick() {
            m_runGameElements = !m_isFrozen || m_frozenTicksToRun > 0;
            if (m_frozenTicksToRun > 0) --m_frozenTicksToRun;
        }

        // Both return false when there was nothing to do, which is what the
        // command turns into its `.fail` message.
        bool stepGameIfPaused(int ticks);
        bool stopStepping();

        // ── Sprint ──────────────────────────────────────────────────────────
        bool isSprinting() const { return m_scheduledCurrentSprintTicks > 0; }
        // Returns true if this interrupted a sprint already in progress — the
        // command reports that separately before announcing the new one.
        bool requestGameToSprint(int ticks);
        bool stopSprinting();

        // Called by the server loop. `checkShouldSprintThisTick` decrements the
        // sprint counter and answers "run this tick with a zero budget";
        // `endTickWork` closes the timing window it opened.
        bool checkShouldSprintThisTick();
        void endTickWork();

        // ── Tick-time samples (MC MinecraftServer.tickTimesNanos) ───────────
        // A 100-entry ring, the same size vanilla uses, because /tick query
        // reports P50/P95/P99 over exactly that window.
        static constexpr int kSampleCount = 100;
        void    recordTickTime(int64_t nanos);
        int64_t averageTickTimeNanos() const;
        // Copies the ring out for sorting. Caller owns the buffer.
        void    copyTickTimes(int64_t (&out)[kSampleCount]) const;

        // MC ServerTickRateManager.updateJoiningPlayer — a client that connects
        // into an already-frozen world has to learn about it, or its mobs keep
        // animating until somebody happens to toggle the freeze.
        void updateJoiningPlayer(ServerConnection& connection) const;

    private:
        // MC's two broadcasts. Split the same way vanilla splits them: `/tick
        // step` must push only the step count, without restating a freeze flag
        // that a concurrent `/tick unfreeze` may already have changed.
        void updateStateToClients() const;
        void updateStepTicks() const;

        // MC's finishTickSprint: reports the achieved rate, restores the
        // pre-sprint freeze state.
        void finishTickSprint();

        IntegratedServer& m_server;

        float   m_tickrate           = kDefaultTickRate;
        int64_t m_nanosecondsPerTick = kNanosPerSecond / 20;
        int     m_frozenTicksToRun   = 0;
        bool    m_runGameElements    = true;
        bool    m_isFrozen           = false;

        int64_t m_remainingSprintTicks        = 0;
        int64_t m_sprintTickStartTime         = 0;
        int64_t m_sprintTimeSpend             = 0;
        int64_t m_scheduledCurrentSprintTicks = 0;
        bool    m_previousIsFrozen            = false;

        int64_t m_tickTimes[kSampleCount] = {};
        int     m_tickTimeIndex = 0;
        int     m_tickTimeFilled = 0;
    };

    // PARTIALLY PORTED. MC's two ticking packets ARE sent now — see
    // updateStateToClients / updateStepTicks above — and the client mirrors
    // them into Client::g_clientTickRate.
    //
    // What the client does with them is narrower than vanilla: it gates ENTITY
    // ticking (mobs, dropped items) on `runsNormally`, which is what `/tick
    // freeze` visibly needs, since client-side mobs run their own animation
    // clock and would otherwise keep swinging in a stopped world. It does NOT
    // yet retime its own 20 TPS loop from `tickRate`: movement prediction,
    // mining progress and block-break animation are all built on a fixed 50 ms
    // client tick, and re-basing them is a separate piece of work. So
    // `/tick rate` still changes server simulation speed while the client
    // predicts at 20 — but `/tick freeze` and `/tick step` are now correct on
    // both sides, and the local player stays movable exactly as in vanilla
    // (TickRateManager.isEntityFrozen exempts players).

} // namespace Server
