#pragma once

#include <cstdint>

// Reference: net/minecraft/world/level/chunk/status/ChunkType.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * ChunkType - Indicates whether a chunk is a ProtoChunk or LevelChunk
 * Reference: ChunkType.java lines 4-6
 */
enum class ChunkType : int32_t {
    PROTOCHUNK,   // Chunk being generated
    LEVELCHUNK    // Fully generated chunk
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
