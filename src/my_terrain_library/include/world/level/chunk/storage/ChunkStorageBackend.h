#pragma once

#include "world/ChunkPos.h"
#include "nbt/CompoundTag.h"

#include <memory>

namespace minecraft {
namespace world {
namespace level {
namespace chunk {
namespace storage {

/**
 * ChunkStorageBackend - where a ChunkMap's IOWorker reads and writes chunk
 * NBT: MC's RegionFileStorage role behind IOWorker. The standalone library
 * (the parity harness) uses its own RegionFileStorage; an embedder hands in
 * its region I/O instead, so the library and the game share one view of the
 * region files.
 */
class ChunkStorageBackend {
public:
    virtual ~ChunkStorageBackend() = default;

    /** The saved chunk NBT, or nullptr when the chunk was never saved.
     *  Throws on an I/O or decode failure. */
    virtual std::unique_ptr<nbt::CompoundTag> read(const ChunkPos& pos) = 0;

    /** Writes the chunk NBT; nullptr writes nothing. Throws on failure.
     *  A backend may refuse to replace a chunk it owns (a FULL chunk the
     *  embedder saved) and then returns without writing. */
    virtual void write(const ChunkPos& pos, const nbt::CompoundTag* tag) = 0;

    virtual bool hasChunk(const ChunkPos& pos) { return read(pos) != nullptr; }
    virtual void flush() {}
    virtual void close() {}
};

} // namespace storage
} // namespace chunk
} // namespace level
} // namespace world
} // namespace minecraft
