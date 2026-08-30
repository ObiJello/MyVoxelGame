// File: src/client/world/LevelLoadTracker.cpp
#include "LevelLoadTracker.hpp"

#include "ClientChunkManager.hpp"
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

#include <cmath>
#include <string>

namespace Client {

    LevelLoadTracker g_levelLoadTracker;

    namespace {
        // MC LevelLoadTracker.CLIENT_WAIT_TIMEOUT_MS = 30 seconds. The escape
        // hatch: past this the client declares itself loaded regardless, with a
        // warning, rather than leaving the player unable to interact. The
        // server has its own, much shorter 60-tick fail-open on top of this —
        // this one only exists so the client stops waiting on a section that is
        // never going to compile.
        constexpr auto kClientWaitTimeout = std::chrono::seconds(30);
    }

    void LevelLoadTracker::StartClientLoad() {
        m_stage = Stage::WaitingForPlayerChunk;
        m_timeoutAfter = std::chrono::steady_clock::now() + kClientWaitTimeout;
    }

    bool LevelLoadTracker::IsPlayerSectionCompiled(const glm::vec3& playerFeetPos) const {
        const int worldY = static_cast<int>(std::floor(playerFeetPos.y));

        // MC: `!this.level.isOutsideBuildHeight(playerPos.getY()) ? ... : true`
        // — a player outside build height has no section to wait on.
        const int sectionY = (worldY + 64) / Game::Math::SECTION_HEIGHT;
        if (worldY + 64 < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) {
            return true;
        }

        if (!g_clientChunkManager) {
            return false;
        }

        const Game::Math::ChunkPos chunkPos{
            static_cast<int32_t>(std::floor(playerFeetPos.x / 16.0f)),
            static_cast<int32_t>(std::floor(playerFeetPos.z / 16.0f))
        };

        // MC LevelRenderer.isSectionCompiledAndVisible: the render section
        // exists and its mesh is no longer UNCOMPILED. `builtOnce` is our flag
        // for exactly that — it survives a section meshing to nothing, which
        // GPU data does not (empty sections are erased at upload time), so a
        // player standing in mid-air still becomes ready.
        //
        // Divergence: MC additionally requires `getVisibility(...) >= 0.3F`,
        // the section's fade-in alpha. We have no fade-in, so there is nothing
        // to test — the section is either drawn or it is not.
        const auto* info = g_clientChunkManager->GetSectionInfo(chunkPos, sectionY);
        if (!info) return false;
        // MC LevelRenderer.isSectionCompiledAndVisible:1323 tests
        //     sectionMesh.get() != CompiledSectionMesh.UNCOMPILED
        // which is satisfied by EMPTY as well as by a real compile. Both of our
        // terminal states therefore count: builtOnce is "compiled",
        // meshResolvedEmpty is MC's EMPTY.
        //
        // Testing builtOnce alone is what held a mid-air spawn for the whole
        // 30 s timeout — an all-air section is never compiled by design, so it
        // could never satisfy the only condition being checked.
        return info->builtOnce || info->meshResolvedEmpty;
    }

    // Why is the player's section not compiled? Answered from the tracker's
    // own inputs, so it costs nothing until a timeout actually happens.
    //
    // A never-compiled section waits for all eight surrounding columns
    // (ClientChunkManager's compileSections admission test, MC's
    // hasAllNeighbors), so a single column that never arrives holds the player
    // still for the full 30 s with nothing in the log to say which one.
    void LevelLoadTracker::LogWaitDiagnosis(const glm::vec3& playerFeetPos) const {
        if (!g_clientChunkManager) {
            Log::Warning("[LevelLoadTracker]   no client chunk manager");
            return;
        }
        const int worldY   = static_cast<int>(std::floor(playerFeetPos.y));
        const int sectionY = (worldY + 64) / Game::Math::SECTION_HEIGHT;
        const Game::Math::ChunkPos chunkPos{
            static_cast<int32_t>(std::floor(playerFeetPos.x / 16.0f)),
            static_cast<int32_t>(std::floor(playerFeetPos.z / 16.0f))
        };

        const auto* info = g_clientChunkManager->GetSectionInfo(chunkPos, sectionY);
        Log::Warning("[LevelLoadTracker]   waiting on chunk (%d,%d) sectionY %d "
                     "(player %.2f,%.2f,%.2f): sectionInfo=%s builtOnce=%s",
                     chunkPos.x, chunkPos.z, sectionY,
                     playerFeetPos.x, playerFeetPos.y, playerFeetPos.z,
                     info ? "present" : "MISSING",
                     info && info->builtOnce ? "yes" : "NO");
        if (info) {
            Log::Warning("[LevelLoadTracker]   isAllAir=%s meshResolvedEmpty=%s dirty=%s",
                         info->isAllAir ? "yes" : "no",
                         info->meshResolvedEmpty ? "yes" : "NO",
                         info->dirty ? "yes" : "no");
        }

        static constexpr int kDX[8] = { -1, 0, 1, 0, -1, -1, 1, 1 };
        static constexpr int kDZ[8] = { 0, -1, 0, 1, -1, 1, -1, 1 };
        std::string missing;
        for (int i = 0; i < 8; ++i) {
            const Game::Math::ChunkPos n{chunkPos.x + kDX[i], chunkPos.z + kDZ[i]};
            if (!g_clientChunkManager->IsChunkLoaded(n)) {
                missing += " (" + std::to_string(n.x) + "," + std::to_string(n.z) + ")";
            }
        }
        if (missing.empty()) {
            Log::Warning("[LevelLoadTracker]   all 8 neighbour columns ARE loaded — "
                         "the section was admitted but never finished meshing");
        } else {
            Log::Warning("[LevelLoadTracker]   neighbour columns NOT loaded:%s "
                         "— the section is never admitted for meshing while any is missing",
                         missing.c_str());
        }
    }

    void LevelLoadTracker::Tick(const glm::vec3& playerFeetPos) {
        if (m_stage != Stage::WaitingForPlayerChunk) {
            return;
        }

        bool ready = IsPlayerSectionCompiled(playerFeetPos);
        if (!ready && std::chrono::steady_clock::now() > m_timeoutAfter) {
            // MC logs the same thing and lets the player in anyway. We add the
            // WHY: a bare "timed out" turns every occurrence into an
            // archaeology session across the whole chunk pipeline, and the one
            // thing the tracker actually knows — which section it is waiting on
            // and which neighbour column has not arrived — is exactly what
            // narrows it in one line.
            Log::Warning("[LevelLoadTracker] Timed out waiting for the client to load "
                         "chunks, letting the player into the world anyway");
            LogWaitDiagnosis(playerFeetPos);
            ready = true;
        }
        if (!ready) {
            return;
        }

        // MC ClientPacketListener.notifyPlayerLoaded: one packet, no payload.
        // Sent even in singleplayer — the integrated server is reached over a
        // real loopback socket here, so there is no in-memory shortcut to take.
        if (g_networkClient && g_networkClient->IsConnected()) {
            if (auto connection = g_networkClient->GetConnection()) {
                connection->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PlayerLoadedC2S),
                    std::vector<uint8_t>{});
            }
        }

        m_stage = Stage::Ready;
        Log::Info("[LevelLoadTracker] Level ready — player loaded");
    }

} // namespace Client
