#pragma once

#include <atomic>
#include <cstdint>

// Process-wide live-object gauges for the embedder's memory reports
// (ServerStressStats): how many of the library's big per-chunk objects exist
// right now, and how many holders processUnloads has actually freed. Relaxed
// atomics — a reading, not a synchronisation point.

namespace minecraft {
namespace util {

struct LiveCounters {
    // ProtoChunk: a chunk's blocks, biomes and heightmaps (~all of a
    // holder's memory once its NoiseChunk is gone).
    static std::atomic<int64_t>& protoChunks() { static std::atomic<int64_t> v{0}; return v; }
    // NoiseChunk: the per-chunk density-function cache (~600 KB each).
    static std::atomic<int64_t>& noiseChunks() { static std::atomic<int64_t> v{0}; return v; }
    // Holders whose deferred delete has RUN (ChunkMap::processUnloads queues
    // it behind the dispatchers; this counts completions, not requests).
    static std::atomic<int64_t>& holdersDeleted() { static std::atomic<int64_t> v{0}; return v; }
};

} // namespace util
} // namespace minecraft
