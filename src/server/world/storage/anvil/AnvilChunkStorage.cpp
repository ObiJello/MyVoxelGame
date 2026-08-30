// File: src/server/world/storage/anvil/AnvilChunkStorage.cpp
#include "common/core/Profiling_Tracy.hpp"
#include "server/world/storage/anvil/AnvilChunkStorage.hpp"

#include "server/world/storage/anvil/ChunkSerializer.hpp"

#include "common/core/Log.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/chunk/Chunk.hpp"

namespace Game::Anvil {

    // ── AnvilChunkIo ────────────────────────────────────────────────────────

    bool AnvilChunkIo::ReadChunkNbt(DimensionId dim, RegionKind kind, Math::ChunkPos pos,
                                    std::vector<uint8_t>& out, std::string& error) {
        std::lock_guard<std::mutex> lock(m_mutex);
        error.clear();

        AnvilRegion* region = m_store->Get(dim, kind,
                                           RegionStore::RegionCoord(pos.x),
                                           RegionStore::RegionCoord(pos.z), error,
                                           /*createIfMissing=*/false);
        if (!region) return false;      // missing file: not an error

        return region->Read(RegionStore::LocalCoord(pos.x),
                            RegionStore::LocalCoord(pos.z), out, error);
    }

    bool AnvilChunkIo::WriteChunkNbt(DimensionId dim, RegionKind kind, Math::ChunkPos pos,
                                     const std::vector<uint8_t>& payload, std::string& error) {
        if (!m_writable) { error = "region store is read-only"; return false; }

        std::lock_guard<std::mutex> lock(m_mutex);
        AnvilRegion* region = m_store->Get(dim, kind,
                                           RegionStore::RegionCoord(pos.x),
                                           RegionStore::RegionCoord(pos.z), error,
                                           /*createIfMissing=*/true);
        if (!region) {
            if (error.empty()) error = "could not open the region file for writing";
            return false;
        }
        return region->Write(RegionStore::LocalCoord(pos.x),
                             RegionStore::LocalCoord(pos.z),
                             payload, AnvilRegion::kCompressionZlib, error);
    }

    bool AnvilChunkIo::ClearChunk(DimensionId dim, RegionKind kind, Math::ChunkPos pos,
                                  std::string& error) {
        if (!m_writable) { error = "region store is read-only"; return false; }

        std::lock_guard<std::mutex> lock(m_mutex);
        AnvilRegion* region = m_store->Get(dim, kind,
                                           RegionStore::RegionCoord(pos.x),
                                           RegionStore::RegionCoord(pos.z), error,
                                           /*createIfMissing=*/false);
        if (!region) return true;       // nothing there to clear
        return region->Clear(RegionStore::LocalCoord(pos.x),
                             RegionStore::LocalCoord(pos.z), error);
    }

    void AnvilChunkIo::CloseAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_store->CloseAll();
    }

    // ── AnvilChunkStorage ───────────────────────────────────────────────────

    AnvilChunkStorage::AnvilChunkStorage(std::shared_ptr<AnvilChunkIo> io,
                                         DimensionId dim, int dataVersion)
        : m_io(std::move(io)), m_dimension(dim), m_dataVersion(dataVersion) {}

    AnvilChunkStorage::~AnvilChunkStorage() { FlushAndJoin(); }

    bool AnvilChunkStorage::Initialize() {
        if (m_thread.joinable()) return true;
        m_thread = std::thread([this] { IoThreadMain(); });
        Log::Info("[Anvil] chunk storage started for dimension %d", static_cast<int>(m_dimension));
        return true;
    }

    bool AnvilChunkStorage::Encode(const Chunk& chunk, std::vector<uint8_t>& out,
                                   std::string& error) {
        std::vector<uint8_t> nbt;
        if (!SerialiseChunk(chunk, m_dataVersion,
                            m_gameTime.load(std::memory_order_relaxed),
                            nbt, error)) return false;

#ifndef NDEBUG
        // Design risk R1: a palette-repack bug is silent until Minecraft
        // refuses one chunk in a thousand. Reading back what we just produced
        // and comparing all 24x4096 states turns that into a caught mismatch
        // here, on a developer's machine, instead of a dead chunk on a
        // player's. Costs a full deserialise, so debug builds only.
        std::string mismatch;
        if (!VerifyRoundTrip(chunk, nbt, mismatch)) {
            error = "round-trip check failed, refusing to write: " + mismatch;
            Log::Error("[Anvil] chunk (%d,%d): %s", chunk.pos.x, chunk.pos.z, error.c_str());
            return false;
        }
#endif

        if (!Nbt::ZlibCompress(nbt, out)) { error = "zlib compression failed"; return false; }
        return true;
    }

    void AnvilChunkStorage::Enqueue(Job&& job) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Math::ChunkPos pos = job.pos;
            auto it = m_pending.find(pos);
            if (it == m_pending.end()) {
                m_order.push_back(pos);
                m_pending.emplace(pos, std::move(job));
            } else {
                // Replace with the NEWER snapshot, and settle the promise the
                // superseded job was carrying so nothing waits on it forever.
                if (it->second.promise) {
                    it->second.promise->set_value(
                        ChunkSaveResult::Success(pos, it->second.payload.size()));
                }
                it->second = std::move(job);
            }
        }
        m_wake.notify_one();
    }

    std::future<ChunkSaveResult> AnvilChunkStorage::SaveChunkAsync(const Chunk& chunk) {
        auto promise = std::make_shared<std::promise<ChunkSaveResult>>();
        auto future  = promise->get_future();

        if (m_closed.load()) {
            promise->set_value(ChunkSaveResult::Failure(chunk.pos, "chunk storage is closed"));
            return future;
        }

        // Cooldown check before the expensive part, so a chunk being edited
        // rapidly costs nothing rather than being serialised and thrown away.
        //
        // LOCK ORDER, everywhere in this file: m_mutex before m_statsMutex,
        // never the reverse. The stats update below deliberately happens after
        // m_mutex is released rather than nested inside it.
        if (WithinWriteCooldown(chunk.pos)) {
            {
                std::lock_guard<std::mutex> s(m_statsMutex);
                ++m_stats.chunksSkipped;
            }
            promise->set_value(ChunkSaveResult::Success(chunk.pos, 0));
            return future;
        }

        std::vector<uint8_t> payload;
        std::string error;
        if (!Encode(chunk, payload, error)) {
            promise->set_value(ChunkSaveResult::Failure(chunk.pos, error));
            std::lock_guard<std::mutex> s(m_statsMutex);
            ++m_stats.saveFailures;
            return future;
        }

        Enqueue(Job{chunk.pos, std::move(payload), promise});
        return future;
    }

    bool AnvilChunkStorage::WithinWriteCooldown(Math::ChunkPos pos) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_lastWrite.find(pos);
        return it != m_lastWrite.end() &&
               std::chrono::steady_clock::now() - it->second < kWriteCooldown &&
               m_pending.find(pos) == m_pending.end();
    }

    std::future<ChunkSaveResult> AnvilChunkStorage::SaveEvictedAsync(std::shared_ptr<const Chunk> chunk) {
        auto promise = std::make_shared<std::promise<ChunkSaveResult>>();
        auto future  = promise->get_future();
        if (!chunk) {
            promise->set_value(ChunkSaveResult::Failure(Math::ChunkPos{0, 0}, "null chunk"));
            return future;
        }
        if (m_closed.load()) {
            promise->set_value(ChunkSaveResult::Failure(chunk->pos, "chunk storage is closed"));
            return future;
        }
        PROFILE_ZONE_N("SaveEvicted");
        // No write cooldown here, unlike SaveChunkAsync: a resident chunk
        // skipped now is saved by a later autosave, but an EVICTED chunk has
        // no later — skipping it would drop every edit made since its last
        // write (load, place a block, walk away within 10 s: gone).
        // No Encode here — that is the whole point. The IO thread does it.
        const Math::ChunkPos pos = chunk->pos;
        Enqueue(Job{pos, {}, promise, std::move(chunk)});
        return future;
    }

    bool AnvilChunkStorage::TryReadPending(Math::ChunkPos pos, std::vector<uint8_t>& nbtOut,
                                           std::string& error) {
        std::shared_ptr<const Chunk> chunk;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_pending.find(pos);
            if (it == m_pending.end()) return false;
            chunk = it->second.chunk;
        }
        // A snapshot job (payload already encoded, no chunk) belongs to a
        // chunk that is still resident, so nobody loads it; fall through to
        // the region file as before.
        if (!chunk) return false;
        return SerialiseChunk(*chunk, m_dataVersion,
                              m_gameTime.load(std::memory_order_relaxed), nbtOut, error);
    }

    ChunkSaveResult AnvilChunkStorage::SaveChunk(const Chunk& chunk) {
        // Blocking, and it reports what actually happened. The old saver
        // returned Success as soon as a chunk was queued, and
        // ChunkCache::SaveAllDirty cleared the dirty flag on that — so a
        // failed write looked identical to a successful one.
        return SaveChunkAsync(chunk).get();
    }

    std::vector<ChunkSaveResult> AnvilChunkStorage::SaveChunks(
        const std::vector<std::shared_ptr<const Chunk>>& chunks) {
        std::vector<std::future<ChunkSaveResult>> futures;
        futures.reserve(chunks.size());
        for (const auto& chunk : chunks) {
            if (!chunk) continue;
            futures.push_back(SaveChunkAsync(*chunk));
        }
        std::vector<ChunkSaveResult> results;
        results.reserve(futures.size());
        for (auto& f : futures) results.push_back(f.get());
        return results;
    }

    void AnvilChunkStorage::IoThreadMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return !m_order.empty() || m_draining.load(); });

                if (m_order.empty()) {
                    // Drain, do not discard. The old worker looped on
                    // `while (!m_shutdownRequested)` and its queue reader
                    // returned early on the same flag, so shutdown threw away
                    // every queued chunk.
                    if (m_draining.load()) break;
                    continue;
                }
                const Math::ChunkPos pos = m_order.front();
                m_order.pop_front();
                auto it = m_pending.find(pos);
                if (it == m_pending.end()) continue;
                job = std::move(it->second);
                m_pending.erase(it);
            }

            std::string error;
            if (job.payload.empty() && job.chunk) {
                // Deferred eviction save: serialise + compress here, off the
                // server thread. The chunk left the cache when it was queued,
                // so nothing has written to it since.
                if (!Encode(*job.chunk, job.payload, error)) {
                    {
                        std::lock_guard<std::mutex> s(m_statsMutex);
                        ++m_stats.saveFailures;
                    }
                    Log::Error("[Anvil] failed to encode evicted chunk (%d,%d): %s",
                               job.pos.x, job.pos.z, error.c_str());
                    if (job.promise) job.promise->set_value(ChunkSaveResult::Failure(job.pos, error));
                    continue;
                }
                job.chunk.reset();
            }
            const bool ok = m_io->WriteChunkNbt(m_dimension, RegionKind::Chunks,
                                                job.pos, job.payload, error);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastWrite[job.pos] = std::chrono::steady_clock::now();
            }
            {
                std::lock_guard<std::mutex> s(m_statsMutex);
                if (ok) { ++m_stats.chunksSaved; m_stats.bytesWritten += job.payload.size(); }
                else    { ++m_stats.saveFailures; }
            }
            if (!ok) {
                Log::Error("[Anvil] failed to write chunk (%d,%d): %s",
                           job.pos.x, job.pos.z, error.c_str());
            }
            if (job.promise) {
                job.promise->set_value(ok ? ChunkSaveResult::Success(job.pos, job.payload.size())
                                          : ChunkSaveResult::Failure(job.pos, error));
            }
        }
    }

    void AnvilChunkStorage::FlushAndJoin() {
        if (!m_thread.joinable()) { m_closed.store(true); return; }
        m_draining.store(true);
        m_wake.notify_all();
        m_thread.join();
        m_closed.store(true);
        m_io->CloseAll();          // flushes and pads every open region file

        std::lock_guard<std::mutex> s(m_statsMutex);
        Log::Info("[Anvil] chunk storage stopped: %zu saved, %zu failed, %zu skipped (cooldown), %.1f MB",
                  m_stats.chunksSaved, m_stats.saveFailures, m_stats.chunksSkipped,
                  m_stats.bytesWritten / (1024.0 * 1024.0));
    }

    void AnvilChunkStorage::Shutdown() { FlushAndJoin(); }

    IChunkSaver::SaverStats AnvilChunkStorage::GetStats() const {
        // m_mutex first, then m_statsMutex — see the note in SaveChunkAsync.
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pending = m_order.size();
        }
        std::lock_guard<std::mutex> s(m_statsMutex);
        SaverStats out = m_stats;
        out.pendingChunks = pending;
        return out;
    }

    // ── AnvilChunkLoader ────────────────────────────────────────────────────

    bool AnvilChunkLoader::LoadChunk(Math::ChunkPos pos, Chunk& out, std::string& error) {
        std::vector<uint8_t> nbt;
        if (auto storage = m_pending.lock()) {
            // Evicted a moment ago and not yet on disk: read what WILL be
            // written, not what the region file still says.
            if (storage->TryReadPending(pos, nbt, error)) {
                return DeserialiseChunk(nbt, pos, out, error, ReadMode::Strict,
                                        m_gameTime.load(std::memory_order_relaxed));
            }
            if (!error.empty()) return false;
        }
        if (!m_io->ReadChunkNbt(m_dimension, RegionKind::Chunks, pos, nbt, error)) {
            return false;               // absent (error empty) or a real failure
        }
        return DeserialiseChunk(nbt, pos, out, error, ReadMode::Strict,
                                m_gameTime.load(std::memory_order_relaxed));
    }

} // namespace Game::Anvil
