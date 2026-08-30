// File: src/server/world/interfaces/IChunkSaver.hpp
//
// The seam between the chunk cache and whatever writes chunks to disk.
//
// It is deliberately tiny, and it stays a seam rather than becoming a direct
// dependency, because IChunkSaver.hpp and ChunkCache are compiled into the
// `imgui` CMake target — which must not pull in Boost/Asio-class dependencies.
// The interface is the firewall: it lives in `imgui`, the implementation
// (NBT, zlib, <filesystem>) does not. Naming a concrete saver in ChunkCache
// would drag the whole save stack across that line.
//
// This used to declare ~45 virtuals — backups, retry policies, compression
// levels, per-chunk verification, autosave intervals — of which four were ever
// called. The rest described a saver nobody wrote.
#pragma once

#include "common/world/math/WorldMath.hpp"

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace Game {

    class Chunk;

    struct ChunkSaveResult {
        Math::ChunkPos position;
        bool           success = false;
        std::string    errorMessage;
        size_t         bytesWritten = 0;

        ChunkSaveResult() = default;
        ChunkSaveResult(Math::ChunkPos pos, bool ok) : position(pos), success(ok) {}

        static ChunkSaveResult Success(Math::ChunkPos position, size_t bytes = 0) {
            ChunkSaveResult r(position, true);
            r.bytesWritten = bytes;
            return r;
        }
        static ChunkSaveResult Failure(Math::ChunkPos position, std::string error) {
            ChunkSaveResult r(position, false);
            r.errorMessage = std::move(error);
            return r;
        }
    };

    class IChunkSaver {
    public:
        struct SaverStats {
            size_t chunksSaved     = 0;
            size_t saveFailures    = 0;
            size_t bytesWritten    = 0;
            size_t pendingChunks   = 0;
            size_t chunksSkipped   = 0;   // inside the per-chunk write cooldown
        };

        virtual ~IChunkSaver() = default;

        virtual bool Initialize() = 0;

        // Blocking. Reports the REAL outcome — the old saver returned Success
        // the moment a chunk was queued, and ChunkCache cleared the dirty flag
        // on that, so a failed write looked like a successful one.
        virtual ChunkSaveResult SaveChunk(const Chunk& chunk) = 0;

        // Snapshots the chunk on THIS thread (serialise + compress) and queues
        // the bytes. The chunk need not outlive the call.
        virtual std::future<ChunkSaveResult> SaveChunkAsync(const Chunk& chunk) = 0;

        // Save a chunk that has just LEFT the cache. Nothing can write to it
        // any more, so the saver may defer serialisation to its own thread
        // instead of snapshotting on the caller's — SaveChunkAsync must
        // snapshot immediately because a live chunk keeps changing. Default:
        // behave like SaveChunkAsync.
        virtual std::future<ChunkSaveResult> SaveEvictedAsync(std::shared_ptr<const Chunk> chunk) {
            return SaveChunkAsync(*chunk);
        }

        // Batch form used by ChunkCache::SaveAllDirty.
        virtual std::vector<ChunkSaveResult> SaveChunks(
            const std::vector<std::shared_ptr<const Chunk>>& chunks) = 0;

        // Drain the queue and join the writer. After this, further saves fail
        // rather than being silently dropped. Safe to call more than once.
        virtual void FlushAndJoin() = 0;

        virtual SaverStats GetStats() const = 0;

        virtual void Shutdown() = 0;
    };

    using ChunkSaverFactory = std::function<std::unique_ptr<IChunkSaver>()>;

} // namespace Game
