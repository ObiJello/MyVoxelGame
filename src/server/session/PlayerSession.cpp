// File: src/server/session/PlayerSession.cpp
#include "PlayerSession.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../network/NetworkServer.hpp"
#include "../network/SendScheduler.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/LootTables.hpp"
#include "../IntegratedServer.hpp"
#include "common/inventory/AbstractContainerMenu.hpp"
#include "common/inventory/ChestMenu.hpp"
#include "common/inventory/CraftingMenu.hpp"
#include "common/world/block/entity/BaseContainerBlockEntity.hpp"
#include "common/world/block/entity/FurnaceBlockEntity.hpp"
#include "common/inventory/FurnaceMenu.hpp"
#include "common/inventory/UtilityMenus.hpp"
#include "common/inventory/SystemMenus.hpp"
#include "common/inventory/CompoundContainer.hpp"
#include "common/world/block/entity/DoubleChest.hpp"
#include "common/network/packets/game/ContainerSetDataS2CPacket.hpp"
#include <climits>   // INT_MIN — the "force a resend" sentinel for m_remoteData
#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN
#include "../portal/PortalRegistry.hpp"
#endif
#include "common/entity/Item.hpp"
#include "common/entity/Inventory.hpp"
#include <algorithm>
#include <array>
#include <cmath>


namespace {
    // MC ChestBlock.updateShape, reduced to the two transitions that actually
    // occur here: a chest is placed (its chosen partner must take the
    // complementary type) and a chest is removed (its ex-partner must fall back
    // to SINGLE). Without the first, placing against a lone chest leaves that
    // chest typed `single`, so it keeps rendering and opening alone; without
    // the second, breaking one half leaves the other pointing at nothing.
    //
    // Server-only on purpose: the resulting block change is broadcast, so the
    // client corrects itself rather than having to predict the fix-up.
    void SetChestType(Game::World& world, const glm::ivec3& pos, const char* type) {
        const Game::BlockID id = world.GetBlock(pos.x, pos.y, pos.z);
        if (id != Game::BlockID::Chest && id != Game::BlockID::TrappedChest) return;
        const auto& def = Game::BlockRegistry::GetStateDefinition(id);
        const uint8_t cur = world.GetBlockState(pos.x, pos.y, pos.z);
        if (def.ValueOf(cur, "type") == type) return;      // already right
        Game::BlockRegistry::BlockStateDefinition::PropertyMap props;
        props["facing"] = std::string(def.ValueOf(cur, "facing"));
        props["type"]   = type;
        world.SetBlock(pos.x, pos.y, pos.z, id, Game::World::UpdateFlags::All,
                       def.IndexOf(props));
    }

    // The cell this chest's stored type points at, or nullopt when SINGLE.
    std::optional<glm::ivec3> ChestPartnerCell(Game::BlockID id, uint8_t state,
                                               const glm::ivec3& pos) {
        const auto& def = Game::BlockRegistry::GetStateDefinition(id);
        const std::string_view type = def.ValueOf(state, "type");
        if (type != "left" && type != "right") return std::nullopt;
        const std::string_view f = def.ValueOf(state, "facing");
        // getConnectedDirection: LEFT -> clockwise, RIGHT -> counter-clockwise.
        auto cw  = [](std::string_view d) -> std::string_view {
            if (d=="north") return "east"; if (d=="east") return "south";
            if (d=="south") return "west"; return "north"; };
        auto ccw = [](std::string_view d) -> std::string_view {
            if (d=="north") return "west"; if (d=="west") return "south";
            if (d=="south") return "east"; return "north"; };
        const std::string_view dir = (type == "left") ? cw(f) : ccw(f);
        if (dir == "north") return pos + glm::ivec3{0,0,-1};
        if (dir == "south") return pos + glm::ivec3{0,0, 1};
        if (dir == "west")  return pos + glm::ivec3{-1,0,0};
        return pos + glm::ivec3{1,0,0};
    }

    // A chest at `pos` is going away: any neighbour still claiming it as its
    // other half falls back to SINGLE.
    //
    // MC pushes neighbour updates OUTWARD and lets each neighbour re-evaluate
    // itself — ChestBlock.updateShape runs on the SURVIVING chest, and is
    // handed the changed neighbour's position. Asking the survivors is not
    // just stylistic fidelity here, it is the only thing that works: by the
    // time the server handles a break in integrated mode, the client's
    // prediction has already cleared the cell in the shared World, so the
    // broken chest's own `type` is no longer readable. It reads back as state
    // 0 — which by the default-first invariant is `single` — so deriving the
    // ex-partner from it found nothing and quietly left that partner claiming
    // a chest that no longer exists.
    //
    // A neighbour whose connected direction points at `pos` is orphaned by
    // definition, whatever `pos` used to hold, so this needs no history at all.
    void ResetOrphanedChestPartners(Game::World& world, const glm::ivec3& pos) {
        static constexpr glm::ivec3 kHorizontal[4] = {
            {0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}
        };
        for (const glm::ivec3& off : kHorizontal) {
            const glm::ivec3 n = pos + off;
            const Game::BlockID id = world.GetBlock(n.x, n.y, n.z);
            if (id != Game::BlockID::Chest && id != Game::BlockID::TrappedChest) continue;
            const uint8_t st = world.GetBlockState(n.x, n.y, n.z);
            const auto claimed = ChestPartnerCell(id, st, n);
            if (claimed && *claimed == pos) SetChestType(world, n, "single");
        }
    }
}

namespace Server {

    ItemEntityManager* PlayerSession::ItemEntitiesOrNull() const {
        auto* server = g_integratedServer.get();
        return server ? server->GetItemEntities() : nullptr;
    }

    PlayerSession::PlayerSession(uint32_t playerId, uint32_t connectionId)
        : m_playerId(playerId)
        , m_connectionId(connectionId)
    {
        m_lastTickTime = std::chrono::steady_clock::now();
        m_lastKeepAliveRx = m_lastTickTime;
        m_lastKeepAliveTx = m_lastTickTime;

        // Initialize stats to prevent immediate timeout
        m_stats.lastKeepAlive = m_lastTickTime;
    }

    PlayerSession::~PlayerSession() {
        Cleanup();
    }

    // === LIFECYCLE ===

    void PlayerSession::Initialize(const Config& config, int dimensionId, const glm::vec3& spawnPos) {
        m_config = config;
        m_simulationDistance = config.simulationDistance;
        m_viewDistance = std::min(config.viewDistance, m_simulationDistance);
        
        // Calculate initial chunk position
        m_currentChunk = Game::Math::ChunkPos(
            static_cast<int>(std::floor(spawnPos.x / 16.0f)),
            static_cast<int>(std::floor(spawnPos.z / 16.0f))
        );
        m_anchorChunk = m_currentChunk;
        m_lastKnownChunk = m_currentChunk;
        
        // Clear any existing state first
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();

        // The player is IN THE WORLD from here on — MC PlayerList.placeNewPlayer
        // has no intermediate "joining" phase that gates interaction, and
        // neither do we any more. What used to happen here was a transition to
        // PLAYING keyed on a server-side chunk queue being non-empty; that
        // queue is filled and drained later in the same server tick, so once
        // delivery outran generation the session simply never left JOINING and
        // every block placement was rejected.
        //
        // Readiness is now MC's: HasClientLoaded(), driven by the client's own
        // PlayerLoadedC2S with a 60-tick fail-open timeout.
        m_state = State::PLAYING;

        // MC ServerGamePacketListenerImpl's constructor (:273) does exactly
        // this — the play listener is born with the 60-tick timer armed.
        RestartClientLoadTimerAfterRespawn();
        m_wasPlayerDead = false;

        // The tracking view starts EMPTY; the server's first UpdateChunkTracking
        // diffs it against the real view, so the whole initial set arrives
        // through the ordinary enter path with no special-casing.
        
        Log::Info("PlayerSession: Initialized session for player %u in dimension %d",
                 m_playerId, dimensionId);
    }
    
    void PlayerSession::AttachPlayer(ServerPlayer* player) {
        m_player = player;
        if (m_player) {
            // Sync chunk position from player
            m_currentChunk = m_player->getChunkPosition();
            m_anchorChunk = m_currentChunk;
            Log::Info("PlayerSession: Attached player %u '%s' to session",
                     m_player->getPlayerId(), m_player->getName().c_str());
        }
    }
    
    void PlayerSession::DetachPlayer() {
        if (m_player) {
            Log::Info("PlayerSession: Detached player %u from session", m_player->getPlayerId());
            m_player = nullptr;
        }
    }

    void PlayerSession::Tick(int64_t serverTick) {
        PROFILE_ZONE_N("SessionTick");
        auto tickStart = std::chrono::steady_clock::now();
        
        // Skip if not in playing state
        if (m_state != State::PLAYING && m_state != State::JOINING) {
            return;
        }
        
        // Reset per-tick budgets
        m_bytesOutThisTick = 0;
        m_chunksOutThisTick = 0;
        m_diffsOutThisTick = 0;
        
        // Chunk tracking is NOT updated here. MC drives it from ChunkMap
        // (move/updatePlayerStatus), because entering a chunk means "send it if
        // loaded, else request it" — which needs the world and the load queue,
        // neither of which the session owns. IntegratedServer calls
        // UpdateChunkTracking every tick; its centre/view-distance early-out
        // makes that free when nothing moved.

        // MC ServerPlayer.tick's very first line (:588):
        //     this.connection.tickClientLoadTimeout();
        // Counts down the join/respawn grace window. It runs before the player
        // ticks so a client that never sends PlayerLoadedC2S is let in after
        // 60 ticks regardless.
        TickClientLoadTimeout();

        // Tick the player entity (movement physics, mining, item-use
        // countdown). Lives HERE — per-session — so REMOTE players tick too;
        // the old host-only call in IntegratedServer::ServerTick was removed
        // (it only ever ticked m_serverPlayer, so a LAN client's eat timer
        // never advanced).
        if (m_player) {
            ASSERT_SERVER_THREAD();
            Game::World* world = nullptr;
            if (auto* server = Server::g_integratedServer.get()) {
                world = server->GetWorld();
            }
            m_player->tick(world, static_cast<int>(serverTick));

            // MC ServerPlayer.die (:932) ends with
            //     this.connection.markClientUnloadedAfterDeath();
            // which blocks interaction until PERFORM_RESPAWN re-arms the timer.
            // Our ServerPlayer has no back-pointer to its connection, so the
            // session watches the alive→dead edge instead; the effect is the
            // same, one call at the moment of death.
            const bool deadNow = m_player->isDead();
            if (deadNow && !m_wasPlayerDead) {
                MarkClientUnloadedAfterDeath();
            }
            m_wasPlayerDead = deadNow;

            // Stats sync — mirrors ServerPlayer.tick's dirty-check on
            // lastSentHealth / lastSentFood / lastSaturationLevel: send
            // SetHealthS2C only when the triple changed (first PLAYING tick
            // always sends because the cached values start impossible).
            if (m_connection) {
                const float health     = m_player->getHealth();
                const int   food       = m_player->getFoodData().getFoodLevel();
                const float saturation = m_player->getFoodData().getSaturationLevel();
                if (health != m_lastSentHealth || food != m_lastSentFood
                    || saturation != m_lastSentSaturation) {
                    Network::SetHealthS2CPacket out;
                    out.health     = health;
                    out.food       = static_cast<uint32_t>(food);
                    out.saturation = saturation;
                    auto data = Network::Serialization::Serialize(out);
                    m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SetHealthS2C), data);
                    m_lastSentHealth     = health;
                    m_lastSentFood       = food;
                    m_lastSentSaturation = saturation;
                }
            }

            // Per-tick container diff — MC ServerPlayer.doTick's
            // containerMenu.broadcastChanges(). This SUPERSEDES the old
            // hand-rolled dirty-slot broadcast: routing every server-side
            // inventory mutation through the same diff is what keeps
            // m_remoteSlots an accurate model of the client. The old loop sent
            // slots directly and left the model stale, which then made the
            // next click re-send those slots needlessly.
            //
            // dirtySlots is still drained (other code marks it), but it no
            // longer drives sends — the diff finds every real change on its
            // own, including ones nothing bothered to mark.
            m_player->dirtySlots().clear();
            BroadcastContainerChanges();
        }
        
        // Store tick number
        m_lastServerTick = serverTick;
        
        // Update statistics
        auto tickEnd = std::chrono::steady_clock::now();
        float tickTime = std::chrono::duration<float, std::milli>(tickEnd - tickStart).count();
        
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.lastTickTime = tickTime;
            m_stats.averageTickTime = m_stats.averageTickTime * 0.95f + tickTime * 0.05f;
            m_stats.bytesOutThisTick = m_bytesOutThisTick;
        }
        
        m_lastTickTime = tickEnd;
    }

    void PlayerSession::Cleanup() {
        m_state = State::DISCONNECTING;
        
        // Clear all data structures
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        Log::Info("PlayerSession: Cleaned up session for player %u", m_playerId);
    }

    // === PLAYER STATE ===

    void PlayerSession::UpdatePosition(const glm::vec3& position, const glm::vec2& rotation) {
        // Delegate to ServerPlayer if attached
        if (m_player) {
            m_player->setPosition(glm::dvec3(position));
            m_player->setRotation(rotation.x, rotation.y);
            
            // Calculate new chunk position from player
            Game::Math::ChunkPos newChunk = m_player->getChunkPosition();
            
            // Update chunk position if changed
            if (newChunk != m_currentChunk) {
                UpdateChunkPosition(newChunk);
            }
        }
    }

    void PlayerSession::UpdateChunkPosition(Game::Math::ChunkPos newChunk) {
        if (newChunk == m_currentChunk) {
            return;
        }
        
        m_lastKnownChunk = m_currentChunk;
        m_currentChunk = newChunk;
        m_anchorChunk = newChunk;
        
        Log::Info("SESSION CHUNK MOVE: player %u from (%d,%d) to (%d,%d)",
                  m_playerId, m_lastKnownChunk.x, m_lastKnownChunk.z, newChunk.x, newChunk.z);
    }

    void PlayerSession::ChangeDimension(int newDimensionId, const glm::vec3& targetPos) {
        if (!m_player) return;
        
        if (m_player->getDimensionId() == newDimensionId) {
            return;
        }
        
        Log::Info("PlayerSession: Player %u changing dimension from %d to %d", 
                 m_playerId, m_player->getDimensionId(), newDimensionId);
        
        m_isChangingDimension = true;
        
        // Send unload for all tracked chunks. Snapshot the view first —
        // SendChunkUnload clears it, and ForEach reads it.
        const ChunkTrackingView previousView = m_trackingView;
        previousView.ForEach([this](Game::Math::ChunkPos chunk) { SendChunkUnload(chunk); });
        
        // Clear all watch sets
        ClearWatchSets();
        ClearQueues();
        ClearDiffs();
        
        // Update player dimension and position
        m_player->setDimensionId(newDimensionId);
        m_player->teleport(glm::dvec3(targetPos));
        
        m_currentChunk = m_player->getChunkPosition();
        m_anchorChunk = m_currentChunk;
        
        // Recompute watch set for new dimension
        m_isChangingDimension = false;
    }

    void PlayerSession::Respawn(const glm::vec3& spawnPos) {
        Log::Info("PlayerSession: Player %u respawning at (%.1f, %.1f, %.1f)",
                 m_playerId, spawnPos.x, spawnPos.y, spawnPos.z);
        
        m_isRespawning = true;
        m_state = State::RESPAWNING;
        
        // Respawn player entity
        if (m_player) {
            m_player->respawn(spawnPos);
            m_currentChunk = m_player->getChunkPosition();
            m_anchorChunk = m_currentChunk;
        }
        
        // The tracking view re-centres on its own: m_anchorChunk moved, so the
        // next UpdateChunkTracking diffs the old view against the new one and
        // emits the enter/leave pair. Nothing to flag here.

        m_state = State::PLAYING;
        m_isRespawning = false;

        // MC handleClientCommand PERFORM_RESPAWN (:1789 / :1798) calls
        // restartClientLoadTimerAfterRespawn right after PlayerList.respawn:
        // clears waitingForRespawn (set by die()) and re-arms the 60-tick wait
        // for the client to report the new level is ready.
        RestartClientLoadTimerAfterRespawn();
        m_wasPlayerDead = false;
    }

    // === VIEW CONFIGURATION ===

    void PlayerSession::SetViewDistance(int distance) {
        if (distance == m_viewDistance) {
            return;
        }
        
        // Clamp to valid range and simulation distance
        m_viewDistance = std::clamp(distance, 2, std::min(32, m_simulationDistance));
        
        Log::Info("PlayerSession: Player %u view distance changed to %d", 
                 m_playerId, m_viewDistance);
    }

    void PlayerSession::SetSimulationDistance(int distance) {
        if (distance == m_simulationDistance) {
            return;
        }
        
        m_simulationDistance = std::clamp(distance, 2, 32);
        
        // Ensure view distance doesn't exceed simulation distance
        if (m_viewDistance > m_simulationDistance) {
            m_viewDistance = m_simulationDistance;
        }
        
        
        Log::Info("PlayerSession: Player %u simulation distance changed to %d", 
                 m_playerId, m_simulationDistance);
    }

    // === CHUNK TRACKING (MC ChunkMap.updateChunkTracking) ===

    void PlayerSession::UpdateChunkTracking(
        const std::function<void(Game::Math::ChunkPos)>& onEnter,
        const std::function<void(Game::Math::ChunkPos)>& onLeave) {
        PROFILE_ZONE;

        const ChunkTrackingView next =
            ChunkTrackingView::Of(m_anchorChunk, m_viewDistance);

        // MC ChunkMap.updateChunkTracking's early-out: same centre and same
        // view distance means the tracked set is identical, so there is nothing
        // to diff. This runs every tick for every session, and this branch is
        // what makes that free — a player moving within one chunk does no work.
        if (m_trackingView.SameAs(next)) {
            return;
        }

        int entered = 0, left = 0;
        ChunkTrackingView::Difference(
            m_trackingView, next,
            [&](Game::Math::ChunkPos pos) { ++entered; onEnter(pos); },
            [&](Game::Math::ChunkPos pos) { ++left;    onLeave(pos); });

        m_trackingView = next;

        Log::Info("UpdateChunkTracking: player %u centre=(%d,%d) viewDist=%d entered=%d left=%d",
                  m_playerId, m_anchorChunk.x, m_anchorChunk.z, m_viewDistance, entered, left);

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksInWatch = m_sentChunks.size() + m_pendingChunksToSend.size();
            m_stats.chunksPending = m_pendingChunksToSend.size();
        }
    }

    bool PlayerSession::IsWatching(Game::Math::ChunkPos chunk) const {
        return m_trackingView.Contains(chunk);
    }

    bool PlayerSession::HasSentChunk(Game::Math::ChunkPos chunk) const {
        return m_sentChunks.count(chunk) > 0;
    }

    // === CHUNK SENDER (Minecraft's PlayerChunkSender) ===

    void PlayerSession::MarkChunkPendingToSend(Game::Math::ChunkPos pos) {
        // Queue unconditionally, exactly like MC
        // PlayerChunkSender.markChunkPendingToSend, which is a bare
        // `pendingChunks.add(chunk.getPos().toLong())`.
        //
        // There must be NO tracking-view test here. Both callers have already
        // established membership, and one of them cannot pass such a test:
        // UpdateChunkTracking runs the enter callbacks while m_trackingView is
        // still the PREVIOUS view (MC applyChunkTrackingView assigns the new
        // view after difference() too), so a chunk that just entered is by
        // definition absent from it. A Contains() guard here therefore drops
        // every already-loaded chunk at the moment it comes into view — which
        // meant the spawn chunk, generated before the player joined, was never
        // sent, and the client then ran its occlusion BFS from a camera chunk
        // it did not have.
        //
        // The push path does the test at its own call site
        // (PlayerSessionManager::ForEachSessionWatching), where the view is
        // current, and SendNextChunks re-checks before sending — so a chunk
        // queued and then walked away from is still dropped correctly.
        m_pendingChunksToSend.insert(pos);
    }

    void PlayerSession::DropChunk(Game::Math::ChunkPos pos) {
        if (!m_pendingChunksToSend.erase(pos)) {
            // Wasn't pending to send — if already sent, send unload to client
            if (m_sentChunks.erase(pos)) {
                SendChunkUnload(pos);
            }
        }
    }

    void PlayerSession::SendNextChunks(Game::World* world) {
        if (!world || !m_connection) return;
        if (m_pendingChunksToSend.empty()) return;
        PROFILE_ZONE_N("SendNextChunks");

        // Back-pressure: don't send if too many unacknowledged batches
        if (m_unackedBatches >= m_maxUnackedBatches) return;

        // Accumulate fractional budget
        float maxBatchSize = std::max(1.0f, m_desiredChunksPerTick);
        m_batchQuota = std::min(m_batchQuota + m_desiredChunksPerTick, maxBatchSize);

        if (m_batchQuota < 1.0f) return;

        int maxBatch = static_cast<int>(m_batchQuota);

        // Collect loaded chunks, sorted by distance from anchor
        struct ChunkDist {
            Game::Math::ChunkPos pos;
            std::shared_ptr<Game::Chunk> chunk;
            int distSq;
        };
        std::vector<ChunkDist> candidates;
        candidates.reserve(m_pendingChunksToSend.size());

        // Also collect chunks to remove from pending if they left the view
        std::vector<Game::Math::ChunkPos> staleChunks;

        for (const auto& pos : m_pendingChunksToSend) {
            // Skip chunks no longer tracked (queued, then the player walked away)
            if (!m_trackingView.Contains(pos)) {
                staleChunks.push_back(pos);
                continue;
            }

            // Cache-only. This chunk was queued because it WAS loaded, but it
            // can have been evicted since — and the blocking GetChunk would
            // then regenerate it here, on the server thread, inside the send
            // loop. Skipping is what the "picked up later" below always meant.
            auto chunk = world->GetLoadedChunk(pos.x, pos.z);
            if (!chunk) continue;  // Not resident right now — skip, retried later

            int dx = pos.x - m_anchorChunk.x;
            int dz = pos.z - m_anchorChunk.z;
            candidates.push_back({pos, chunk, dx * dx + dz * dz});
        }

        // Remove stale chunks from pending set
        for (const auto& pos : staleChunks) {
            m_pendingChunksToSend.erase(pos);
            Log::Debug("SendNextChunks: removed stale chunk (%d, %d) from pending (no longer watched)",
                      pos.x, pos.z);
        }

        if (candidates.empty()) return;

        size_t toSend = std::min(static_cast<size_t>(maxBatch), candidates.size());
        if (candidates.size() > toSend) {
            std::partial_sort(candidates.begin(), candidates.begin() + toSend, candidates.end(),
                              [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        } else {
            std::sort(candidates.begin(), candidates.end(),
                      [](const ChunkDist& a, const ChunkDist& b) { return a.distSq < b.distSq; });
        }

        // Send ChunkBatchStartS2C
        {
            auto data = Network::Serialization::Serialize(Network::ChunkBatchStartS2CPacket{});
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchStartS2C), data);
        }

        // Send each chunk
        size_t sentCount = 0;
        for (size_t i = 0; i < toSend; ++i) {
            const auto& cd = candidates[i];
            PROFILE_ZONE_N("SerializeChunk");

            // Build ChunkDataS2CPacket
            Network::ChunkDataS2CPacket packet;
            packet.chunkX = cd.pos.x;
            packet.chunkZ = cd.pos.z;
            packet.groundUpContinuous = true;
            packet.primaryBitmask = 0;

            // MC ClientboundLevelChunkPacketData.extractChunkData:
            //
            //     for (LevelChunkSection section : chunk.getSections())
            //        section.write(buffer);
            //
            // The section's containers ARE the wire format, so this copies a
            // palette and a block of words rather than re-packing 4096 voxels
            // per section per player.
            auto copyContainer = [](const Game::PalettedContainer& src,
                                    Network::ChunkDataS2CPacket::ContainerData& dst) {
                dst.bits    = static_cast<uint8_t>(src.StorageBits());
                dst.palette = src.Palette();
                dst.words   = src.RawWords();
            };

            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                const auto* section = cd.chunk->GetSection(sectionY);
                if (!section) continue;
                // MC hasOnlyAir() — one comparison against the palette.
                if (section->IsAllAir()) continue;

                packet.primaryBitmask |= (1 << sectionY);

                Network::ChunkDataS2CPacket::SectionData sectionData;

                // MC nonEmptyBlockCount. Counted off the palette rather than by
                // walking voxels: the container already knows how many of each
                // distinct state it holds.
                uint32_t nonAir = 0;
                section->States().ForEachValue([&](uint32_t stateId, int count) {
                    if (Game::BlockStateIds::Unpack(stateId).id != Game::BlockID::Air) {
                        nonAir += static_cast<uint32_t>(count);
                    }
                });
                sectionData.blockCount = static_cast<uint16_t>(nonAir > 0xFFFFu ? 0xFFFFu : nonAir);

                copyContainer(section->States(), sectionData.states);
                copyContainer(section->Biomes(), sectionData.biomes);

                packet.sections.push_back(std::move(sectionData));
            }

            // Biomes now ride inside each section's container above, where MC
            // keeps them — not as a flat per-chunk array.

            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkDataS2C), data);

            // Move from pending to sent
            m_pendingChunksToSend.erase(cd.pos);
            m_sentChunks.insert(cd.pos);
            sentCount++;

            Log::Debug("Sent chunk (%d, %d) to player %u", cd.pos.x, cd.pos.z, m_playerId);
        }

        // Send ChunkBatchFinishedS2C
        {
            Network::ChunkBatchFinishedS2CPacket finishPacket(static_cast<int32_t>(sentCount));
            auto data = Network::Serialization::Serialize(finishPacket);
            m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChunkBatchFinishedS2C), data);
        }

        m_batchQuota -= static_cast<float>(sentCount);
        m_unackedBatches++;

        Log::Debug("Sent chunk batch: %zu chunks (quota=%.1f, unacked=%d, rate=%.1f) to player %u",
                  sentCount, m_batchQuota, m_unackedBatches, m_desiredChunksPerTick, m_playerId);
    }

    void PlayerSession::OnChunkBatchAck(float desiredRate) {
        m_unackedBatches--;
        m_desiredChunksPerTick = std::isnan(desiredRate) ? 0.01f : std::clamp(desiredRate, 0.01f, 64.0f);
        if (m_unackedBatches == 0) m_batchQuota = 1.0f;
        m_maxUnackedBatches = 10;

        Log::Debug("Chunk batch ack: desiredRate=%.2f, unacked=%d, maxUnacked=%d (player %u)",
                  m_desiredChunksPerTick, m_unackedBatches, m_maxUnackedBatches, m_playerId);
    }

    void PlayerSession::SendChunkUnload(Game::Math::ChunkPos chunk) {
        // Send UnloadChunkS2CPacket directly through the connection
        // (SendScheduler's sendCallback is not implemented, so bypass it)
        if (m_connection) {
            Network::UnloadChunkS2CPacket packet(chunk.x, chunk.z);
            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::UnloadChunkS2C), data);
        }

        // Remove from sets. Not from the tracking view — that is a function of
        // position and view distance, and unloading a chunk changes neither.
        // (MC's dropChunk likewise only touches the sender's queues.)
        m_sentChunks.erase(chunk);
        m_pendingChunksToSend.erase(chunk);

        // Clear any pending diffs for this chunk
        m_pendingDiffs.erase(chunk);
        
        Log::Debug("UNLOAD SENT: chunk (%d, %d) to player %u",
                  chunk.x, chunk.z, m_playerId);
    }

    // === BLOCK UPDATES ===

    void PlayerSession::QueueBlockChange(int worldX, int worldY, int worldZ, Game::BlockID newBlock) {
        // Calculate chunk and local coordinates
        Game::Math::ChunkPos chunk(
            worldX >> 4,  // divide by 16
            worldZ >> 4
        );
        
        // Check if chunk is watched
        if (!IsWatching(chunk)) {
            return;
        }
        
        // NOTE: Block change accumulation is now handled centrally by SectionChangeAccumulator
        // This method is kept for compatibility but doesn't queue for per-player processing
        // The change will be accumulated in World::SetBlock and broadcast by ChunkDeltaBroadcaster
    }

    void PlayerSession::QueueSectionChanges(Game::Math::ChunkPos chunk, int section,
                                           const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes) {
        if (!IsWatching(chunk)) {
            return;
        }
        
        for (const auto& change : changes) {
            CoalesceBlockChange(chunk, section, change.localX, change.localY, change.localZ,
                                change.blockId, change.blockState);
        }
        
        if (HasSentChunk(chunk)) {
            m_diffQueue.push({chunk, section});
        }
    }

    // NOTE: ProcessDiffs is deprecated - block changes are now handled by ChunkDeltaBroadcaster
    // This method is kept for compatibility but does nothing
    void PlayerSession::ProcessDiffs(SendScheduler* scheduler) {
        // Block change broadcasting is now centralized in ChunkDeltaBroadcaster::flush()
        return;
        size_t maxDiffBytes = m_config.maxDiffBytesPerTick;
        size_t diffBytesThisTick = 0;
        
        while (!m_diffQueue.empty() && diffBytesThisTick < maxDiffBytes) {
            auto [chunk, section] = m_diffQueue.front();
            m_diffQueue.pop();
            
            // Get pending diffs for this chunk section
            auto chunkIt = m_pendingDiffs.find(chunk);
            if (chunkIt == m_pendingDiffs.end()) {
                continue;
            }
            
            auto sectionIt = chunkIt->second.find(section);
            if (sectionIt == chunkIt->second.end()) {
                continue;
            }
            
            auto& diffs = sectionIt->second;
            if (diffs.changes.empty()) {
                continue;
            }
            
            // Build and send packet based on number of changes
            if (diffs.changes.size() == 1) {
                // Single block change - use simple packet
                auto& [packedPos, blockId] = *diffs.changes.begin();
                int localX = (packedPos >> 8) & 0xF;
                int localY = (packedPos >> 4) & 0xF;
                int localZ = packedPos & 0xF;
                
                // Convert to world coordinates
                int worldX = chunk.x * 16 + localX;
                int worldY = section * 16 + localY - 64;  // Adjust for world height
                int worldZ = chunk.z * 16 + localZ;
                
                Network::BlockChangeS2CPacket packet(worldX, worldY, worldZ,
                                                     blockId.id, blockId.state);
                SendSingleBlockChange(packet);
                
                // Estimate packet size
                size_t estimatedPacketSize = 20; // Single block change is small
                diffBytesThisTick += estimatedPacketSize;
            } else {
                // Multiple changes in same section - use section update packet
                Network::ClientboundSectionBlocksUpdateS2CPacket packet(chunk, section);
                
                for (const auto& [packedPos, blockId] : diffs.changes) {
                    uint8_t localX = (packedPos >> 8) & 0xF;
                    uint8_t localY = (packedPos >> 4) & 0xF;
                    uint8_t localZ = packedPos & 0xF;
                    packet.AddChange(localX, localY, localZ,
                                     static_cast<uint16_t>(blockId.id), blockId.state);
                }
                
                SendSectionBlocksUpdate(packet);
                
                // Estimate packet size (each block change is roughly 4 bytes as VarInt)
                size_t estimatedPacketSize = diffs.changes.size() * 4 + 16; // +16 for packet header
                diffBytesThisTick += estimatedPacketSize;
            }
            
            // Clear processed diffs
            chunkIt->second.erase(sectionIt);
            if (chunkIt->second.empty()) {
                m_pendingDiffs.erase(chunkIt);
            }
            
            m_diffsOutThisTick++;
        }
    }

    // === PACKET HANDLING ===

    void PlayerSession::HandlePlayerMove(const Network::PlayerMoveC2SPacket& packet) {
        // Drop stale client-predicted moves that were in flight when we issued a
        // teleport (the client may have sent 1–2 MovePlayer packets at the old
        // position before processing our ClientboundPlayerPosition). Applying them
        // would revert the server-side position to the pre-teleport spot and other
        // clients would see this player flicker / stay behind. Matches MC's
        // ServerGamePacketListenerImpl.handleMovePlayer awaitingPositionFromClient
        // null-check.
        if (m_connection && m_connection->IsAwaitingTeleportAck()) {
            return;
        }

        // Dead players don't move — MC freezes the body until the client
        // sends PERFORM_RESPAWN (ServerGamePacketListenerImpl.handleMovePlayer
        // returns early when player.isImmobile()/dead).
        if (m_player && m_player->isDead()) {
            return;
        }

        // Movement statistics BEFORE the position write (needs the old Y /
        // horizontal delta) — fall-distance accumulation + exhaustion
        // sources, mirroring ServerPlayer.checkMovementStatistics and
        // LivingEntity.checkFallDamage.
        if (m_player) {
            UpdateMovementStats(packet);
        }

        UpdatePosition(packet.position, packet.rotation);
        if (m_player) {
            m_player->setSneaking(packet.isCrouching);
        }
    }

    void PlayerSession::UpdateMovementStats(const Network::PlayerMoveC2SPacket& packet) {
        ServerPlayer& p = *m_player;
        const glm::dvec3 oldPos = p.getPosition();
        const glm::dvec3 newPos = glm::dvec3(packet.position);

        // First move after join/teleport snaps can produce huge deltas —
        // teleport() already resets fall distance; distance-based exhaustion
        // is naturally capped by the anti-cheat in setPosition, good enough.
        const double dy = newPos.y - oldPos.y;
        const double dx = newPos.x - oldPos.x;
        const double dz = newPos.z - oldPos.z;
        const double horizontal = std::sqrt(dx * dx + dz * dz);

        // Feet-in-water check (server-side; block at the foot position).
        bool inWater = false;
        if (auto* server = g_integratedServer.get()) {
            if (Game::World* world = server->GetWorld()) {
                const glm::ivec3 feet(static_cast<int>(std::floor(newPos.x)),
                                      static_cast<int>(std::floor(newPos.y)),
                                      static_cast<int>(std::floor(newPos.z)));
                inWater = world->GetBlock(feet.x, feet.y, feet.z) == Game::BlockID::Water;
            }
        }

        // ── Fall damage — client-reported landing distance. The CLIENT's
        //    physics tracks the fall (PlayerPhysics::fallDistance) because
        //    only it knows exact ground contact: reconstructing falls from
        //    20 Hz position snapshots missed bunny-hop landings (land + jump
        //    inside one tick never shows an onGround packet) and stacked hop
        //    descents into phantom damage. Formula per MC
        //    LivingEntity.calculateFallDamage: floor(fd + 1e-6 - 3.0)
        //    (SAFE_FALL_DISTANCE = 3, FALL_DAMAGE_MULTIPLIER = 1).
        //    Sanity clamp: terminal-velocity falls in a 384-block world
        //    can't meaningfully exceed ~512 blocks.
        if (packet.fallDistance > 0.0f && !p.isFlying() && !inWater) {
            const float fd = std::min(packet.fallDistance, 512.0f);
            const int dmg = static_cast<int>(std::floor(fd + 1.0e-6f - 3.0f));
            if (dmg > 0) {
                p.damage(static_cast<float>(dmg), DamageSource::FALL);
            }

            // NOT ported, by choice: MC FarmBlock.fallOn (FarmBlock.java:91-100)
            // turns farmland back into dirt when something lands on it hard
            // enough (`random.nextFloat() < fallDistance - 0.5F`). Deliberately
            // omitted here — jumping around your own farm should not destroy
            // it. If it is ever wanted back, this landing hook is where it
            // goes: `fd` is already the clamped fall distance MC feeds that
            // roll.
        }

        // ── Exhaustion sources — FoodConstants: sprint 0.1/m, swim 0.01/m,
        //    jump 0.05 (sprint-jump 0.2). Walking costs nothing in modern MC.
        //    Survival/adventure only (Player.causeFoodExhaustion no-ops when
        //    invulnerable, i.e. creative/spectator).
        const GameMode mode = p.getGameMode();
        if (mode == GameMode::SURVIVAL || mode == GameMode::ADVENTURE) {
            // Ignore absurd per-packet deltas (teleport races) — MC's stat
            // path rounds per-tick distances, which are small; 10 m per move
            // packet (~200 m/s) is a safe outlier cutoff.
            if (horizontal > 0.0 && horizontal < 10.0) {
                if (inWater) {
                    p.getFoodData().addExhaustion(static_cast<float>(0.01 * horizontal));
                } else if (packet.isSprinting && packet.onGround) {
                    p.getFoodData().addExhaustion(static_cast<float>(0.1 * horizontal));
                }
            }
            if (packet.jumpedThisTick) {
                p.getFoodData().addExhaustion(packet.isSprinting ? 0.2f : 0.05f);
            }
        }

        // ── Airborne state, for the combat rules that read it ──────────────
        //
        // MC keeps fallDistance on the SERVER's player entity too, updated by
        // Entity.checkFallDamage as handleMovePlayer replays the client's
        // reported position (ServerGamePacketListenerImpl -> player.move ->
        // checkFallDamage). Player.canCriticalAttack then reads
        // `fallDistance > 0 && !onGround`, which is what makes a crit a hit
        // taken on the way DOWN — rising out of a jump accumulates nothing.
        //
        // This is a second, independent accumulator from the fall-DAMAGE path
        // above on purpose: that one wants the client's exact landing distance
        // (it alone sees ground contact between snapshots), while this one has
        // to be live mid-air, which a landing-only report can never be.
        // MC Player.aiStep:435-437 resets it every tick while flying, which is
        // what stops a creative player from critting on the way down.
        if (packet.onGround || p.isFlying()) {
            p.resetFallDistance();
        } else if (dy < 0.0) {
            p.addFallDistance(static_cast<float>(-dy));
        }
        p.setOnGround(packet.onGround);
        p.setSprinting(packet.isSprinting);

        // MC LivingEntity.getKnownMovement — the per-tick movement the sweep
        // check compares against the walk speed. Move packets are one per
        // client tick (20 Hz, as MC's are), so this delta IS a tick's worth.
        p.setKnownHorizontalMovement(horizontal < 10.0 ? horizontal : 0.0);
    }

    void PlayerSession::HandleBlockAction(const Network::BlockActionC2SPacket& packet) {
        // MC acks EVERY ServerboundPlayerActionPacket regardless of outcome
        // (ServerGamePacketListenerImpl.handlePlayerAction line 1332) — the ack
        // is "I processed this", not "I agreed with it". Rejections still get
        // corrected, because a refused break leaves the block where it was and
        // the client's prediction rolls back to it on retirement.
        //
        // This runs before the early returns below on purpose: a reach-check
        // rejection MUST still ack, or the client's predicted break would be
        // stranded forever, permanently swallowing every future server update
        // at that position.
        AckBlockChangesUpTo(packet.sequenceNumber);

        if (!m_player) return;

        // MC folds digging into handlePlayerAction, whose entire body sits
        // behind hasClientLoaded() (:1193). The ack above is deliberately
        // outside the gate — see the comment on it.
        if (!HasClientLoaded()) {
            Log::Debug("HandleBlockAction: client not loaded yet");
            return;
        }

        switch (packet.action) {
            // MC's START_DESTROY / ABORT_DESTROY are purely informational
            // (the server doesn't track per-player mining progress for
            // single-player; the client is authoritative on timing).
            // Treat them as no-ops for now — could be wired into anti-cheat /
            // per-player "currently mining" state later.
            case Network::BlockActionType::START_DESTROY:
            case Network::BlockActionType::ABORT_DESTROY:
                break;
            // Both BREAK (legacy) and STOP_DESTROY (new) finalize the dig.
            case Network::BlockActionType::STOP_DESTROY:
            case Network::BlockActionType::BREAK: {
                glm::ivec3 pos(packet.worldX, packet.worldY, packet.worldZ);

                // Validate reach
                glm::vec3 blockCenter = glm::vec3(pos) + glm::vec3(0.5f);
                if (!m_player->canReach(blockCenter)) {
                    Log::Warning("HandleBlockAction: Player %u cannot reach (%d,%d,%d)",
                                m_playerId, pos.x, pos.y, pos.z);
                    return;
                }

                // Get world and break the block (set to air)
                IntegratedServer* server = g_integratedServer.get();
                if (!server || !server->GetWorld()) return;
                Game::World* world = server->GetWorld();

                // Trust the packet's blockId for inventory purposes. In integrated-server
                // mode the client and server share one World, so by the time we get here
                // the client's local SetBlock(Air) prediction has already cleared the
                // world block — world->GetBlock(pos) would return Air. The packet's
                // blockId carries what the player actually broke. Fall back to the world
                // value when the packet doesn't supply one (e.g. older clients).
                Game::BlockID oldBlock = (packet.blockId != Game::BlockID::Air)
                                       ? packet.blockId
                                       : world->GetBlock(pos.x, pos.y, pos.z);
                if (oldBlock == Game::BlockID::Air) return;
                // The state the block was broken at — loot tables condition on
                // it (MC passes the BlockState into LootParams,
                // Block.getDrops:315).
                //
                // Taken from the PACKET for exactly the reason blockId is
                // above: in integrated mode the client's break prediction has
                // already cleared this cell, so reading the world here returns
                // the default state. That is what made a fully grown wheat
                // evaluate as age=0 and drop seeds instead of wheat — and the
                // same for carrots, potatoes and beetroots. The world is only
                // consulted as a fallback, for a sender that predates the
                // field (its state byte decodes as 0 either way).
                uint8_t oldBlockState = packet.blockState;
                if (oldBlockState == 0) {
                    oldBlockState = world->GetBlockState(pos.x, pos.y, pos.z);
                }

                // MC ChestBlock.updateShape: the surviving half of a broken
                // pair falls back to SINGLE, or it keeps claiming a partner
                // that is no longer there — and, worse, stays ineligible as a
                // future partner, since candidatePartnerFacing only accepts a
                // neighbour still typed SINGLE. Each un-reset break therefore
                // burned one neighbour permanently.
                if (oldBlock == Game::BlockID::Chest ||
                    oldBlock == Game::BlockID::TrappedChest) {
                    ResetOrphanedChestPartners(*world, pos);
                }

                // MC Containers.dropContents (called from BaseEntityBlock's
                // onRemove): a broken container spills what it held. This has
                // to happen BEFORE SetBlock, because clearing the cell tears
                // the block entity down and takes the contents with it.
                std::vector<Game::ItemStack> spilled;
                {
                    const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
                    if (auto chunk = world->GetChunk(cp.x, cp.z)) {
                        auto* be = chunk->GetBlockEntity(pos.x - cp.x * 16, pos.y,
                                                         pos.z - cp.z * 16);
                        if (auto* container =
                                dynamic_cast<Game::BaseContainerBlockEntity*>(be)) {
                            spilled = container->TakeAllContents();
                        }
                    }
                }
                // Bedrock is unbreakable in survival/adventure, but creative
                // destroys it outright (MC's ServerPlayerGameMode never
                // consults destroyTime on the creative path).
                const bool creativeBreak =
                    (m_player->getGameMode() == Server::GameMode::CREATIVE);
                if (oldBlock == Game::BlockID::Bedrock && !creativeBreak) return;

                // SetBlock may already be a no-op (the world is already Air in integrated
                // mode), but call it anyway so dedicated multiplayer still clears the
                // server's world.
                world->SetBlock(pos.x, pos.y, pos.z, Game::BlockID::Air);
#if ENABLE_PORTAL_GUN
                // Remove any portal mounted on this block. Block-break
                // bypasses IntegratedServer::ApplyBlockChange so the
                // notification has to happen here too.
                Game::Portal::ServerRegistry().OnBlockChanged(pos);
#endif
                Log::Debug("HandleBlockAction: Player %u broke block at (%d,%d,%d)",
                          m_playerId, pos.x, pos.y, pos.z);

                // Mining exhaustion — MC Player.causeFoodExhaustion on block
                // destroy, EXHAUSTION_MINE = 0.005F (FoodConstants.java:23).
                // Survival only (creative never accrues exhaustion).
                if (m_player->getGameMode() == Server::GameMode::SURVIVAL) {
                    m_player->getFoodData().addExhaustion(0.005f);
                }

                // Container contents pop out as world entities. They come back
                // regardless of game mode and regardless of the tool: they were
                // never the block's loot, they were the player's items being
                // stored. MC drops them even in creative for the same reason.
                if (auto* items = ItemEntitiesOrNull()) {
                    for (const Game::ItemStack& stored : spilled) {
                        items->PopResource(pos, stored);
                    }
                }

                // Roll the block's loot table and pop the result into the world.
                //
                // Creative is exempt: MC's ServerPlayerGameMode.destroyBlock
                // bails out immediately after removing the block when
                // isCreative(), so no drop is ever produced.
                if (m_connection && !creativeBreak) {
                    const Game::Block& brokenBlock = Game::BlockRegistry::Get(oldBlock);
                    const Game::ItemStack& heldStack =
                        m_player->getInventory().GetSelectedStack();

                    // MC's binary drop gate (ServerPlayerGameMode.destroyBlock:278
                    // → Player.hasCorrectToolForDrops:605): a block flagged
                    // requiresCorrectTool yields NOTHING to the wrong tool, no
                    // matter what its loot table says. Blocks without the flag
                    // always pass. Note this same predicate already picks the
                    // ×30 vs ×100 mining-speed divisor in MiningSpeed.cpp:71 —
                    // it just wasn't consulted for drops until now.
                    if (Game::HasCorrectToolForDrops(heldStack.itemId, brokenBlock)) {
                        Game::LootContext lootCtx;
                        lootCtx.block          = oldBlock;
                        lootCtx.blockState     = oldBlockState;
                        lootCtx.tool           = &heldStack;
                        lootCtx.world          = world;
                        lootCtx.pos            = pos;
                        lootCtx.brokenByEntity = true;   // a player did this
                        lootCtx.rng            = &m_lootRandom;

                        // Loot pops into the WORLD, not straight into the
                        // breaker's inventory (MC Block.dropResources →
                        // popResource). The player collects it by walking over
                        // it a moment later. Going through an entity is also
                        // what stops a full inventory from destroying the drop,
                        // which is what the old AddStack path did.
                        if (auto* items = ItemEntitiesOrNull()) {
                            for (const Game::ItemStack& drop : Game::LootTables::GetDrops(lootCtx)) {
                                items->PopResource(pos, drop);
                            }
                        }
                    }
                }
                break;
            }
            case Network::BlockActionType::PLACE:
                // Handled by HandleUseItemOn
                break;
            case Network::BlockActionType::INTERACT:
                // TODO: Implement block interaction (chests, doors, etc.)
                break;
        }
    }
    
    void PlayerSession::HandleHeldItemChange(const Network::HeldItemChangeC2SPacket& packet) {
        if (!m_player) return;
        int slot = packet.slot;
        if (slot >= 0 && slot < 9) {
            // Only update the selected-slot index. The packet's `blockId` field is
            // legacy from when the client was inventory-authoritative — applying it
            // via setHotbarBlock(64) would clobber the real (server-authoritative)
            // stack with a 64-count of whatever block the client thinks is here.
            m_player->selectHotbarSlot(slot);
            Log::Debug("[PlayerSession] Player %u: selected slot %d", m_playerId, slot);
        }
    }

    namespace {
        // MC compares with ItemStack.matches (item + count + components).
        // Game::ItemStacksMatch is exactly that; it used to be open-coded here
        // by serializing both stacks and diffing the bytes, which allocated two
        // PacketBuffers per call — 46 slots × every player × every tick.
        bool StacksIdentical(const Game::ItemStack& a, const Game::ItemStack& b) {
            return Game::ItemStacksMatch(a, b);
        }
    } // namespace

    void PlayerSession::InvalidateRemoteSlot(int index) {
        if (index < 0 || index >= static_cast<int>(m_remoteSlots.size())) return;
        // count = -1 is unreachable for a real stack (empty slots are count 0),
        // so ItemStacksMatch is guaranteed to report a difference and the diff
        // will resend this slot.
        m_remoteSlots[index] = Game::ItemStack{};
        m_remoteSlots[index].count = -1;
    }

    void PlayerSession::InvalidateRemoteInventorySlot(int inventoryIndex) {
        if (!m_player) return;
        InvalidateRemoteSlot(m_player->container().MenuIndexForInventorySlot(inventoryIndex));
    }

    bool PlayerSession::CloseMenuIfBlockGone() {
        // MC AbstractContainerMenu.stillValid → ContainerLevelAccess.evaluate:
        // every tick a block menu re-checks that its block is still there, and
        // closes if it isn't. That check is not cosmetic here — a block menu's
        // Slots point straight at the block entity's container, and a furnace
        // menu's data slots capture the block entity itself, so once SetBlock
        // frees it (World.cpp's RemoveBlockEntity) the very next slot diff
        // reads freed memory. Breaking an open chest or furnace was a
        // use-after-free crash before this.
        if (!m_player || !m_menuIsBlockBacked || !m_player->hasOpenContainerMenu()) {
            return false;
        }
        IntegratedServer* server = g_integratedServer.get();
        if (!server || !server->GetWorld()) return false;

        const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(
            m_openMenuPos.x, m_openMenuPos.z);
        auto chunk = server->GetWorld()->GetChunk(cp.x, cp.z);
        Game::BlockEntity* be = chunk
            ? chunk->GetBlockEntity(m_openMenuPos.x - cp.x * 16, m_openMenuPos.y,
                                    m_openMenuPos.z - cp.z * 16)
            : nullptr;
        if (dynamic_cast<Game::BaseContainerBlockEntity*>(be)) {
            // A double chest's CompoundContainer points at BOTH block
            // entities, so losing either half is just as fatal as losing the
            // one that was clicked.
            if (!m_hasMenuPartner) return false;
            const auto pcp = Game::Math::WorldCoordinates::WorldToChunkPos(
                m_openMenuPartnerPos.x, m_openMenuPartnerPos.z);
            auto pchunk = server->GetWorld()->GetChunk(pcp.x, pcp.z);
            Game::BlockEntity* pbe = pchunk
                ? pchunk->GetBlockEntity(m_openMenuPartnerPos.x - pcp.x * 16,
                                         m_openMenuPartnerPos.y,
                                         m_openMenuPartnerPos.z - pcp.z * 16)
                : nullptr;
            if (dynamic_cast<Game::BaseContainerBlockEntity*>(pbe)) return false;
        }

        // Gone (broken, or its chunk unloaded). Drop the menu before anything
        // can dereference it, and tell the client so its screen comes down.
        m_menuIsBlockBacked = false;
        m_hasMenuPartner    = false;
        m_player->closeContainerMenu();
        // Re-seeds m_remoteSlots/m_remoteCarried from the menu we just fell
        // back to, and tells the client to show the plain inventory. The data
        // diff reseeds on its own next tick, when the slot count changes.
        SendInventoryFull();
        return true;
    }

    void PlayerSession::BroadcastContainerChanges() {
        if (!m_player || !m_connection) return;
        // MUST be the first thing that touches the menu this tick.
        if (CloseMenuIfBlockGone()) return;

        // Walk the OPEN MENU's slots, not the inventory's. For the player's own
        // menu the two are the same list; for a crafting table the menu also
        // covers the table's grid and output, which the inventory knows nothing
        // about. Slot::GetItem resolves each index to the right container.
        auto& menu = m_player->container();
        const int slotCount = menu.SlotCount();
        if (static_cast<int>(m_remoteSlots.size()) != slotCount) {
            // The menu changed under us without a full sync. Re-seed and send
            // everything rather than diff against a mismatched model.
            SendInventoryFull();
            return;
        }

        for (int i = 0; i < slotCount; ++i) {
            const Game::ItemStack& actual = menu.GetSlot(i).GetItem();
            if (StacksIdentical(actual, m_remoteSlots[i])) continue;

            m_remoteSlots[i] = actual;
            BumpContainerState();
            Network::InventorySetSlotS2CPacket out;
            out.slotIndex = static_cast<int16_t>(i);
            out.stack     = actual;
            out.stateId   = m_containerStateId;
            auto data = Network::Serialization::Serialize(out);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::InventorySetSlotS2C), data);
        }

        // ContainerData deltas (MC AbstractContainerMenu.broadcastChanges sends
        // one ClientboundContainerSetDataPacket per changed index). These do
        // NOT bump the container state id: stateId guards CLICK staleness, and
        // a furnace ticking its flame every tick would otherwise invalidate
        // every click the player had in flight.
        const int dataCount = menu.DataCount();
        if (static_cast<int>(m_remoteData.size()) != dataCount) {
            m_remoteData.assign(static_cast<size_t>(dataCount), INT_MIN);   // force a resend
        }
        for (int i = 0; i < dataCount; ++i) {
            const int value = menu.GetData(i);
            if (m_remoteData[static_cast<size_t>(i)] == value) continue;
            m_remoteData[static_cast<size_t>(i)] = value;
            Network::ContainerSetDataS2CPacket out;
            out.containerId = menu.containerId;
            out.id          = static_cast<uint16_t>(i);
            out.value       = value;
            auto data = Network::Serialization::Serialize(out);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::ContainerSetDataS2C), data);
        }

        const Game::ItemStack& carried = m_player->getCarried();
        if (!StacksIdentical(carried, m_remoteCarried)) {
            m_remoteCarried = carried;
            BumpContainerState();
            Network::InventorySetCarriedS2CPacket out;
            out.stack   = carried;
            out.stateId = m_containerStateId;
            auto data = Network::Serialization::Serialize(out);
            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::InventorySetCarriedS2C), data);
        }
    }

    // Flip to 1 to trace every container click through the authoritative
    // handler. Logs the cursor and clicked slot on BOTH sides of the call, so
    // a divergence between what the client is showing and what the server
    // believes shows up as the first line whose "before" state is not what the
    // previous line's "after" state left behind.
#define INVENTORY_CLICK_TRACE 0

    void PlayerSession::HandleInventoryClick(const Network::InventoryClickC2SPacket& packet) {
        if (!m_player || !m_connection) return;
        // The other path that dereferences menu slots. A click can arrive
        // between the block being broken and the next per-tick diff, so the
        // same stillValid check has to run here — the containerId guard below
        // would not save us, because reaching it already means touching the
        // menu whose block entity is gone.
        if (CloseMenuIfBlockGone()) return;

#if INVENTORY_CLICK_TRACE
        const auto& traceInv = m_player->getInventory();
        const auto  beforeCarried = m_player->getCarried();
        const auto  beforeSlot = (packet.slotIndex >= 0 && packet.slotIndex < Game::Inventory::TOTAL_SIZE)
                               ? traceInv.GetSlot(packet.slotIndex) : Game::ItemStack{};
        Log::Info("[ClickTrace] IN  action=%u slot=%d btn=%u | cursor=%u x%d | slot=%u x%d",
                  (unsigned)packet.action, (int)packet.slotIndex, (unsigned)packet.button,
                  (unsigned)beforeCarried.itemId, beforeCarried.count,
                  (unsigned)beforeSlot.itemId, beforeSlot.count);
#endif

        // MC handleContainerClick's very first check: does this click even
        // target the menu that is open? The server bumps containerId on close,
        // so a click aimed at a menu that has since been replaced is dropped
        // rather than applied to whatever took its place. id 0 means the client
        // has not learned an id yet (pre-first-snapshot) and is accepted.
        if (packet.containerId != 0
            && packet.containerId != m_player->container().containerId) {
            Log::Debug("[PlayerSession %u] Click for container %u (open: %u) — dropped",
                       m_playerId, packet.containerId,
                       m_player->container().containerId);
            return;
        }

        auto result = m_player->container().DoClick(packet);

#if INVENTORY_CLICK_TRACE
        {
            const auto afterCarried = m_player->getCarried();
            const auto afterSlot = (packet.slotIndex >= 0 && packet.slotIndex < Game::Inventory::TOTAL_SIZE)
                                 ? traceInv.GetSlot(packet.slotIndex) : Game::ItemStack{};
            Log::Info("[ClickTrace] OUT cursor=%u x%d | slot=%u x%d | changed=%zu carriedChanged=%d",
                      (unsigned)afterCarried.itemId, afterCarried.count,
                      (unsigned)afterSlot.itemId, afterSlot.count,
                      result.changedSlots.size(), result.carriedChanged ? 1 : 0);
        }
#endif

        // Adopt the client's PREDICTED outcome as our model of what it now
        // believes (MC: setRemoteSlotNoCopy / setRemoteCarried for every entry
        // in the packet's changedSlots). This is the piece that makes the diff
        // below able to catch a slot the client wrote but the server did not —
        // the client tells us it wrote it, so the model disagrees with the
        // truth and we correct it. Without this, such a slot is invisible to
        // any delta scheme and survives as a ghost item.
        if (packet.hasPrediction) {
            for (const auto& [slot, stack] : packet.predictedSlots) {
                if (slot < m_remoteSlots.size()) m_remoteSlots[slot] = stack;
            }
            m_remoteCarried = packet.predictedCarried;
        }

        // Predicted against a revision we have already moved past → the
        // client's whole picture is suspect, so replace it wholesale rather
        // than patching (MC does the same on a state mismatch).
        if (packet.stateId != 0 && packet.stateId != m_containerStateId) {
            Log::Debug("[PlayerSession %u] Click predicted against state %u (server at %u) — full resync",
                       m_playerId, packet.stateId, m_containerStateId);
            SendInventoryFull();
            return;
        }

        // Normal path: send only what the client actually has wrong. A correct
        // prediction sends nothing at all.
        BroadcastContainerChanges();

        // A THROW click (Q on a slot) or a click outside the window with a
        // carried stack puts the items here. HandleThrow / DropCarriedOutside
        // have already removed them from the container, so this is the only
        // thing standing between them and being destroyed.
        DropItemFromPlayer(result.droppedItem);
        for (const auto& extra : result.extraDrops) {
            DropItemFromPlayer(extra);
        }
    }

    void PlayerSession::DropItemFromPlayer(const Game::ItemStack& stack) {
        if (stack.IsEmpty() || !m_player) return;

        auto* items = ItemEntitiesOrNull();
        if (!items) return;

        // Thrown from eye level so it appears to leave the hand.
        const glm::dvec3 eye =
            m_player->getPosition()
            + glm::dvec3(0.0, Game::PlayerPhysics::EYE_HEIGHT_STANDING, 0.0);

        // Resolved to a VECTOR here so the entity manager stays free of any
        // angle convention at all — MC's drop formula is written against MC's
        // angles and transcribing it against stored degrees is how a thrown
        // item ends up flying backwards.
        const glm::dvec3 forward = glm::dvec3(
            Game::Mth::ViewVector(m_player->getPitch(), m_player->getYaw()));

        items->DropFromPlayer(eye, forward, stack);
    }

    void PlayerSession::HandleInventoryClose(const Network::InventoryCloseC2SPacket&) {
        if (!m_player || !m_connection) return;
#if INVENTORY_CLICK_TRACE
        {
            const auto c = m_player->getCarried();
            Log::Info("[ClickTrace] CLOSE cursor=%u x%d (goes back into inventory)",
                      (unsigned)c.itemId, c.count);
        }
#endif
        // MC drops the cursor item as a world entity on close. This engine
        // instead tries to put it back into the player's inventory first, which
        // is the friendlier reading of "pressed E while holding something", and
        // is a deliberate divergence kept from before item entities existed.
        // Only the part that genuinely does not fit now goes into the world —
        // which is the half that used to be destroyed outright.
        // MC doCloseContainer → menu.removed(player): a menu with its own
        // storage hands it back before it disappears, and containerMenu drops
        // to inventoryMenu so the id the client was clicking against stops
        // being current — any click still in flight for the closed menu is then
        // rejected by the containerId guard in HandleInventoryClick.
        //
        // closeContainerMenu does both for a block container. With only the
        // player's own menu open there is nothing to swap, so its 2x2 grid is
        // emptied and the id bumped here instead — otherwise items parked in
        // the crafting square would sit there invisibly until next time.
        if (m_player->hasOpenContainerMenu()) {
            // Same deal as the else-branch: a block menu hands its inputs back
            // on close, and whatever didn't fit must not evaporate.
            Game::ContainerClickResult closed = m_player->closeContainerMenu();
            for (const auto& extra : closed.extraDrops) {
                DropItemFromPlayer(extra);
            }
        } else {
            Game::ContainerClickResult removal;
            m_player->container().Removed(removal);
            m_player->container().containerId++;
            // Anything the closing menu could not hand back (crafting grid or
            // anvil inputs against a full inventory) goes into the world.
            for (const auto& extra : removal.extraDrops) {
                DropItemFromPlayer(extra);
            }
        }

        auto& carried = m_player->getCarried();
        if (!carried.IsEmpty()) {
            // AddStack, not AddItems: the cursor may hold an enchanted book or
            // any other stack with per-stack components, and (id, count) would
            // drop them.
            const int leftover = m_player->getInventory().AddStack(carried);
            if (leftover > 0) {
                Game::ItemStack overflow = carried;
                overflow.count = leftover;
                DropItemFromPlayer(overflow);
            }
            carried.Clear();
        }

        // Always full-sync on close, even when nothing moved: containerId rides
        // ONLY on InventoryFullS2C, and a client that never learns the new id
        // would have every subsequent click rejected by the guard above. This
        // is also what MC does when the open menu changes (sendAllDataToRemote).
        // No hand-rolled per-slot sends here — the snapshot covers the returned
        // cursor and every slot it landed in, and refreshes m_remoteSlots.
        SendInventoryFull();
    }

    void PlayerSession::HandlePlayerAbilities(const Network::PlayerAbilitiesC2SPacket& packet) {
        if (!m_player || !m_connection) return;
        // MC ServerGamePacketListenerImpl.handlePlayerAbilities: only the
        // FLYING bit is client-writable, and only while mayFly. A client
        // claiming flight without permission gets a corrective resend.
        if (m_player->canFly()) {
            m_player->setFlying(packet.flying());
        } else if (packet.flying()) {
            Log::Warning("[PlayerSession %u] Client requested flight without mayFly — correcting",
                         m_playerId);
            m_connection->SendPlayerAbilities(*m_player);
        }
    }

    void PlayerSession::SendInventoryFull() {
        if (!m_player || !m_connection) return;
        const auto& inv = m_player->getInventory();
        auto& menu = m_player->container();

        Network::InventoryFullS2CPacket out;
        out.menuType = m_player->openMenuType();
        out.slots.reserve(static_cast<size_t>(menu.SlotCount()));
        for (int i = 0; i < menu.SlotCount(); ++i) {
            out.slots.push_back(menu.GetSlot(i).GetItem());  // components ride along
        }
        out.carried            = m_player->getCarried();
        out.selectedHotbarSlot = static_cast<uint8_t>(inv.GetSelectedSlot());
        BumpContainerState();
        out.stateId            = m_containerStateId;
        // The only packet carrying containerId — this is how the client learns
        // which menu to stamp on its clicks.
        out.containerId        = m_player->container().containerId;
#if INVENTORY_CLICK_TRACE
        // This is the other path that pushes a cursor to the client (join,
        // instant item-use in DispatchUseItem, respawn). If a stale cursor is
        // being resurrected, expect to see it here.
        Log::Info("[ClickTrace] FULLSYNC cursor=%u x%d",
                  (unsigned)out.carried.itemId, out.carried.count);
#endif

        auto data = Network::Serialization::Serialize(out);
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::InventoryFullS2C), data);

        // The client adopts this snapshot verbatim, so our model of its state
        // is now exactly what we just sent — including its SIZE, which is how
        // the model follows a menu swap.
        m_remoteSlots   = out.slots;
        m_remoteCarried = out.carried;
    }

    void PlayerSession::BroadcastBlockEntity(const glm::ivec3& pos, Game::BlockEntity* be) {
        if (!be || !be->GetType()) return;
        auto* server = Server::g_integratedServer.get();
        if (!server || !server->GetNetworkServer()) return;

        be->MarkDirty();
        Network::BlockEntityDataS2CPacket pkt(pos.x, pos.y, pos.z, be->GetType()->TypeId());
        Network::PacketBuffer scratch;
        be->Save(scratch);
        pkt.dataBlob = scratch.GetData();
        auto data = Network::Serialization::Serialize(pkt);
        // Every watcher, not just the player who caused it — the block entity
        // is world state.
        server->GetNetworkServer()->BroadcastPacket(
            static_cast<uint8_t>(Network::PacketId::BlockEntityDataS2C), data);
    }

    void PlayerSession::FlushPendingCampfireFood() {
        if (!m_player) return;
        auto pending = m_player->takePendingCampfireFood();
        if (!pending) return;

        IntegratedServer* server = g_integratedServer.get();
        if (!server || !server->GetWorld()) return;
        Game::World* world = server->GetWorld();

        const glm::ivec3& pos = pending->pos;
        const auto chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
        auto chunk = world->GetChunk(chunkPos.x, chunkPos.z);
        if (!chunk) return;

        auto* campfire = dynamic_cast<Game::CampfireBlockEntity*>(
            chunk->GetBlockEntity(pos.x - chunkPos.x * 16, pos.y, pos.z - chunkPos.z * 16));
        if (!campfire) return;

        // PlaceFood consumes one from the held stack on success and leaves it
        // untouched when the fire is full — MC's placeFood contract exactly.
        Game::ItemStack& held = m_player->getItemInHand(pending->hand);
        const Game::ItemStack before = held;
        if (!campfire->PlaceFood(held)) return;

        if (m_player->isCreative()) {
            held = before;              // creative never runs the stack down
        } else {
            m_player->markSlotDirty(m_player->handSlotIndex(pending->hand));
        }

        // The four food slots live in the block entity, so the client only
        // learns what is on the fire from a BE update — without this the
        // campfire renderer would draw nothing until something else forced a
        // resync.
        BroadcastBlockEntity(pos, campfire);
    }

    void PlayerSession::FlushPendingMenuOpen() {
        if (!m_player || !m_connection) return;
        auto pending = m_player->takePendingMenuOpen();
        if (!pending) return;

        // Block-backed menus need the container living at the clicked cell.
        // MC gets there via state.getMenuProvider(level, pos), which resolves
        // the block entity; ours is the same lookup the placement path uses.
        auto containerAt = [this](const glm::ivec3& pos) -> Game::BaseContainerBlockEntity* {
            IntegratedServer* server = g_integratedServer.get();
            if (!server || !server->GetWorld()) return nullptr;
            const auto chunkPos =
                Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            auto chunk = server->GetWorld()->GetChunk(chunkPos.x, chunkPos.z);
            if (!chunk) return nullptr;
            const int lx = pos.x - chunkPos.x * 16;
            const int lz = pos.z - chunkPos.z * 16;
            auto* be = chunk->GetBlockEntity(lx, pos.y, lz);

            // Create one on demand if the block wants a block entity but has
            // none. Block entities are not persisted yet (see the note in
            // BlockEntity.hpp) and are only created by SetBlock, so ANY
            // container that came from world generation, from a loaded chunk,
            // or from a previous run has no block entity — right-clicking it
            // would find nothing and silently refuse to open. Only a container
            // placed during this session would work, which is exactly the
            // "only the crafting table opens" symptom, since that menu is the
            // one that needs no block entity.
            //
            // This is also what makes the contents survive a chunk reload
            // becoming a real feature later: the lazy create is the same hook
            // a load would fill in.
            if (!be) {
                const Game::BlockID blockId =
                    server->GetWorld()->GetBlock(pos.x, pos.y, pos.z);
                if (const auto* type = Game::BlockEntityTypes::ForBlock(blockId)) {
                    auto created = type->Create(pos, blockId);
                    be = created.get();
                    chunk->SetBlockEntity(lx, pos.y, lz, std::move(created));
                }
            }

            // dynamic_cast rather than a static one: plenty of block entities
            // are not containers (signs, banners), and a right-click on one
            // must decline rather than reinterpret it as storage.
            return dynamic_cast<Game::BaseContainerBlockEntity*>(be);
        };

        // MC EnchantmentMenu.slotsChanged's bookshelf scan: a 5x5 ring two
        // blocks out, at the table's level and one above, and a shelf only
        // counts when the cell BETWEEN it and the table is air. That air check
        // is the whole reason you can wall a table off from its shelves.
        auto CountBookshelvesAround = [](const glm::ivec3& tablePos) -> int {
            IntegratedServer* server = g_integratedServer.get();
            if (!server || !server->GetWorld()) return 0;
            Game::World* w = server->GetWorld();
            int power = 0;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) continue;
                    for (int dy = 0; dy <= 1; ++dy) {
                        // The cell adjacent to the table must be clear.
                        if (w->GetBlock(tablePos.x + dx, tablePos.y + dy,
                                        tablePos.z + dz) != Game::BlockID::Air) {
                            continue;
                        }
                        auto isShelf = [&](int x, int y, int z) {
                            return w->GetBlock(x, y, z) == Game::BlockID::Bookshelf;
                        };
                        if (isShelf(tablePos.x + dx * 2, tablePos.y + dy, tablePos.z + dz * 2)) ++power;
                        if (dx != 0 && dz != 0) {
                            if (isShelf(tablePos.x + dx * 2, tablePos.y + dy, tablePos.z + dz)) ++power;
                            if (isShelf(tablePos.x + dx, tablePos.y + dy, tablePos.z + dz * 2)) ++power;
                        }
                    }
                }
            }
            return power;
        };

        // Screen title = the block's display name, as vanilla does for every
        // container without a custom name (MC BaseContainerBlockEntity
        // .getDisplayName falls back to the block's description id).
        auto blockNameAt = [](const glm::ivec3& pos) -> std::string {
            IntegratedServer* server = g_integratedServer.get();
            if (!server || !server->GetWorld()) return {};
            return Game::BlockRegistry::Get(
                server->GetWorld()->GetBlock(pos.x, pos.y, pos.z)).name;
        };

        // Cleared BEFORE the switch: the double-chest branch sets it true, and
        // clearing afterwards wiped that — leaving the stillValid check blind
        // to the second half, so breaking it would strand the menu's
        // CompoundContainer on a freed block entity.
        //
        // It must still be cleared on EVERY open. Left stale, a partner
        // position from a previous double chest outlives it: the next single
        // chest opens, the very next tick's check looks for a partner that has
        // since been broken, and closes the menu instantly — which reads as
        // "the first right-click does nothing, the second works".
        m_hasMenuPartner = false;

        std::unique_ptr<Game::AbstractContainerMenu> menu;
        std::string title;
        switch (pending->type) {
            case Game::MenuType::Crafting:
                menu  = std::make_unique<Game::CraftingMenu>(&m_player->getInventory());
                title = "Crafting";
                break;

            // Storage. Rows are the only thing that differs (MC keys these on
            // GENERIC_9xN for exactly that reason); the block entity supplies
            // the storage and the title follows the block.
            case Game::MenuType::Generic9x1:
            case Game::MenuType::Generic9x2:
            case Game::MenuType::Generic9x3:
            case Game::MenuType::Generic9x4:
            case Game::MenuType::Generic9x5:
            case Game::MenuType::Generic9x6: {
                Game::BaseContainerBlockEntity* container = containerAt(pending->pos);
                if (!container) return;   // no BE there — nothing to open
                int rows = 1 + (static_cast<int>(pending->type) -
                                static_cast<int>(Game::MenuType::Generic9x1));

                // MC ChestBlock.MENU_PROVIDER_COMBINER: two chests standing
                // together are ONE 54-slot menu over a CompoundContainer, not
                // two 27-slot ones. The pair is resolved from the world every
                // time it opens (DoubleChest.hpp), so breaking one half simply
                // stops it pairing rather than leaving stale state behind.
                IntegratedServer* srv = g_integratedServer.get();
                Game::World* world = srv ? srv->GetWorld() : nullptr;
                auto pair = world ? Game::FindChestPartner(*world, pending->pos)
                                  : std::nullopt;
                if (pair) {
                    if (auto* other = containerAt(pair->partnerPos)) {
                        // selfIsFirst decides which chest fills the TOP half —
                        // MC's RIGHT chest is first (ChestBlock.java:93).
                        auto compound = pair->selfIsFirst
                            ? std::make_unique<Game::CompoundContainer>(container, other)
                            : std::make_unique<Game::CompoundContainer>(other, container);
                        rows = 6;
                        menu = std::make_unique<Game::ChestMenu>(
                            &m_player->getInventory(), std::move(compound), rows);
                        // Both halves must stay alive for the menu's lifetime.
                        m_openMenuPartnerPos = pair->partnerPos;
                        m_hasMenuPartner     = true;
                        // MC names a paired chest "Large Chest".
                        title = "Large Chest";
                        // The client must build a 6-row menu, so correct the
                        // type it is told about.
                        pending->type = Game::MenuType::Generic9x6;
                        break;
                    }
                }

                menu  = std::make_unique<Game::ChestMenu>(&m_player->getInventory(),
                                                          container, rows);
                title = blockNameAt(pending->pos);
                break;
            }
            // Dispenser / dropper: 3 wide, 3 tall (MC DispenserMenu).
            case Game::MenuType::Generic3x3: {
                Game::IContainer* container = containerAt(pending->pos);
                if (!container) return;
                menu  = std::make_unique<Game::ChestMenu>(&m_player->getInventory(),
                                                          container, 3, 3);
                title = blockNameAt(pending->pos);
                break;
            }
            // Hopper: 5 wide, 1 tall (MC HopperMenu).
            case Game::MenuType::Hopper: {
                Game::IContainer* container = containerAt(pending->pos);
                if (!container) return;
                menu  = std::make_unique<Game::ChestMenu>(&m_player->getInventory(),
                                                          container, 1, 5);
                title = blockNameAt(pending->pos);
                break;
            }

            // Furnace family. The block entity carries the CookingKind, so one
            // branch covers all three.
            case Game::MenuType::Furnace:
            case Game::MenuType::BlastFurnace:
            case Game::MenuType::Smoker: {
                // Through containerAt so a furnace from a loaded chunk gets
                // its block entity created on demand too.
                auto* furnace = dynamic_cast<Game::FurnaceBlockEntity*>(
                    containerAt(pending->pos));
                if (!furnace) return;
                menu  = std::make_unique<Game::FurnaceMenu>(&m_player->getInventory(),
                                                            furnace);
                title = blockNameAt(pending->pos);
                break;
            }

            // Utility blocks — no block entity, the menu owns its inputs.
            case Game::MenuType::Stonecutter:
                menu  = std::make_unique<Game::StonecutterMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::Grindstone:
                menu  = std::make_unique<Game::GrindstoneMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::CartographyTable:
                menu  = std::make_unique<Game::CartographyTableMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::Loom:
                menu  = std::make_unique<Game::LoomMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::Smithing:
                menu  = std::make_unique<Game::SmithingMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::Anvil:
                menu  = std::make_unique<Game::AnvilMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;

            // Blocks with a gameplay system behind them.
            case Game::MenuType::Enchantment: {
                auto ench = std::make_unique<Game::EnchantmentMenu>(&m_player->getInventory());
                // MC EnchantmentMenu counts bookshelves in a 5x5 ring two
                // blocks out, at the table's level and one above, each needing
                // clear air between it and the table. Without that scan the
                // table would always offer level-1 enchantments.
                ench->SetBookshelfPower(CountBookshelvesAround(pending->pos));
                menu  = std::move(ench);
                title = blockNameAt(pending->pos);
                break;
            }
            case Game::MenuType::BrewingStand: {
                Game::IContainer* container = containerAt(pending->pos);
                if (!container) return;
                menu  = std::make_unique<Game::BrewingStandMenu>(&m_player->getInventory(),
                                                                 container);
                title = blockNameAt(pending->pos);
                break;
            }
            case Game::MenuType::Beacon:
                menu  = std::make_unique<Game::BeaconMenu>(&m_player->getInventory());
                title = blockNameAt(pending->pos);
                break;
            case Game::MenuType::Crafter3x3: {
                Game::IContainer* container = containerAt(pending->pos);
                if (!container) return;
                menu  = std::make_unique<Game::CrafterMenu>(&m_player->getInventory(),
                                                            container);
                title = blockNameAt(pending->pos);
                break;
            }

            case Game::MenuType::Inventory:
                // Not a thing a block can ask for — the player menu is always
                // open behind whatever else is.
                return;

            default:
                // Menu types whose screens land in later phases.
                return;
        }
        if (!menu) {
            Log::Warning("[Menu] type=%u produced no menu — nothing will open",
                         static_cast<unsigned>(pending->type));
            return;
        }

        // Remember what this menu points INTO. Its slots (and, for a furnace,
        // its data slots) hold raw pointers to the block entity, so the menu
        // must not outlive the block — see CloseMenuIfBlockGone.
        m_menuIsBlockBacked = (pending->type != Game::MenuType::Crafting);
        m_openMenuPos       = pending->pos;
        m_player->openContainerMenu(std::move(menu), pending->type);

        // MC ServerPlayer.openMenu: ClientboundOpenScreenPacket first (so the
        // client builds the matching menu), then the contents.
        Network::OpenScreenS2CPacket open;
        open.containerId = m_player->container().containerId;
        open.menuType    = pending->type;
        open.title       = title;
        auto data = Network::Serialization::Serialize(open);
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::OpenScreenS2C), data);

        SendInventoryFull();
        Log::Debug("[PlayerSession %u] Opened menu type %u (container %u)",
                   m_playerId, static_cast<unsigned>(pending->type),
                   m_player->container().containerId);
    }

    void PlayerSession::HandleUseItemOn(const Network::UseItemOnC2SPacket& packet) {
        // === 1. Thread safety & basic validation ===
        ASSERT_SERVER_THREAD();

        // Record the ack up front so EVERY exit path below is covered,
        // including the bail-outs that predate prediction (no world, stale
        // sequence, …). An interaction that is never acked strands the
        // client's prediction for that position forever, and a stranded
        // prediction permanently swallows all future server block updates
        // there — a far worse failure than acking a request we ignored.
        //
        // Recording early is safe because the ack PACKET is not sent here: it
        // is emitted by FlushBlockChangeAck after this tick's block updates
        // (see IntegratedServer's tick), so any correction this handler sends
        // still reaches the client first.
        AckBlockChangesUpTo(packet.sequence);

        if (!m_player) {
            Log::Warning("HandleUseItemOn: No player attached to session");
            return;
        }
        
        // Get world instance
        IntegratedServer* server = g_integratedServer.get();
        if (!server) {
            Log::Warning("HandleUseItemOn: No integrated server");
            return;
        }
        
        Game::World* world = server->GetWorld();
        if (!world) {
            Log::Warning("HandleUseItemOn: No world available");
            return;
        }
        
        // === 2. Fast guards (reject early, no world touch) ===
        
        // Validate sequence number
        if (packet.sequence <= m_lastInteractionSequence) {
            // Stale packet, ignore
            Log::Debug("HandleUseItemOn: Stale sequence %u <= %u", packet.sequence, m_lastInteractionSequence);
            return;
        }
        
        // MC ServerGamePacketListenerImpl.handleUseItemOn (:1613) gates the
        // whole body on hasClientLoaded() — the client's own readiness, not
        // any server-side queue. Fails open after 60 ticks.
        if (!HasClientLoaded()) {
            Log::Debug("HandleUseItemOn: client not loaded yet");
            AckInteraction(packet.sequence, false);
            return;
        }
        
        glm::ivec3 clicked(packet.blockX, packet.blockY, packet.blockZ);
        
        // Check if chunk is loaded
        if (!world->IsPositionLoaded(clicked.x, clicked.y, clicked.z)) {
            Log::Warning("HandleUseItemOn: Chunk not loaded at (%d,%d,%d)", clicked.x, clicked.y, clicked.z);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // Build height checks
        if (!world->IsValidPosition(clicked.x, clicked.y, clicked.z)) {
            Log::Warning("HandleUseItemOn: Invalid position (%d,%d,%d)", clicked.x, clicked.y, clicked.z);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // === 3. Rebuild the authoritative hit context ===
        
        // Convert packet data to block hit result
        glm::vec3 hitPoint = Game::faceLocalUVToWorld(
            packet.direction,
            packet.cursorX,
            packet.cursorY,
            packet.cursorZ,
            clicked
        );
        
        Game::BlockHitResult hit(clicked, packet.direction, hitPoint, packet.insideBlock);
        Game::UseOnContext context(world, m_player, packet.hand, hit);
        context.playerYaw = m_player->getYaw();
        context.playerPitch = m_player->getPitch();
        context.altInteract = packet.altInteract;
        
        // === 4. Reach validation ===

        // Reconstruct ray from player eye to hit point
        glm::dvec3 playerPos = m_player->getPosition();
        glm::vec3 eyePos = glm::vec3(playerPos.x, playerPos.y + 1.62, playerPos.z); // Eye height
        float distance = glm::length(hitPoint - eyePos);
        float maxReach = m_player->getGameMode() == GameMode::CREATIVE ? 5.0f : 4.5f;

#if ENABLE_PORTAL_GUN
        // The portal gun fires a projectile that travels up to
        // sv_portal_projectile_delay × BLAST_SPEED ≈ 28.5 m before
        // expiring. Client-side collision sweeps it forward each tick
        // and only sends UseItemOnC2S on the impact face, so the impact
        // point is genuinely much farther than melee reach. Exempt the
        // portal gun from the reach cap so long-range shots place.
        {
            const int slot = m_player->getInventory().GetSelectedSlot();
            const Game::ItemStack& s = m_player->getInventory().GetSlot(
                Game::Inventory::HotbarToIndex(slot));
            if (s.itemId == Game::Items::PortalGun) {
                maxReach = 256.0f;  // long-range portal shots
            }
        }
#endif

        if (distance > maxReach) {
            Log::Warning("HandleUseItemOn: Out of reach %.2f > %.2f", distance, maxReach);
            ResyncAndAck(clicked, clicked, packet.sequence);
            return;
        }
        
        // === 5. MC-style dispatch — mirrors ServerPlayerGameMode.useItemOn
        //    (ServerPlayerGameMode.java lines 329-381) ===
        //
        //   feature-gating: if !state.block.isEnabled(level.enabledFeatures())
        //       → return FAIL                                    [TODO, see below]
        //   if gameMode == SPECTATOR:
        //       open menu provider if any (chests etc.)          [TODO, no menus]
        //       → CONSUME / PASS
        //   suppressUsingBlock = isSecondaryUseActive() && haveSomethingInOurHands
        //   if !suppressUsingBlock:
        //       result = block.useItemOn(stack, ...)            // block reacts to held item
        //       if consumesAction:
        //           CriteriaTriggers.ITEM_USED_ON_BLOCK         [TODO, no advancements]
        //           → done
        //       if result == TryEmptyHandInteraction && mainHand:
        //           result = block.useWithoutItem(...)          // block reacts as if empty hand
        //           if consumesAction:
        //               CriteriaTriggers.DEFAULT_BLOCK_USE      [TODO, no advancements]
        //               → done
        //   if !stack.isEmpty() && !player.cooldowns.isOnCooldown(stack):  [TODO, no cooldowns]
        //       if hasInfiniteMaterials (creative):
        //           int count = stack.count
        //           result = stack.useOn(ctx)
        //           stack.count = count                         // creative count preservation
        //       else:
        //           result = stack.useOn(ctx)
        //       if consumesAction:
        //           CriteriaTriggers.ITEM_USED_ON_BLOCK         [TODO, no advancements]
        //           → done
        //   else PASS → fall through to BlockItem placement.
        const Game::BlockID clickedBlkId  = world->GetBlock(clicked.x, clicked.y, clicked.z);
        const Game::Block&  clickedBlock  = Game::BlockRegistry::Get(clickedBlkId);
        const int           selectedSlot  = m_player->getInventory().GetSelectedSlot();
        Game::ItemStack&    heldStack     = m_player->getInventory().MutableSlot(
                                                Game::Inventory::HotbarToIndex(selectedSlot));
        const Game::Item&   heldItem      = Game::ItemRegistry::Get(heldStack.itemId);
        const bool isMainHand             = (packet.hand == 0);
        const bool isCreative             = (m_player->getGameMode() == Server::GameMode::CREATIVE);
        // const bool isSpectator         = (m_player->getGameMode() == Server::GameMode::SPECTATOR);

        // (void)clickedBlkId; — kept for future feature-flag check:
        //   if (!IsBlockFeatureEnabled(clickedBlkId)) { ResyncAndAck(...); return; }

        // TODO(spectator): if (isSpectator) {
        //     auto provider = clickedBlock.menuProvider;
        //     if (provider) { m_player->openMenu(provider); AckInteraction(true); return; }
        //     AckInteraction(false); return;
        // }
        // (Spectators today fall through to the regular dispatch — same as if SURVIVAL —
        // which is harmless because they can't actually mutate the world via SetBlock
        // gating elsewhere. Revisit once we have menu providers.)

        // MC: `suppressUsingBlock = isSecondaryUseActive() && haveSomethingInOurHands`
        //  — sneaking + something in hand → skip block-use, run item.useOn directly.
        const bool sneaking               = m_player->IsSneaking();
        const bool somethingInHands       = !heldStack.IsEmpty();
        const bool suppressBlockUse       = sneaking && somethingInHands;

        if (!suppressBlockUse) {
            if (clickedBlock.useItemOn) {
                Game::UseResult r = clickedBlock.useItemOn(
                    heldStack, world, clicked, m_player, packet.hand, hit);
                if (Game::ConsumesAction(r)) {
                    // TODO(advancements): CriteriaTriggers.ITEM_USED_ON_BLOCK.trigger(player, pos, stackCopy);
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
                if (r == Game::UseResult::TryEmptyHandInteraction && isMainHand
                    && clickedBlock.useWithoutItem) {
                    Game::UseResult r2 = clickedBlock.useWithoutItem(
                        world, clicked, m_player, hit);
                    if (Game::ConsumesAction(r2)) {
                        // TODO(advancements): CriteriaTriggers.DEFAULT_BLOCK_USE.trigger(player, pos);
                        AckInteraction(packet.sequence, true);
                        m_lastInteractionSequence = packet.sequence;
                        return;
                    }
                }
            } else if (clickedBlock.useWithoutItem) {
                // Block declares no item-on-block reaction but does have an
                // empty-hand reaction (the common case: doors, levers, buttons).
                if (heldStack.IsEmpty() || isMainHand) {
                    Game::UseResult r = clickedBlock.useWithoutItem(
                        world, clicked, m_player, hit);
                    if (Game::ConsumesAction(r)) {
                        // TODO(advancements): CriteriaTriggers.DEFAULT_BLOCK_USE.trigger(player, pos);
                        AckInteraction(packet.sequence, true);
                        m_lastInteractionSequence = packet.sequence;
                        return;
                    }
                }
            }
        }

        // Item.useOn — the item acts on the targeted block (FlintAndSteel,
        // Hoe, Bucket, Shovel, BoneMeal, Shears, …).
        // TODO(cooldowns): if (m_player->cooldowns().isOnCooldown(heldStack)) skip this block.
        // (MC.useItemOn line 362: `if (!itemStack.isEmpty() && !player.getCooldowns().isOnCooldown(itemStack))`)
        if (heldItem.useOn && !heldStack.IsEmpty()) {
            // MC's "creative count preservation" trick (ServerPlayerGameMode.java
            // line 365-371): in creative, snapshot BEFORE useOn and restore
            // AFTER, so a single block placed/transformed doesn't decrement the
            // infinite stack. We mutate the stack via the &-reference, so the
            // snapshot/restore must wrap the call.
            //
            // The WHOLE stack is snapshotted, not just the count. MC can get
            // away with `itemStack.setCount(count)` because its shrink() only
            // decrements — the item reference survives a drop to zero. Ours
            // doesn't: every useOn that consumes an item follows `count -= 1`
            // with `if (count <= 0) Clear()`, and Clear() wipes the id and the
            // components too. Restoring the count alone therefore left an
            // itemId of Air behind, and the stack vanished — but only when it
            // held exactly one, which is why this looked like a bone-meal bug
            // rather than a dispatch bug. It also hit honeycomb waxing and any
            // future stack-transforming item.
            const Game::ItemStack stackBefore = heldStack;
            Game::UseResult r = heldItem.useOn(context, heldStack);
            if (isCreative) {
                // MC restores only the COUNT. Restoring the WHOLE stack also
                // undoes any COMPONENT the callback wrote, and at least one
                // callback writes a component it must keep: the portal gun
                // lazily assigns itself a PORTAL_GUN_INSTANCE_ID on its first
                // shot, and that id is what pairs its blue portal with its
                // orange one. Wiping it made every creative shot allocate a
                // fresh pair — so portals piled up and never linked.
                //
                // The whole-stack restore is still needed for the one case MC
                // does not have: our shrink helpers call Clear() at zero, which
                // wipes the id and the components too, leaving no count to put
                // back. That is the bone-meal case the old comment described.
                if (heldStack.IsEmpty()) heldStack = stackBefore;
                else                     heldStack.count = stackBefore.count;
            }
            if (Game::ConsumesAction(r)) {
                // TODO(advancements): CriteriaTriggers.ITEM_USED_ON_BLOCK.trigger(player, pos, stackCopy);
                AckInteraction(packet.sequence, true);
                m_lastInteractionSequence = packet.sequence;
                return;
            }
            if (r == Game::UseResult::Fail) {
                // Item explicitly rejected — DO NOT fall through to placement.
                AckInteraction(packet.sequence, false);
                m_lastInteractionSequence = packet.sequence;
                return;
            }
        }

        // === 6. BlockItem placement fallback (existing behaviour) ===

        // Get block to place from player's hand
        Game::BlockID blockToPlace = m_player->getHeldBlock();

        // If holding air or no block, can't place
        if (blockToPlace == Game::BlockID::Air) {
            // Use-item fallthrough — MC's CLIENT falls through useItemOn →
            // useItem when the whole block chain didn't consume
            // (Minecraft.startUseItem, Minecraft.java:1656, iterating
            // MAIN_HAND then OFF_HAND). We have no client-side interaction
            // prediction, so the fallthrough runs server-side here instead.
            // ANY non-empty stack dispatches — Item_DefaultUse routes the
            // component chain (CONSUMABLE → EQUIPPABLE swap → BLOCKS_ATTACKS)
            // and returns Pass harmlessly for inert items. The old gate here
            // (`use duration > 0 || item.use`) skipped armor entirely: its
            // equip lives in the Item_DefaultUse fallback, so right-clicking
            // with a helmet while aiming at the ground never equipped it.
            if (!heldStack.IsEmpty()) {
                const Game::UseResult used = DispatchUseItem(packet.hand);
                if (Game::ConsumesAction(used)) {
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
            }
            {
                const Game::ItemStack& offhand = m_player->getItemInHand(1);
                if (!offhand.IsEmpty() && Game::GetUseDuration(offhand) > 0) {
                    DispatchUseItem(1);
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
            }
            AckInteraction(packet.sequence, false);
            m_lastInteractionSequence = packet.sequence;
            return;
        }

        // MC BlockItem.useOn (BlockItem.java):
        //
        //     InteractionResult placeResult = this.place(new BlockPlaceContext(context));
        //     return !placeResult.consumesAction() && context.getItemInHand().has(CONSUMABLE)
        //         ? super.use(level, player, hand)
        //         : placeResult;
        //
        // i.e. an item that is BOTH a block and a food falls through to eating
        // when the placement fails. This matters the moment seeds exist: a
        // carrot and a potato place carrots/potatoes on farmland, so without
        // this every rejected planting — which is every right-click that is not
        // aimed at farmland — would silently swallow the click and you could
        // never eat one again.
        //
        // Used by each placement rejection below in place of a bare
        // ResyncAndAck.
        auto failPlacement = [&](const glm::ivec3& targetPos) {
            if (!heldStack.IsEmpty() && Game::GetUseDuration(heldStack) > 0) {
                const Game::UseResult used = DispatchUseItem(packet.hand);
                if (Game::ConsumesAction(used)) {
                    AckInteraction(packet.sequence, true);
                    m_lastInteractionSequence = packet.sequence;
                    return;
                }
            }
            ResyncAndAck(clicked, targetPos, packet.sequence);
        };

        // === 6a. Resolve the placement cell (MC BlockPlaceContext) ===
        //
        // Vanilla asks three questions in a fixed order, and the ORDER is what
        // makes segmented ground cover behave:
        //
        //   replaceClicked = clickedState.canBeReplaced(ctx)          // ctor
        //   getClickedPos() = replaceClicked ? clicked : relativePos
        //   canPlace()      = replaceClicked
        //                     || stateAt(getClickedPos()).canBeReplaced(ctx)
        //
        // Only after that does getStateForPlacement look at the block sitting
        // at the resolved position. That is why clicking the GRASS BLOCK under a
        // leaf litter clump still grows the clump: grass isn't replaceable, so
        // the position resolves up into the litter's own cell, and the growth
        // check happens there rather than on whatever the crosshair touched.
        const Game::BlockID clickedBlockId = world->GetBlock(clicked.x, clicked.y, clicked.z);
        const uint8_t clickedBlockState = world->GetBlockState(clicked.x, clicked.y, clicked.z);

        const bool replaceClicked = Game::CanBeReplacedByPlacement(
            clickedBlockId, clickedBlockState, blockToPlace, sneaking);

        glm::ivec3 targetPos = replaceClicked ? clicked : context.getPlacementPos();

        // Validate target position
        if (!world->IsValidPosition(targetPos.x, targetPos.y, targetPos.z)) {
            Log::Warning("HandleUseItemOn: Target position invalid (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            failPlacement(targetPos);
            return;
        }

        const Game::BlockID targetBlockId = world->GetBlock(targetPos.x, targetPos.y, targetPos.z);
        const uint8_t targetBlockState = world->GetBlockState(targetPos.x, targetPos.y, targetPos.z);

        // MC BlockPlaceContext.canPlace. Without it we'd silently overwrite the
        // block already sitting in the resolved cell — clicking a wall whose
        // +X neighbour holds a slab would replace that slab, consuming an
        // inventory item while appearing to do nothing.
        if (!replaceClicked &&
            !Game::CanBeReplacedByPlacement(targetBlockId, targetBlockState, blockToPlace, sneaking)) {
            Log::Debug("HandleUseItemOn: Target cell already occupied at (%d,%d,%d) by block %u",
                       targetPos.x, targetPos.y, targetPos.z, static_cast<unsigned>(targetBlockId));
            failPlacement(targetPos);
            return;
        }

        // === 6b. Segmented ground cover grows in place ===
        //
        // MC SegmentableBlock.getStateForPlacement, run against the RESOLVED
        // cell as vanilla does:
        //
        //   BlockState state = level.getBlockState(context.getClickedPos());
        //   return state.is(block) ? state.setValue(segment, min(4, n + 1)) : …
        //
        // The facing is deliberately not recomputed — `state.setValue` mutates
        // the state already there, so a clump never re-orients as you add to it.
        bool growInPlace = false;
        {
            const Game::BlockID base = Game::SegmentedFamilyBase(targetBlockId);
            if (base != Game::BlockID::Air &&
                base == Game::SegmentedFamilyBase(blockToPlace)) {
                const Game::BlockID grown = Game::SegmentedGrowth(targetBlockId);
                if (grown != Game::BlockID::Air) {
                    blockToPlace = grown;
                    growInPlace  = true;
                }
            }
        }
        
        // === 6c. Slab orientation (top vs bottom half) ===
        // Mirrors MC's SlabBlock.getStateForPlacement (SlabBlock.java:66-90):
        // the slab is placed in the TOP half when the player clicked on the
        // bottom face of a block (face == DOWN), or when they clicked the
        // side of a block above its vertical midpoint (cursor.y > 0.5). The
        // BOTTOM half is the default — picked when clicking on a TOP face or
        // the lower portion of a side. We promote each "*SlabTop" variant to
        // its own BlockID, so the rule is a simple BlockID swap here.
        {
            const Game::BlockID topVariant =
                Game::BlockRegistry::SlabTopVariant(blockToPlace);
            if (topVariant != Game::BlockID::Air) {
                bool placeAsTop = false;
                switch (packet.direction) {
                    case 0:                                 // -Y (bottom face)
                        placeAsTop = true;
                        break;
                    case 1:                                 // +Y (top face)
                        placeAsTop = false;
                        break;
                    default:                                // side faces
                        placeAsTop = (packet.cursorY > 0.5f);
                        break;
                }
                if (placeAsTop) {
                    blockToPlace = topVariant;
                }
            }
        }

        // === 7. Validate placement ===

        // MC BlockItem.canPlace → `stateForPlacement.canSurvive(level, pos)`.
        // Only the families with a modelled rule are constrained (see
        // CanSurviveOn); everything else still places anywhere, as before.
        //
        // This is what stops leaf litter from stacking on itself. A full
        // 4-segment clump is no longer replaceable by its own item, so the
        // position resolves to the cell ABOVE — and there LeafLitterBlock's
        // canSurvive asks for a sturdy top face below, which a leaf litter's
        // empty collision shape does not provide.
        // Crops go through the same gate: CanSurviveAt is what refuses a seed
        // planted on anything but farmland, and what sugar cane consults for
        // adjacent water.
        {
            if (!Game::CanSurviveAt(*world, targetPos, blockToPlace)) {
                Log::Debug("HandleUseItemOn: Block cannot survive at (%d,%d,%d)",
                           targetPos.x, targetPos.y, targetPos.z);
                // The carrot-on-stone case: placement is impossible, so this
                // becomes an eat.
                failPlacement(targetPos);
                return;
            }
        }

        // === 8. Collision check with entities ===
        
        // Check if any players are in the target block space
        // TODO: Get all players from PlayerSessionManager
        glm::dvec3 playerPosDouble = m_player->getPosition();
        glm::vec3 playerCollisionPos = glm::vec3(playerPosDouble.x, playerPosDouble.y, playerPosDouble.z);
        glm::vec3 blockCenter = glm::vec3(targetPos) + glm::vec3(0.5f, 0.5f, 0.5f);
        
        // Simple AABB check - player is 0.6x1.8x0.6, block is 1x1x1.
        //
        // Skipped entirely for blocks MC declares `.noCollision()`. Vanilla's
        // gate is `level.isUnobstructed(state, pos, CollisionContext.empty())`
        // (BlockItem.java), which tests the block's COLLISION shape — empty for
        // flowers, grass and leaf litter, so those always place. Without this
        // you can't add a segment to the clump you're standing on, or put a
        // flower down at your own feet.
        bool playerCollides = false;
        if (Game::BlockRegistry::HasCollision(blockToPlace) &&
            std::abs(playerCollisionPos.x - blockCenter.x) < 0.8f &&
            std::abs(playerCollisionPos.z - blockCenter.z) < 0.8f &&
            playerCollisionPos.y < targetPos.y + 1.0f &&
            playerCollisionPos.y + 1.8f > targetPos.y) {
            playerCollides = true;
        }
        
        if (playerCollides) {
            Log::Debug("HandleUseItemOn: Player collides with placement at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            failPlacement(targetPos);
            return;
        }
        
        // TODO: Check collision with other entities
        
        // === 9. Mutate the world ===

        // MC Block.getStateForPlacement, applied through one shared table (see
        // BlockPlacement.cpp). Returns 0 — the block's default state — for
        // anything with no orientation, so it applies unconditionally. The
        // client mirrors this exact call when it predicts the placement, so the
        // block never visibly flips when this authoritative update lands.
        // Growing a clump carries its existing facing across (see 6b); every
        // other placement derives orientation from how the player was standing.
        uint8_t placedState =
            growInPlace ? targetBlockState
                        : Game::ComputePlacementState(blockToPlace, context);
        // Blocks whose orientation comes from their NEIGHBOURS rather than
        // from the player — redstone dust resolving its four connections.
        // A no-op for everything else.
        if (!growInPlace) {
            placedState = Game::ComputeWorldPlacementState(*world, targetPos,
                                                           blockToPlace, placedState);
        }

        // Second survival gate, now that the state is known. The check in step
        // 7 above is state-free, and a button's support depends entirely on its
        // `face`/`facing` — without this you could hang one on any surface,
        // including nothing at all.
        if (!Game::CanSurviveAt(*world, targetPos, blockToPlace, placedState)) {
            Log::Debug("HandleUseItemOn: Block state cannot survive at (%d,%d,%d)",
                       targetPos.x, targetPos.y, targetPos.z);
            failPlacement(targetPos);
            return;
        }

        bool changed = world->SetBlock(
            targetPos.x, targetPos.y, targetPos.z,
            blockToPlace,
            Game::World::UpdateFlags::All,
            placedState
        );
        
        if (!changed) {
            Log::Warning("HandleUseItemOn: SetBlock failed at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }

        // A chest that placed itself as LEFT/RIGHT chose a partner; that
        // partner is still typed SINGLE and has to be told (MC does this
        // through updateShape on the neighbour). Skipping it leaves the older
        // chest rendering and opening alone while the new one claims a pair.
        if (blockToPlace == Game::BlockID::Chest ||
            blockToPlace == Game::BlockID::TrappedChest) {
            const auto& cdef = Game::BlockRegistry::GetStateDefinition(blockToPlace);
            if (const auto partner =
                    ChestPartnerCell(blockToPlace, placedState, targetPos)) {
                const bool selfIsLeft = cdef.ValueOf(placedState, "type") == "left";
                SetChestType(*world, *partner, selfIsLeft ? "right" : "left");
            }
        }
        
        // === 10. Run block hooks ===

        // BlockEntity item-data merge. World::SetBlock already created the
        // BE (via its lifecycle hook); we just need to apply any relevant
        // components from the held stack onto it — sign text, banner
        // patterns, custom name, dye colour, …. Mirrors MC's
        // BlockItem.updateCustomBlockEntityTag + BlockEntity.applyImplicitComponents
        // (BlockItem.java:118-160).
        if (Game::BlockEntityTypes::HasBlockEntity(blockToPlace)) {
            const auto chunkPos = Game::Math::WorldCoordinates::WorldToChunkPos(
                targetPos.x, targetPos.z);
            if (auto chunk = world->GetChunk(chunkPos.x, chunkPos.z)) {
                const int lx = targetPos.x - chunkPos.x * 16;
                const int lz = targetPos.z - chunkPos.z * 16;
                if (auto* be = chunk->GetBlockEntity(lx, targetPos.y, lz)) {
                    be->ApplyItemComponents(heldStack.components);

                    // NOTE: chest facing is NOT set here any more. In vanilla,
                    // orientation is a blockstate property and the block entity
                    // carries none — ChestBlockEntity's whole field set is
                    // items + openersCounter + chestLidController
                    // (ChestBlockEntity.java:33-35), and ChestRenderer reads the
                    // angle off the BLOCK (ChestRenderer.java:67:
                    // blockState.getValue(ChestBlock.FACING).toYRot()). Chest is
                    // in the placement table like every other oriented block, so
                    // its facing rode in with the SetBlock above.

                    // The BE was just CREATED with default state and the
                    // initial BlockEntityDataS2C went out alongside the block
                    // change. ApplyItemComponents may have mutated it — mark
                    // dirty + re-broadcast so clients see the updated contents.
                    BroadcastBlockEntity(targetPos, be);
                }
            }
        }

        // TODO: Run block hooks when BlockRegistry is fully implemented
        // if (blockToPl) {
        //     // Call onPlace hook
        //     blockToPl->onPlace(world, targetPos, m_player);
        //
        //     // TODO: Call setPlacedBy for orientation
        // }
        
        // TODO: Schedule systems
        // - Redstone neighbor updates
        // - Fluid ticks (water/lava) for flow and waterlogging
        // - Gravity blocks (sand) if implemented
        
        // === 11. Inventory update ===
        // Decrement the selected hotbar slot and broadcast the new count.
        // Without this the server's inventory keeps the original 64-stack while
        // the client's local prediction decrements toward 0; clicking the slot
        // in the inventory then "refills" it to whatever the server still has.
        // Creative keeps infinite stacks — MC's ItemStack.consume no-ops when
        // hasInfiniteMaterials() (the client mirrors this in its prediction).
        if (m_player->getGameMode() != GameMode::CREATIVE) {
            auto& inv = m_player->getInventory();
            int selUnified = Game::Inventory::HOTBAR_BEGIN + inv.GetSelectedSlot();
            auto& slot = inv.MutableSlot(selUnified);
            if (!slot.IsEmpty()) {
                slot.count--;
                if (slot.count <= 0) slot.Clear();
                // Mutate only — this tick's BroadcastContainerChanges sends the
                // delta with a correct stateId and updates m_remoteSlots.
            }
        }
        
        // === 12. Accumulate outbound notifications ===

        // TODO: Play sound effects
        // TODO: Send particle effects
        
        // === 13. Success acknowledgment ===
        
        AckInteraction(packet.sequence, true);
        m_lastInteractionSequence = packet.sequence;
        
        Log::Debug("HandleUseItemOn: Successfully placed %d at (%d,%d,%d)",
                  static_cast<int>(blockToPlace), targetPos.x, targetPos.y, targetPos.z);
    }
    
    void PlayerSession::HandleUseItem(const Network::UseItemC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handleUseItem
        // (ServerGamePacketListenerImpl.java:1329-1354).
        ASSERT_SERVER_THREAD();

        if (!m_player) {
            Log::Warning("HandleUseItem: No player attached to session");
            return;
        }
        // :1331 — same hasClientLoaded() gate as handleUseItemOn.
        if (!HasClientLoaded()) {
            Log::Debug("HandleUseItem: client not loaded yet");
            AckInteraction(packet.sequence, false);
            return;
        }
        // Stale-sequence guard — same shared counter as dig/useOn.
        if (packet.sequence <= m_lastInteractionSequence) {
            Log::Debug("HandleUseItem: Stale sequence %u <= %u",
                       packet.sequence, m_lastInteractionSequence);
            return;
        }

        // :1332 ackBlockChangesUpTo(sequence)
        AckInteraction(packet.sequence, true);
        m_lastInteractionSequence = packet.sequence;

        // :1335 — the stack in the used hand. :1336 resetLastActionTime — no
        // idle-kick system. :1337 isItemEnabled feature-flag check omitted.
        const Game::ItemStack& stack = m_player->getItemInHand(packet.hand);
        if (stack.IsEmpty()) return;

        // :1338-1342 — wrap the client-reported rotation and snap the player
        // to it, so the use action happens with the exact aim the client had.
        auto wrapDegrees = [](float deg) {
            float d = std::fmod(deg + 180.0f, 360.0f);
            if (d < 0.0f) d += 360.0f;
            return d - 180.0f;
        };
        const float yRot = wrapDegrees(packet.yRot);
        const float xRot = wrapDegrees(packet.xRot);
        if (xRot != m_player->getPitch() || yRot != m_player->getYaw()) {
            m_player->setRotation(yRot, xRot);   // absSnapRotationTo
        }

        // :1344 — the game-mode useItem logic.
        DispatchUseItem(packet.hand);

        // :1345-1350 swing-on-SERVER-source — the local client swings
        // predictively; remote-player swing broadcast is a viewmodel-only
        // concern we don't replicate yet.
    }

    Game::UseResult PlayerSession::DispatchUseItem(uint32_t hand) {
        // Mirrors ServerPlayerGameMode.useItem (ServerPlayerGameMode.java:290-327).
        ASSERT_SERVER_THREAD();
        if (!m_player) return Game::UseResult::Pass;

        IntegratedServer* server = g_integratedServer.get();
        Game::World* world = server ? server->GetWorld() : nullptr;

        // :291-292 spectator → PASS. (No spectator interaction support — same
        // fallthrough note as HandleUseItemOn's dispatch.)
        // :293-294 cooldown check omitted — no cooldown system (excluded).

        Game::ItemStack& stack = m_player->getItemInHand(hand);
        const int oldCount = stack.count;                       // :296
        // :297 oldDamage — durability excluded.

        const Game::Item& item = Game::ItemRegistry::Get(stack.itemId);
        const Game::UseResult result =
            item.use ? item.use(world, m_player, hand, stack)
                     : Game::Item_DefaultUse(world, m_player, hand, stack);

        // :299-305 — resultStack. Our callbacks mutate the hand stack in
        // place (BlockInteraction.hpp:44-51 documents the equivalence with
        // MC's heldItemTransformedTo), so resultStack IS the hand slot.
        Game::ItemStack& resultStack = m_player->getItemInHand(hand);

        // :307 — nothing observable changed and the item has no use duration
        // → done, no resync needed.
        if (resultStack.count == oldCount
            && Game::GetUseDuration(resultStack) <= 0) {
            return result;
        }
        // :309-310 — an aborted consumable start (FAIL from e.g. "not hungry")
        // must not trigger a resync either.
        if (result == Game::UseResult::Fail
            && Game::GetUseDuration(resultStack) > 0
            && !m_player->isUsingItem()) {
            return result;
        }
        // :316-318 — fully consumed → make sure the slot reads as empty.
        if (resultStack.IsEmpty()) {
            resultStack.Clear();
            m_player->markSlotDirty(m_player->handSlotIndex(hand));
        }
        // :320-322 — if we are NOT in a hold-to-use (instant action: equip
        // swap, instant consume), push the authoritative inventory now
        // (MC: inventoryMenu.sendAllDataToRemote()).
        if (!m_player->isUsingItem()) {
            SendInventoryFull();
        }
        return result;
    }

    void PlayerSession::HandlePlayerAction(const Network::PlayerActionC2SPacket& packet) {
        // Mirrors ServerGamePacketListenerImpl.handlePlayerAction
        // (ServerGamePacketListenerImpl.java:1191-1248).
        ASSERT_SERVER_THREAD();
        if (!m_player) return;

        // MC gates the whole of handlePlayerAction on hasClientLoaded()
        // (:1193). PERFORM_RESPAWN is the one action that must stay outside
        // the gate: it lives on a DIFFERENT packet in MC
        // (handleClientCommand, which has no such check) precisely because a
        // dead player has waitingForRespawn set — gating it would make death
        // permanent.
        if (packet.action != Network::PlayerAction::PERFORM_RESPAWN
            && !HasClientLoaded()) {
            Log::Debug("HandlePlayerAction: client not loaded yet");
            return;
        }

        switch (packet.action) {
            case Network::PlayerAction::RELEASE_USE_ITEM:
                // :1235-1237 — stop the hold-to-use early (bow fires here in
                // MC via releaseUsing; food simply doesn't finish).
                m_player->releaseUsingItem();
                return;

            case Network::PlayerAction::SWAP_ITEM_WITH_OFFHAND: {
                // :1214-1220 — swap main-hand (selected hotbar) and offhand
                // stacks, then stopUsingItem.
                auto& inv = m_player->getInventory();
                const int mainIdx = m_player->handSlotIndex(0);
                const int offIdx  = m_player->handSlotIndex(1);
                Game::ItemStack tmp = inv.GetSlot(offIdx);
                inv.SetSlotFull(offIdx, inv.GetSlot(mainIdx));
                inv.SetSlotFull(mainIdx, tmp);
                m_player->stopUsingItem();          // :1218
                m_player->markSlotDirty(mainIdx);
                m_player->markSlotDirty(offIdx);
                return;
            }

            case Network::PlayerAction::DROP_ITEM:
            case Network::PlayerAction::DROP_ALL_ITEMS: {
                // MC ServerPlayer.drop(all) → Inventory.removeFromSelected →
                // LivingEntity.drop: take the items OUT of the hand and throw
                // them into the world along the look direction.
                Game::ItemStack& held = m_player->getItemInHand(0);
                if (held.IsEmpty()) return;

                // Split off what is being thrown, preserving components — a
                // dropped enchanted tool has to keep its enchantments.
                Game::ItemStack thrown = held;
                if (packet.action == Network::PlayerAction::DROP_ALL_ITEMS) {
                    held.Clear();
                } else {
                    thrown.count = 1;
                    held.count--;
                    if (held.count <= 0) held.Clear();
                }
                m_player->markSlotDirty(m_player->handSlotIndex(0));

                DropItemFromPlayer(thrown);
                return;
            }

            case Network::PlayerAction::START_DESTROY_BLOCK:
            case Network::PlayerAction::ABORT_DESTROY_BLOCK:
            case Network::PlayerAction::STOP_DESTROY_BLOCK:
                // Dig still rides BlockActionC2S (HandleBlockAction) —
                // migrating it onto PlayerAction is a separate cleanup.
                Log::Debug("[PlayerSession] PlayerAction dig stage %u ignored "
                           "(dig uses BlockActionC2S)",
                           static_cast<unsigned>(packet.action));
                return;

            case Network::PlayerAction::STAB:
                // No combat system.
                return;

            case Network::PlayerAction::PERFORM_RESPAWN: {
                // MC ServerGamePacketListenerImpl.handleClientCommand
                // PERFORM_RESPAWN → PlayerList.respawn. Only honored while
                // actually dead.
                if (!m_player->isDead()) return;

                if (auto* server = g_integratedServer.get()) {
                    if (auto* sessions = server->GetSessionManager()) {
                        // Resets health/food/position via ServerPlayer::respawn.
                        sessions->OnPlayerRespawn(m_playerId);
                    }
                }

                if (m_connection) {
                    // Authoritative snap to the spawn point (same teleport-id
                    // channel as /tp, so stale death-position moves get
                    // dropped), then refresh inventory + abilities. The next
                    // session tick's SetHealthS2C (health back to 20) closes
                    // the client's death screen.
                    const glm::dvec3 pos = m_player->getPosition();
                    m_connection->Teleport(pos.x, pos.y, pos.z,
                                           m_player->getYaw(), m_player->getPitch());
                    SendInventoryFull();
                    m_connection->SendPlayerAbilities(*m_player);
                }
                return;
            }
        }
    }

    void PlayerSession::ResyncAndAck(const glm::ivec3& clicked, const glm::ivec3& target, uint32_t sequence) {
        // Send authoritative block states back to client to resync
        if (m_connection) {
            // Get world
            Server::IntegratedServer* server = Server::g_integratedServer.get();
            if (server && server->GetWorld()) {
                Game::World* world = server->GetWorld();

                // Send clicked block, WITH its state index. Dropping the state
                // here is what made a full leaf litter clump spin north every
                // time a right-click on it was rejected: the resync told the
                // client "leaf_litter, default state", overwriting the facing
                // the client had rendered correctly all along. Same applied to
                // every furnace, chest and log that ever got resynced.
                SendBlockUpdate(clicked,
                                world->GetBlock(clicked.x, clicked.y, clicked.z),
                                world->GetBlockState(clicked.x, clicked.y, clicked.z));

                // Send target block if different
                if (clicked != target) {
                    SendBlockUpdate(target,
                                    world->GetBlock(target.x, target.y, target.z),
                                    world->GetBlockState(target.x, target.y, target.z));
                }
            }

            // Also resync the held hotbar slot. The client predictively
            // decrements its inventory the instant the player right-clicks
            // (so the HUD count drops without a network round trip). When
            // the server rejects the placement we need to push the true
            // count back, otherwise the client's count keeps ticking down
            // toward 0 even though nothing was consumed.
            //
            // This is the one path where a plain diff is not enough: the
            // server's slot never changed (that's the point — the placement was
            // rejected), so it still matches m_remoteSlots and
            // BroadcastContainerChanges would send nothing. Invalidate the
            // model entry first to force the resend.
            if (m_player) {
                const int sel = Game::Inventory::HOTBAR_BEGIN
                              + m_player->getInventory().GetSelectedSlot();
                // Player-inventory index → menu index: they differ whenever a
                // block container is on top.
                InvalidateRemoteInventorySlot(sel);
                BroadcastContainerChanges();
            }
        }

        // Send failure acknowledgment
        AckInteraction(sequence, false);
    }

    void PlayerSession::HandleKeepAlive(const Network::KeepAliveC2SPacket& packet) {
        m_lastKeepAliveRx = std::chrono::steady_clock::now();
        
        // Calculate latency
        auto roundTrip = std::chrono::duration<float, std::milli>(
            m_lastKeepAliveRx - m_lastKeepAliveTx).count();
        
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.latency = roundTrip / 2.0f;
            m_stats.lastKeepAlive = m_lastKeepAliveRx;
        }
    }

    // === SEND METHODS ===
    
    void PlayerSession::SendPositionSync() {
        if (!m_player || !m_connection) return;

        Network::PlayerUpdateS2CPacket packet;
        packet.playerId = m_player->getPlayerId();
        packet.position = glm::vec3(m_player->getPosition()); // dvec3 -> vec3
        packet.rotation = m_player->getRotation();
        packet.sequenceNumber = 0; // not used for broadcast

        auto data = Network::Serialization::Serialize(packet);
        m_connection->SendPacket(
            static_cast<uint8_t>(Network::PacketId::PlayerUpdateS2C), data);
    }
    
    void PlayerSession::SendBlockUpdate(const glm::ivec3& pos, Game::BlockID block,
                                        uint8_t stateIndex) {
        if (!m_connection) return;

        Network::BlockChangeS2CPacket packet(pos.x, pos.y, pos.z, block, stateIndex);
        SendSingleBlockChange(packet);
    }
    
    void PlayerSession::SendSingleBlockChange(const Network::BlockChangeS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server)
        if (g_integratedServer) {
            g_integratedServer->SendBlockChangeS2CPacket(packet);
        }
        // TODO: Add network connection support for multiplayer
    }
    
    void PlayerSession::SendSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server)
        if (g_integratedServer) {
            g_integratedServer->SendSectionBlocksUpdateS2CPacket(packet);
        }
        // TODO: Add network connection support for multiplayer
    }
    
    void PlayerSession::SendInventoryUpdate(int slot) {
        // TODO: Implement when inventory system exists
        // if (!m_player || !m_connection) return;
        // Network::SetSlotS2CPacket packet;
        // packet.windowId = 0; // Player inventory
        // packet.slot = slot;
        // packet.item = m_player->getInventory().getSlot(slot);
        // m_connection->SendPacket(packet);
    }
    
    void PlayerSession::AckInteraction(uint32_t sequence, bool success) {
        m_lastInteractionSequence = sequence;

        // The ack itself carries no success flag — MC's
        // ClientboundBlockChangedAckPacket is a bare sequence. "Failure" is
        // communicated by the corrective block updates the reject paths
        // already send (ResyncAndAck), which the client applies when the
        // prediction retires. Keeping the flag in this signature documents
        // intent at the call sites and drives the debug log.
        AckBlockChangesUpTo(sequence);

        Log::Debug("PlayerSession: Acknowledged interaction seq=%u success=%s",
                  sequence, success ? "true" : "false");
    }

    void PlayerSession::AckBlockChangesUpTo(uint32_t sequence) {
        // MC takes the max so a tick that handled several interactions emits
        // one ack covering all of them.
        if (sequence > m_ackBlockChangesUpTo) {
            m_ackBlockChangesUpTo = sequence;
        }
    }

    void PlayerSession::FlushBlockChangeAck() {
        if (m_ackBlockChangesUpTo == 0 || !m_connection) return;

        Network::BlockChangedAckS2CPacket packet(m_ackBlockChangesUpTo);
        auto data = Network::Serialization::Serialize(packet);
        m_connection->SendPacket(
            static_cast<uint8_t>(Network::PacketId::BlockChangedAckS2C), data);
        m_ackBlockChangesUpTo = 0;
    }
    
    void PlayerSession::OnChunkSendComplete(Game::Math::ChunkPos chunk) {
        // Mark as sent
        m_sentChunks.insert(chunk);

        // Process any buffered diffs for this chunk
        auto diffIt = m_pendingDiffs.find(chunk);
        if (diffIt != m_pendingDiffs.end()) {
            for (const auto& [section, diffs] : diffIt->second) {
                if (!diffs.changes.empty()) {
                    m_diffQueue.push({chunk, section});
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksSent++;
        }
    }

    void PlayerSession::OnChunkUnloadComplete(Game::Math::ChunkPos chunk) {
        // Ensure chunk is removed from all sets. The tracking view is not a
        // set and is not touched here — it is derived from position and view
        // distance, and a chunk being unloaded does not change either.
        m_sentChunks.erase(chunk);
        m_pendingChunksToSend.erase(chunk);
    }

    // === STATISTICS ===

    PlayerSession::Stats PlayerSession::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        return m_stats;
    }

    // === GETTERS ===
    
    glm::vec3 PlayerSession::GetPosition() const {
        if (m_player) {
            return glm::vec3(m_player->getPosition());
        }
        return glm::vec3(0.0f);
    }
    
    glm::vec2 PlayerSession::GetRotation() const {
        if (m_player) {
            return m_player->getRotation();
        }
        return glm::vec2(0.0f);
    }
    
    int PlayerSession::GetDimensionId() const {
        if (m_player) {
            return m_player->getDimensionId();
        }
        return 0;
    }
    
    void PlayerSession::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    // === INTERNAL METHODS ===

    void PlayerSession::CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                           uint8_t localX, uint8_t localY, uint8_t localZ,
                                           Game::BlockID blockId, uint8_t stateIndex) {
        auto& sectionDiffs = m_pendingDiffs[chunk][section];
        sectionDiffs.chunkPos = chunk;
        sectionDiffs.sectionIndex = section;
        sectionDiffs.AddChange(localX, localY, localZ, blockId, stateIndex);
    }

    size_t PlayerSession::EstimatePacketSize(const Network::ChunkDataS2CPacket& packet) const {
        return packet.CalculateDataSize() + 32; // Add header overhead
    }

    size_t PlayerSession::EstimatePacketSize(const Network::MultiBlockChangeS2CPacket& packet) const {
        return packet.changes.size() * 8 + 16; // Estimate
    }

    void PlayerSession::ClearWatchSets() {
        m_trackingView = ChunkTrackingView::Empty();
        m_sentChunks.clear();
    }

    void PlayerSession::ClearQueues() {
        m_pendingChunksToSend.clear();

        // Clear diff queue
        while (!m_diffQueue.empty()) {
            m_diffQueue.pop();
        }
    }

    void PlayerSession::ClearDiffs() {
        m_pendingDiffs.clear();
    }

} // namespace Server
