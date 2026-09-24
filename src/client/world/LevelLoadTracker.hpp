// File: src/client/world/LevelLoadTracker.hpp
#pragma once

#include <chrono>
#include <glm/glm.hpp>

namespace Client {

    // Port of MC's client-side LevelLoadTracker
    // (net/minecraft/client/multiplayer/LevelLoadTracker.java), reduced to the
    // half that matters to the server: deciding when THIS client's level is
    // ready and telling the server so.
    //
    // Why this exists at all: the server refuses interactions until it knows
    // the client is loaded (PlayerSession::HasClientLoaded). MC drives that
    // from the client rather than from any server-side queue, because only the
    // client knows whether the section it is standing in has actually been
    // compiled. Without this the server just waits out its 60-tick timeout.
    //
    // MC has three client states — WaitingForServer, WaitingForPlayerChunk,
    // ClientLevelReady — and leaves the first when the server sends the
    // LEVEL_CHUNKS_LOAD_START game event. We have no such event, so the
    // WaitingForServer stage is folded away and the timeout clock starts at
    // StartClientLoad. The consequence is only that the 30-second escape hatch
    // begins slightly earlier than MC's would.
    class LevelLoadTracker {
    public:
        // MC LevelLoadTracker.startClientLoad — called when the client enters a
        // level: once per session, and again after a respawn.
        // `closeDelayMs`: MC LevelLoadTracker(closeDelayMs) — 500 for a
        // world created by this launch (Minecraft.doWorldLoad), 0 otherwise;
        // the level counts as ready that long after the section compiled.
        void StartClientLoad(int closeDelayMs = 0);

        // MC ClientPacketListener.tick's `levelLoadTracker.tickClientLoad()` +
        // `notifyPlayerLoaded()`. Sends PlayerLoadedC2S exactly once per load,
        // the moment the player's own section is compiled (or the timeout
        // expires). `playerFeetPos` is the local player's world position.
        void Tick(const glm::vec3& playerFeetPos);

        // True once the packet has gone out for the current load.
        bool IsLoaded() const { return m_stage == Stage::Ready; }

    private:
        enum class Stage { Idle, WaitingForPlayerChunk, Ready };

        // MC WaitingForPlayerChunk.isReady: the section containing the player
        // has been compiled, or the player is outside build height.
        bool IsPlayerSectionCompiled(const glm::vec3& playerFeetPos) const;
        // Explains a timeout: which section, and which neighbour column is missing.
        void LogWaitDiagnosis(const glm::vec3& playerFeetPos) const;

        Stage m_stage = Stage::Idle;
        int   m_closeDelayMs = 0;
        std::chrono::steady_clock::time_point m_readyAt{};      // ClientLevelReady.readyAt
        std::chrono::steady_clock::time_point m_timeoutAfter{};
        std::chrono::steady_clock::time_point m_startedAt{};   // StartClientLoad, for the log
    };

    // One per client process, like MC's (it hangs off the connection listener,
    // which is likewise single).
    extern LevelLoadTracker g_levelLoadTracker;

} // namespace Client
