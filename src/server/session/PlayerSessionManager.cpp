// File: src/server/session/PlayerSessionManager.cpp
#include "PlayerSessionManager.hpp"
#include "server/items/HushItems.hpp"
#include "server/IntegratedServer.hpp"
#include "server/level/ServerLevel.hpp"   // TicketsForDimension / CleanupSession
#include "server/entity/ServerLevelBridge.hpp"
#include <algorithm>
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"
#include "platform/GameDirectory.hpp"
#include "server/level/PlayerSpawnFinder.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/World.hpp"
#include "common/world/block/BedBlock.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace Server {

    PlayerSessionManager::PlayerSessionManager() {
    }

    PlayerSessionManager::~PlayerSessionManager() {
        Shutdown();
    }

    // === INITIALIZATION ===

    void PlayerSessionManager::Initialize(
        const Config& config,
        ChunkTicketManager* ticketMgr,
        ChunkStatusManager* statusMgr,
        SendScheduler* scheduler
    ) {
        m_config = config;
        m_sendScheduler = scheduler;

        // The ticket and status managers now belong to a ServerLevel, and a
        // ServerLevel cannot be built until this manager exists (its mob
        // bridge needs it). They arrive later, via SetLevelServices, which is
        // also what adds the spawn tickets that depend on them.
        //
        // The two parameters are kept so every existing caller compiles, and
        // are honoured when a caller does pass them — the server passes null.
        if (ticketMgr || statusMgr) {
            SetLevelServices(ticketMgr, statusMgr);
        }

        m_initialized = true;
        
        Log::Info("PlayerSessionManager: Initialized with spawn at (%.1f, %.1f, %.1f)",
                 m_config.worldSpawn.x, m_config.worldSpawn.y, m_config.worldSpawn.z);
    }

    void PlayerSessionManager::Shutdown() {
        if (!m_initialized) {
            return;
        }
        
        Log::Info("PlayerSessionManager: Shutting down with %zu active sessions", m_sessions.size());
        
        // Remove all sessions
        {
            std::lock_guard<std::mutex> lock(m_sessionMutex);
            
            for (auto& [playerId, session] : m_sessions) {
                // Clean up tickets and watchers
                CleanupSession(playerId);
                
                // Cleanup session
                session->Cleanup();
            }
            
            m_sessions.clear();
            m_connectionToPlayer.clear();
            m_playerNames.clear();
        }
        
        m_initialized = false;
    }

    // === SESSION MANAGEMENT ===

    std::shared_ptr<PlayerSession> PlayerSessionManager::CreateSession(
        uint32_t playerId,
        uint32_t connectionId,
        const std::string& playerName
    ) {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        
        // Check if session already exists
        if (m_sessions.count(playerId) > 0) {
            Log::Warning("PlayerSessionManager: Session already exists for player %u", playerId);
            return m_sessions[playerId];
        }
        
        // Create new session
        auto session = std::make_shared<PlayerSession>(playerId, connectionId);
        session->SetSendScheduler(m_sendScheduler);

        // Store session
        m_sessions[playerId] = session;
        m_connectionToPlayer[connectionId] = playerId;
        m_playerNames[playerId] = playerName;
        
        // Update stats
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSessions++;
            m_stats.activeSessions++;
        }
        
        Log::Info("PlayerSessionManager: Created session for player %u (%s)", playerId, playerName.c_str());
        
        return session;
    }

    std::shared_ptr<PlayerSession> PlayerSessionManager::GetSession(uint32_t playerId) const {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        
        auto it = m_sessions.find(playerId);
        if (it != m_sessions.end()) {
            return it->second;
        }
        
        return nullptr;
    }

    std::shared_ptr<PlayerSession> PlayerSessionManager::GetSessionByConnection(uint32_t connectionId) const {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        
        auto connIt = m_connectionToPlayer.find(connectionId);
        if (connIt == m_connectionToPlayer.end()) {
            return nullptr;
        }
        
        auto sessionIt = m_sessions.find(connIt->second);
        if (sessionIt != m_sessions.end()) {
            return sessionIt->second;
        }
        
        return nullptr;
    }

    void PlayerSessionManager::RemoveSession(uint32_t playerId) {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        
        auto it = m_sessions.find(playerId);
        if (it == m_sessions.end()) {
            return;
        }
        
        // Clean up resources
        CleanupSession(playerId);
        // The Hush's per-player item state (cooldowns, the compass cache and
        // what the client was last told): a returning player starts clean,
        // so their fresh client is sent the compass target again.
        HushItems::ForgetPlayer(playerId);
        
        // Get connection ID for cleanup
        uint32_t connectionId = it->second->GetConnectionId();
        
        // Cleanup session
        it->second->Cleanup();
        
        // Remove from maps
        m_sessions.erase(it);
        m_connectionToPlayer.erase(connectionId);
        m_playerNames.erase(playerId);
        
        // Update stats
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.activeSessions--;
        }
        
        Log::Info("PlayerSessionManager: Removed session for player %u", playerId);
    }

    std::vector<std::shared_ptr<PlayerSession>> PlayerSessionManager::GetAllSessions() const {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        
        std::vector<std::shared_ptr<PlayerSession>> result;
        result.reserve(m_sessions.size());
        
        for (const auto& [playerId, session] : m_sessions) {
            result.push_back(session);
        }
        
        return result;
    }

    size_t PlayerSessionManager::GetSessionCount() const {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        return m_sessions.size();
    }

    // === PLAYER LIFECYCLE ===

    void PlayerSessionManager::OnPlayerJoin(
        uint32_t playerId,
        uint32_t connectionId,
        const std::string& playerName
    ) {
        Log::Info("PlayerSessionManager: Player %u (%s) joining", playerId, playerName.c_str());
        
        // Create or get session
        auto session = CreateSession(playerId, connectionId, playerName);
        if (!session) {
            Log::Error("PlayerSessionManager: Failed to create session for player %u", playerId);
            return;
        }
        
        // Get spawn position
        glm::vec3 spawnPos = GetWorldSpawn();
        
        // Initialize at minimum view distance (2), like Minecraft's ServerPlayer default.
        // The client sends its actual render distance via ClientConfigC2S right after login,
        // which calls OnClientSettingsReceived → SetViewDistance to expand to the real value.
        PlayerSession::Config sessionConfig;
        // The server default only until ClientConfigC2S lands (see
        // IntegratedServer::ApplyClientViewDistance). This was
        // maxViewDistance (32) for a long time — not by design but because
        // SetViewDistance capped at the simulation distance, so the only way
        // to let a client see 32 chunks was to tick 32 chunks for it. That
        // cap is gone; the two distances are independent, as in MC.
        sessionConfig.simulationDistance = m_config.defaultSimulationDistance;
        sessionConfig.viewDistance = 2;
        sessionConfig.maxChunksPerTick = m_config.maxChunksPerPlayerPerTick;
        sessionConfig.maxBytesPerTick = m_config.maxBytesPerPlayerPerTick;
        sessionConfig.maxDiffBytesPerTick = m_config.maxDiffBytesPerPlayerPerTick;
        
        session->Initialize(sessionConfig, 0, spawnPos);  // Dimension 0 = overworld
        
        // Calculate spawn chunk
        Game::Math::ChunkPos spawnChunk(
            static_cast<int>(std::floor(spawnPos.x / 16.0f)),
            static_cast<int>(std::floor(spawnPos.z / 16.0f))
        );
        
        // MC PrepareSpawnTask/Preparing.tick: a short, self-expiring
        // PLAYER_SPAWN ticket at the position the player is about to occupy,
        // so their terrain is already on its way before the entity exists.
        //
        // Deliberately NOT the simulation ticket. That one is placed by
        // ProcessSessionTick from the player's LIVE position once they are
        // attached — which is the ordering fix. Placing real PLAYER tickets
        // here, around a spawn position the player may be nowhere near after
        // their save is restored, is exactly what froze the world before.
        if (ChunkTicketManager* tickets = TicketsForDimension(session->GetDimensionId())) {
            tickets->AddPlayerSpawnTicket(spawnChunk, /*radius=*/3,
                                          /*lifespanTicks=*/20);
        }

        // Trigger join callback        // Trigger join callback
        if (m_joinCallback) {
            m_joinCallback(playerId);
        }
    }

    void PlayerSessionManager::OnPlayerLeave(uint32_t playerId, const std::string& reason) {
        Log::Info("PlayerSessionManager: Player %u leaving: %s", playerId, reason.c_str());
        
        // Trigger leave callback
        if (m_leaveCallback) {
            m_leaveCallback(playerId, reason);
        }
        
        // Remove session
        RemoveSession(playerId);
    }

    void PlayerSessionManager::OnPlayerDeath(uint32_t playerId) {
        auto session = GetSession(playerId);
        if (!session) {
            return;
        }
        
        Log::Info("PlayerSessionManager: Player %u died", playerId);
        
        // Player will respawn, prepare for respawn
        // Session state will be updated on respawn
    }

    void PlayerSessionManager::OnPlayerRespawn(uint32_t playerId) {
        auto session = GetSession(playerId);
        if (!session) {
            return;
        }
        
        ServerPlayer* player = session->GetPlayer();
        auto* server = Server::g_integratedServer.get();

        // MC PlayerList.respawn → ServerPlayer.findRespawnAndUseSpawnBlock:
        // the bed the player last slept in, when it still stands, is where
        // they get up (AbstractBedBlock.findStandUpPosition); otherwise the
        // point is forgotten, the player is told, and it is the Overworld
        // spawn — whichever dimension they died in.
        std::optional<glm::vec3> spawnPos;
        int spawnDimension = Game::DimensionToRaw(Game::DimensionId::Overworld);
        if (player && player->getRespawnConfig() && server) {
            const ServerPlayer::RespawnConfig config = *player->getRespawnConfig();
            // GetOrCreateLevel: a bed in a Nether this session has not yet
            // built is still a bed.
            ServerLevel* level = server->GetOrCreateLevel(Game::DimensionFromRaw(config.dimensionId));
            Game::World* world = level ? level->World() : nullptr;
            if (world) {
                // MC ServerPlayer.findRespawnAndUseSpawnBlock reads the bed
                // through ServerLevel.getBlockState, which LOADS the chunk
                // (ServerChunkCache.getChunk with load = true). Same here:
                // every chunk the stand-up search can touch — the bed cell
                // and the 3-block ring around it — is brought to FULL,
                // blocking, before a single block is read. Usually one
                // chunk, at most four, and cache hits unless the death was
                // far away.
                const auto lo = Game::Math::WorldCoordinates::WorldToChunkPos(config.pos.x - 3, config.pos.z - 3);
                const auto hi = Game::Math::WorldCoordinates::WorldToChunkPos(config.pos.x + 3, config.pos.z + 3);
                bool loaded = true;
                for (int cx = lo.x; cx <= hi.x; ++cx) {
                    for (int cz = lo.z; cz <= hi.z; ++cz) {
                        if (!level->GetChunkBlocking(Game::Math::ChunkPos(cx, cz))) loaded = false;
                    }
                }
                if (loaded) {
                    const Game::BlockState state =
                        world->GetBlockState(config.pos.x, config.pos.y, config.pos.z);
                    if (Game::IsBedBlock(state.Block())) {
                        if (auto p = Game::FindBedStandUpPosition(*world, config.pos,
                                                                  Game::BedFacing(state), config.yaw)) {
                            spawnPos = glm::vec3(*p);
                            spawnDimension = config.dimensionId;
                        }
                    }
                }
                // A load that failed (shutdown) leaves spawnPos empty: the
                // world spawn below, the same answer as a missing bed.
            }
            if (!spawnPos) {
                // ClientboundGameEventPacket NO_RESPAWN_BLOCK_AVAILABLE →
                // block.minecraft.spawn.not_valid, and setRespawnPosition(null).
                player->setRespawnConfig(std::nullopt);
                session->SendSystemMessage(
                    "You have no home bed or charged respawn anchor, or it was obstructed");
            }
        }

        if (!spawnPos) {
            glm::vec3 worldSpawn = GetWorldSpawn();
            // MC PlayerList.respawn with no respawn point: PlayerSpawnFinder
            // .findSpawn scatters the arrival within respawn_radius of the
            // world spawn (validated standable columns; the suggestion itself
            // as the fallback). The finder reads Overworld terrain, which is
            // where a spawn-point-less respawn always lands.
            Game::World* overworld = server ? server->Overworld().World() : nullptr;
            if (overworld) {
                const glm::ivec3 suggestion(static_cast<int>(std::floor(worldSpawn.x)),
                                            static_cast<int>(std::floor(worldSpawn.y)),
                                            static_cast<int>(std::floor(worldSpawn.z)));
                worldSpawn = PlayerSpawnFinder::FindSpawn(*overworld, suggestion,
                                                          Game::Rules::GetInt(Game::Rules::Id::RespawnRadius));
            }
            spawnPos = worldSpawn;
        }

        // A death in another dimension comes back through a dimension change
        // (MC respawn always builds the player in the respawn level).
        if (player && player->getDimensionId() != spawnDimension) {
            session->ChangeDimension(spawnDimension, *spawnPos);
        }
        session->Respawn(*spawnPos);
        
        Log::Info("PlayerSessionManager: Player %u respawned at (%.1f, %.1f, %.1f)",
                 playerId, spawnPos->x, spawnPos->y, spawnPos->z);
    }

    // === TICK PROCESSING ===

    void PlayerSessionManager::ResetSharedVitals() {
        m_vitalsSnapshot.clear();
        m_sharedPool = Vitals{};
    }

    void PlayerSessionManager::ShareVitals(const std::vector<std::shared_ptr<PlayerSession>>& sessions) {
        struct Participant { ServerPlayer* player; uint32_t id; };
        std::vector<Participant> participants;
        float dHealth = 0.0f, dSaturation = 0.0f, dExhaustion = 0.0f;
        int   dFood = 0;
        bool  anyPrevious = false;
        bool  someoneDied = false;
        DamageSource deathSource = DamageSource::GENERIC;
        std::string  deathAttacker;

        for (const auto& session : sessions) {
            ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (!player) continue;
            const uint32_t id = player->getPlayerId();
            const bool participates = !player->isDead() &&
                                      player->getGameMode() != GameMode::CREATIVE &&
                                      player->getGameMode() != GameMode::SPECTATOR;
            if (const auto it = m_vitalsSnapshot.find(id); it != m_vitalsSnapshot.end() && it->second.alive) {
                anyPrevious = true;
                const Vitals& was = it->second;
                if (participates) {
                    dHealth     += player->getHealth() - was.health;
                    dFood       += player->getFoodData().getFoodLevel() - was.food;
                    dSaturation += player->getFoodData().getSaturationLevel() - was.saturation;
                    dExhaustion += player->getFoodData().getExhaustionLevel() - was.exhaustion;
                } else if (player->isDead()) {
                    // Died since the last pass: their whole health left the
                    // pool, and their killer is everyone's.
                    dHealth      -= was.health;
                    someoneDied   = true;
                    deathSource   = player->getLastDamageSource();
                    deathAttacker = player->getLastAttackerName();
                }
            }
            if (participates) participants.push_back({ player, id });
        }

        if (participants.empty()) {
            // Nobody to share between (all dead, or all creative): the next
            // living player seeds a fresh pool.
            m_vitalsSnapshot.clear();
            m_sharedPool = Vitals{};
            return;
        }

        if (!m_sharedPool.alive || !anyPrevious) {
            const ServerPlayer& seed = *participants.front().player;
            m_sharedPool.health     = seed.getHealth();
            m_sharedPool.food       = seed.getFoodData().getFoodLevel();
            m_sharedPool.saturation = seed.getFoodData().getSaturationLevel();
            m_sharedPool.exhaustion = seed.getFoodData().getExhaustionLevel();
            m_sharedPool.alive      = true;
        } else {
            m_sharedPool.health     = std::clamp(m_sharedPool.health + dHealth, 0.0f, 20.0f);
            m_sharedPool.food       = std::clamp(m_sharedPool.food + dFood, 0, 20);
            m_sharedPool.saturation = std::clamp(m_sharedPool.saturation + dSaturation, 0.0f,
                                                 static_cast<float>(m_sharedPool.food));
            m_sharedPool.exhaustion = std::max(m_sharedPool.exhaustion + dExhaustion, 0.0f);
            if (someoneDied) m_sharedPool.health = 0.0f;
        }

        for (const Participant& p : participants) {
            p.player->applySharedVitals(m_sharedPool.health, m_sharedPool.food, m_sharedPool.saturation,
                                        m_sharedPool.exhaustion, deathSource, deathAttacker);
        }

        m_vitalsSnapshot.clear();
        for (const auto& session : sessions) {
            ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (!player) continue;
            const bool participates = !player->isDead() &&
                                      player->getGameMode() != GameMode::CREATIVE &&
                                      player->getGameMode() != GameMode::SPECTATOR;
            m_vitalsSnapshot[player->getPlayerId()] = Vitals{
                player->getHealth(), player->getFoodData().getFoodLevel(),
                player->getFoodData().getSaturationLevel(), player->getFoodData().getExhaustionLevel(),
                participates };
        }
        // Everyone died together: the pool is spent until someone respawns.
        if (m_sharedPool.health <= 0.0f) m_sharedPool = Vitals{};
    }

    void PlayerSessionManager::Tick(int64_t serverTick) {
        m_currentTick = serverTick;
        
        std::vector<std::shared_ptr<PlayerSession>> sessionsToProcess;
        
        {
            std::lock_guard<std::mutex> lock(m_sessionMutex);
            sessionsToProcess.reserve(m_sessions.size());
            
            for (const auto& [playerId, session] : m_sessions) {
                sessionsToProcess.push_back(session);
            }
        }
        
        // Process sessions (limited by max per tick)
        // /gamerule shared_vitals — before the sessions tick, so the pool
        // every session then sends (SetHealthS2C) is this tick's truth.
        if (Server::g_integratedServer && Server::g_integratedServer->SharedVitalsEnabled()) {
            ShareVitals(sessionsToProcess);
        } else if (!m_vitalsSnapshot.empty() || m_sharedPool.alive) {
            ResetSharedVitals();
        }

        size_t processed = 0;
        for (const auto& session : sessionsToProcess) {
            if (processed >= static_cast<size_t>(m_config.maxPlayersPerTick)) {
                break;
            }
            
            ProcessSessionTick(session);
            processed++;
        }
        
        // NOTE: Chunk sending is now driven by IntegratedServer calling session->SendNextChunks()
        // per tick, matching Minecraft's tickChildren "send chunks" phase.

        // Process block diffs
        ProcessBlockDiffs();
        
        // Process keep-alive and timeouts
        ProcessKeepAlive();
    }

    void PlayerSessionManager::ProcessChunkStreaming() {
        // Chunk sending is now driven by IntegratedServer calling session->SendNextChunks()
        // directly per tick, matching Minecraft's tickChildren "send chunks" phase.
        // This method is kept for API compatibility but does nothing.
    }

    void PlayerSessionManager::ProcessBlockDiffs() {
        auto sessions = GetAllSessions();
        
        for (const auto& session : sessions) {
            session->ProcessDiffs(m_sendScheduler);
        }
    }

    void PlayerSessionManager::ProcessKeepAlive() {
        if (!m_config.kickOnTimeout) {
            return;
        }
        
        std::vector<uint32_t> timedOutPlayers;
        
        {
            std::lock_guard<std::mutex> lock(m_sessionMutex);
            
            for (const auto& [playerId, session] : m_sessions) {
                if (IsSessionTimedOut(session)) {
                    timedOutPlayers.push_back(playerId);
                }
            }
        }
        
        // Kick timed out players
        for (uint32_t playerId : timedOutPlayers) {
            OnPlayerLeave(playerId, "Connection timed out");
            
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.playersTimedOut++;
            }
        }
    }

    // === WORLD UPDATES ===

    void PlayerSessionManager::BroadcastBlockChange(
        Game::DimensionId dimension,
        int worldX, int worldY, int worldZ,
        Game::BlockID newBlock
    ) {
        // Calculate chunk position
        Game::Math::ChunkPos chunk(worldX >> 4, worldZ >> 4);

        ForEachSessionWatching(dimension, chunk, [&](PlayerSession& session) {
            session.QueueBlockChange(worldX, worldY, worldZ, newBlock);
        });
    }

    void PlayerSessionManager::BroadcastSectionChanges(
        Game::DimensionId dimension,
        Game::Math::ChunkPos chunk,
        int section,
        const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes
    ) {
        ForEachSessionWatching(dimension, chunk, [&](PlayerSession& session) {
            session.QueueSectionChanges(chunk, section, changes);
        });
    }

    void PlayerSessionManager::BroadcastChunkUpdate(Game::Math::ChunkPos chunk) {
        // This would trigger a full chunk resend to watchers
        // Implementation depends on chunk status manager
    }

    void PlayerSessionManager::BroadcastLightUpdate(
        Game::Math::ChunkPos chunk,
        const std::vector<uint8_t>& sectionMask
    ) {
        // This would send light updates to watchers
        // Implementation depends on light system
    }

    // === PLAYER POSITION BROADCASTING ===

    void PlayerSessionManager::BroadcastPlayerPositions() {
        std::lock_guard<std::mutex> lock(m_sessionMutex);

        // Fewer than 2 players means nothing to broadcast
        if (m_sessions.size() < 2) return;

        // For each player, send their position to every OTHER player
        for (const auto& [srcId, srcSession] : m_sessions) {
            auto* srcPlayer = srcSession->GetPlayer();
            if (!srcPlayer) continue;

            Network::PlayerUpdateS2CPacket packet;
            packet.playerId = srcPlayer->getPlayerId();
            packet.position = srcPlayer->getPosition();
            packet.rotation = srcPlayer->getRotation();
            packet.isCrouching = srcPlayer->IsSneaking();
            // The hurt flash. LivingEntity::Hurt already set this on the
            // player's entity view when the damage landed; it is only read here.
            packet.hurtTime = 0;
            packet.deathTime = 0;
            if (Server::g_integratedServer) {
                if (auto* view = Server::g_integratedServer->GetPlayerEntityView(srcId)) {
                    packet.hurtTime = static_cast<uint8_t>(
                        std::clamp(view->hurtTime, 0, 255));
                    // The corpse's topple clock — see PlayerEntityView::
                    // TickCombatState, which is what counts it.
                    packet.deathTime = static_cast<uint8_t>(
                        std::clamp(view->deathTime, 0, 255));
                    // MC DATA_EFFECT_PARTICLES / the invisible and glowing
                    // shared flags, as every watcher of this player sees them.
                    const Game::EffectVisuals& visuals = view->GetEffectVisuals();
                    packet.effectFlags     = visuals.flags;
                    packet.effectParticles = visuals.particles;
                }
            }
            packet.sequenceNumber = 0;
            // The receiving client files the update under this dimension's
            // level, so a player in the Nether is not drawn at Nether
            // coordinates inside the Overworld.
            packet.dimensionId = static_cast<int8_t>(srcSession->GetDimensionId());
            // The body size, so /scale and a scaled portal show on every
            // other client's copy of this player.
            packet.scale = srcPlayer->getScale();
            // /invisible, or MC Entity.isInvisible via the INVISIBILITY effect
            // (LivingEntity.updateInvisibilityStatus).
            packet.invisible = srcPlayer->isInvisible() ||
                               srcPlayer->hasEffect(Game::MobEffectId::Invisibility);
            packet.morph = srcPlayer->getMorph();   // /morph: drawn as that body
            packet.morphAnim = srcPlayer->getMorphAnim();

            auto data = Network::Serialization::Serialize(packet);

            for (const auto& [dstId, dstSession] : m_sessions) {
                if (dstId == srcId) continue; // don't send to self
                auto* conn = dstSession->GetConnection();
                if (!conn) continue;
                conn->SendPacket(
                    static_cast<uint8_t>(Network::PacketId::PlayerUpdateS2C), data);
            }
        }
    }

    // === SPAWN MANAGEMENT ===

    void PlayerSessionManager::SetWorldSpawn(const glm::vec3& spawnPos) {
        m_config.worldSpawn = spawnPos;

        // Update spawn chunk tickets
        InitializeSpawnChunks();
    }

    glm::vec3 PlayerSessionManager::GetWorldSpawn() const {
        return m_config.worldSpawn;
    }


    // === CONFIGURATION ===

    void PlayerSessionManager::SetConfig(const Config& config) {
        m_config = config;
        
        // Update spawn chunks if spawn changed
        InitializeSpawnChunks();
    }

    const PlayerSessionManager::Config& PlayerSessionManager::GetConfig() const {
        return m_config;
    }

    void PlayerSessionManager::SetMaxViewDistance(int distance) {
        m_config.maxViewDistance = std::clamp(distance, 2, 32);
        
        // Update all sessions
        auto sessions = GetAllSessions();
        for (const auto& session : sessions) {
            int currentView = session->GetViewDistance();
            if (currentView > m_config.maxViewDistance) {
                session->SetViewDistance(m_config.maxViewDistance);
            }
        }
    }

    void PlayerSessionManager::SetDefaultSimulationDistance(int distance) {
        m_config.defaultSimulationDistance = std::clamp(distance, 2, 32);
    }

    // === STATISTICS ===

    PlayerSessionManager::Stats PlayerSessionManager::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        
        Stats stats = m_stats;
        
        // Calculate averages
        auto sessions = GetAllSessions();
        if (!sessions.empty()) {
            float totalLatency = 0;
            float totalChunks = 0;
            
            for (const auto& session : sessions) {
                auto sessionStats = session->GetStats();
                totalLatency += sessionStats.latency;
                totalChunks += sessionStats.chunksInWatch;
            }
            
            stats.averageLatency = totalLatency / sessions.size();
            stats.averageChunksPerPlayer = totalChunks / sessions.size();
        }
        
        return stats;
    }

    void PlayerSessionManager::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = Stats{};
    }

    // === CALLBACKS ===

    void PlayerSessionManager::SetJoinCallback(JoinCallback callback) {
        m_joinCallback = callback;
    }

    void PlayerSessionManager::SetLeaveCallback(LeaveCallback callback) {
        m_leaveCallback = callback;
    }

    // === INTERNAL METHODS ===

    void PlayerSessionManager::UpdatePlayerTickets(uint32_t playerId,
                                                   int rawDimensionId,
                                                   Game::Math::ChunkPos chunk,
                                                   int simulationDistance) {
        // MC DistanceManager.addPlayer: ONE ticket, at the chunk the player
        // occupies. No square, no old-versus-new, nothing centred by a caller.
        //
        // ChunkTicketManager::AddPlayer is idempotent and self-correcting — it
        // moves the ticket if the player is registered somewhere else — so this
        // is safe to call unconditionally, which is what lets ProcessSessionTick
        // drop the anchor comparison entirely.
        if (ChunkTicketManager* tickets = TicketsForDimension(rawDimensionId)) {
            // The ticket manager owns the simulation distance now — it is what
            // turns into a ticket LEVEL (ChunkLevel::PlayerTicketLevel), so it
            // cannot live only on the session. Idempotent: SetSimulationDistance
            // early-returns when unchanged, and re-levels every player ticket in
            // place when it does change, so no chunk drops out mid-move.
            //
            // MC treats simulation distance as server-wide; ours is per-session,
            // so with several players this is last-writer-wins per dimension —
            // which lands on MC's semantics anyway.
            tickets->SetSimulationDistance(simulationDistance);
            tickets->AddPlayer(chunk, playerId);

            // No level query here. Levels move only in RunAllUpdates (MC
            // DistanceManager.runAllUpdates, once a tick from the server),
            // which also asserts that every player stands in their own
            // entity-ticking range. Asking here forced a propagation per
            // chunk border crossed — per player, several times a tick.
        }
    }

    // MC ChunkMap.onChunkReadyToSend: iterate players and ask each one's
    // tracking view. There is no reverse chunk->players index to keep in sync,
    // and with a handful of players this is cheaper than maintaining one.
    void PlayerSessionManager::ForEachSessionWatching(
        Game::DimensionId dimension,
        Game::Math::ChunkPos chunk,
        const std::function<void(PlayerSession&)>& fn) const {
        // Snapshot under the lock, invoke outside it. The callbacks here queue
        // block changes and chunk sends, and some of those paths read back
        // through this manager — holding a non-recursive mutex across them
        // would deadlock. The old GetWatchers()-then-GetSession() shape had the
        // same property by accident; this keeps it on purpose.
        std::vector<std::shared_ptr<PlayerSession>> watching;
        {
            std::lock_guard<std::mutex> lock(m_sessionMutex);
            watching.reserve(m_sessions.size());
            for (const auto& [playerId, session] : m_sessions) {
                if (!session) continue;
                // The dimension test comes FIRST and is not an optimisation:
                // a tracking view is a set of ChunkPos, which is the same set
                // of numbers in every world, so Contains() alone would call a
                // player in the Nether a watcher of the Overworld chunk they
                // happen to share coordinates with.
                // A session may watch chunks in several dimensions at once
                // (its own view plus portal far sides); IsWatching is keyed
                // by (dimension, chunk), so this is exact.
                if (session->IsWatching(dimension, chunk)) {
                    watching.push_back(session);
                }
            }
        }
        for (const auto& session : watching) {
            fn(*session);
        }
    }

    std::vector<uint32_t> PlayerSessionManager::GetChunkWatchers(
        Game::DimensionId dimension, Game::Math::ChunkPos chunk) const {
        std::vector<uint32_t> watchers;
        ForEachSessionWatching(dimension, chunk, [&](PlayerSession& session) {
            // Only sessions the chunk's DATA has already been sent to. A
            // block delta for a chunk the client has not received yet can
            // only sit in its bounded pending-diffs buffer — and a mass
            // detonation overflows that and silently loses blocks. The chunk
            // is serialized and marked sent in the same server-thread block
            // (PlayerSession::SendNextChunks), and deltas flush at tick
            // start, so a change is always either inside the snapshot this
            // session will get or broadcast after it — never dropped.
            if (!session.HasSentChunk(dimension, chunk)) return;
            watchers.push_back(session.GetPlayerId());
        });
        return watchers;
    }

    void PlayerSessionManager::ProcessSessionTick(std::shared_ptr<PlayerSession> session) {
        session->Tick(m_currentTick);

        // Re-register the player's ticket from their LIVE position, every tick,
        // unconditionally. This is the whole fix.
        //
        // MC calls ChunkMap.move on movement and compares lastSectionPos (where
        // the ticket sits) against SectionPos.of(player) (where the player is) —
        // never one cache against another. It gets away with an event-driven
        // call because those two are written in the same statement block, so a
        // missed call leaves the anchor AGREEING with the ticket and the next
        // call repairs it.
        //
        // We had no such guarantee — three notions of player position with
        // three writers — so rather than reproduce MC's convention we remove
        // the question. AddPlayer is O(1) when the chunk has not changed, so
        // asking every tick costs nothing and there is no cached value left
        // that anyone could pre-answer. The bug this replaces froze every
        // entity near the player for a whole session, because the ticket square
        // stayed at world spawn while a cached anchor claimed it had moved.
        if (ServerPlayer* player = session->GetPlayer()) {
            UpdatePlayerTickets(session->GetPlayerId(),
                                session->GetDimensionId(),
                                player->getChunkPosition(),
                                session->GetSimulationDistance());
        }

        // Portal far sides (ChunkLoader::Source::Portal/IndirectPortal) are
        // kept loaded with a temporary ticket at the loader's centre, refreshed
        // well inside its lifespan. The level is chosen so everything within
        // the loader radius is at least FULL (loaded) and the chunks nearest
        // the destination are entity-ticking — mobs on the far side move,
        // as the mod's region tickets make them.
        constexpr int kLoaderTicketRefreshTicks = 20;
        constexpr int kLoaderTicketLifespan     = 40;
        if ((m_currentTick % kLoaderTicketRefreshTicks) == (session->GetPlayerId() % kLoaderTicketRefreshTicks)) {
            for (const ChunkLoader& loader : session->Loaders()) {
                if (loader.source == ChunkLoader::Source::Player) continue;
                if (loader.IsSimulation()) continue;   // its levels come from the player's own ticket
                ChunkTicketManager* tickets = TicketsForDimension(Game::DimensionToRaw(loader.dimension));
                if (!tickets) continue;
                const int level = std::min(ChunkTicketManager::FULL_LEVEL - loader.Radius(),
                                           ChunkTicketManager::ENTITY_TICKING_LEVEL - 1);
                tickets->AddTemporaryTicket(loader.Center(), std::max(0, level), kLoaderTicketLifespan);
            }
        }

        // No watch-index synchronization: there is no index. "Who is watching
        // chunk X" is answered by asking each session's tracking view
        // (ForEachSessionWatching), exactly as MC asks each player's
        // ChunkTrackingView in ChunkMap.onChunkReadyToSend.
    }

    bool PlayerSessionManager::IsSessionTimedOut(std::shared_ptr<PlayerSession> session) const {
        auto stats = session->GetStats();
        auto now = std::chrono::steady_clock::now();
        auto timeSinceLastKeepAlive = std::chrono::duration_cast<std::chrono::seconds>(
            now - stats.lastKeepAlive).count();
        
        return timeSinceLastKeepAlive > m_config.sessionTimeoutSeconds;
    }

    void PlayerSessionManager::InitializeSpawnChunks() {
        if (!m_ticketManager) {
            return;
        }
        
        // Calculate spawn chunk
        Game::Math::ChunkPos spawnChunk(
            static_cast<int>(std::floor(m_config.worldSpawn.x / 16.0f)),
            static_cast<int>(std::floor(m_config.worldSpawn.z / 16.0f))
        );
        
        // Add spawn tickets
        m_ticketManager->AddSpawnTickets(spawnChunk, m_config.spawnChunkRadius);
    }

    void PlayerSessionManager::CleanupSession(uint32_t playerId) {
        // Remove all tickets for this player, in EVERY dimension. A player who
        // walked through a portal and then disconnected holds tickets in both,
        // and only the ones in the level they happen to be standing in now
        // would be found by TicketsForDimension.
        if (Server::g_integratedServer) {
            Server::g_integratedServer->ForEachLevel([playerId](ServerLevel& level) {
                if (level.Tickets()) level.Tickets()->RemoveAllPlayerTickets(playerId);
            });
        } else if (m_ticketManager) {
            m_ticketManager->RemoveAllPlayerTickets(playerId);
        }
        
    }

} // namespace Server
namespace Server {

    void PlayerSessionManager::SetLevelServices(ChunkTicketManager* tickets,
                                                ChunkStatusManager* status) {
        m_ticketManager = tickets;
        m_statusManager = status;
        InitializeSpawnChunks();
    }

    ChunkTicketManager* PlayerSessionManager::TicketsForDimension(int rawDimensionId) const {
        if (Server::g_integratedServer) {
            ServerLevel* level = Server::g_integratedServer->GetLevel(
                Game::DimensionFromRaw(rawDimensionId));
            if (level && level->Tickets()) return level->Tickets();
        }
        return m_ticketManager;
    }

    bool PlayerSessionManager::AnySessionLoadsDimension(Game::DimensionId dimension) const {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        for (const auto& [playerId, session] : m_sessions) {
            if (session && session->LoadsDimension(dimension)) return true;
        }
        return false;
    }

} // namespace Server
