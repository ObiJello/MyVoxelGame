// File: src/client/world/LevelLoadTracker.cpp
#include "LevelLoadTracker.hpp"
#include "client/renderer/mesh/SectionFade.hpp"
#include "platform/GameDirectory.hpp"

#include "ClientChunkManager.hpp"
#include "ClientLevel.hpp"
#include "ClientWorkerPool.hpp"
#include "client/renderer/mesh/MeshPriority.hpp"
#include "common/core/Features.hpp"
#include "common/portal/PortalRoute.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "client/portal/ClientImmersivePortals.hpp"
#endif
#if ENABLE_PORTAL_GUN
#include "client/portal/ClientPortalManager.hpp"
#endif
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketRegistry.hpp"

#include <algorithm>
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

        // The portal-view half of readiness (LevelLoadTracker::Tick): the
        // portals it waits for are within this many blocks of the eye —
        // close enough that their view is a large part of the first frame —
        // and it waits at most this long past the player's own section.
        // Past it the hand-over happens as it always did and the view fills
        // in behind it (with the portal-aware mesh order, promptly).
        constexpr double kPortalViewRange = 24.0;
        constexpr auto   kPortalViewWait  = std::chrono::milliseconds(3000);
        // "In view": the portal's nearest point or centre within ~60° of
        // the view direction (the join's 85° panorama lens and the game's
        // default both span more than that), or the eye practically at it.
        constexpr double kViewConeCos      = 0.5;
        constexpr double kAlwaysInViewDist = 2.0;
        constexpr size_t kMaxPortalTargets = 16;

        // MC LevelRenderer.isSectionCompiledAndVisible for one section of
        // one level — see IsPlayerSectionCompiled for each half.
        bool SectionShown(const ClientChunkManager& chunks, Game::Math::ChunkPos chunkPos, int sectionY) {
            const auto* info = chunks.GetSectionInfo(chunkPos, sectionY);
            if (!info) return false;
            if (!(info->builtOnce || info->meshResolvedEmpty)) return false;
            const int32_t fadeMs = static_cast<int32_t>(Platform::g_gameSettings.GetChunkFadeInTime() * 1000.0f);
            return ::Render::SectionFade::Visibility(info->fadeStartMs, ::Render::SectionFade::NowMs(), fadeMs) >= 0.3f;
        }
    }

    void LevelLoadTracker::StartClientLoad(int closeDelayMs) {
        m_stage        = Stage::WaitingForPlayerChunk;
        m_closeDelayMs = closeDelayMs;
        m_readyAt      = {};
        m_startedAt    = std::chrono::steady_clock::now();
        m_timeoutAfter = m_startedAt + kClientWaitTimeout;
        m_playerSectionReadyAt = {};
        m_portalTargets.clear();
        m_portalDecisionsLogged = false;
        // The first mesh pass of each level whose order a portal changed
        // logs it once ([MeshOrder], ClientChunkManager).
        ::Render::MeshPriority::ArmOrderLog();
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
        if (!(info->builtOnce || info->meshResolvedEmpty)) return false;
        // MC's second half: `renderSection.getVisibility(now, fade) >= 0.3F`
        // — the section is not just compiled but at least 30 % faded in
        // (an all-air section never uploads and reads as fully visible).
        return SectionShown(*g_clientChunkManager, chunkPos, sectionY);
    }

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

    void LevelLoadTracker::CollectPortalViewTargets(const ViewInfo& view, std::vector<PortalViewTarget>& out,
                                                    bool log) {
        out.clear();
        if (!ClientLevels::HasSession()) return;
        const Game::DimensionId here = ClientLevels::ActiveDimension();
        const glm::dvec3 eye = view.eye;
        const glm::dvec3 forward(view.forward);

        // In view if any of the portal's nearest point, centre or corners
        // lies in the cone — a portal half on screen is a portal in view.
        auto inView = [&](const glm::dvec3* points, int count, double entry) {
            if (entry < kAlwaysInViewDist) return true;
            for (int i = 0; i < count; ++i) {
                const glm::dvec3 d = points[i] - eye;
                const double len = glm::length(d);
                if (len < 1e-6 || glm::dot(d / len, forward) >= kViewConeCos) return true;
            }
            return false;
        };
        // One view per far AREA: two records opening onto the same place (a
        // bi-faced frame, a pair seen twice) wait as one.
        struct Area { Game::DimensionId dimension; glm::dvec3 farPoint; uint32_t key; };
        std::vector<Area> areas;
        auto areaKey = [&](Game::DimensionId dimension, const glm::dvec3& farPoint) -> uint32_t {
            for (const Area& a : areas) {
                if (a.dimension == dimension && glm::length(a.farPoint - farPoint) <= 16.0) return a.key;
            }
            areas.push_back({dimension, farPoint, static_cast<uint32_t>(areas.size() + 1)});
            return areas.back().key;
        };
        // The sections the view opens into: just past where it comes out,
        // and one section further along the far side's depth.
        auto push = [&](Game::DimensionId dimension, const glm::dvec3& farPoint, const glm::dvec3& inward,
                        uint32_t portalId) {
            const uint32_t key = areaKey(dimension, farPoint);
            for (const double depth : { 1.0, 17.0 }) {
                if (out.size() >= kMaxPortalTargets) return;
                const glm::dvec3 p = farPoint + inward * depth;
                const int by = static_cast<int>(std::floor(p.y));
                const int sectionY = (by + 64) >> 4;
                if (by + 64 < 0 || sectionY >= Game::Math::SECTIONS_PER_CHUNK) continue;
                PortalViewTarget t;
                t.dimension = dimension;
                t.chunk     = Game::Math::ChunkPos{ static_cast<int>(std::floor(p.x)) >> 4,
                                                    static_cast<int>(std::floor(p.z)) >> 4 };
                t.sectionY  = sectionY;
                t.portalId  = portalId;
                t.viewKey   = key;
                const bool dup = std::any_of(out.begin(), out.end(), [&](const PortalViewTarget& o) {
                    return o.dimension == t.dimension && o.chunk == t.chunk && o.sectionY == t.sectionY;
                });
                if (!dup) out.push_back(t);
            }
        };
        // A far side right beside the player (same level, within the wait's
        // own range of the eye) is part of the player's own surroundings:
        // it neither needs nor gets a portal wait of its own.
        auto nearPlayer = [&](Game::DimensionId dimension, const glm::dvec3& farPoint) {
            return dimension == here && glm::length(farPoint - eye) <= kPortalViewRange;
        };
        auto decision = [&](uint32_t id, const char* what, const glm::dvec3& farPoint, double entry) {
            if (!log) return;
            Log::Info("[LevelLoadTracker] portal #%u %.1f blocks away, far (%.0f,%.0f,%.0f): %s",
                      id, entry, farPoint.x, farPoint.y, farPoint.z, what);
        };

#if ENABLE_IMMERSIVE_PORTALS
        // Global surfaces (the wrap border, the stack seams) are world-sized:
        // what they show is not "a portal view" to wait for. A mirror shows
        // this level beside the player, which the player's own wait covers.
        ClientLevels::Active().Portals().ForEach([&](const Game::Immersive::Portal& p) {
            if (p.Has(Game::Immersive::PortalFlag::Global) || p.IsMirror()) return;
            const auto route = Game::PortalRoute::ThroughImmersive(p, eye, kPortalViewRange);
            if (!route) return;
            if (nearPlayer(route->dimension, route->farPoint)) {
                decision(p.id, "far side beside the player, not waited for", route->farPoint, route->entryCost);
                return;
            }
            glm::dvec3 points[6];
            p.Corners(points);
            points[4] = p.InverseTransformPoint(route->farPoint);
            points[5] = p.origin;
            if (!inView(points, 6, route->entryCost)) {
                decision(p.id, "not in view, not waited for", route->farPoint, route->entryCost);
                return;
            }
            decision(p.id, "in view, waited for", route->farPoint, route->entryCost);
            push(route->dimension, route->farPoint, p.ContentDirection(), p.id);
        });
#endif
#if ENABLE_PORTAL_GUN
        // Vanilla gun pairs (an immersive-mode pair is a surface above).
        GetClientPortalManager().ForEachPair([&](uint64_t, const ClientPortalPair& pair) {
            if (!pair.blue.active || !pair.orange.active) return;
            if (pair.blue.immersive || pair.orange.immersive) return;
            const ClientPortal* ends[2][2] = { { &pair.blue, &pair.orange }, { &pair.orange, &pair.blue } };
            for (const auto& end : ends) {
                const ClientPortal& from = *end[0];
                const ClientPortal& to   = *end[1];
                if (from.dimension != here) continue;
                const auto route = Game::PortalRoute::ThroughGun(from.origin, glm::dvec3(from.normal), to.dimension,
                                                                 to.origin, glm::dvec3(to.normal), eye,
                                                                 kPortalViewRange);
                if (!route) continue;
                if (nearPlayer(route->dimension, route->farPoint)) {
                    decision(0, "gun pair: far side beside the player, not waited for", route->farPoint,
                             route->entryCost);
                    continue;
                }
                const glm::dvec3 up(from.upDir);
                const glm::dvec3 points[3] = { from.origin, from.origin + up, from.origin - up };
                if (!inView(points, 3, route->entryCost)) {
                    decision(0, "gun pair: not in view, not waited for", route->farPoint, route->entryCost);
                    continue;
                }
                decision(0, "gun pair: in view, waited for", route->farPoint, route->entryCost);
                push(route->dimension, route->farPoint, glm::dvec3(to.normal), 0);
            }
        });
#endif
    }

    bool LevelLoadTracker::IsTargetShown(const PortalViewTarget& target) {
        const ClientLevel* level = ClientLevels::Get(target.dimension);
        const ClientChunkManager* chunks = level ? level->Chunks() : nullptr;
        return chunks && SectionShown(*chunks, target.chunk, target.sectionY);
    }

    void LevelLoadTracker::LogRouteCoverage() {
        auto* pool = Threading::g_clientWorkerPool.get();
        if (!pool) return;
        constexpr int kRadius = 2;   // the route's 5x5 columns around where its view comes out
        for (const Game::DimensionId dimension : Game::kAllDimensions) {
            const ::Render::MeshPriority::Field field = pool->GetMeshPriorityField(dimension);
            if (field.count == 0) continue;
            const ClientLevel* level = ClientLevels::Get(dimension);
            const ClientChunkManager* chunks = level ? level->Chunks() : nullptr;
            for (int i = 0; i < field.count; ++i) {
                const auto& r = field.routes[i];
                const Game::Math::ChunkPos c{ static_cast<int>(std::floor(r.farPoint.x)) >> 4,
                                              static_cast<int>(std::floor(r.farPoint.z)) >> 4 };
                const int by = static_cast<int>(std::floor(r.farPoint.y));
                const int sectionY = std::clamp((by + 64) >> 4, 0, Game::Math::SECTIONS_PER_CHUNK - 1);
                int loaded = 0, meshed = 0, total = 0;
                for (int dz = -kRadius; dz <= kRadius; ++dz) {
                    for (int dx = -kRadius; dx <= kRadius; ++dx) {
                        ++total;
                        const Game::Math::ChunkPos p{c.x + dx, c.z + dz};
                        if (!chunks || !chunks->IsChunkLoaded(p)) continue;
                        ++loaded;
                        const auto* info = chunks->GetSectionInfo(p, sectionY);
                        if (info && (info->builtOnce || info->meshResolvedEmpty)) ++meshed;
                    }
                }
                Log::Info("[LevelLoadTracker] route portal #%u -> %s far chunk (%d,%d) sectionY %d, %.1f blocks to "
                          "it: %d/%d columns loaded, %d/%d sections meshed at hand-over",
                          r.portalId, std::string(Game::DimensionName(dimension)).c_str(), c.x, c.z, sectionY,
                          r.entryCost, loaded, total, meshed, total);
            }
        }
    }

    bool LevelLoadTracker::PortalViewsReady(const ViewInfo* view) {
        if (!view) return true;
        const auto now = std::chrono::steady_clock::now();
        if (m_playerSectionReadyAt == std::chrono::steady_clock::time_point{}) m_playerSectionReadyAt = now;

        // Re-collected every tick while waiting: a portal record rides
        // behind its chunk, so one a little further out can arrive after
        // the player's section is ready. The decisions are logged once.
        CollectPortalViewTargets(*view, m_portalTargets, !m_portalDecisionsLogged);
        m_portalDecisionsLogged = true;
        if (m_portalTargets.empty()) return true;

        // Per portal view: EACH in-view portal's own first sections must be
        // shown — one portal's far side being ready says nothing of the
        // next one's.
        struct ViewState { uint32_t key; uint32_t portalId; Game::DimensionId dimension;
                           Game::Math::ChunkPos chunk; int shown; int total; };
        std::vector<ViewState> views;
        for (const PortalViewTarget& t : m_portalTargets) {
            auto it = std::find_if(views.begin(), views.end(), [&](const ViewState& v) { return v.key == t.viewKey; });
            if (it == views.end()) {
                views.push_back({t.viewKey, t.portalId, t.dimension, t.chunk, 0, 0});
                it = views.end() - 1;
            }
            ++it->total;
            if (IsTargetShown(t)) ++it->shown;
        }
        const bool allShown = std::all_of(views.begin(), views.end(),
                                          [](const ViewState& v) { return v.shown == v.total; });
        const float waited = std::chrono::duration<float>(now - m_playerSectionReadyAt).count();
        const bool overdue = now >= m_playerSectionReadyAt + kPortalViewWait;
        if (!allShown && !overdue) return false;

        std::string perView;
        for (const ViewState& v : views) {
            perView += " [portal #" + std::to_string(v.portalId) + " " +
                       std::string(Game::DimensionName(v.dimension)) + " (" + std::to_string(v.chunk.x) + "," +
                       std::to_string(v.chunk.z) + ") " + std::to_string(v.shown) + "/" +
                       std::to_string(v.total) + "]";
        }
        if (allShown) {
            Log::Info("[LevelLoadTracker] %zu portal view(s) ready, %.2f s after the player's own section:%s",
                      views.size(), waited, perView.c_str());
        } else {
            Log::Warning("[LevelLoadTracker] portal views not all shown after %.2f s, not waiting longer:%s",
                         waited, perView.c_str());
        }
        return true;
    }

    void LevelLoadTracker::Tick(const glm::vec3& playerFeetPos, const ViewInfo* view) {
        if (m_stage != Stage::WaitingForPlayerChunk) {
            return;
        }

        bool ready = IsPlayerSectionCompiled(playerFeetPos);
        bool timedOut = false;
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
            timedOut = true;
        }
        if (!ready) {
            return;
        }
        // Beyond MC: the portal views in front of the player, bounded (see
        // Tick's comment). Not after the 30 s escape hatch.
        if (!timedOut && !PortalViewsReady(view)) {
            return;
        }
        // MC ClientLevelReady(readyAt) + isLevelReady: ready `closeDelayMs`
        // after the section compiled — 500 ms for a brand-new world.
        const auto now = std::chrono::steady_clock::now();
        if (m_readyAt == std::chrono::steady_clock::time_point{}) m_readyAt = now;
        if (now < m_readyAt + std::chrono::milliseconds(m_closeDelayMs)) return;

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
        // How far each portal route's far area got by now (its 5x5 around
        // where the view comes out): loaded, meshed.
        LogRouteCoverage();
        Log::Info("[LevelLoadTracker] Level ready — player loaded, %.2f s after the load began",
                  std::chrono::duration<float>(std::chrono::steady_clock::now() - m_startedAt).count());
    }

} // namespace Client
