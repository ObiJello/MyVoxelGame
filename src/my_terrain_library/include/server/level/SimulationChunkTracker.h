#pragma once

#include "server/level/ChunkTracker.h"
#include "world/ChunkPos.h"
#include <unordered_map>
#include <cstdint>

// Reference: net/minecraft/server/level/SimulationChunkTracker.java

// Forward declarations
namespace minecraft {
namespace world {
namespace level {
class TicketStorage;
}
}
}

namespace minecraft {
namespace server {
namespace level {

/**
 * SimulationChunkTracker - Tracks simulation levels for chunks
 * Reference: SimulationChunkTracker.java
 *
 * This tracker propagates simulation levels (for entity/block ticking)
 * based on simulation tickets from the TicketStorage.
 */
class SimulationChunkTracker : public ChunkTracker {
public:
    // Reference: SimulationChunkTracker.java line 9
    static constexpr int MAX_LEVEL = 33;

    /**
     * Constructor
     * Reference: SimulationChunkTracker.java lines 13-17
     *
     * @param ticketStorage The ticket storage to get simulation levels from
     */
    explicit SimulationChunkTracker(world::level::TicketStorage& ticketStorage);

    /**
     * Get the level for a chunk position
     * Reference: SimulationChunkTracker.java lines 24-26
     */
    int getLevel(const world::ChunkPos& pos) const;

    /**
     * Run all pending updates
     * Reference: SimulationChunkTracker.java lines 41-43
     */
    void runAllUpdates();

protected:
    // Reference: SimulationChunkTracker.java lines 19-21
    int getLevelFromSource(int64_t to) override;

    // Reference: SimulationChunkTracker.java lines 28-30
    int getLevel(int64_t node) override;

    // Reference: SimulationChunkTracker.java lines 32-39
    void setLevel(int64_t node, int level) override;

private:
    // Reference: SimulationChunkTracker.java line 10
    std::unordered_map<int64_t, uint8_t> m_chunks;
    world::level::TicketStorage& m_ticketStorage;
};

} // namespace level
} // namespace server
} // namespace minecraft
