// File: src/client/world/LevelLoadTracker.hpp
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

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

        // Where the viewer looks from, for the portal-view half of the
        // readiness test (below).
        struct ViewInfo {
            glm::dvec3 eye{0.0};
            glm::vec3  forward{0.0f, 0.0f, 1.0f};   // unit
        };

        // MC ClientPacketListener.tick's `levelLoadTracker.tickClientLoad()` +
        // `notifyPlayerLoaded()`. Sends PlayerLoadedC2S exactly once per load,
        // the moment the player's own section is compiled (or the timeout
        // expires). `playerFeetPos` is the local player's world position.
        //
        // Beyond MC: with `view`, the portals in view close to the player
        // count too. Once the player's section is ready, the tracker also
        // waits — at most kPortalViewWait — for the far-side sections
        // just behind each portal within kPortalViewRange blocks that the
        // view faces (immersive surfaces, gun pairs; not global surfaces or
        // mirrors), in whichever level they lie, to be compiled and 30 %
        // faded in the same way. The hand-over then reveals a filled portal
        // view, not one that fills in a second later.
        void Tick(const glm::vec3& playerFeetPos, const ViewInfo* view = nullptr);

        // True once the packet has gone out for the current load.
        bool IsLoaded() const { return m_stage == Stage::Ready; }
        // When the current load became ready (the hand-over); epoch while not.
        std::chrono::steady_clock::time_point LoadedAt() const { return m_loadedAt; }

    private:
        enum class Stage { Idle, WaitingForPlayerChunk, Ready };

        // MC WaitingForPlayerChunk.isReady: the section containing the player
        // has been compiled, or the player is outside build height.
        bool IsPlayerSectionCompiled(const glm::vec3& playerFeetPos) const;
        // Explains a timeout: which section, and which neighbour column is missing.
        void LogWaitDiagnosis(const glm::vec3& playerFeetPos) const;

        // The portal-view half (see Tick).
        struct PortalViewTarget {
            Game::DimensionId    dimension = Game::DimensionId::Overworld;
            Game::Math::ChunkPos chunk{0, 0};
            int                  sectionY = 0;
            uint32_t             portalId = 0;   // 0: a vanilla gun pair
            uint32_t             viewKey  = 0;   // which portal view (one far area) this belongs to
        };
        // `log`: say, per portal in range, whether it was taken and why not.
        static void CollectPortalViewTargets(const ViewInfo& view, std::vector<PortalViewTarget>& out, bool log);
        // Per portal route (MeshPriority fields): how much of its far area
        // is loaded and meshed — logged once at the hand-over.
        static void LogRouteCoverage();
        static bool IsTargetShown(const PortalViewTarget& target);
        // Why a target is not shown yet (a timeout's diagnosis).
        static std::string DescribeTargetWait(const PortalViewTarget& target);
        // True when the portal views are ready or their wait is over.
        bool PortalViewsReady(const ViewInfo* view);
        std::vector<PortalViewTarget> m_portalTargets;
        std::chrono::steady_clock::time_point m_playerSectionReadyAt{};   // portal wait starts here
        bool m_portalDecisionsLogged = false;
        std::chrono::steady_clock::time_point m_loadedAt{};

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
