#pragma once
#include "ScheduledTickQueue.h"
#include "CompoundTag.h"
namespace console {
class ChunkRecord;
// Original OldChunkStorage TileTicks fields. These operations are explicit: raw
// ChunkRecord reads still preserve unported NBT without executing simulation.
class ScheduledTickCodec {
public:
    static void write(CompoundTag& extra,std::span<const TickNextTickData> ticks,std::int64_t time);
    static std::vector<SavedTileTick> read(CompoundTag& extra,int chunkX,int chunkZ);
    // Use on the simulation thread with block mutations paused, alongside the
    // block snapshot. Saving is non-destructive; loading is an atomic queue batch.
    static void saveChunk(ChunkRecord& record,ScheduledTickQueue& queue);
    static void loadChunk(ChunkRecord& record,ScheduledTickQueue& queue);
};
}
