// File: src/server/world/tracking/ChunkDeltaBroadcaster.cpp
#include "ChunkDeltaBroadcaster.hpp"
#include "../../IntegratedServer.hpp"
#include "../../session/PlayerSession.hpp"
#include "../../session/PlayerSessionManager.hpp"
#include "../../network/NetworkServer.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"

namespace Server {

ChunkDeltaBroadcaster::ChunkDeltaBroadcaster(IntegratedServer* server,
                                           SectionChangeAccumulator* accumulator,
                                           ChunkWatchIndex* watchIndex)
    : m_server(server)
    , m_accumulator(accumulator)
    , m_watchIndex(watchIndex) {
}

ChunkDeltaBroadcaster::~ChunkDeltaBroadcaster() = default;

void ChunkDeltaBroadcaster::flush() {
    if (!m_accumulator || !m_watchIndex || !m_server) {
        return;
    }
    
    // Reset per-flush statistics
    size_t sectionsProcessed = 0;
    size_t totalPackets = 0;
    
    // Drain all accumulated changes
    auto allChanges = m_accumulator->drain();
    
    if (allChanges.empty()) {
        return;  // Nothing to broadcast
    }
    
    // Process each section's changes
    for (const auto& [sectionPos, changes] : allChanges) {
        sectionsProcessed++;
        
        Log::Info("[ChunkDeltaBroadcaster] Processing section (%d,%d,%d) with %zu changes",
                  sectionPos.chunkX, sectionPos.sectionY, sectionPos.chunkZ, changes.size());
        
        // Get all players watching this section
        auto watcherSet = m_watchIndex->GetSectionWatchers(sectionPos);
        if (watcherSet.empty()) {
            Log::Warning("[ChunkDeltaBroadcaster] No watchers for section (%d,%d,%d), skipping broadcast",
                        sectionPos.chunkX, sectionPos.sectionY, sectionPos.chunkZ);
            continue;  // No one watching, skip
        }
        
        // Convert set to vector for easier use
        std::vector<uint32_t> watchers(watcherSet.begin(), watcherSet.end());
        
        // Decide packet type based on number of changes
        const size_t numChanges = changes.size();
        
        if (numChanges == 0) {
            continue;  // Shouldn't happen, but be safe
        } else if (numChanges == 1) {
            // Single block change - use simple packet
            const auto& [idx, blockId] = changes[0];
            broadcastSingleBlock(sectionPos, idx, blockId, watchers);
            m_stats.singleBlockPackets++;
            totalPackets++;
        } else if (numChanges <= MULTI_THRESHOLD) {
            // Multiple changes but not too many - use section update
            broadcastSectionUpdate(sectionPos, changes, watchers);
            m_stats.multiBlockPackets++;
            totalPackets++;
        } else if (numChanges <= CHUNK_RESEND_THRESHOLD) {
            // Still use section update for moderately large changes
            broadcastSectionUpdate(sectionPos, changes, watchers);
            m_stats.multiBlockPackets++;
            totalPackets++;
        } else {
            // Massive changes - might want to resend the whole chunk
            // For now, still use section update (chunk resend would need more infrastructure)
            Log::Warning("ChunkDeltaBroadcaster: %zu changes in section (%d,%d,%d), using section update",
                        numChanges, sectionPos.chunkX, sectionPos.sectionY, sectionPos.chunkZ);
            broadcastSectionUpdate(sectionPos, changes, watchers);
            m_stats.multiBlockPackets++;
            totalPackets++;
        }
        
        m_stats.playersNotified += watchers.size();
    }
    
    // Update statistics
    m_stats.sectionsProcessed += sectionsProcessed;
    m_stats.totalPacketsSent += totalPackets;
    
    // Log if significant activity
    if (sectionsProcessed > 5 || m_verboseLogging) {
        Log::Debug("ChunkDeltaBroadcaster: Processed %zu sections, sent %zu packets (%zu single, %zu multi) to watchers",
                  sectionsProcessed, totalPackets, m_stats.singleBlockPackets, m_stats.multiBlockPackets);
    }
}

void ChunkDeltaBroadcaster::broadcastSingleBlock(const Game::Math::SectionPos& sp,
                                                uint16_t idx,
                                                Game::BlockID state,
                                                const std::vector<uint32_t>& watchers) {
    // Unpack the local coordinates
    uint8_t localX, localY, localZ;
    SectionChangeAccumulator::unpackLocalIndex(idx, localX, localY, localZ);
    
    // Convert to world coordinates
    glm::ivec3 worldPos = sectionCellToWorld(sp, localX, localY, localZ);
    
    // Build the packet
    Network::BlockChangeS2CPacket packet(worldPos.x, worldPos.y, worldPos.z, state);
    
    // Send to all watchers
    sendToAllWatchers(watchers, packet);
    
    if (m_verboseLogging) {
        Log::Debug("ChunkDeltaBroadcaster: Single block at (%d,%d,%d) -> %d sent to %zu watchers",
                  worldPos.x, worldPos.y, worldPos.z, static_cast<int>(state), watchers.size());
    }
}

void ChunkDeltaBroadcaster::broadcastSectionUpdate(const Game::Math::SectionPos& sp,
                                                  const std::vector<std::pair<uint16_t, Game::BlockID>>& changes,
                                                  const std::vector<uint32_t>& watchers) {
    // Build the section blocks update packet
    Network::ClientboundSectionBlocksUpdateS2CPacket packet(sp.getChunkPos(), sp.sectionY);
    
    // Add all changes to the packet
    for (const auto& [idx, blockId] : changes) {
        uint8_t localX, localY, localZ;
        SectionChangeAccumulator::unpackLocalIndex(idx, localX, localY, localZ);
        
        // Add to packet using its helper method
        packet.AddChange(localX, localY, localZ, static_cast<uint16_t>(blockId));
    }
    
    // Send to all watchers
    sendToAllWatchers(watchers, packet);
    
    if (m_verboseLogging) {
        Log::Debug("ChunkDeltaBroadcaster: Section (%d,%d,%d) with %zu changes sent to %zu watchers",
                  sp.chunkX, sp.sectionY, sp.chunkZ, changes.size(), watchers.size());
    }
}

void ChunkDeltaBroadcaster::broadcastChunkResend(const Game::Math::SectionPos& sp,
                                                const std::vector<uint32_t>& watchers) {
    // TODO: Implement full chunk resend for massive changes
    // This would require access to the World to get current chunk data
    // For now, log a warning
    Log::Warning("ChunkDeltaBroadcaster: Full chunk resend not yet implemented for chunk (%d,%d)",
                sp.chunkX, sp.chunkZ);
}

glm::ivec3 ChunkDeltaBroadcaster::sectionCellToWorld(const Game::Math::SectionPos& sp,
                                                    uint8_t localX, uint8_t localY, uint8_t localZ) {
    // Convert section-local coordinates to world coordinates
    int worldX = sp.chunkX * 16 + localX;
    int worldY = sp.sectionY * 16 + localY - 64;  // Adjust for world min Y of -64
    int worldZ = sp.chunkZ * 16 + localZ;
    
    return glm::ivec3(worldX, worldY, worldZ);
}

void ChunkDeltaBroadcaster::sendToAllWatchers(const std::vector<uint32_t>& watchers,
                                             const Network::BlockChangeS2CPacket& packet) {
    if (!m_server) return;
    
    // For integrated server, broadcast through the network server
    auto* networkServer = m_server->GetNetworkServer();
    if (networkServer) {
        // Serialize packet once
        auto data = Network::Serialization::Serialize(packet);
        
        // Broadcast to all connections (network server will filter by authentication)
        networkServer->BroadcastPacket(static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
        
        m_stats.totalBytesSent += data.size() * watchers.size();
    }
}

void ChunkDeltaBroadcaster::sendToAllWatchers(const std::vector<uint32_t>& watchers,
                                             const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
    if (!m_server) return;
    
    // For integrated server, use the server's send methods
    m_server->SendSectionBlocksUpdateS2CPacket(packet);
    
    // Estimate packet size for statistics
    size_t estimatedSize = 16 + packet.packedRecords.size() * 4;
    m_stats.totalBytesSent += estimatedSize * watchers.size();
}

std::vector<PlayerSession*> ChunkDeltaBroadcaster::getPlayerSessions(const std::vector<uint32_t>& playerIds) {
    std::vector<PlayerSession*> sessions;
    sessions.reserve(playerIds.size());
    
    // TODO: Get sessions from PlayerSessionManager once it's accessible
    // For now, just return empty (packets will still be broadcast via NetworkServer)
    
    return sessions;
}

} // namespace Server