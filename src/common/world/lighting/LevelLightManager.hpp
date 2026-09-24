// File: src/common/world/lighting/LevelLightManager.hpp
//
// One level's light engine on the server: MC's ThreadedLevelLightEngine
// reduced to what this engine needs, run on the SERVER THREAD (MC's light
// thread reads chunk block states while the main thread writes them; a C++
// port cannot, so the light work goes where the block writes are, batched
// once per tick, and the per-chunk initial lighting — the expensive part —
// stays on the chunk workers, see Lighting::LightChunk).
//
//   * Chunks take part once registered: AddChunk (a chunk just made
//     resident, IntegratedServer::ProcessAsyncChunkResults) or lazily the
//     first time propagation reaches a lit chunk the cache holds (a chunk a
//     blocking load brought in). Registration seeds border increases against
//     the registered neighbours, both ways.
//   * OnBlockChanged is LevelChunk.setBlockState's light half: the sky source
//     column update and checkBlock, when the two states light differently.
//   * RunUpdates drains everything (MC runLightUpdates) holding each touched
//     chunk's exclusive content lock, so the saver — which may serialise a
//     resident chunk on another thread under the shared lock — never reads a
//     half-written layer.
//   * TakeAffectedSections hands the server the sections to re-send
//     (LightUpdateS2C), re-save and re-stamp.
//
// OBEY_LIGHT_ENGINE=0 disables the whole engine (A/B of its server cost):
// chunks still get their initial light, but nothing reconciles or updates it.
#pragma once

#include "common/world/lighting/LightEngine.hpp"
#include "common/world/math/WorldMath.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {
    class Chunk;
}

namespace Game::Lighting {

    class LevelLightManager final : public LightChunkGetter {
    public:
        // Cache-only lookup of a resident chunk (never loads).
        using ChunkLookup = std::function<std::shared_ptr<Chunk>(Math::ChunkPos)>;

        LevelLightManager(bool hasSkyLight, ChunkLookup lookup);
        ~LevelLightManager() override;

        // OBEY_LIGHT_ENGINE (read once): false disables registration and updates.
        static bool EngineEnabled();

        bool HasSkyLight() const { return m_engine.HasSkyLight(); }
        void SetHasSkyLight(bool hasSkyLight);

        // ── Chunk registry (server thread) ──────────────────────────────────
        void AddChunk(const std::shared_ptr<Chunk>& chunk);
        void RemoveChunk(Math::ChunkPos pos);
        bool IsRegistered(Math::ChunkPos pos) const;
        // Thread-safe: a cache eviction on any thread. Drained by RunUpdates.
        void NoteEvicted(Math::ChunkPos pos, const std::shared_ptr<Chunk>& chunk);

        // ── Block changes (server thread) ────────────────────────────────────
        // After the chunk holds `newState` at (x, y, z).
        void OnBlockChanged(Chunk& chunk, int x, int y, int z, BlockState oldState, BlockState newState);

        // ── Tick (server thread) ─────────────────────────────────────────────
        bool HasWork() const;
        // Drain all queued work. Returns the number of queue entries processed.
        int  RunUpdates();

        // Sections (SectionKey) whose light changed or whose mesh reads a
        // changed cell, since the last call. Registered chunks only.
        std::vector<int64_t> TakeAffectedSections();
        // Forget queued affected sections of one chunk — its packet has not
        // gone out yet and will carry the final light anyway.
        // Returns whether any were queued.
        bool DropAffectedSectionsOf(Math::ChunkPos pos);

        // ── Queries (server thread) ──────────────────────────────────────────
        // MC Level.getBrightness(layer, pos). Unregistered: sky 15, block 0.
        int GetBrightness(LightLayer layer, int x, int y, int z);
        // MC LevelLightEngine.getRawBrightness(pos, skyDarken).
        int GetRawBrightness(int x, int y, int z, int skyDarken);

        // LightChunkGetter.
        Chunk* GetChunkForLighting(int chunkX, int chunkZ) override;

        struct Stats {
            size_t registered = 0;
            uint64_t runs = 0;
            uint64_t entries = 0;
            double   lastRunMs = 0.0;
            double   maxRunMs = 0.0;
        };
        Stats GetStats() const;

    private:
        struct Entry {
            std::shared_ptr<Chunk> chunk;
        };
        static int64_t Key(int cx, int cz) {
            return (static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz));
        }
        Chunk* Lookup(int cx, int cz);
        void   Register(const std::shared_ptr<Chunk>& chunk);
        void   ReconcileBorders(Chunk& chunk);
        void   ReconcileFace(Chunk& a, Chunk& b, Direction aToB);
        void   DrainEvictions();
        void   ReleaseRunLocks();

        LevelLightEngine m_engine;
        ChunkLookup      m_lookup;
        std::unordered_map<int64_t, Entry> m_chunks;
        std::unordered_map<int64_t, std::vector<int64_t>> m_deferredChecks;   // chunk key -> block positions
        std::unordered_set<int64_t> m_missing;          // lookups that found nothing, this run
        SectionSet m_affected;

        bool m_inRun = false;
        std::vector<std::unique_lock<std::shared_mutex>> m_runLocks;
        std::unordered_set<int64_t> m_lockedThisRun;

        std::mutex m_evictMutex;
        std::vector<std::pair<Math::ChunkPos, std::weak_ptr<Chunk>>> m_evicted;

        Stats m_stats;
    };

} // namespace Game::Lighting
