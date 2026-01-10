#include "server/level/SimulationChunkTracker.h"
#include "world/level/TicketStorage.h"

// Reference: net/minecraft/server/level/SimulationChunkTracker.java

namespace minecraft {
namespace server {
namespace level {

// Reference: SimulationChunkTracker.java lines 13-17
SimulationChunkTracker::SimulationChunkTracker(world::level::TicketStorage& ticketStorage)
    : ChunkTracker(MAX_LEVEL + 1, 16, 256)
    , m_ticketStorage(ticketStorage)
{
    // Register as listener for simulation ticket updates
    ticketStorage.setSimulationChunkUpdatedListener(
        [this](int64_t node, int newLevel, bool onlyDecreased) {
            this->update(node, newLevel, onlyDecreased);
        });
}

// Reference: SimulationChunkTracker.java lines 19-21
int SimulationChunkTracker::getLevelFromSource(int64_t to) {
    return m_ticketStorage.getTicketLevelAt(to, true);
}

// Reference: SimulationChunkTracker.java lines 24-26
int SimulationChunkTracker::getLevel(const world::ChunkPos& pos) const {
    return getLevel(pos.toLong());
}

// Reference: SimulationChunkTracker.java lines 28-30
int SimulationChunkTracker::getLevel(int64_t node) {
    auto it = m_chunks.find(node);
    if (it != m_chunks.end()) {
        return static_cast<int>(it->second);
    }
    return MAX_LEVEL;
}

// Reference: SimulationChunkTracker.java lines 32-39
void SimulationChunkTracker::setLevel(int64_t node, int level) {
    if (level >= MAX_LEVEL) {
        m_chunks.erase(node);
    } else {
        m_chunks[node] = static_cast<uint8_t>(level);
    }
}

// Reference: SimulationChunkTracker.java lines 41-43
void SimulationChunkTracker::runAllUpdates() {
    runUpdates(std::numeric_limits<int>::max());
}

} // namespace level
} // namespace server
} // namespace minecraft
