// File: src/server/world/tracking/ChunkDeltaBroadcaster.cpp
#include "ChunkDeltaBroadcaster.hpp"
#include "../../IntegratedServer.hpp"
#include "../../session/PlayerSession.hpp"
#include "../../session/PlayerSessionManager.hpp"
#include "../../network/NetworkServer.hpp"
#include "../../network/ServerConnection.hpp"
#include "common/core/Log.hpp"
#include "common/network/PacketTypes.hpp"

namespace Server {

ChunkDeltaBroadcaster::ChunkDeltaBroadcaster(IntegratedServer* server,
                                           SectionChangeAccumulator* accumulator,
                                           PlayerSessionManager* sessionManager,
                                           Game::DimensionId dimension)
    : m_server(server)
    , m_accumulator(accumulator)
    , m_sessionManager(sessionManager)
    , m_dimension(dimension) {
}

ChunkDeltaBroadcaster::~ChunkDeltaBroadcaster() = default;

void ChunkDeltaBroadcaster::flush() {
    if (!m_accumulator || !m_sessionManager || !m_server) {
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
        
        // No per-section logging here. Every Log:: call is a 2 KB vsnprintf, the
        // GLOBAL log mutex, an fprintf to stdout, a timestamp allocation, a
        // ring push and an fprintf to the log file — none of it compiled out,
        // since only Log::Debug is NDEBUG-guarded. On the SERVER TICK THREAD,
        // once per changed section, while an explosion is feeding hundreds of
        // sections into the accumulator. It also serialises the tick thread
        // against the render and network threads on that one mutex.
        // MC's ChunkHolder.broadcastChanges logs nothing.

        // Who is watching this section = who is tracking its chunk. Asked of
        // the sessions directly (MC ChunkMap.onChunkReadyToSend does the same
        // walk) rather than of a reverse index that has to be kept in sync.
        const Game::Math::ChunkPos chunkPos{sectionPos.chunkX, sectionPos.chunkZ};
        std::vector<uint32_t> watchers =
            m_sessionManager->GetChunkWatchers(m_dimension, chunkPos);
        if (watchers.empty()) {
            // Routine, not a warning: a section changing with nobody in range
            // is the normal case for anything away from a player.
            Log::Debug("[ChunkDeltaBroadcaster] No watchers for section (%d,%d,%d), skipping",
                       sectionPos.chunkX, sectionPos.sectionY, sectionPos.chunkZ);
            continue;  // No one watching, skip
        }
        
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
            // Massive changes — a chunk resend would be better but needs more
            // infrastructure, so still a section update.
            //
            // Deliberately NOT logged: MULTI_THRESHOLD is 64, so an explosion
            // trips this for essentially every section it touches, and a
            // Log::Warning additionally does a synchronous fflush on the tick
            // thread.
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
                                                Game::BlockState state,
                                                const std::vector<uint32_t>& watchers) {
    // Unpack the local coordinates
    uint8_t localX, localY, localZ;
    SectionChangeAccumulator::unpackLocalIndex(idx, localX, localY, localZ);
    
    // Convert to world coordinates
    glm::ivec3 worldPos = sectionCellToWorld(sp, localX, localY, localZ);
    
    // Build the packet
    Network::BlockChangeS2CPacket packet(worldPos.x, worldPos.y, worldPos.z, state.Block(), state.Index());
    
    // Send to all watchers
    sendToAllWatchers(watchers, packet);
    
    if (m_verboseLogging) {
        Log::Debug("ChunkDeltaBroadcaster: Single block at (%d,%d,%d) -> %d sent to %zu watchers",
                  worldPos.x, worldPos.y, worldPos.z, static_cast<int>(state.Block()), watchers.size());
    }
}

void ChunkDeltaBroadcaster::broadcastSectionUpdate(const Game::Math::SectionPos& sp,
                                                  const std::vector<std::pair<uint16_t, Game::BlockState>>& changes,
                                                  const std::vector<uint32_t>& watchers) {
    // Build the section blocks update packet
    Network::ClientboundSectionBlocksUpdateS2CPacket packet(sp.getChunkPos(), sp.sectionY);
    
    // Add all changes to the packet
    for (const auto& [idx, blockId] : changes) {
        uint8_t localX, localY, localZ;
        SectionChangeAccumulator::unpackLocalIndex(idx, localX, localY, localZ);
        
        // Add to packet using its helper method
        packet.AddChange(localX, localY, localZ, static_cast<uint16_t>(blockId.Block()), blockId.Index());
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
    if (!m_sessionManager || watchers.empty()) return;

    // Sent to the watcher list this flush already computed, NOT broadcast.
    // A BlockChangeS2C carries no dimension, so a broadcast delivers a Nether
    // edit to a player in the Overworld, who applies it to their chunk at the
    // same x/z. The list is already dimension-scoped by GetChunkWatchers.
    const auto data = Network::Serialization::Serialize(packet);

    for (uint32_t playerId : watchers) {
        auto session = m_sessionManager->GetSession(playerId);
        if (!session) continue;
        auto* conn = session->GetConnection();
        if (!conn) continue;
        conn->SendPacketIn(m_dimension, static_cast<uint8_t>(Network::PacketId::BlockChangeS2C), data);
        m_stats.totalBytesSent += data.size();
    }
}

void ChunkDeltaBroadcaster::sendToAllWatchers(const std::vector<uint32_t>& watchers,
                                             const Network::ClientboundSectionBlocksUpdateS2CPacket& packet) {
    if (!m_server) return;

    // The section packet's hand-rolled serialization lives on the server, so
    // this still delegates — but the server scopes it to this dimension's
    // watchers now, for the same reason as the single-block path above.
    m_server->SendSectionBlocksUpdateS2CPacket(m_dimension, packet);

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