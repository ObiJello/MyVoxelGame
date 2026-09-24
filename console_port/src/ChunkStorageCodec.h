#pragma once
#include "ChunkStorage.h"
#include "ChunkRecord.h"
namespace console {
struct RestoredChunkStorage {
    std::unique_ptr<ChunkStorage> storage;
    // Recomputed host heights differed from the saved byte map. Callers must
    // prepare lighting before treating this chunk as ready for simulation.
    bool needsLightRebuild=false;
};
// Snapshot boundary between the flat generation store and original compressed
// sections. Callers must pause mutations while capturing/restoring a chunk.
class ChunkStorageCodec {
public:
    // Preserve coordinates, time, population flags and opaque NBT from context.
    // This copies light arrays; it does not declare them initialized or correct.
    static std::unique_ptr<ChunkRecord> capture(const ChunkStorage& storage,const ChunkRecord& context);
    // Only blocks registered by the generation adapter can enter ChunkStorage.
    // The raw record codecs continue to preserve other original block IDs.
    static RestoredChunkStorage restore(const ChunkRecord& record);
};
}
