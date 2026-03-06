#pragma once

// Reference: net/minecraft/server/level/FullChunkStatus.java

namespace minecraft {
namespace server {
namespace level {

/**
 * FullChunkStatus - Represents the full status of a chunk
 * Reference: FullChunkStatus.java
 */
enum class FullChunkStatus {
    INACCESSIBLE = 0,
    FULL = 1,
    BLOCK_TICKING = 2,
    ENTITY_TICKING = 3
};

/**
 * Check if this status is at or after the given step
 * Reference: FullChunkStatus.java lines 9-11
 */
inline bool isOrAfter(FullChunkStatus status, FullChunkStatus step) {
    return static_cast<int>(status) >= static_cast<int>(step);
}

} // namespace level
} // namespace server
} // namespace minecraft
