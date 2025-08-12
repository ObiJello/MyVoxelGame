// File: src/server/world/ticketing/ChunkTicketManager.cpp
#include "ChunkTicketManager.hpp"
#include <algorithm>
#include <limits>

namespace Server {

    ChunkTicketManager::ChunkTicketManager() {
        m_chunkLevels.reserve(4096); // Pre-allocate for typical world size
    }

    ChunkTicketManager::~ChunkTicketManager() {
        Clear();
    }

    // === TICKET MANAGEMENT ===

    void ChunkTicketManager::AddPlayerTicket(uint32_t playerId, Game::Math::ChunkPos chunk, int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Create the ticket
        PlayerTicket ticket(TicketType::PLAYER, level, playerId, m_currentTick);
        
        // Track player->chunks mapping
        m_playerChunks[playerId].insert(chunk);
        
        // Add ticket to chunk
        m_playerTickets[chunk].insert(ticket);
        
        // Invalidate cache
        InvalidateLevelCache();
    }

    void ChunkTicketManager::RemovePlayerTicket(uint32_t playerId, Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Remove from player->chunks mapping
        auto playerIt = m_playerChunks.find(playerId);
        if (playerIt != m_playerChunks.end()) {
            playerIt->second.erase(chunk);
            if (playerIt->second.empty()) {
                m_playerChunks.erase(playerIt);
            }
        }
        
        // Remove ticket from chunk
        auto chunkIt = m_playerTickets.find(chunk);
        if (chunkIt != m_playerTickets.end()) {
            auto& tickets = chunkIt->second;
            tickets.erase(PlayerTicket(TicketType::PLAYER, 0, playerId, 0));
            if (tickets.empty()) {
                m_playerTickets.erase(chunkIt);
            }
        }
        
        InvalidateLevelCache();
    }

    void ChunkTicketManager::RemoveAllPlayerTickets(uint32_t playerId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Find all chunks for this player
        auto playerIt = m_playerChunks.find(playerId);
        if (playerIt == m_playerChunks.end()) {
            return;
        }
        
        // Remove tickets from each chunk
        for (const auto& chunk : playerIt->second) {
            auto chunkIt = m_playerTickets.find(chunk);
            if (chunkIt != m_playerTickets.end()) {
                auto& tickets = chunkIt->second;
                tickets.erase(PlayerTicket(TicketType::PLAYER, 0, playerId, 0));
                if (tickets.empty()) {
                    m_playerTickets.erase(chunkIt);
                }
            }
        }
        
        // Remove player entry
        m_playerChunks.erase(playerIt);
        InvalidateLevelCache();
    }

    void ChunkTicketManager::AddSpawnTickets(Game::Math::ChunkPos spawnChunk, int radius) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Add tickets for spawn area using Chebyshev distance
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                Game::Math::ChunkPos chunk(spawnChunk.x + dx, spawnChunk.z + dz);
                int distance = std::max(std::abs(dx), std::abs(dz));
                int level = CalculateTicketLevel(distance);
                
                SpawnTicket ticket(TicketType::SPAWN, level, "spawn", m_currentTick);
                m_spawnTickets[chunk].insert(ticket);
            }
        }
        
        InvalidateLevelCache();
    }

    void ChunkTicketManager::AddForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk, int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        ForcedTicket ticket(TicketType::FORCED, level, identifier, m_currentTick);
        
        m_forcedChunks[identifier].insert(chunk);
        m_forcedTickets[chunk].insert(ticket);
        
        InvalidateLevelCache();
    }

    void ChunkTicketManager::RemoveForcedTicket(const std::string& identifier, Game::Math::ChunkPos chunk) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Remove from identifier->chunks mapping
        auto idIt = m_forcedChunks.find(identifier);
        if (idIt != m_forcedChunks.end()) {
            idIt->second.erase(chunk);
            if (idIt->second.empty()) {
                m_forcedChunks.erase(idIt);
            }
        }
        
        // Remove ticket from chunk
        auto chunkIt = m_forcedTickets.find(chunk);
        if (chunkIt != m_forcedTickets.end()) {
            auto& tickets = chunkIt->second;
            tickets.erase(ForcedTicket(TicketType::FORCED, 0, identifier, 0));
            if (tickets.empty()) {
                m_forcedTickets.erase(chunkIt);
            }
        }
        
        InvalidateLevelCache();
    }

    void ChunkTicketManager::AddTemporaryTicket(Game::Math::ChunkPos chunk, int level, int lifespanTicks) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        TempTicket ticket(TicketType::TEMPORARY, level, m_currentTick, m_currentTick, lifespanTicks);
        m_temporaryTickets[chunk].insert(ticket);
        
        InvalidateLevelCache();
    }

    // === LEVEL QUERIES ===

    int ChunkTicketManager::GetChunkLevel(Game::Math::ChunkPos chunk) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        RebuildLevelCacheIfNeeded();
        
        auto it = m_chunkLevels.find(chunk);
        if (it != m_chunkLevels.end()) {
            return it->second;
        }
        
        return MAX_LEVEL; // No tickets = inaccessible
    }

    bool ChunkTicketManager::ShouldChunkBeLoaded(Game::Math::ChunkPos chunk) const {
        return GetChunkLevel(chunk) < INACCESSIBLE_LEVEL;
    }

    bool ChunkTicketManager::ShouldChunkTickBlocks(Game::Math::ChunkPos chunk) const {
        return GetChunkLevel(chunk) <= BLOCK_TICKING_LEVEL;
    }

    bool ChunkTicketManager::ShouldChunkTickEntities(Game::Math::ChunkPos chunk) const {
        return GetChunkLevel(chunk) <= ENTITY_TICKING_LEVEL;
    }

    std::vector<Game::Math::ChunkPos> ChunkTicketManager::GetLoadedChunks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        RebuildLevelCacheIfNeeded();
        
        std::vector<Game::Math::ChunkPos> result;
        result.reserve(m_chunkLevels.size());
        
        for (const auto& [chunk, level] : m_chunkLevels) {
            if (level < INACCESSIBLE_LEVEL) {
                result.push_back(chunk);
            }
        }
        
        return result;
    }

    // === DISTANCE CALCULATIONS ===

    int ChunkTicketManager::SimulationDistanceToLevel(int distance) {
        // Convert simulation distance to ticket level
        // Level 33 at distance 0, increases by 1 per chunk distance
        return ENTITY_TICKING_LEVEL + distance;
    }

    int ChunkTicketManager::LevelToDistance(int level) {
        // Convert ticket level back to distance
        if (level < ENTITY_TICKING_LEVEL) {
            return 0;
        }
        return level - ENTITY_TICKING_LEVEL;
    }

    int ChunkTicketManager::CalculateTicketLevel(int chunkDistance) {
        // Calculate appropriate ticket level for a given chunk distance
        if (chunkDistance <= 0) {
            return ENTITY_TICKING_LEVEL;
        }
        
        int level = ENTITY_TICKING_LEVEL + chunkDistance;
        return std::min(level, MAX_LEVEL);
    }

    // === MAINTENANCE ===

    void ChunkTicketManager::ProcessExpiredTickets(int64_t currentTick) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        m_currentTick = currentTick;
        bool anyExpired = false;
        
        // Process temporary tickets
        for (auto it = m_temporaryTickets.begin(); it != m_temporaryTickets.end();) {
            auto& tickets = it->second;
            
            // Remove expired tickets
            for (auto ticketIt = tickets.begin(); ticketIt != tickets.end();) {
                if (ticketIt->IsExpired(currentTick)) {
                    ticketIt = tickets.erase(ticketIt);
                    anyExpired = true;
                } else {
                    ++ticketIt;
                }
            }
            
            // Remove empty chunk entries
            if (tickets.empty()) {
                it = m_temporaryTickets.erase(it);
            } else {
                ++it;
            }
        }
        
        if (anyExpired) {
            InvalidateLevelCache();
        }
    }

    void ChunkTicketManager::UpdateChunkLevels() {
        std::lock_guard<std::mutex> lock(m_mutex);
        InvalidateLevelCache();
        RebuildLevelCacheIfNeeded();
    }

    ChunkTicketManager::Stats ChunkTicketManager::GetStats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        Stats stats{};
        
        // Count tickets by type
        for (const auto& [chunk, tickets] : m_playerTickets) {
            stats.playerTickets += tickets.size();
        }
        
        for (const auto& [chunk, tickets] : m_spawnTickets) {
            stats.spawnTickets += tickets.size();
        }
        
        for (const auto& [chunk, tickets] : m_forcedTickets) {
            stats.forcedTickets += tickets.size();
        }
        
        for (const auto& [chunk, tickets] : m_temporaryTickets) {
            stats.temporaryTickets += tickets.size();
        }
        
        stats.totalTickets = stats.playerTickets + stats.spawnTickets + 
                           stats.forcedTickets + stats.temporaryTickets;
        
        // Count chunks by load level
        RebuildLevelCacheIfNeeded();
        
        for (const auto& [chunk, level] : m_chunkLevels) {
            if (level < INACCESSIBLE_LEVEL) {
                stats.loadedChunks++;
                if (level <= BLOCK_TICKING_LEVEL) {
                    stats.tickingChunks++;
                    if (level <= ENTITY_TICKING_LEVEL) {
                        stats.entityTickingChunks++;
                    }
                }
            }
        }
        
        return stats;
    }

    void ChunkTicketManager::Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        m_playerChunks.clear();
        m_playerTickets.clear();
        m_spawnTickets.clear();
        m_forcedChunks.clear();
        m_forcedTickets.clear();
        m_temporaryTickets.clear();
        m_chunkLevels.clear();
        m_levelsCacheDirty = true;
    }

    // === INTERNAL HELPERS ===

    int ChunkTicketManager::ComputeChunkLevel(Game::Math::ChunkPos chunk) const {
        int minLevel = MAX_LEVEL;
        
        // Check player tickets
        auto playerIt = m_playerTickets.find(chunk);
        if (playerIt != m_playerTickets.end()) {
            for (const auto& ticket : playerIt->second) {
                minLevel = std::min(minLevel, ticket.level);
            }
        }
        
        // Check spawn tickets
        auto spawnIt = m_spawnTickets.find(chunk);
        if (spawnIt != m_spawnTickets.end()) {
            for (const auto& ticket : spawnIt->second) {
                minLevel = std::min(minLevel, ticket.level);
            }
        }
        
        // Check forced tickets
        auto forcedIt = m_forcedTickets.find(chunk);
        if (forcedIt != m_forcedTickets.end()) {
            for (const auto& ticket : forcedIt->second) {
                minLevel = std::min(minLevel, ticket.level);
            }
        }
        
        // Check temporary tickets
        auto tempIt = m_temporaryTickets.find(chunk);
        if (tempIt != m_temporaryTickets.end()) {
            for (const auto& ticket : tempIt->second) {
                if (!ticket.IsExpired(m_currentTick)) {
                    minLevel = std::min(minLevel, ticket.level);
                }
            }
        }
        
        return minLevel;
    }

    void ChunkTicketManager::RemoveExpiredTemporaryTickets(Game::Math::ChunkPos chunk) {
        auto it = m_temporaryTickets.find(chunk);
        if (it == m_temporaryTickets.end()) {
            return;
        }
        
        auto& tickets = it->second;
        for (auto ticketIt = tickets.begin(); ticketIt != tickets.end();) {
            if (ticketIt->IsExpired(m_currentTick)) {
                ticketIt = tickets.erase(ticketIt);
            } else {
                ++ticketIt;
            }
        }
        
        if (tickets.empty()) {
            m_temporaryTickets.erase(it);
        }
    }

    void ChunkTicketManager::InvalidateLevelCache() {
        m_levelsCacheDirty = true;
    }

    void ChunkTicketManager::RebuildLevelCacheIfNeeded() const {
        if (!m_levelsCacheDirty) {
            return;
        }
        
        m_chunkLevels.clear();
        
        // Collect all chunks with tickets
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> allChunks;
        
        for (const auto& [chunk, tickets] : m_playerTickets) {
            allChunks.insert(chunk);
        }
        for (const auto& [chunk, tickets] : m_spawnTickets) {
            allChunks.insert(chunk);
        }
        for (const auto& [chunk, tickets] : m_forcedTickets) {
            allChunks.insert(chunk);
        }
        for (const auto& [chunk, tickets] : m_temporaryTickets) {
            allChunks.insert(chunk);
        }
        
        // Compute level for each chunk
        for (const auto& chunk : allChunks) {
            int level = ComputeChunkLevel(chunk);
            if (level < MAX_LEVEL) {
                m_chunkLevels[chunk] = level;
            }
        }
        
        m_levelsCacheDirty = false;
    }

} // namespace Server