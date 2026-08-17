// File: src/client/world/LevelLoadTracker.cpp
#include "LevelLoadTracker.hpp"

#include "ClientChunkManager.hpp"
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

#include <cmath>

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
        return info != nullptr && info->builtOnce;
    }

    void LevelLoadTracker::Tick(const glm::vec3& playerFeetPos) {
        if (m_stage != Stage::WaitingForPlayerChunk) {
            return;
        }

        bool ready = IsPlayerSectionCompiled(playerFeetPos);
        if (!ready && std::chrono::steady_clock::now() > m_timeoutAfter) {
            // MC logs the same thing and lets the player in anyway.
            Log::Warning("[LevelLoadTracker] Timed out waiting for the client to load "
                         "chunks, letting the player into the world anyway");
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
