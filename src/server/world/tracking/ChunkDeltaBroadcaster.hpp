// File: src/server/world/tracking/ChunkDeltaBroadcaster.hpp
#pragma once

#include "SectionChangeAccumulator.hpp"
#include "../watch/ChunkWatchIndex.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/network/PacketTypes.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Server {

// Forward declarations
class IntegratedServer;
class PlayerSession;

// Broadcasts accumulated block changes to watching players
// This is the single place where "single vs multi" packet decision is made
class ChunkDeltaBroadcaster {
public:
    // Threshold for deciding between single and multi-block packets
    static constexpr size_t MULTI_THRESHOLD = 64;  // Use section update for > 64 changes
    static constexpr size_t CHUNK_RESEND_THRESHOLD = 256;  // Consider full chunk resend above this
    
    ChunkDeltaBroadcaster(IntegratedServer* server,
                         SectionChangeAccumulator* accumulator,
                         ChunkWatchIndex* watchIndex);
    ~ChunkDeltaBroadcaster();
    
    // === MAIN FLUSH ===
    
    // Called once per tick from ServerTick()
    // Drains the accumulator and broadcasts changes to all watchers
    void flush();
    
    // === STATISTICS ===
    
    struct Stats {
        size_t sectionsProcessed = 0;
        size_t singleBlockPackets = 0;
        size_t multiBlockPackets = 0;
        size_t chunkResends = 0;
        size_t totalPacketsSent = 0;
        size_t totalBytesSent = 0;
        size_t playersNotified = 0;
    };
    
    Stats getStats() const { return m_stats; }
    void resetStats() { m_stats = Stats{}; }
    
private:
    // === PACKET BUILDING AND BROADCASTING ===
    
    // Build and send a single block change packet
    void broadcastSingleBlock(const Game::Math::SectionPos& sp, 
                              uint16_t idx, 
                              Game::BlockID state,
                              const std::vector<uint32_t>& watchers);
    
    // Build and send a section blocks update packet
    void broadcastSectionUpdate(const Game::Math::SectionPos& sp,
                               const std::vector<std::pair<uint16_t, Game::BlockID>>& changes,
                               const std::vector<uint32_t>& watchers);
    
    // Build and send a full chunk (for massive changes)
    void broadcastChunkResend(const Game::Math::SectionPos& sp,
                             const std::vector<uint32_t>& watchers);
    
    // === COORDINATE CONVERSION ===
    
    // Convert section-local coordinates to world coordinates
    static glm::ivec3 sectionCellToWorld(const Game::Math::SectionPos& sp, 
                                         uint8_t localX, uint8_t localY, uint8_t localZ);
    
    // === PACKET SENDING ===
    
    // Send a packet to all players in the watcher list
    void sendToAllWatchers(const std::vector<uint32_t>& watchers,
                          const Network::BlockChangeS2CPacket& packet);
    
    void sendToAllWatchers(const std::vector<uint32_t>& watchers,
                          const Network::ClientboundSectionBlocksUpdateS2CPacket& packet);
    
    // Get player sessions for the given player IDs
    std::vector<PlayerSession*> getPlayerSessions(const std::vector<uint32_t>& playerIds);
    
    // === MEMBERS ===
    
    IntegratedServer* m_server;
    SectionChangeAccumulator* m_accumulator;
    ChunkWatchIndex* m_watchIndex;
    
    // Statistics
    Stats m_stats;
    
    // Logging control
    bool m_verboseLogging = false;
};

} // namespace Server