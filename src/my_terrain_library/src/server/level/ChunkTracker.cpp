#include "server/level/ChunkTracker.h"
#include "world/ChunkPos.h"
#include <limits>

// Reference: net/minecraft/server/level/ChunkTracker.java

namespace minecraft {
namespace server {
namespace level {

// Reference: ChunkTracker.java lines 7-9
ChunkTracker::ChunkTracker(int levelCount, int minQueueSize, int minMapSize)
    : DynamicGraphMinFixedPoint(levelCount, minQueueSize, minMapSize)
{}

// Reference: ChunkTracker.java lines 11-13
bool ChunkTracker::isSource(int64_t node) {
    return node == world::ChunkPos::INVALID_CHUNK_POS;
}

// Reference: ChunkTracker.java lines 15-30
void ChunkTracker::checkNeighborsAfterUpdate(int64_t node, int level, bool onlyDecrease) {
    if (!onlyDecrease || level < m_levelCount - 2) {
        world::ChunkPos pos(node);
        int x = pos.x();
        int z = pos.z();

        // Check all 8 neighbors (Chebyshev distance 1)
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
                int64_t neighbor = world::ChunkPos::asLong(x + offsetX, z + offsetZ);
                if (neighbor != node) {
                    checkNeighbor(node, neighbor, level, onlyDecrease);
                }
            }
        }
    }
}

// Reference: ChunkTracker.java lines 32-60
int ChunkTracker::getComputedLevel(int64_t node, int64_t knownParent, int knownLevelFromParent) {
    int computedLevel = knownLevelFromParent;
    world::ChunkPos pos(node);
    int x = pos.x();
    int z = pos.z();

    // Check all 8 neighbors plus the source
    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
        for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
            int64_t neighbor = world::ChunkPos::asLong(x + offsetX, z + offsetZ);
            if (neighbor == node) {
                // Use INVALID_CHUNK_POS to represent the source
                neighbor = world::ChunkPos::INVALID_CHUNK_POS;
            }

            if (neighbor != knownParent) {
                int costFromNeighbor = computeLevelFromNeighbor(neighbor, node, getLevel(neighbor));
                if (computedLevel > costFromNeighbor) {
                    computedLevel = costFromNeighbor;
                }

                // Early exit if we found the minimum possible level
                if (computedLevel == 0) {
                    return computedLevel;
                }
            }
        }
    }

    return computedLevel;
}

// Reference: ChunkTracker.java lines 62-64
int ChunkTracker::computeLevelFromNeighbor(int64_t from, int64_t to, int fromLevel) {
    if (from == world::ChunkPos::INVALID_CHUNK_POS) {
        return getLevelFromSource(to);
    }
    return fromLevel + 1;
}

// Reference: ChunkTracker.java lines 68-70
void ChunkTracker::update(int64_t node, int newLevelFrom, bool onlyDecreased) {
    checkEdge(world::ChunkPos::INVALID_CHUNK_POS, node, newLevelFrom, onlyDecreased);
}

} // namespace level
} // namespace server
} // namespace minecraft
