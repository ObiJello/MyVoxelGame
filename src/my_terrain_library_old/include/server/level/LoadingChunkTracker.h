#pragma once

#include "server/level/ChunkTracker.h"
#include "server/level/ChunkLevel.h"
#include <cstdint>

// Reference: net/minecraft/server/level/LoadingChunkTracker.java

// Forward declarations
namespace minecraft {
namespace world {
namespace level {
class TicketStorage;
}
}
namespace server {
namespace level {
class DistanceManager;
class ChunkHolder;
}
}
}

namespace minecraft {
namespace server {
namespace level {

/**
 * LoadingChunkTracker - Tracks loading levels and updates chunk holders
 * Reference: LoadingChunkTracker.java
 *
 * This tracker propagates loading ticket levels and updates
 * ChunkHolder ticket levels through the DistanceManager.
 */
class LoadingChunkTracker : public ChunkTracker {
public:
    /**
     * Maximum level for loading (MAX_LEVEL + 1)
     * Reference: LoadingChunkTracker.java lines 6-7
     */
    static int getMaxLevel() {
        return ChunkLevel::getMaxLevel() + 1;
    }

    /**
     * Constructor
     * Reference: LoadingChunkTracker.java lines 10-14
     *
     * @param distanceManager The distance manager
     * @param ticketStorage The ticket storage
     */
    LoadingChunkTracker(DistanceManager& distanceManager,
                        world::level::TicketStorage& ticketStorage);

    /**
     * Run distance updates
     * Reference: LoadingChunkTracker.java lines 44-46
     *
     * @param count Maximum number of updates to process
     * @return Remaining count after processing
     */
    int runDistanceUpdates(int count);

protected:
    // Reference: LoadingChunkTracker.java lines 17-19
    int getLevelFromSource(int64_t to) override;

    // Reference: LoadingChunkTracker.java lines 21-30
    int getLevel(int64_t node) override;

    // Reference: LoadingChunkTracker.java lines 32-42
    void setLevel(int64_t node, int level) override;

private:
    DistanceManager& m_distanceManager;
    world::level::TicketStorage& m_ticketStorage;
};

} // namespace level
} // namespace server
} // namespace minecraft
