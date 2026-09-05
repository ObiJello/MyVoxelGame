// File: src/server/session/PlayerSession.cpp
#include "server/level/ServerLevel.hpp"
#include "server/entity/MobManager.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/entity/SpawnEggs.hpp"
#include "PlayerSession.hpp"
#include "common/world/block/TntBlock.hpp"
#include "common/core/Mth.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../network/NetworkServer.hpp"
#include "../network/SendScheduler.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../entity/ExperienceOrbManager.hpp"
#include "PlayerSessionManager.hpp"
#include "common/core/Log.hpp"
#include "common/core/Assert.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/level/DimensionId.hpp"   // ChangeDimension's packet fields
#include "../level/ServerLevel.hpp"                 // SessionWorld
#include "../level/ServerLevel.hpp"                 // SessionWorld
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include <limits>
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/block/entity/BlockEntityTypes.hpp"
#include "common/world/block/entity/ChestBlockEntity.hpp"
#include "common/world/block/entity/CampfireBlockEntity.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "common/world/loot/LootTables.hpp"
#include "../IntegratedServer.hpp"
#include "../portal/ImmersivePortalRegistry.hpp"   // self-guarded by ENABLE_IMMERSIVE_PORTALS
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
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/Inventory.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>


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
        const Game::BlockState cur = world.GetBlockState(pos.x, pos.y, pos.z);
        if (cur.GetValueByName("type") == type) return;      // already right
        Game::BlockRegistry::BlockStateDefinition::PropertyMap props;
        props["facing"] = std::string(cur.GetValueByName("facing"));
        props["type"]   = type;
        world.SetBlock(pos.x, pos.y, pos.z, id, Game::World::UpdateFlags::All,
                       def.IndexOf(props));
    }

    // The cell this chest's stored type points at, or nullopt when SINGLE.
    std::optional<glm::ivec3> ChestPartnerCell(Game::BlockState state, const glm::ivec3& pos) {
        const std::string_view type = state.GetValueByName("type");
        if (type != "left" && type != "right") return std::nullopt;
        const std::string_view f = state.GetValueByName("facing");
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
            const Game::BlockState st = world.GetBlockState(n.x, n.y, n.z);
            const auto claimed = ChestPartnerCell(st, n);
            if (claimed && *claimed == pos) SetChestType(world, n, "single");
        }
    }
}

namespace Server {

    ItemEntityManager* PlayerSession::ItemEntitiesOrNull() const {
        // THIS session's dimension — GetItemEntities() is Overworld-pinned,
        // which is how an item dropped in the End spawned (invisibly, a
        // dimension away) in the Overworld.
        auto* server = g_integratedServer.get();
        if (!server) return nullptr;
        ServerLevel* level =
            server->GetLevel(Game::DimensionFromRaw(GetDimensionId()));
        return level ? level->Items() : nullptr;
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
        m_simulationDistance = std::clamp(config.simulationDistance, 2, 32);
        m_viewDistance = std::clamp(config.viewDistance, 2, 32);
        
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

        // Last, and only on the way out: this is what tells
        // IntegratedServer::OnClientSettingsReceived that applying a view
        // distance to this session will STICK. Everything above resets the
        // distances from `config`, so client settings applied before this
        // point are silently discarded (see the comment there).
        m_initialized = true;

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
        
        // Latency now comes from the connection's MC-style EMA, refreshed on
        // the network I/O thread by ServerConnection::HandleKeepAliveResponse
        // ((latency * 3 + time) / 4, MC ServerCommonPacketListenerImpl:85).
        // PlayerSession::HandleKeepAlive no longer runs — the keep-alive is
        // answered off the tick thread — and the value it used to compute was
        // meaningless anyway, since m_lastKeepAliveTx was written once at
        // session start and never again.
        if (m_connection) {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.latency = static_cast<float>(m_connection->GetLatencyMs());
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
            m_player->tick(SessionWorld(), static_cast<int>(serverTick));

            // MC ServerPlayer.die (:932) ends with
            //     this.connection.markClientUnloadedAfterDeath();
            // which blocks interaction until PERFORM_RESPAWN re-arms the timer.
            // Our ServerPlayer has no back-pointer to its connection, so the
            // session watches the alive→dead edge instead; the effect is the
            // same, one call at the moment of death.
            const bool deadNow = m_player->isDead();
            if (deadNow && !m_wasPlayerDead) {
                MarkClientUnloadedAfterDeath();

                // MC ServerPlayer.die (:900): when the showDeathMessages game
                // rule is on it does
                //     this.server.getPlayerList()
                //         .broadcastSystemMessage(deathMessage, false);
                // with the text from CombatTracker.getDeathMessage. We have no
                // combat tracker, so the message comes from the killing blow's
                // damage source, recorded on ServerPlayer.
                //
                // Not styled yellow: MC death messages use the default white,
                // unlike the join/leave notices.
                if (auto* server = g_integratedServer.get()) {
                    server->BroadcastSystemMessage(
                        Server::BuildDeathMessage(m_player->getName(),
                                                  m_player->getLastDamageSource(),
                                                  m_player->getLastAttackerName()),
                        0xFFFFFFFFu);
                }
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

                // XP triple — MC ServerPlayer.tick's lastSentExp dirty-check
                // (ClientboundSetExperiencePacket). Same shape as the health
                // sync above: first PLAYING tick always sends because the
                // cached values start impossible.
                const auto& xp = m_player->getExperience();
                if (xp.Level() != m_lastSentXpLevel
                    || xp.Progress() != m_lastSentXpProgress) {
                    Network::SetExperienceS2CPacket xpOut;
                    xpOut.progress = xp.Progress();
                    xpOut.level    = static_cast<uint32_t>(xp.Level());
                    xpOut.total    = static_cast<uint32_t>(xp.Total());
                    auto xpData = Network::Serialization::Serialize(xpOut);
                    m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SetExperienceS2C), xpData);
                    m_lastSentXpLevel    = xp.Level();
                    m_lastSentXpProgress = xp.Progress();
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

    void PlayerSession::ChangeDimension(int newDimensionId, const glm::vec3& targetPos,
                                        bool keepPrevious) {
        if (m_player && m_player->getDimensionId() == newDimensionId) return;
        const int previousRaw = m_player ? m_player->getDimensionId() : 0;
        const Game::DimensionId previous = Game::DimensionFromRaw(previousRaw);
        const Game::DimensionId dim      = Game::DimensionFromRaw(newDimensionId);

        m_isChangingDimension = true;

        // One barrier packet, not one unload per chunk. The client holds a
        // level per dimension (ClientLevel): this switches its active level
        // and, unless keepPrevious, frees the one being left. At view
        // distance 32 that is ~3,000 chunks in a single message rather than a
        // packet each, which is what overflowed the old inbound queue.
        if (m_connection) {
            Network::ChangeDimensionS2CPacket packet;
            packet.dimensionId  = static_cast<int8_t>(Game::DimensionToRaw(dim));
            packet.flags = static_cast<uint8_t>(
                (Game::DimensionHasSkyLight(dim)
                     ? Network::ChangeDimensionS2CPacket::kFlagHasSkyLight : 0) |
                (Game::DimensionHasCeiling(dim)
                     ? Network::ChangeDimensionS2CPacket::kFlagHasCeiling : 0) |
                (keepPrevious
                     ? Network::ChangeDimensionS2CPacket::kFlagKeepPrevious : 0));
            // MC DimensionTypes.java: the Nether's ambient light is 0.1, and
            // everywhere else it is 0.
            packet.ambientLight = (dim == Game::DimensionId::Nether) ? 0.1f : 0.0f;
            packet.minY   = Game::DimensionMinY(dim);
            packet.height = Game::DimensionLogicalHeight(dim);

            m_connection->SendPacket(
                static_cast<uint8_t>(Network::PacketId::ChangeDimensionS2C),
                Network::Serialization::Serialize(packet));
            // The client's stream scope is now the new dimension.
            m_connection->SetOutboundDimension(dim);
        }

        if (!keepPrevious) {
            // The client freed the previous level wholesale, so the server's
            // record of what that level held must go the same way — silently,
            // or the next tracking diff would send an unload for every chunk
            // of a level that no longer exists.
            ForgetDimensionSilently(previous);
        }
        // Any loader still covering the previous dimension (a portal at the
        // arrival point looking back) is re-evaluated by the next
        // UpdateChunkTracking from the new position.
        ClearQueues();
        ClearDiffs();

        if (m_player) {
            m_player->setDimensionId(newDimensionId);
            m_player->teleport(glm::dvec3(targetPos));
            m_currentChunk = m_player->getChunkPosition();
            m_anchorChunk  = m_currentChunk;
        }

        // A hard change shows the loading screen until the new level's chunks
        // arrive (MC's respawn path). A seamless crossing does not: the far
        // side was already streamed in through the portal.
        if (!keepPrevious) RestartClientLoadTimerAfterRespawn();
        m_isChangingDimension = false;
    }

#if ENABLE_IMMERSIVE_PORTALS
    void PlayerSession::HandlePortalTeleport(const Network::PortalTeleportC2SPacket& packet) {
        ASSERT_SERVER_THREAD();
        if (!HasClientLoaded()) return;
        if (m_player && m_player->isDead()) return;
        if (g_integratedServer) g_integratedServer->OnClientPortalTeleport(*this, packet);
    }
#endif

    void PlayerSession::ResyncChunkPosition() {
        if (m_player) UpdateChunkPosition(m_player->getChunkPosition());
    }

    void PlayerSession::SendDimensionResync() {
        if (!m_connection || !m_player) return;
        const Game::DimensionId dim = Game::DimensionFromRaw(m_player->getDimensionId());
        Network::ChangeDimensionS2CPacket packet;
        packet.dimensionId  = static_cast<int8_t>(Game::DimensionToRaw(dim));
        packet.flags = static_cast<uint8_t>(
            (Game::DimensionHasSkyLight(dim) ? Network::ChangeDimensionS2CPacket::kFlagHasSkyLight : 0) |
            (Game::DimensionHasCeiling(dim)  ? Network::ChangeDimensionS2CPacket::kFlagHasCeiling  : 0) |
            Network::ChangeDimensionS2CPacket::kFlagKeepPrevious);
        packet.ambientLight = (dim == Game::DimensionId::Nether) ? 0.1f : 0.0f;
        packet.minY   = Game::DimensionMinY(dim);
        packet.height = Game::DimensionLogicalHeight(dim);
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::ChangeDimensionS2C),
                                 Network::Serialization::Serialize(packet));
        m_connection->SetOutboundDimension(dim);
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
        
        // MC ChunkMap.setServerViewDistance / PlayerList.setViewDistance:
        // clamped to the protocol range only. This used to be capped at the
        // simulation distance as well, which is why the session manager had
        // to set every player's simulation distance to the server's view
        // cap (32) just to let them SEE 32 chunks — and so the server ticked
        // a 65x65-chunk area for everyone, whatever their settings said.
        m_viewDistance = std::clamp(distance, 2, 32);

        Log::Info("PlayerSession: Player %u view distance changed to %d",
                 m_playerId, m_viewDistance);
    }

    void PlayerSession::SetSimulationDistance(int distance) {
        if (distance == m_simulationDistance) {
            return;
        }

        // Independent of the view distance (see Config). The ticket manager
        // reads this on the next session tick (PlayerSessionManager::
        // UpdatePlayerTickets) and re-levels the player's ticket in place.
        m_simulationDistance = std::clamp(distance, 2, 32);

        Log::Info("PlayerSession: Player %u simulation distance changed to %d",
                 m_playerId, m_simulationDistance);
    }

    // === CHUNK TRACKING (MC ChunkMap.updateChunkTracking, per loader) ===

    void PlayerSession::UpdateChunkTracking(
        std::vector<ChunkLoader> loaders,
        const std::function<void(Game::DimensionId, Game::Math::ChunkPos)>& onEnter,
        const std::function<void(Game::DimensionId, Game::Math::ChunkPos)>& onLeave) {
        PROFILE_ZONE;

        // MC ChunkMap.updateChunkTracking's early-out: same loaders means the
        // tracked set is identical, so there is nothing to diff. This runs
        // every tick for every session, and this branch is what makes that
        // free — a player moving within one chunk does no work.
        bool same = loaders.size() == m_loaders.size();
        for (size_t i = 0; same && i < loaders.size(); ++i) {
            same = loaders[i].SameAs(m_loaders[i]);
        }
        if (same) return;

        std::unordered_set<DimChunkKey, DimChunkKeyHash> next;
        for (const ChunkLoader& loader : loaders) {
            loader.view.ForEach([&](Game::Math::ChunkPos pos) {
                next.insert(DimChunkKey::Of(loader.dimension, pos));
            });
        }

        // The callbacks run while m_watched is still the PREVIOUS set (MC
        // applyChunkTrackingView assigns the new view after difference()
        // too) — see MarkChunkPendingToSend for why that matters.
        int entered = 0, left = 0;
        for (const DimChunkKey& key : next) {
            if (m_watched.count(key)) continue;
            ++entered;
            onEnter(key.Dimension(), key.Pos());
        }
        for (const DimChunkKey& key : m_watched) {
            if (next.count(key)) continue;
            ++left;
            onLeave(key.Dimension(), key.Pos());
        }

        m_watched = std::move(next);
        m_loaders = std::move(loaders);

        Log::Info("UpdateChunkTracking: player %u loaders=%zu watched=%zu entered=%d left=%d",
                  m_playerId, m_loaders.size(), m_watched.size(), entered, left);

        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksInWatch = GetSentChunkCount() + GetPendingChunksToSendCount();
            m_stats.chunksPending = GetPendingChunksToSendCount();
        }
    }

    bool PlayerSession::IsWatching(Game::DimensionId dimension, Game::Math::ChunkPos chunk) const {
        return m_watched.count(DimChunkKey::Of(dimension, chunk)) > 0;
    }

    bool PlayerSession::HasSentChunk(Game::DimensionId dimension, Game::Math::ChunkPos chunk) const {
        return Dim(dimension).sent.count(chunk) > 0;
    }

    size_t PlayerSession::GetSentChunkCount() const {
        size_t n = 0;
        for (const auto& st : m_dimState) n += st.sent.size();
        return n;
    }

    size_t PlayerSession::GetPendingChunksToSendCount() const {
        size_t n = 0;
        for (const auto& st : m_dimState) n += st.pending.size();
        return n;
    }

    bool PlayerSession::LoadsDimension(Game::DimensionId dimension) const {
        if (Game::DimensionFromRaw(GetDimensionId()) == dimension) return true;
        for (const ChunkLoader& loader : m_loaders) {
            if (loader.dimension == dimension) return true;
        }
        return false;
    }

    int PlayerSession::DistanceSqToNearestLoader(Game::DimensionId dimension,
                                                 Game::Math::ChunkPos pos) const {
        int best = INT32_MAX;
        for (const ChunkLoader& loader : m_loaders) {
            if (loader.dimension != dimension) continue;
            const int dx = pos.x - loader.view.center.x;
            const int dz = pos.z - loader.view.center.z;
            best = std::min(best, dx * dx + dz * dz);
        }
        if (best == INT32_MAX) {
            const int dx = pos.x - m_anchorChunk.x;
            const int dz = pos.z - m_anchorChunk.z;
            best = dx * dx + dz * dz;
        }
        return best;
    }

    void PlayerSession::ForgetDimensionSilently(Game::DimensionId dimension) {
        m_globalPortalsSynced.erase(dimension);
        DimensionSendState& st = Dim(dimension);
        st.sent.clear();
        st.pending.clear();
        st.clientStamps.clear();
        for (auto it = m_watched.begin(); it != m_watched.end();) {
            it = (it->Dimension() == dimension) ? m_watched.erase(it) : std::next(it);
        }
        m_loaders.erase(std::remove_if(m_loaders.begin(), m_loaders.end(),
                                       [&](const ChunkLoader& l) { return l.dimension == dimension; }),
                        m_loaders.end());
    }

    // === CHUNK SENDER (Minecraft's PlayerChunkSender) ===

    void PlayerSession::MarkChunkPendingToSend(Game::DimensionId dimension, Game::Math::ChunkPos pos) {
        // Queue unconditionally, exactly like MC
        // PlayerChunkSender.markChunkPendingToSend, which is a bare
        // `pendingChunks.add(chunk.getPos().toLong())`.
        //
        // There must be NO tracking test here. Both callers have already
        // established membership, and one of them cannot pass such a test:
        // UpdateChunkTracking runs the enter callbacks while m_watched is
        // still the PREVIOUS set (MC applyChunkTrackingView assigns the new
        // view after difference() too), so a chunk that just entered is by
        // definition absent from it. A guard here therefore drops every
        // already-loaded chunk at the moment it comes into view — which meant
        // the spawn chunk, generated before the player joined, was never
        // sent, and the client then ran its occlusion BFS from a camera chunk
        // it did not have.
        //
        // The push path does the test at its own call site
        // (PlayerSessionManager::ForEachSessionWatching), where the set is
        // current, and SendNextChunks re-checks before sending — so a chunk
        // queued and then walked away from is still dropped correctly.
        Dim(dimension).pending.insert(pos);
    }

    void PlayerSession::DropChunk(Game::DimensionId dimension, Game::Math::ChunkPos pos) {
        DimensionSendState& st = Dim(dimension);
        if (!st.pending.erase(pos)) {
            // Wasn't pending to send — if already sent, send unload to client
            if (st.sent.erase(pos)) {
                SendChunkUnload(dimension, pos);
            }
        }
    }

    void PlayerSession::SendNextChunks(const std::function<Game::World*(Game::DimensionId)>& worldFor,
                                       std::vector<PendingChunkRef>* outNotResident) {
        if (!m_connection) return;
        bool anyPending = false;
        for (const auto& st : m_dimState) if (!st.pending.empty()) { anyPending = true; break; }
        if (!anyPending) return;
        PROFILE_ZONE_N("SendNextChunks");

        // Retained-on-client chunks go out immediately, outside the batch
        // quota: a ChunkUnchangedS2C is 20 bytes and the client's work for it
        // is a pointer swap, so pacing them like 20 KB chunk payloads would
        // only delay an instant revisit. Capped per tick for sanity.
        {
            size_t sentUnchanged = 0;
            for (int slot = 0; slot < Game::kDimensionCount && sentUnchanged < 4096; ++slot) {
                DimensionSendState& st = m_dimState[slot];
                if (st.pending.empty()) continue;
                const Game::DimensionId dim = Game::DimensionFromSlot(slot);
                Game::World* world = worldFor(dim);
                if (!world) continue;
                for (auto it = st.pending.begin(); it != st.pending.end() && sentUnchanged < 4096; ) {
                    const Game::Math::ChunkPos pos = *it;
                    auto known = st.clientStamps.find(pos);
                    if (known == st.clientStamps.end() || !IsWatching(dim, pos)) { ++it; continue; }
                    auto chunk = world->GetLoadedChunk(pos.x, pos.z);
                    if (!chunk || chunk->ModStamp() != known->second) { ++it; continue; }
                    Network::ChunkUnchangedS2CPacket unchanged;
                    unchanged.chunkX = pos.x; unchanged.chunkZ = pos.z; unchanged.modStamp = known->second;
                    m_connection->SendPacketIn(dim,
                                               static_cast<uint8_t>(Network::PacketId::ChunkUnchangedS2C),
                                               Network::Serialization::Serialize(unchanged));
                    st.sent.insert(pos);
                    if (g_integratedServer) g_integratedServer->OnChunkSentToClient(*this, dim, pos);
                    ++m_unchangedSent; ++sentUnchanged;
                    it = st.pending.erase(it);
                }
            }
            anyPending = false;
            for (const auto& st : m_dimState) if (!st.pending.empty()) { anyPending = true; break; }
            if (!anyPending) return;
        }

        // Back-pressure: don't send if too many unacknowledged batches
        if (m_unackedBatches >= m_maxUnackedBatches) return;

        // Accumulate fractional budget
        float maxBatchSize = std::max(1.0f, m_desiredChunksPerTick);
        m_batchQuota = std::min(m_batchQuota + m_desiredChunksPerTick, maxBatchSize);

        if (m_batchQuota < 1.0f) return;

        int maxBatch = static_cast<int>(m_batchQuota);

        // Collect loaded chunks from EVERY dimension, sorted by distance from
        // the loader that wants them — nearest first across all worlds, as
        // the mod's PlayerChunkLoading does.
        struct ChunkDist {
            Game::DimensionId dimension;
            Game::Math::ChunkPos pos;
            std::shared_ptr<Game::Chunk> chunk;
            int distSq;
        };
        std::vector<ChunkDist> candidates;
        candidates.reserve(GetPendingChunksToSendCount());

        for (int slot = 0; slot < Game::kDimensionCount; ++slot) {
            DimensionSendState& st = m_dimState[slot];
            if (st.pending.empty()) continue;
            const Game::DimensionId dim = Game::DimensionFromSlot(slot);
            Game::World* world = worldFor(dim);

            // Also collect chunks to remove from pending if they left the view
            std::vector<Game::Math::ChunkPos> staleChunks;
            for (const auto& pos : st.pending) {
                // Skip chunks no longer tracked (queued, then the player walked away)
                if (!IsWatching(dim, pos)) {
                    staleChunks.push_back(pos);
                    continue;
                }
                // Cache-only. This chunk was queued because it WAS loaded, but
                // it can have been evicted since — and the blocking GetChunk
                // would then regenerate it here, on the server thread, inside
                // the send loop. Skipping is what "picked up later" means.
                auto chunk = world ? world->GetLoadedChunk(pos.x, pos.z) : nullptr;
                if (!chunk) {
                    // Evicted between "ready" and "sent" (or its level is not
                    // built yet). Nothing reloads a chunk that is already
                    // watched, so hand it back to the caller to request again
                    // instead of waiting here forever.
                    staleChunks.push_back(pos);
                    if (outNotResident) outNotResident->push_back(PendingChunkRef{dim, pos});
                    continue;
                }
                candidates.push_back({dim, pos, chunk, DistanceSqToNearestLoader(dim, pos)});
            }
            for (const auto& pos : staleChunks) {
                st.pending.erase(pos);
                Log::Debug("SendNextChunks: removed stale chunk (%d, %d) from pending (no longer watched)",
                          pos.x, pos.z);
            }
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
        // A time budget on top of the count quota. The client may ask for up
        // to 256 chunks a tick and serialising one costs ~90 µs, so a burst
        // after a join or a portal crossing was 20-27 ms of a 50 ms tick
        // (Tracy, 2026-09-04: every tick over 25 ms in the capture was this).
        // Nearest-first order is kept: what does not fit stays pending for
        // the next tick, a delay nobody can see, and the tick stays flat.
        constexpr auto kSerializeBudget = std::chrono::milliseconds(12);
        const auto sendStart = std::chrono::steady_clock::now();
        for (size_t i = 0; i < toSend; ++i) {
            if (i > 0 && std::chrono::steady_clock::now() - sendStart > kSerializeBudget) break;
            const auto& cd = candidates[i];
            DimensionSendState& st = Dim(cd.dimension);
            PROFILE_ZONE_N("SerializeChunk");

            // Retained on the client and unchanged since -> 20-byte packet.
            {
                const uint64_t stamp = cd.chunk->ModStamp();
                auto known = st.clientStamps.find(cd.pos);
                if (known != st.clientStamps.end() && known->second == stamp) {
                    Network::ChunkUnchangedS2CPacket unchanged;
                    unchanged.chunkX = cd.pos.x; unchanged.chunkZ = cd.pos.z; unchanged.modStamp = stamp;
                    m_connection->SendPacketIn(cd.dimension,
                                               static_cast<uint8_t>(Network::PacketId::ChunkUnchangedS2C),
                                               Network::Serialization::Serialize(unchanged));
                    st.pending.erase(cd.pos);
                    st.sent.insert(cd.pos);
                    if (g_integratedServer) g_integratedServer->OnChunkSentToClient(*this, cd.dimension, cd.pos);
                    sentCount++;
                    ++m_unchangedSent;
                    continue;
                }
                st.clientStamps[cd.pos] = stamp;
            }

            // Build ChunkDataS2CPacket
            Network::ChunkDataS2CPacket packet;
            packet.chunkX = cd.pos.x;
            packet.chunkZ = cd.pos.z;
            packet.groundUpContinuous = true;
            packet.modStamp = cd.chunk->ModStamp();
            packet.sections.reserve(Game::Math::SECTIONS_PER_CHUNK);

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

            // EVERY section, in order, with no skipping — the wire is
            // positional now and section Y is the index. This loop used to
            // `continue` past all-air sections and record the survivors in a
            // bitmask; the omission took their BIOMES with it, which is why
            // anything placed high above terrain tinted with the fallback
            // biome. MC has no such filter and no such mask.
            //
            // An all-air section costs about seven bytes: a two-byte count,
            // then two single-value containers, each one `bits` byte plus a
            // one-entry VarInt palette and no words at all.
            for (int sectionY = 0; sectionY < Game::Math::SECTIONS_PER_CHUNK; ++sectionY) {
                const auto* section = cd.chunk->GetSection(sectionY);

                Network::ChunkDataS2CPacket::SectionData sectionData;

                // Defensive, and load-bearing if it ever fires: a `continue`
                // here would shift every later section down one slot on a
                // positional wire. Emit a placeholder instead so the stream
                // stays in phase. GetSection is total for an in-range index,
                // so this is unreachable today.
                if (!section) {
                    Log::Warning("[PlayerSession] Chunk (%d, %d) section %d missing; "
                                 "sending an empty placeholder to keep the stream aligned",
                                 cd.pos.x, cd.pos.z, sectionY);
                    sectionData.states.bits = 0;
                    sectionData.states.palette = { Game::BlockState{}.RawId() };
                    sectionData.biomes.bits = 0;
                    sectionData.biomes.palette = { Game::kFallbackBiomeId };
                    packet.sections.push_back(std::move(sectionData));
                    continue;
                }

                // MC nonEmptyBlockCount. Counted off the palette rather than by
                // walking voxels: the container already knows how many of each
                // distinct state it holds. IsAllAir short-circuits the common
                // sky case to zero without touching the palette at all.
                uint32_t nonAir = 0;
                if (!section->IsAllAir()) {
                    section->States().ForEachValue([&](uint32_t stateId, int count) {
                        if (Game::BlockState::FromRawId(stateId).Block() != Game::BlockID::Air) {
                            nonAir += static_cast<uint32_t>(count);
                        }
                    });
                }
                sectionData.blockCount = static_cast<uint16_t>(nonAir > 0xFFFFu ? 0xFFFFu : nonAir);

                copyContainer(section->States(), sectionData.states);
                copyContainer(section->Biomes(), sectionData.biomes);

                packet.sections.push_back(std::move(sectionData));
            }

            // Biomes ride inside each section's container above, where MC
            // keeps them — not as a flat per-chunk array.

            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacketIn(cd.dimension,
                                       static_cast<uint8_t>(Network::PacketId::ChunkDataS2C), data);

            // The chunk's block entities follow it (MC carries them inside
            // ClientboundLevelChunkWithLightPacket). Without this a chest
            // only ever reached a client that watched it being placed: a
            // chunk streamed in — a rejoin, a walk back — had the chest
            // block and nothing to draw or open in it.
            for (const auto& [localPos, be] : cd.chunk->GetAllBlockEntities()) {
                if (!be || !be->GetType()) continue;
                const glm::ivec3 world = be->GetWorldPos();
                Network::BlockEntityDataS2CPacket bePacket(world.x, world.y, world.z,
                                                           be->GetType()->TypeId());
                Network::PacketBuffer scratch;
                be->Save(scratch);
                bePacket.dataBlob = scratch.GetData();
                m_connection->SendPacketIn(cd.dimension,
                                           static_cast<uint8_t>(Network::PacketId::BlockEntityDataS2C),
                                           Network::Serialization::Serialize(bePacket));
            }

            // Move from pending to sent
            st.pending.erase(cd.pos);
            st.sent.insert(cd.pos);
            // Portals (and, later, anything else anchored in the chunk) ride
            // right behind the terrain so the client never holds one without
            // the blocks it sits in.
            if (g_integratedServer) g_integratedServer->OnChunkSentToClient(*this, cd.dimension, cd.pos);
            sentCount++;

            Log::Debug("Sent chunk (%d, %d) [%s] to player %u", cd.pos.x, cd.pos.z,
                       std::string(Game::DimensionName(cd.dimension)).c_str(), m_playerId);
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

    void PlayerSession::OnChunkRequestFull(Game::DimensionId dimension, Game::Math::ChunkPos pos) {
        // The client evicted its retained copy: forget the stamp so the next
        // send is a full ChunkDataS2C, and queue it again if still in view.
        DimensionSendState& st = Dim(dimension);
        st.clientStamps.erase(pos);
        if (IsWatching(dimension, pos)) {
            st.sent.erase(pos);
            st.pending.insert(pos);
        }
    }

    void PlayerSession::OnChunkBatchAck(float desiredRate) {
        m_unackedBatches--;
        m_desiredChunksPerTick = std::isnan(desiredRate) ? 0.01f : std::clamp(desiredRate, 0.01f, 256.0f);   // matches the client clamp (was vanilla's 64)
        if (m_unackedBatches == 0) m_batchQuota = 1.0f;
        m_maxUnackedBatches = 10;

        Log::Debug("Chunk batch ack: desiredRate=%.2f, unacked=%d, maxUnacked=%d (player %u)",
                  m_desiredChunksPerTick, m_unackedBatches, m_maxUnackedBatches, m_playerId);
    }

    void PlayerSession::SendChunkUnload(Game::DimensionId dimension, Game::Math::ChunkPos chunk) {
        // Send UnloadChunkS2CPacket directly through the connection
        // (SendScheduler's sendCallback is not implemented, so bypass it)
        if (m_connection) {
            Network::UnloadChunkS2CPacket packet(chunk.x, chunk.z);
            auto data = Network::Serialization::Serialize(packet);
            m_connection->SendPacketIn(dimension,
                static_cast<uint8_t>(Network::PacketId::UnloadChunkS2C), data);
        }

        // Remove from sets. Not from the watched set — that is a function of
        // the loaders, and unloading a chunk changes none of them.
        // (MC's dropChunk likewise only touches the sender's queues.)
        DimensionSendState& st = Dim(dimension);
        st.sent.erase(chunk);
        st.pending.erase(chunk);

        // Clear any pending diffs for this chunk
        m_pendingDiffs.erase(chunk);

        Log::Debug("UNLOAD SENT: chunk (%d, %d) [%s] to player %u",
                  chunk.x, chunk.z, std::string(Game::DimensionName(dimension)).c_str(), m_playerId);
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
                                                     blockId.Block(), blockId.Index());
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
                                     static_cast<uint16_t>(blockId.Block()), blockId.Index());
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
        // Server thread only. Until the packet-threading rework this ran
        // inline on the network I/O thread, racing the server tick that
        // mutates the same ServerPlayer; the assert is the standing proof
        // that it does not any more.
        ASSERT_SERVER_THREAD();
        // Stale client-predicted moves that were in flight when we issued a
        // teleport (the client may have sent 1–2 MovePlayer packets at the old
        // position before processing our ClientboundPlayerPosition). Applying
        // their POSITION would revert the server-side player to the
        // pre-teleport spot and other clients would see this player flicker.
        //
        // MC handleMovePlayer (:1030) does NOT drop the packet outright — it
        // takes the rotation and discards only the position:
        //
        //     if (this.updateAwaitingTeleport()) {
        //        this.player.absSnapRotationTo(targetYRot, targetXRot);
        //     } else { ...full move... }
        //
        // which is why looking around still works in vanilla while a teleport
        // is pending. updateAwaitingTeleport also re-sends the teleport if it
        // has gone unacknowledged for 20 ticks, so this can never latch.
        // MC handleMovePlayer:1027 puts its ENTIRE body — rotation included —
        // behind hasClientLoaded(). A position produced before the client's
        // world exists is a position produced against empty air, and adopting
        // it is what let a rejoining player's saved Y sink a few blocks per
        // session. The client is gated too (LocalPlayer.tick:212, ported in
        // PlatformMain); this is the half that does not depend on the client
        // being well-behaved.
        if (!HasClientLoaded()) return;

        // A move stamped with another dimension was produced before the
        // client learned of (or predicted) a dimension change; its
        // coordinates belong to the other world.
        if (m_player && packet.dimensionId != Network::PlayerMoveC2SPacket::kDimensionUnknown &&
            packet.dimensionId != m_player->getDimensionId()) {
            return;
        }

        const bool awaitingTeleport =
            m_connection && m_connection->UpdateAwaitingTeleport();

        // Dead players don't move — MC freezes the body until the client
        // sends PERFORM_RESPAWN (ServerGamePacketListenerImpl.handleMovePlayer
        // returns early when player.isImmobile()/dead).
        if (m_player && m_player->isDead()) {
            return;
        }

        if (awaitingTeleport) {
            if (m_player) {
                m_player->setRotation(packet.rotation.x, packet.rotation.y);
            }
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
        {
            if (Game::World* world = SessionWorld()) {
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
            // In body heights, not blocks: a player half the size falls
            // twice as far relative to themselves (the scaled portal's
            // "the world is just bigger" rule, see ApplyGravity).
            const float fd = std::min(packet.fallDistance, 512.0f) / std::max(p.getScale(), 0.05f);
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
        // Server thread only. Until the packet-threading rework this ran
        // inline on the network I/O thread, racing the server tick that
        // mutates the same ServerPlayer; the assert is the standing proof
        // that it does not any more.
        ASSERT_SERVER_THREAD();
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
            // MC's START_DESTROY / ABORT_DESTROY are purely informational for
            // mining progress (the client is authoritative on timing) — but
            // START is also where MC fires BlockBehaviour.attack, the block's
            // reaction to being punched. ServerPlayerGameMode only attacks in
            // survival; a creative press instabreaks without ever attacking.
            case Network::BlockActionType::START_DESTROY: {
                if (m_player->isCreative()) break;
                const glm::ivec3 pos(packet.worldX, packet.worldY, packet.worldZ);
                glm::vec3 eye;
                InteractionScope scope;
                Game::World* world = InteractionWorld(packet.dimensionId, pos, eye, scope);
                if (!world) break;
                if (glm::length(glm::vec3(pos) + glm::vec3(0.5f) - eye) > m_player->getReachDistance()) break;
                const Game::BlockID id = world->GetBlock(pos.x, pos.y, pos.z);
                const Game::Block& def = Game::BlockRegistry::Get(id);
                if (def.attack) def.attack(*world, pos);
                break;
            }
            case Network::BlockActionType::ABORT_DESTROY:
                break;
            // Both BREAK (legacy) and STOP_DESTROY (new) finalize the dig.
            case Network::BlockActionType::STOP_DESTROY:
            case Network::BlockActionType::BREAK: {
                glm::ivec3 pos(packet.worldX, packet.worldY, packet.worldZ);

                // Validate reach
                glm::vec3 blockCenter = glm::vec3(pos) + glm::vec3(0.5f);
                // The world the block is in (its own level, or one reached
                // through a portal) and the eye the reach is measured from.
                glm::vec3 eye;
                InteractionScope scope;
                Game::World* world = InteractionWorld(packet.dimensionId, pos, eye, scope);
                if (!world) {
                    Log::Warning("HandleBlockAction: Player %u has no way into dimension %d for (%d,%d,%d)",
                                m_playerId, static_cast<int>(packet.dimensionId), pos.x, pos.y, pos.z);
                    return;
                }
                if (glm::length(blockCenter - eye) > m_player->getReachDistance()) {
                    Log::Warning("HandleBlockAction: Player %u cannot reach (%d,%d,%d)",
                                m_playerId, pos.x, pos.y, pos.z);
                    return;
                }

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
                Game::BlockState oldBlockState =
                    Game::BlockStates::FromIndex(oldBlock, packet.blockState);
                if (packet.blockState == 0) {
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
                // A door's other half goes with the one that was broken (MC
                // does it through updateShape; this engine has no double-
                // block linkage, so it is explicit here, as in the zombie's
                // break-door goal).
                if (Game::IsDoorBlock(oldBlock)) {
                    const bool lower = oldBlockState.GetValueByName("half") == "lower";
                    const glm::ivec3 other = pos + glm::ivec3(0, lower ? 1 : -1, 0);
                    if (world->GetBlock(other.x, other.y, other.z) == oldBlock) {
                        world->SetBlock(other.x, other.y, other.z, Game::BlockID::Air);
                    }
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
                        // A furnace destroyed with banked smelting XP pays it
                        // out at the block — MC AbstractFurnaceBlockEntity
                        // .preRemoveSideEffects → getRecipesToAwardAndPop-
                        // Experience(level, Vec3.atCenterOf(pos)). Like the
                        // contents spill above, this runs regardless of game
                        // mode and tool: the XP was already earned by the
                        // smelts, it was never the block's loot.
                        if (auto* furnace = dynamic_cast<Game::FurnaceBlockEntity*>(be)) {
                            AwardBankedExperience(
                                glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                furnace->TakeStoredExperience());
                        }
                    }
                }
                // Bedrock is unbreakable in survival/adventure, but creative
                // destroys it outright (MC's ServerPlayerGameMode never
                // consults destroyTime on the creative path).
                const bool creativeBreak =
                    (m_player->getGameMode() == Server::GameMode::CREATIVE);
                if (oldBlock == Game::BlockID::Bedrock && !creativeBreak) return;

                // MC TntBlock.playerWillDestroy — an UNSTABLE TNT primes
                // instead of dropping when a survival player breaks it. Runs
                // BEFORE the cell is cleared, because priming reads the state.
                // Nothing sets `unstable` true yet (it needs a datapack or a
                // /setblock), so this is inert but correct.
                bool primedOnBreak = false;
                if (oldBlock == Game::BlockID::Tnt) {
                    primedOnBreak = Game::TntPlayerWillDestroy(
                        *world, pos, oldBlockState, nullptr, creativeBreak);
                }

                // SetBlock may already be a no-op (the world is already Air in integrated
                // mode), but call it anyway so dedicated multiplayer still clears the
                // server's world.
                //
                // MC Level.destroyBlock:266 is
                // `setBlock(pos, fluidState.createLegacyBlock(), 3, ...)` — the
                // cell becomes the FLUID that was in it, not air. That is what
                // leaves water behind when you break a waterlogged fence or a
                // kelp stalk, and it is the only reason those don't punch a dry
                // hole through an ocean.
                //
                // Read from the packet's block+state for the same reason the
                // two are read from the packet above: in integrated mode the
                // client's break prediction has already cleared this cell.
                const Game::BlockID replacement =
                    Game::BlockRegistry::ContainsWater(oldBlockState)
                        ? Game::BlockID::Water
                        : Game::BlockID::Air;
                world->SetBlock(pos.x, pos.y, pos.z, replacement);
                // A TNT that primed on break has become an entity; dropping the
                // item as well would duplicate it.
                if (primedOnBreak) return;
#if ENABLE_PORTAL_GUN
                // Remove any portal mounted on this block. Block-break
                // bypasses IntegratedServer::ApplyBlockChange so the
                // notification has to happen here too.
                Game::Portal::ServerRegistry().OnBlockChanged(world->GetDimension(), pos);
#endif
#if ENABLE_IMMERSIVE_PORTALS
                // An immersive nether portal's frame block. World::SetBlock's
                // own obsidian hook does not fire on this path: in integrated
                // mode the client's prediction cleared the shared cell before
                // the packet arrived, so the SetBlock above saw air -> air.
                // The packet still says what was broken.
                if ((oldBlock == Game::BlockID::Obsidian || oldBlock == Game::BlockID::CryingObsidian) &&
                    g_integratedServer) {
                    g_integratedServer->OnObsidianRemoved(world->GetDimension(), pos);
                }
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
                        lootCtx.blockState     = oldBlockState.Index();
                        lootCtx.tool           = &heldStack;
                        lootCtx.blocks         = world;
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

                        // MC Block.spawnAfterBreak's XP half — ores, sculk and
                        // the spawner pay orbs at the block's centre
                        // (Block.popExperience → ExperienceOrb.award at
                        // Vec3.atCenterOf(pos)). Same gate as the loot: player
                        // break, correct tool, not creative.
                        const int blockXp = Game::LootTables::RollBlockBreakExperience(
                            oldBlock, &heldStack, m_lootRandom);
                        if (blockXp > 0) {
                            AwardWorldExperience(
                                glm::dvec3(pos.x + 0.5, pos.y + 0.5, pos.z + 0.5),
                                blockXp);
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
        // Server thread only. Until the packet-threading rework this ran
        // inline on the network I/O thread, racing the server tick that
        // mutates the same ServerPlayer; the assert is the standing proof
        // that it does not any more.
        ASSERT_SERVER_THREAD();
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

    void PlayerSession::HandlePickItem(const Network::PickItemC2SPacket& packet) {
        ASSERT_SERVER_THREAD();
        if (!m_player || !m_connection) return;
        // This game's rule: pick block is a creative tool. (MC also lets a
        // survival player pick a stack they already carry.)
        if (!m_player->isCreative()) return;
        auto* server = g_integratedServer.get();
        if (!server) return;
        ServerLevel* level = server->GetLevel(Game::DimensionFromRaw(GetDimensionId()));
        if (!level || !level->World()) return;

        // ── What was aimed at → the item (BlockState.getCloneItemStack /
        //    Entity.getPickResult) ──────────────────────────────────────
        Game::ItemID item = Game::Items::Air;
        if (packet.kind == Network::PickItemC2SPacket::Kind::Block) {
            // MC isWithinBlockInteractionRange(pos, 1.0): creative reach 5
            // plus the one-block allowance, measured to the block's box.
            const glm::dvec3 eye = m_player->getPosition() + glm::dvec3(0.0, 1.62, 0.0);
            const glm::dvec3 nearest = glm::clamp(eye, glm::dvec3(packet.x, packet.y, packet.z),
                                                  glm::dvec3(packet.x + 1, packet.y + 1, packet.z + 1));
            if (glm::distance(eye, nearest) > 6.0) return;
            const Game::BlockID block = level->World()->GetBlockState(packet.x, packet.y, packet.z).Block();
            if (block == Game::BlockID::Air) return;
            item = Game::ItemRegistry::FromBlock(block);
        } else {
            Game::Mob* mob = level->Mobs() ? level->Mobs()->Find(packet.entityId) : nullptr;
            if (!mob) return;
            if (glm::distance(m_player->getPosition(), mob->position) > 8.0) return;   // isWithinEntityInteractionRange(3) + slack
            for (const Game::SpawnEggEntry& e : Game::kSpawnEggTable) {
                if (e.type == mob->GetType()) { item = e.item; break; }
            }
        }
        if (item == Game::Items::Air) return;
        Game::ItemStack picked;
        picked.itemId = item;
        picked.count  = 1;

        // ── tryPickItem, on MC's 36 player slots (hotbar first, then main) ─
        Game::Inventory& inv = m_player->getInventory();
        auto playerSlots = [&](auto&& fn) {   // MC Inventory.items order
            for (int i = 0; i < Game::Inventory::HOTBAR_SIZE; ++i) if (fn(Game::Inventory::HotbarToIndex(i))) return;
            for (int i = 0; i < Game::Inventory::MAIN_SIZE;   ++i) if (fn(Game::Inventory::MAIN_BEGIN + i)) return;
        };
        auto setSlot = [&](int index, const Game::ItemStack& stack) {
            inv.SetSlotFull(index, stack);
            m_player->markSlotDirty(index);
        };
        // Inventory.getSuitableHotbarSlot: the first empty hotbar slot from
        // the selected one round, else the selected slot itself.
        auto suitableHotbarSlot = [&]() {
            const int selected = inv.GetSelectedSlot();
            for (int i = 0; i < Game::Inventory::HOTBAR_SIZE; ++i) {
                const int slot = (selected + i) % Game::Inventory::HOTBAR_SIZE;
                if (inv.GetSlot(Game::Inventory::HotbarToIndex(slot)).IsEmpty()) return slot;
            }
            return selected;
        };

        // Inventory.findSlotMatchingItem — already carrying it: switch to it
        // (hotbar) or swap it into the hotbar (pickSlot).
        int matching = -1;
        playerSlots([&](int index) {
            const Game::ItemStack& s = inv.GetSlot(index);
            if (!s.IsEmpty() && Game::IsSameItemSameComponents(s, picked)) { matching = index; return true; }
            return false;
        });
        if (matching >= 0) {
            if (Game::Inventory::IsHotbarSlot(matching)) {
                m_player->selectHotbarSlot(Game::Inventory::IndexToHotbar(matching));
            } else {
                // Inventory.pickSlot: into a suitable hotbar slot, the held
                // stack taking the picked one's place in the inventory.
                m_player->selectHotbarSlot(suitableHotbarSlot());
                const int hot = Game::Inventory::HotbarToIndex(inv.GetSelectedSlot());
                const Game::ItemStack held = inv.GetSlot(hot);
                const Game::ItemStack found = inv.GetSlot(matching);
                setSlot(hot, found);
                setSlot(matching, held);
            }
        } else {
            // Inventory.addAndPickItem: into an empty hotbar slot (the
            // selected one when empty); with the hotbar full, into the
            // selected slot and the held stack moves to a free inventory
            // slot — or, this game's addition, onto the ground when the
            // inventory is full too (MC would overwrite it).
            m_player->selectHotbarSlot(suitableHotbarSlot());
            const int hot = Game::Inventory::HotbarToIndex(inv.GetSelectedSlot());
            const Game::ItemStack held = inv.GetSlot(hot);
            if (!held.IsEmpty()) {
                int freeSlot = -1;
                playerSlots([&](int index) { if (inv.GetSlot(index).IsEmpty()) { freeSlot = index; return true; } return false; });
                if (freeSlot >= 0) {
                    setSlot(freeSlot, held);
                } else {
                    Game::DropItemStackAt(Game::DimensionFromRaw(GetDimensionId()), m_player->getPosition(), held);
                }
            }
            setSlot(hot, picked);
        }

        // ClientboundSetHeldSlotPacket; the slot writes ride the per-tick
        // inventory diff (broadcastChanges).
        Network::SetHeldSlotS2CPacket out;
        out.slot = static_cast<uint8_t>(inv.GetSelectedSlot());
        m_connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SetHeldSlotS2C),
                                 Network::Serialization::Serialize(out));
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
        Game::World* sessionWorld = SessionWorld();
        if (!sessionWorld) return false;

        const auto cp = Game::Math::WorldCoordinates::WorldToChunkPos(
            m_openMenuPos.x, m_openMenuPos.z);
        auto chunk = sessionWorld->GetChunk(cp.x, cp.z);
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
            auto pchunk = sessionWorld->GetChunk(pcp.x, pcp.z);
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
        // Server thread only. Until the packet-threading rework this ran
        // inline on the network I/O thread, racing the server tick that
        // mutates the same ServerPlayer; the assert is the standing proof
        // that it does not any more.
        ASSERT_SERVER_THREAD();
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

        // Taking from a furnace's result slot freed its banked smelting XP
        // (FurnaceResultSlot::OnTake). MC pays it as orbs at the player's own
        // position (AbstractFurnaceBlockEntity.awardUsedRecipesAndPop-
        // Experience → player.position()), where the pickup pull collects
        // them immediately.
        AwardBankedExperience(m_player->getPosition(), result.xpBanked);
    }

    void PlayerSession::AwardWorldExperience(const glm::dvec3& pos, int amount) {
        if (amount <= 0) return;
        // THIS session's level — GetXpOrbs() is Overworld-pinned, so mining
        // or smelting XP earned in another dimension paid out a world away.
        auto* server = g_integratedServer.get();
        if (!server) return;
        ServerLevel* level =
            server->GetLevel(Game::DimensionFromRaw(GetDimensionId()));
        if (auto* orbs = level ? level->Orbs() : nullptr) {
            orbs->Award(pos, amount);
        }
    }

    void PlayerSession::AwardBankedExperience(const glm::dvec3& pos, float banked) {
        if (banked <= 0.0f) return;
        // MC AbstractFurnaceBlockEntity.createExperience: floor the banked
        // total, then a random chance at the fractional remainder. (MC rounds
        // per recipe type; we bank one float total and round once — the
        // expected value is identical, and the difference is at most one
        // fractional roll per payout.)
        const int amount = Server::PlayerExperience::RoundBankedExperience(
            banked, m_lootRandom.NextFloat());
        AwardWorldExperience(pos, amount);
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
        // Server thread only. Until the packet-threading rework this ran
        // inline on the network I/O thread, racing the server tick that
        // mutates the same ServerPlayer; the assert is the standing proof
        // that it does not any more.
        ASSERT_SERVER_THREAD();
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
        // Noclip is NOT gated on canFly: the client resolves it in its own
        // physics either way, so refusing the bit would only stop the state
        // being saved. Recorded first so it survives even the corrective path.
        m_player->setNoclip(packet.noclip());

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

        Game::World* world = SessionWorld();
        if (!world) return;

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

    void PlayerSession::FlushPendingDrops() {
        if (!m_player) return;
        for (const Game::ItemStack& stack : m_player->takePendingDrops()) {
            DropItemFromPlayer(stack);
        }
    }

    void PlayerSession::FlushPendingMenuOpen() {
        if (!m_player || !m_connection) return;
        auto pending = m_player->takePendingMenuOpen();
        if (!pending) return;

        // Block-backed menus need the container living at the clicked cell.
        // MC gets there via state.getMenuProvider(level, pos), which resolves
        // the block entity; ours is the same lookup the placement path uses.
        auto containerAt = [this](const glm::ivec3& pos) -> Game::BaseContainerBlockEntity* {
            Game::World* world = SessionWorld();
            if (!world) return nullptr;
            const auto chunkPos =
                Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            auto chunk = world->GetChunk(chunkPos.x, chunkPos.z);
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
                    world->GetBlock(pos.x, pos.y, pos.z);
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
        auto CountBookshelvesAround = [this](const glm::ivec3& tablePos) -> int {
            Game::World* w = SessionWorld();
            if (!w) return 0;
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
        auto blockNameAt = [this](const glm::ivec3& pos) -> std::string {
            Game::World* world = SessionWorld();
            if (!world) return {};
            return Game::BlockRegistry::Get(
                world->GetBlock(pos.x, pos.y, pos.z)).name;
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
                Game::World* world = SessionWorld();
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
        
        // The clicked block's world — the player's own, or the level behind
        // a portal they are reaching through — and the eye to measure reach
        // from (mapped through that portal in the second case).
        glm::vec3 interactionEye;
        InteractionScope interactionScope;
        double portalSearchRadius = 0.0;
#if ENABLE_PORTAL_GUN
        // A gun shot lands up to its whole range away, and the portal it
        // flew through can be anywhere along that line (the reach check
        // below uses the same 256 for the gun).
        {
            const int slot = m_player->getInventory().GetSelectedSlot();
            const Game::ItemStack& held = m_player->getInventory().GetSlot(
                Game::Inventory::HotbarToIndex(slot));
            if (held.itemId == Game::Items::PortalGun) portalSearchRadius = 256.0;
        }
#endif
        Game::World* world = InteractionWorld(packet.dimensionId,
                                              glm::ivec3(packet.blockX, packet.blockY, packet.blockZ),
                                              interactionEye, interactionScope, portalSearchRadius);
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
        const glm::vec3 eyePos = interactionEye;   // the player's eye, or its image through the portal
        float distance = glm::length(hitPoint - eyePos);
        float maxReach = (m_player->getGameMode() == GameMode::CREATIVE ? 5.0f : 4.5f) * m_player->getScale();

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

        // ── End crystal (MC EndCrystalItem.useOn) ─────────────────────────
        // Lives here rather than in ItemBehaviors because placing one spawns
        // an ENTITY into this session's level and pokes the dragon fight —
        // neither of which common item code can reach (the same reason spawn
        // eggs route server-side).
        if (!heldStack.IsEmpty() && heldStack.itemId == Game::Items::EndCrystal) {
            const bool placed = server->PlaceEndCrystalFromUse(*this, clicked);
            if (placed && !isCreative) {
                heldStack.count -= 1;
                if (heldStack.count <= 0) heldStack.Clear();
            }
            AckInteraction(packet.sequence, placed);
            m_lastInteractionSequence = packet.sequence;
            return;
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
        const Game::BlockState clickedBlockState =
            world->GetBlockState(clicked.x, clicked.y, clicked.z);

        // MC BlockPlaceContext's click data, which the slab rule reads to
        // decide whether two halves merge into a full block.
        Game::PlacementClick click;
        click.clickedFace = context.getClickedFace();
        click.hitY        = packet.cursorY;
        click.replacingClickedOnBlock = true;

        const bool replaceClicked = Game::CanBeReplacedByPlacement(
            clickedBlockState, blockToPlace, sneaking, click);

        glm::ivec3 targetPos = replaceClicked ? clicked : context.getPlacementPos();

        // Skull items are MC StandingAndWallBlockItems: clicking a horizontal
        // face hangs the WALL variant against the clicked block, anything
        // else stands the floor variant. Only when the placement actually
        // resolved against the clicked face — a replaceable clicked block
        // keeps the cell and there is no wall to hang from. The client
        // mirrors this in ComputePredictedPlacement, so the choice never
        // flips when this authoritative update lands.
        if (!replaceClicked) {
            blockToPlace = Game::SkullPlacementBlock(blockToPlace,
                                                     context.getClickedFace());
        }

        // Validate target position
        if (!world->IsValidPosition(targetPos.x, targetPos.y, targetPos.z)) {
            Log::Warning("HandleUseItemOn: Target position invalid (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            failPlacement(targetPos);
            return;
        }

        const Game::BlockID targetBlockId = world->GetBlock(targetPos.x, targetPos.y, targetPos.z);
        const Game::BlockState targetBlockState =
            world->GetBlockState(targetPos.x, targetPos.y, targetPos.z);

        // MC BlockPlaceContext.canPlace. Without it we'd silently overwrite the
        // block already sitting in the resolved cell — clicking a wall whose
        // +X neighbour holds a slab would replace that slab, consuming an
        // inventory item while appearing to do nothing.
        Game::PlacementClick resolvedClick = click;
        resolvedClick.replacingClickedOnBlock = false;
        if (!replaceClicked &&
            !Game::CanBeReplacedByPlacement(targetBlockState, blockToPlace,
                                            sneaking, resolvedClick)) {
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
        Game::BlockState grownState;
        if (Game::IsSegmentedBlock(targetBlockId) && targetBlockId == blockToPlace) {
            const Game::BlockState grown = Game::SegmentGrownState(targetBlockState);
            if (grown != targetBlockState) {
                grownState  = grown;
                growInPlace = true;
            }
        }
        
        // === 6c. Slab orientation (top vs bottom half) ===
        // Mirrors MC's SlabBlock.getStateForPlacement (SlabBlock.java:66-90):
        // the slab is placed in the TOP half when the player clicked on the
        // bottom face of a block (face == DOWN), or when they clicked the side
        // of a block above its vertical midpoint (cursor.y > 0.5). The BOTTOM
        // half is the default — picked when clicking a TOP face or the lower
        // portion of a side.
        //
        // The half is the `type` blockstate, exactly as in vanilla, so this
        // records the decision and applies it to the state in step 9 rather
        // than swapping BlockID.
        std::optional<Game::BlockRegistry::SlabType> slabHalf;
        if (Game::BlockRegistry::IsSlabBlock(blockToPlace)) {
            using SlabType = Game::BlockRegistry::SlabType;
            // MC SlabBlock.getStateForPlacement opens with the merge:
            //
            //   BlockState replaced = level.getBlockState(context.getClickedPos());
            //   if (replaced.is(this)) return replaced.setValue(TYPE, DOUBLE)
            //                                          .setValue(WATERLOGGED, false);
            //
            // against the RESOLVED cell, so a slab that agreed to be replaced
            // (canBeReplaced above) becomes a full block instead of being
            // overwritten. `targetBlockId == blockToPlace` IS `replaced.is(this)`
            // now that one BlockID covers all three halves.
            if (targetBlockId == blockToPlace &&
                Game::BlockRegistry::SlabTypeOf(targetBlockState) != SlabType::Double) {
                slabHalf = SlabType::Double;
            } else {
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
                slabHalf = placeAsTop ? SlabType::Top : SlabType::Bottom;
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
        // Where the player stands IN THE WORLD BEING EDITED: their own feet,
        // or their image through the portal they are reaching through
        // (the interaction eye is that image's eye).
        const glm::dvec3 playerPosDouble = glm::dvec3(interactionEye) - glm::dvec3(0.0, m_player->getEyeHeight(), 0.0);
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
        const float bodyHalfWidth = 0.3f * m_player->getScale();
        const float bodyHeight    = 1.8f * m_player->getScale();
        if (Game::BlockRegistry::HasCollision(blockToPlace) &&
            std::abs(playerCollisionPos.x - blockCenter.x) < 0.5f + bodyHalfWidth &&
            std::abs(playerCollisionPos.z - blockCenter.z) < 0.5f + bodyHalfWidth &&
            playerCollisionPos.y < targetPos.y + 1.0f &&
            playerCollisionPos.y + bodyHeight > targetPos.y) {
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
        Game::BlockState placedState =
            growInPlace ? grownState
                        : Game::ComputePlacementState(blockToPlace, context);
        // Blocks whose orientation comes from their NEIGHBOURS rather than
        // from the player — redstone dust resolving its four connections.
        // A no-op for everything else.
        if (!growInPlace) {
            placedState = Game::ComputeWorldPlacementState(*world, targetPos, placedState);
            // ComputeWorldPlacementState can change the BLOCK, not just its
            // state: MC ConcretePowderBlock.getStateForPlacement returns the
            // CONCRETE when the cell is in or beside water. Taking the block
            // back off the returned state is what makes that land as concrete
            // instead of powder wearing concrete's state index.
            blockToPlace = placedState.Block();
        }
        // A door is two cells tall (MC DoorBlock.getStateForPlacement):
        // the cell above must be free too, and the hinge comes from the
        // walls beside it and the side of the cell that was clicked.
        const bool placingDoor = Game::IsDoorBlock(blockToPlace);
        if (placingDoor) {
            const glm::ivec3 above = targetPos + glm::ivec3(0, 1, 0);
            Game::PlacementClick aboveClick;
            aboveClick.replacingClickedOnBlock = false;
            if (!world->IsValidPosition(above.x, above.y, above.z) ||
                !Game::CanBeReplacedByPlacement(world->GetBlockState(above.x, above.y, above.z),
                                                blockToPlace, sneaking, aboveClick)) {
                failPlacement(targetPos);
                return;
            }
            placedState = Game::DoorPlacementState(*world, targetPos, placedState, hitPoint);
        }
        // Applied last so it composes with, rather than overwrites, the
        // waterlogged bit ComputePlacementState sets when placing into a fluid.
        // MC clears WATERLOGGED when merging to a double; SetIndex on TYPE
        // alone would keep it, so the double case clears it explicitly.
        if (slabHalf) {
            using SlabType = Game::BlockRegistry::SlabType;
            placedState = Game::BlockRegistry::SlabStateWithType(placedState, *slabHalf);
            if (*slabHalf == SlabType::Double) {
                placedState = Game::BlockRegistry::WithWaterlogged(placedState, false);
            }
        }

        // Second survival gate, now that the state is known. The check in step
        // 7 above is state-free, and a button's support depends entirely on its
        // `face`/`facing` — without this you could hang one on any surface,
        // including nothing at all.
        if (!Game::CanSurviveAt(*world, targetPos, placedState)) {
            Log::Debug("HandleUseItemOn: Block state cannot survive at (%d,%d,%d)",
                       targetPos.x, targetPos.y, targetPos.z);
            failPlacement(targetPos);
            return;
        }

        bool changed = world->SetBlock(
            targetPos.x, targetPos.y, targetPos.z,
            blockToPlace,
            Game::World::UpdateFlags::All,
            placedState.Index()
        );
        
        if (!changed) {
            Log::Warning("HandleUseItemOn: SetBlock failed at (%d,%d,%d)", targetPos.x, targetPos.y, targetPos.z);
            ResyncAndAck(clicked, targetPos, packet.sequence);
            return;
        }
        if (placingDoor) {
            // MC DoorBlock.setPlacedBy: the upper half goes in above.
            const glm::ivec3 above = targetPos + glm::ivec3(0, 1, 0);
            world->SetBlock(above.x, above.y, above.z, blockToPlace,
                            Game::World::UpdateFlags::All, Game::DoorUpperState(placedState).Index());
        }

        // A chest that placed itself as LEFT/RIGHT chose a partner; that
        // partner is still typed SINGLE and has to be told (MC does this
        // through updateShape on the neighbour). Skipping it leaves the older
        // chest rendering and opening alone while the new one claims a pair.
        if (blockToPlace == Game::BlockID::Chest ||
            blockToPlace == Game::BlockID::TrappedChest) {
            if (const auto partner = ChestPartnerCell(placedState, targetPos)) {
                const bool selfIsLeft = placedState.GetValueByName("type") == "left";
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

        // MC WitherSkullBlock.setPlacedBy / WitherWallSkullBlock.setPlacedBy →
        // WitherSkullBlock.checkSpawn: placing a wither skeleton skull (floor
        // OR wall) is the ONE trigger for the soul-sand ritual. Verified
        // against the vanilla sources: SoulSandBlock/SoulSoilBlock have no
        // setPlacedBy at all, so completing the T by placing the sand last
        // does NOT summon — only the skull placement does, and that is
        // reproduced here by hooking only this path.
        if (blockToPlace == Game::BlockID::WitherSkeletonSkull ||
            blockToPlace == Game::BlockID::WitherSkeletonWallSkull) {
            server->CheckWitherSpawn(targetPos);
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
    
    void PlayerSession::HandleFillBlocks(const Network::FillBlocksC2SPacket& packet) {
        ASSERT_SERVER_THREAD();
        if (!m_player || !m_connection) return;
        if (!HasClientLoaded()) return;
        IntegratedServer* server = g_integratedServer.get();
        if (!server) return;

        const GameMode mode = m_player->getGameMode();
        if (mode == GameMode::SPECTATOR || mode == GameMode::ADVENTURE) {
            m_connection->SendChatMessage("You cannot build here", 1);
            return;
        }

        // The box, and its size before anything is touched.
        const glm::ivec3 a(packet.x0, packet.y0, packet.z0), b(packet.x1, packet.y1, packet.z1);
        const glm::ivec3 lo = glm::min(a, b), hi = glm::max(a, b);
        const glm::ivec3 size = hi - lo + glm::ivec3(1);
        constexpr int64_t kMaxFillBlocks = 32768;   // a 32-block cube, a 181×181 floor
        const int64_t volume = static_cast<int64_t>(size.x) * size.y * size.z;
        if (volume > kMaxFillBlocks) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Fill too large: %lld blocks (the most is %lld)",
                          static_cast<long long>(volume), static_cast<long long>(kMaxFillBlocks));
            m_connection->SendChatMessage(buf, 1);
            return;
        }
        // Both corners were placement targets in reach; the box between
        // them is the player's to fill, but not from across the map.
        constexpr double kMaxCornerDistance = 128.0;
        const glm::dvec3 here = m_player->getPosition();
        if (glm::length(glm::dvec3(lo) - here) > kMaxCornerDistance ||
            glm::length(glm::dvec3(hi) - here) > kMaxCornerDistance) {
            m_connection->SendChatMessage("That box is too far away", 1);
            return;
        }

        glm::vec3 interactionEye;
        InteractionScope interactionScope;
        Game::World* world = InteractionWorld(packet.dimensionId, (lo + hi) / 2,
                                              interactionEye, interactionScope, 0.0);
        if (!world) return;

        const Game::BlockID block = m_player->getHeldBlock();
        if (block == Game::BlockID::Air) {
            m_connection->SendChatMessage("Hold a block to fill with", 1);
            return;
        }
        // The state the client previewed, if it is a state of the held
        // block; the block's default otherwise.
        Game::BlockState state = Game::BlockState::FromRawId(packet.rawState);
        if (state.Block() != block) state = Game::BlockStates::Default(block);

        auto& inv = m_player->getInventory();
        auto& slot = inv.MutableSlot(Game::Inventory::HOTBAR_BEGIN + inv.GetSelectedSlot());
        const bool creative = mode == GameMode::CREATIVE;
        const int budget = creative ? std::numeric_limits<int>::max() : slot.count;

        // The player's own body: nothing is placed through it.
        const double bodyHalfWidth = 0.3 * m_player->getScale();
        const double bodyHeight    = 1.8 * m_player->getScale();
        const bool solid = Game::BlockRegistry::HasCollision(block);

        Game::PlacementClick click;
        click.replacingClickedOnBlock = false;
        int placed = 0, skipped = 0;
        bool outOfItems = false;
        for (int z = lo.z; z <= hi.z && !outOfItems; ++z) {
            for (int y = lo.y; y <= hi.y && !outOfItems; ++y) {
                for (int x = lo.x; x <= hi.x; ++x) {
                    if (placed >= budget) { outOfItems = true; break; }
                    if (!world->IsValidPosition(x, y, z) || !world->IsPositionLoaded(x, y, z)) { ++skipped; continue; }
                    const Game::BlockState existing = world->GetBlockState(x, y, z);
                    if (existing == state) continue;   // already there
                    if (!Game::CanBeReplacedByPlacement(existing, block, false, click)) { ++skipped; continue; }
                    if (solid &&
                        std::abs(x + 0.5 - here.x) < 0.5 + bodyHalfWidth &&
                        std::abs(z + 0.5 - here.z) < 0.5 + bodyHalfWidth &&
                        here.y < y + 1.0 && here.y + bodyHeight > y) { ++skipped; continue; }
                    const glm::ivec3 pos(x, y, z);
                    if (!Game::CanSurviveAt(*world, pos, state)) { ++skipped; continue; }
                    if (world->SetBlock(x, y, z, block, Game::World::UpdateFlags::All, state.Index())) ++placed;
                    else ++skipped;
                }
            }
        }
        if (!creative && placed > 0) {
            slot.count -= placed;
            if (slot.count <= 0) slot.Clear();
            // The client did not predict the fill's cost; the slot goes back
            // to it as it now is.
            SendInventoryFull();
        }

        char buf[128];
        std::snprintf(buf, sizeof(buf), "Placed %d block%s%s%s", placed, placed == 1 ? "" : "s",
                      skipped > 0 ? " (some cells were not free)" : "",
                      outOfItems ? " - out of items" : "");
        m_connection->SendChatMessage(buf, 1);
        Log::Info("[Fill] %s filled (%d,%d,%d)-(%d,%d,%d) with %d: %d placed, %d skipped",
                  m_player->getName().c_str(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z,
                  static_cast<int>(block), placed, skipped);
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
        Game::World* world = SessionWorld();

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
            {
                Game::World* world = m_interactionWorld ? m_interactionWorld : SessionWorld();

                // Send clicked block, WITH its state index. Dropping the state
                // here is what made a full leaf litter clump spin north every
                // time a right-click on it was rejected: the resync told the
                // client "leaf_litter, default state", overwriting the facing
                // the client had rendered correctly all along. Same applied to
                // every furnace, chest and log that ever got resynced.
                SendBlockUpdate(clicked,
                                world->GetBlock(clicked.x, clicked.y, clicked.z),
                                world->GetBlockState(clicked.x, clicked.y, clicked.z).Index());

                // Send target block if different
                if (clicked != target) {
                    SendBlockUpdate(target,
                                    world->GetBlock(target.x, target.y, target.z),
                                    world->GetBlockState(target.x, target.y, target.z).Index());
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

    // UNREACHABLE. The keep-alive response is handled on the network I/O
    // thread now (ServerConnection::HandleKeepAliveResponse), because MC's
    // handleKeepAlive carries no ensureRunningOnSameThread and answering it on
    // the tick thread is what let a long tick time the player out.
    //
    // Do NOT call this from there: PlayerSession is tick-thread state. Latency
    // is refreshed from the connection in Tick() instead.
    void PlayerSession::HandleKeepAlive(const Network::KeepAliveC2SPacket& packet) {
        (void)packet;
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
                                        Game::BlockStateIndex stateIndex) {
        if (!m_connection) return;

        Network::BlockChangeS2CPacket packet(pos.x, pos.y, pos.z, block, stateIndex);
        SendSingleBlockChange(packet);
    }
    
    void PlayerSession::SendSingleBlockChange(const Network::BlockChangeS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server).
        // The dimension is this session's: the packet is positional and the
        // server scopes the send to that world's watchers.
        if (g_integratedServer) {
            g_integratedServer->SendBlockChangeS2CPacket(
                m_interactionWorld ? m_interactionDimension
                                   : Game::DimensionFromRaw(GetDimensionId()), packet);
        }
        // TODO: Add network connection support for multiplayer
    }

    void PlayerSession::SendSectionBlocksUpdate(const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
        // Send via integrated server (no connection check needed for integrated server)
        if (g_integratedServer) {
            g_integratedServer->SendSectionBlocksUpdateS2CPacket(
                Game::DimensionFromRaw(GetDimensionId()), packet);
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
        // Mark as sent (legacy path: the player's own dimension)
        Dim(Game::DimensionFromRaw(GetDimensionId())).sent.insert(chunk);

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
        DimensionSendState& st = Dim(Game::DimensionFromRaw(GetDimensionId()));
        st.sent.erase(chunk);
        st.pending.erase(chunk);
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

    Game::World* PlayerSession::SessionWorld() const {
        IntegratedServer* server = g_integratedServer.get();
        if (!server) return nullptr;
        ServerLevel* level = server->GetLevel(Game::DimensionFromRaw(GetDimensionId()));
        return level ? level->World() : nullptr;
    }

    Game::World* PlayerSession::InteractionWorld(int8_t packetDimension, const glm::ivec3& target,
                                                 glm::vec3& outEye, InteractionScope& scope,
                                                 double portalSearchRadius) {
        m_interactionWorld = nullptr;
        scope.session = this;
        if (!m_player) return nullptr;
        const glm::dvec3 feet = m_player->getPosition();
        outEye = glm::vec3(feet.x, feet.y + m_player->getEyeHeight(), feet.z);

        constexpr int8_t kUnknown = 127;
        const int8_t own = GetDimensionId();
        const bool sameDimension = (packetDimension == kUnknown || packetDimension == own);

#if ENABLE_IMMERSIVE_PORTALS
        IntegratedServer* server = g_integratedServer.get();
        if (!server || !server->ImmersivePortals()) return sameDimension ? SessionWorld() : nullptr;
        const Game::DimensionId here  = Game::DimensionFromRaw(own);
        const Game::DimensionId there = sameDimension ? here : Game::DimensionFromRaw(packetDimension);
        ServerLevel* level = server->GetLevel(there);
        if (!level || !level->World()) return nullptr;

        // The eye the reach is measured from: the player's own, or its
        // image through the nearby portal that puts it closest to the
        // target — whichever is nearer. A portal into the player's OWN
        // dimension (a gun pair in one world) is a candidate too: the
        // packet cannot tell that case apart, but the distances can.
        // Reach through a portal is bounded by the player's reach on both
        // legs, so a portal farther than that is not a candidate.
        const glm::dvec3 eye(outEye);
        const double reach = portalSearchRadius > 0.0
            ? portalSearchRadius
            : static_cast<double>(m_player->getReachDistance() * m_player->getScale()) + 1.0;
        const glm::dvec3 targetCentre = glm::dvec3(target) + glm::dvec3(0.5);
        const Game::Immersive::Portal* best = nullptr;
        double bestDist = sameDimension ? glm::length(eye - targetCentre) : 1e30;
        for (const Game::Immersive::Portal* p : server->ImmersivePortals()->CollectNear(here, eye, reach)) {
            if (!p->Has(Game::Immersive::PortalFlag::Interactable)) continue;
            if (p->IsMirror() || p->destDimension != there) continue;
            const double d = glm::length(p->TransformPoint(eye) - targetCentre);
            if (d < bestDist) { bestDist = d; best = p; }
        }
        if (!best) {
            if (sameDimension) return SessionWorld();   // the own eye is the nearest
            return nullptr;                             // no surface into that level
        }

        outEye = glm::vec3(best->TransformPoint(eye));
        if (!sameDimension) {
            m_interactionWorld     = level->World();
            m_interactionDimension = there;
        }
        return level->World();
#else
        (void)target;
        return nullptr;
#endif
    }
    
    void PlayerSession::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    // === INTERNAL METHODS ===

    void PlayerSession::CoalesceBlockChange(Game::Math::ChunkPos chunk, int section,
                                           uint8_t localX, uint8_t localY, uint8_t localZ,
                                           Game::BlockID blockId, Game::BlockStateIndex stateIndex) {
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
        m_loaders.clear();
        m_watched.clear();
        for (auto& st : m_dimState) { st.sent.clear(); st.clientStamps.clear(); }
    }

    void PlayerSession::ClearQueues() {
        for (auto& st : m_dimState) st.pending.clear();

        // Clear diff queue
        while (!m_diffQueue.empty()) {
            m_diffQueue.pop();
        }
    }

    void PlayerSession::ClearDiffs() {
        m_pendingDiffs.clear();
    }

} // namespace Server
