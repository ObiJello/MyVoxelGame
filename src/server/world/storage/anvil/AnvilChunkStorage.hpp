// File: src/server/world/storage/anvil/AnvilChunkStorage.hpp
//
// Chunk I/O for one dimension, in vanilla Anvil format.
//
// Three pieces:
//
//   AnvilChunkIo       one mutex-guarded RegionStore serving BOTH directions
//   AnvilChunkStorage  the IChunkSaver: snapshot on the caller's thread,
//                      write on one owned thread
//   AnvilChunkLoader   the read side, synchronous on the caller's thread
//
// WHY ONE STORE. Reads happen on chunk-worker threads — ChunkProvider::GetChunk
// blocks on one — while writes happen on the storage thread. A write RELOCATES
// a chunk (allocate new sectors, patch header, free old), so a second store
// with its own cached sector table would keep pointing at the freed extent,
// which by then may hold a different chunk. Both directions therefore share
// one store behind one mutex.
//
// WHY SNAPSHOT ON THE CALLER'S THREAD. The old saver deep-copied via
// Chunk::Clone() before queuing, and Clone() does not copy the block-entity
// map — so a chest's contents were dropped before the saver ever saw them.
// Serialising where the chunk is still live fixes that, and makes the snapshot
// atomic with respect to the eviction that triggered it. The bytes ARE the
// snapshot, and they are smaller than the chunk.
#pragma once

#include "server/world/interfaces/IChunkSaver.hpp"
#include "server/world/storage/anvil/RegionStore.hpp"
#include "server/world/storage/anvil/SaveRoot.hpp"

#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldMath.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Game {
    class Chunk;
}

namespace Game::Anvil {

    // Serialises every touch of a region file. Shared by the saver and the
    // loader so they cannot disagree about where a chunk lives.
    class AnvilChunkIo {
    public:
        // Writable: only from a SaveRoot.
        explicit AnvilChunkIo(SaveRoot root)
            : m_store(std::make_unique<RegionStore>(std::move(root))), m_writable(true) {}

        // Read-only over an imported Minecraft world.
        static std::shared_ptr<AnvilChunkIo> OpenReadOnly(const std::filesystem::path& worldRoot) {
            return std::shared_ptr<AnvilChunkIo>(
                new AnvilChunkIo(RegionStore::OpenReadOnly(worldRoot)));
        }

        // Decompressed chunk NBT. False with an EMPTY error means "not on
        // disk", which is the ordinary case for an ungenerated chunk.
        bool ReadChunkNbt(DimensionId dim, RegionKind kind, Math::ChunkPos pos,
                          std::vector<uint8_t>& out, std::string& error);

        // `payload` must already be compressed.
        bool WriteChunkNbt(DimensionId dim, RegionKind kind, Math::ChunkPos pos,
                           const std::vector<uint8_t>& payload, std::string& error);

        // Drop a chunk entirely — entities/*.mca needs this when a chunk's
        // entity list empties out.
        bool ClearChunk(DimensionId dim, RegionKind kind, Math::ChunkPos pos, std::string& error);

        void CloseAll();

    private:
        explicit AnvilChunkIo(std::unique_ptr<RegionStore> store)
            : m_store(std::move(store)), m_writable(false) {}

        std::mutex                   m_mutex;
        std::unique_ptr<RegionStore> m_store;
        bool                         m_writable;
    };

    // ── The saver ───────────────────────────────────────────────────────────

    class AnvilChunkStorage final : public IChunkSaver {
    public:
        // MC ChunkMap.saveChunkIfNeeded: at most one write per chunk per
        // 10 seconds, which is what keeps a chunk being edited from
        // rewriting its region file on every block change.
        static constexpr std::chrono::milliseconds kWriteCooldown{10000};

        AnvilChunkStorage(std::shared_ptr<AnvilChunkIo> io, DimensionId dim, int dataVersion);
        ~AnvilChunkStorage() override;

        bool Initialize() override;
        ChunkSaveResult SaveChunk(const Chunk& chunk) override;
        std::future<ChunkSaveResult> SaveChunkAsync(const Chunk& chunk) override;
        // Serialise + compress on the IO thread (see IChunkSaver). Measured
        // 2026-08-29: the caller-thread Encode cost ~0.9 ms per chunk, and a
        // far teleport evicts ~2,400 chunks in one tick — a 2.1 s server stall.
        std::future<ChunkSaveResult> SaveEvictedAsync(std::shared_ptr<const Chunk> chunk) override;

        // A chunk evicted moments ago may be asked for again before its
        // deferred write has happened. The loader consults this first: if a
        // pending job still holds the chunk object, it is serialised here (on
        // the loader's thread) so the read sees exactly what the write will.
        bool TryReadPending(Math::ChunkPos pos, std::vector<uint8_t>& nbtOut, std::string& error);
        std::vector<ChunkSaveResult> SaveChunks(
            const std::vector<std::shared_ptr<const Chunk>>& chunks) override;
        void FlushAndJoin() override;
        SaverStats GetStats() const override;
        void Shutdown() override;

        // The world clock, refreshed once per server tick. Only the
        // scheduled-tick list reads it, and only to turn absolute trigger
        // ticks into the DELAYS vanilla stores — so one-tick staleness is
        // harmless and a lock would be pure ceremony. Encode() runs on the
        // caller's thread, which is not always the server thread, hence atomic.
        void SetGameTime(int64_t gameTime) {
            m_gameTime.store(gameTime, std::memory_order_relaxed);
        }

    private:
        struct Job {
            Math::ChunkPos                       pos;
            std::vector<uint8_t>                 payload;   // already zlib'd
            std::shared_ptr<std::promise<ChunkSaveResult>> promise;
            // SaveEvictedAsync jobs: payload is empty and the IO thread
            // encodes this just before writing. Null for snapshot jobs.
            std::shared_ptr<const Chunk>         chunk;
        };

        // Snapshot + compress, on the CALLER's thread.
        bool Encode(const Chunk& chunk, std::vector<uint8_t>& out, std::string& error);
        bool WithinWriteCooldown(Math::ChunkPos pos);
        void Enqueue(Job&& job);
        void IoThreadMain();

        std::shared_ptr<AnvilChunkIo> m_io;
        DimensionId                   m_dimension;
        int                           m_dataVersion;
        std::atomic<int64_t>          m_gameTime{0};   // see SetGameTime

        mutable std::mutex      m_mutex;
        std::condition_variable m_wake;
        // LAST WRITE WINS. The old saver's dedupe dropped the NEWER snapshot
        // and orphaned its promise forever; queuing the newest is both correct
        // and what vanilla's IOWorker does with its pendingWrites map.
        std::unordered_map<Math::ChunkPos, Job, Math::ChunkPosHash> m_pending;
        std::deque<Math::ChunkPos> m_order;
        std::unordered_map<Math::ChunkPos, std::chrono::steady_clock::time_point,
                           Math::ChunkPosHash> m_lastWrite;

        std::thread       m_thread;
        std::atomic<bool> m_draining{false};   // stop accepting; finish the queue
        std::atomic<bool> m_closed{false};     // joined; further saves fail loudly

        mutable std::mutex m_statsMutex;
        SaverStats         m_stats;
    };

    // ── The loader ──────────────────────────────────────────────────────────

    class AnvilChunkLoader {
    public:
        AnvilChunkLoader(std::shared_ptr<AnvilChunkIo> io, DimensionId dim)
            : m_io(std::move(io)), m_dimension(dim) {}

        // The storage whose not-yet-written evictions this loader must see
        // (AnvilChunkStorage::TryReadPending). Optional.
        void SetPendingSource(std::weak_ptr<AnvilChunkStorage> storage) {
            m_pending = std::move(storage);
        }

        // False with an empty `error` simply means "not saved yet" — the
        // caller then generates. A non-empty error is a real failure and the
        // caller must NOT silently regenerate over it, or a transient read
        // problem quietly destroys a player's build.
        bool LoadChunk(Math::ChunkPos pos, Chunk& out, std::string& error);

        // See AnvilChunkStorage::SetGameTime — the read half. Loads happen on
        // the worker pool, so this is atomic for the same reason.
        void SetGameTime(int64_t gameTime) {
            m_gameTime.store(gameTime, std::memory_order_relaxed);
        }

    private:
        std::shared_ptr<AnvilChunkIo> m_io;
        DimensionId                   m_dimension;
        std::atomic<int64_t>          m_gameTime{0};
        std::weak_ptr<AnvilChunkStorage> m_pending;
    };

} // namespace Game::Anvil
