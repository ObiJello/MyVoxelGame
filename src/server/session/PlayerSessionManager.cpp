// File: src/server/session/PlayerSessionManager.cpp
#include "PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "../network/ServerConnection.hpp"
#include "../world/ticketing/ChunkTicketManager.hpp"
#include "../world/watch/ChunkWatchIndex.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>

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
        ChunkWatchIndex* watchIndex,
        ChunkStatusManager* statusMgr,
        SendScheduler* scheduler
    ) {
        m_config = config;
        m_ticketManager = ticketMgr;
        m_watchIndex = watchIndex;
        m_statusManager = statusMgr;
        m_sendScheduler = scheduler;
        
        // Initialize spawn chunks
        InitializeSpawnChunks();
        
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
            m_playerSpawns.clear();
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
        glm::vec3 spawnPos = GetPlayerSpawn(playerId);
        
        // Initialize at minimum view distance (2), like Minecraft's ServerPlayer default.
        // The client sends its actual render distance via ClientConfigC2S right after login,
        // which calls OnClientSettingsReceived → SetViewDistance to expand to the real value.
        PlayerSession::Config sessionConfig;
        sessionConfig.simulationDistance = m_config.maxViewDistance;
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
        
        // Add player tickets for simulation distance
        if (m_ticketManager) {
            for (int dx = -m_config.defaultSimulationDistance; dx <= m_config.defaultSimulationDistance; ++dx) {
                for (int dz = -m_config.defaultSimulationDistance; dz <= m_config.defaultSimulationDistance; ++dz) {
                    Game::Math::ChunkPos chunk(spawnChunk.x + dx, spawnChunk.z + dz);
                    int distance = std::max(std::abs(dx), std::abs(dz));
                    int level = ChunkTicketManager::CalculateTicketLevel(distance);
                    m_ticketManager->AddPlayerTicket(playerId, chunk, level);
                }
            }
        }
        
        // Trigger join callback
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
        
        glm::vec3 spawnPos = GetPlayerSpawn(playerId);
        session->Respawn(spawnPos);
        
        Log::Info("PlayerSessionManager: Player %u respawned at (%.1f, %.1f, %.1f)",
                 playerId, spawnPos.x, spawnPos.y, spawnPos.z);
    }

    // === TICK PROCESSING ===

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
        int worldX, int worldY, int worldZ,
        Game::BlockID newBlock
    ) {
        // Calculate chunk position
        Game::Math::ChunkPos chunk(worldX >> 4, worldZ >> 4);
        
        // Get watchers from watch index
        if (!m_watchIndex) {
            return;
        }
        
        auto watchers = m_watchIndex->GetWatchers(chunk);
        
        // Queue change for each watching player
        for (uint32_t playerId : watchers) {
            auto session = GetSession(playerId);
            if (session) {
                session->QueueBlockChange(worldX, worldY, worldZ, newBlock);
            }
        }
    }

    void PlayerSessionManager::BroadcastSectionChanges(
        Game::Math::ChunkPos chunk,
        int section,
        const std::vector<Network::MultiBlockChangeS2CPacket::BlockChange>& changes
    ) {
        if (!m_watchIndex) {
            return;
        }
        
        auto watchers = m_watchIndex->GetWatchers(chunk);
        
        for (uint32_t playerId : watchers) {
            auto session = GetSession(playerId);
            if (session) {
                session->QueueSectionChanges(chunk, section, changes);
            }
        }
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
            packet.position = glm::vec3(srcPlayer->getPosition());
            packet.rotation = srcPlayer->getRotation();
            packet.isCrouching = srcPlayer->IsSneaking();
            packet.sequenceNumber = 0;

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

    void PlayerSessionManager::SetPlayerSpawn(uint32_t playerId, const glm::vec3& spawnPos) {
        m_playerSpawns[playerId] = spawnPos;
    }

    glm::vec3 PlayerSessionManager::GetPlayerSpawn(uint32_t playerId) const {
        auto it = m_playerSpawns.find(playerId);
        if (it != m_playerSpawns.end()) {
            return it->second;
        }
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

    void PlayerSessionManager::UpdatePlayerTickets(
        uint32_t playerId,
        Game::Math::ChunkPos oldAnchor,
        Game::Math::ChunkPos newAnchor,
        int simulationDistance
    ) {
        if (!m_ticketManager) {
            return;
        }
        
        // Remove old tickets
        for (int dx = -simulationDistance; dx <= simulationDistance; ++dx) {
            for (int dz = -simulationDistance; dz <= simulationDistance; ++dz) {
                Game::Math::ChunkPos chunk(oldAnchor.x + dx, oldAnchor.z + dz);
                m_ticketManager->RemovePlayerTicket(playerId, chunk);
            }
        }
        
        // Add new tickets
        for (int dx = -simulationDistance; dx <= simulationDistance; ++dx) {
            for (int dz = -simulationDistance; dz <= simulationDistance; ++dz) {
                Game::Math::ChunkPos chunk(newAnchor.x + dx, newAnchor.z + dz);
                int distance = std::max(std::abs(dx), std::abs(dz));
                int level = ChunkTicketManager::CalculateTicketLevel(distance);
                m_ticketManager->AddPlayerTicket(playerId, chunk, level);
            }
        }
    }

    void PlayerSessionManager::UpdatePlayerWatchers(
        uint32_t playerId,
        const std::vector<Game::Math::ChunkPos>& toAdd,
        const std::vector<Game::Math::ChunkPos>& toRemove
    ) {
        if (!m_watchIndex) {
            return;
        }
        
        m_watchIndex->RemoveWatchers(playerId, toRemove);
        m_watchIndex->AddWatchers(playerId, toAdd);
    }

    std::vector<uint32_t> PlayerSessionManager::GetChunkWatchers(Game::Math::ChunkPos chunk) const {
        if (!m_watchIndex) {
            return {};
        }
        
        auto watchers = m_watchIndex->GetWatchers(chunk);
        return std::vector<uint32_t>(watchers.begin(), watchers.end());
    }

    void PlayerSessionManager::ProcessSessionTick(std::shared_ptr<PlayerSession> session) {
        // Get old position for ticket updates
        Game::Math::ChunkPos oldAnchor = session->GetAnchorChunk();

        // Process the session tick
        session->Tick(m_currentTick);

        // Check if anchor changed for ticket updates
        Game::Math::ChunkPos newAnchor = session->GetAnchorChunk();
        if (oldAnchor != newAnchor) {
            UpdatePlayerTickets(
                session->GetPlayerId(),
                oldAnchor,
                newAnchor,
                session->GetSimulationDistance()
            );
        }

        // Apply watch index changes from session's watch delta processing
        const auto& toAdd = session->GetPendingWatchAdds();
        const auto& toRemove = session->GetPendingWatchRemoves();

        if (!toAdd.empty() || !toRemove.empty()) {
            Log::Debug("[PlayerSessionManager] Processing watch deltas for player %u: +%zu -%zu chunks",
                      session->GetPlayerId(), toAdd.size(), toRemove.size());
            UpdatePlayerWatchers(session->GetPlayerId(), toAdd, toRemove);

            // Clear the consumed deltas
            session->ClearPendingWatchDeltas();
        }
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
        // Remove all tickets for this player
        if (m_ticketManager) {
            m_ticketManager->RemoveAllPlayerTickets(playerId);
        }
        
        // Remove all watchers for this player
        if (m_watchIndex) {
            m_watchIndex->RemoveAllWatchersForPlayer(playerId);
        }
    }

} // namespace Server