// File: src/common/world/lighting/LevelLightManager.cpp
#include "common/world/lighting/LevelLightManager.hpp"

#include "common/world/lighting/BlockLightProperties.hpp"
#include "common/world/lighting/LightStateAccess.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace Game::Lighting {

    bool LevelLightManager::EngineEnabled() {
        static const bool enabled = [] {
            const char* v = std::getenv("OBEY_LIGHT_ENGINE");
            const bool on = !(v && std::strcmp(v, "0") == 0);
            if (!on) Log::Info("[Light] OBEY_LIGHT_ENGINE=0: level light engine disabled (initial chunk light only)");
            return on;
        }();
        return enabled;
    }

    LevelLightManager::LevelLightManager(bool hasSkyLight, ChunkLookup lookup)
        : m_engine(this, hasSkyLight), m_lookup(std::move(lookup)) {
        m_engine.SetAffectedSink(&m_affected);
    }

    LevelLightManager::~LevelLightManager() {
        ReleaseRunLocks();
    }

    void LevelLightManager::SetHasSkyLight(bool hasSkyLight) {
        m_engine.SetHasSkyLight(hasSkyLight);
    }

    // ── Registry ─────────────────────────────────────────────────────────────

    Chunk* LevelLightManager::Lookup(int cx, int cz) {
        const int64_t key = Key(cx, cz);
        if (auto it = m_chunks.find(key); it != m_chunks.end()) return it->second.chunk.get();
        if (!EngineEnabled() || !m_lookup) return nullptr;
        if (m_missing.count(key)) return nullptr;
        // A resident, lit chunk nobody registered (a blocking load): take it.
        std::shared_ptr<Chunk> chunk = m_lookup(Math::ChunkPos{cx, cz});
        if (chunk && chunk->light.lightCorrect && chunk->pos.x == cx && chunk->pos.z == cz) {
            Register(chunk);
            return chunk.get();
        }
        m_missing.insert(key);
        return nullptr;
    }

    Chunk* LevelLightManager::GetChunkForLighting(int chunkX, int chunkZ) {
        Chunk* chunk = Lookup(chunkX, chunkZ);
        if (chunk && m_inRun) {
            const int64_t key = Key(chunkX, chunkZ);
            if (m_lockedThisRun.insert(key).second) {
                m_runLocks.push_back(chunk->LockExclusive());
            }
        }
        return chunk;
    }

    void LevelLightManager::ReleaseRunLocks() {
        m_runLocks.clear();          // unique_locks release on destruction
        m_lockedThisRun.clear();
    }

    void LevelLightManager::AddChunk(const std::shared_ptr<Chunk>& chunk) {
        if (!chunk || !EngineEnabled()) return;
        if (!chunk->light.lightCorrect) {
            // Every chunk the server makes resident was lit on its worker
            // (MyTerrainGenerator::ConvertLibChunk / ChunkProvider's load);
            // one that was not is lit here rather than left dark.
            LightChunk(*chunk, HasSkyLight());
        }
        Register(chunk);
    }

    void LevelLightManager::Register(const std::shared_ptr<Chunk>& chunk) {
        const int64_t key = Key(chunk->pos.x, chunk->pos.z);
        m_chunks[key] = Entry{chunk};
        m_missing.erase(key);
        ReconcileBorders(*chunk);
        if (auto it = m_deferredChecks.find(key); it != m_deferredChecks.end()) {
            for (int64_t pos : it->second) {
                m_engine.CheckBlock(Pos::X(pos), Pos::Y(pos), Pos::Z(pos));
            }
            m_deferredChecks.erase(it);
        }
    }

    void LevelLightManager::RemoveChunk(Math::ChunkPos pos) {
        const int64_t key = Key(pos.x, pos.z);
        m_chunks.erase(key);
        m_deferredChecks.erase(key);
        m_lockedThisRun.erase(key);
    }

    bool LevelLightManager::IsRegistered(Math::ChunkPos pos) const {
        return m_chunks.count(Key(pos.x, pos.z)) != 0;
    }

    void LevelLightManager::NoteEvicted(Math::ChunkPos pos, const std::shared_ptr<Chunk>& chunk) {
        std::lock_guard<std::mutex> lock(m_evictMutex);
        m_evicted.emplace_back(pos, chunk);
    }

    void LevelLightManager::DrainEvictions() {
        std::vector<std::pair<Math::ChunkPos, std::weak_ptr<Chunk>>> evicted;
        {
            std::lock_guard<std::mutex> lock(m_evictMutex);
            evicted.swap(m_evicted);
        }
        for (const auto& [pos, weak] : evicted) {
            auto it = m_chunks.find(Key(pos.x, pos.z));
            if (it != m_chunks.end() && it->second.chunk == weak.lock()) {
                RemoveChunk(pos);
            }
        }
    }

    // ── Border reconciliation ────────────────────────────────────────────────
    //
    // The chunk's light came from a pass that could not see its neighbours:
    // a lower bound of the true light on both sides of every border. Seeding
    // an increase from each border cell that can still raise the cell across
    // the boundary (maxPossibleNewToLevel > toLevel, the engine's own first
    // test) and letting propagateIncreases run reaches exactly the fixed point
    // a whole-world lighting would.

    void LevelLightManager::ReconcileBorders(Chunk& chunk) {
        const int cx = chunk.pos.x, cz = chunk.pos.z;
        struct N { int dx, dz; Direction dir; };
        static constexpr N kNeighbours[4] = {
            { 0, -1, Direction::North }, { 0, 1, Direction::South },
            { -1, 0, Direction::West },  { 1, 0, Direction::East },
        };
        for (const N& n : kNeighbours) {
            auto it = m_chunks.find(Key(cx + n.dx, cz + n.dz));
            if (it == m_chunks.end() || !it->second.chunk) continue;
            ReconcileFace(chunk, *it->second.chunk, n.dir);
        }
    }

    void LevelLightManager::ReconcileFace(Chunk& a, Chunk& b, Direction aToB) {
        const bool alongX = aToB == Direction::West || aToB == Direction::East;
        // Local coordinate of the border column on each side.
        const int aEdge = (aToB == Direction::East || aToB == Direction::South) ? 15 : 0;
        const int bEdge = 15 - aEdge;
        const Direction bToA = Opposite(aToB);
        const int aBaseX = a.pos.x * 16, aBaseZ = a.pos.z * 16;
        const int bBaseX = b.pos.x * 16, bBaseZ = b.pos.z * 16;

        for (int layerIdx = 0; layerIdx < 2; ++layerIdx) {
            const LightLayer layer = layerIdx == 0 ? LightLayer::Sky : LightLayer::Block;
            if (layer == LightLayer::Sky && !HasSkyLight()) continue;
            LayerLightEngine& engine = layer == LightLayer::Sky
                ? static_cast<LayerLightEngine&>(m_engine.Sky())
                : static_cast<LayerLightEngine&>(m_engine.Block());
            for (int li = 0; li < kLightSectionCount; ++li) {
                const DataLayer& la = a.light.Layer(layer, li);
                const DataLayer& lb = b.light.Layer(layer, li);
                if (la.IsDefinitelyHomogeneous() && lb.IsDefinitelyHomogeneous() &&
                    std::abs(la.DefaultValue() - lb.DefaultValue()) <= 1) {
                    continue;                                   // nothing can cross
                }
                const int baseY = (kMinLightSectionY + li) * 16;
                for (int ly = 0; ly < 16; ++ly) {
                    for (int t = 0; t < 16; ++t) {
                        const int ax = alongX ? aEdge : t, az = alongX ? t : aEdge;
                        const int bx = alongX ? bEdge : t, bz = alongX ? t : bEdge;
                        const int va = la.Get(ax, ly, az);
                        const int vb = lb.Get(bx, ly, bz);
                        const int y = baseY + ly;
                        if (va - 1 > vb) {
                            const BlockState s = BlockState::FromRawId(StateIdAt(a, ax, y, az));
                            engine.EnqueueIncrease(Pos::Pack(aBaseX + ax, y, aBaseZ + az),
                                QueueEntry::IncreaseOnlyOneDirection(va, BlockLightProperties::IsEmptyShape(s), aToB));
                        } else if (vb - 1 > va) {
                            const BlockState s = BlockState::FromRawId(StateIdAt(b, bx, y, bz));
                            engine.EnqueueIncrease(Pos::Pack(bBaseX + bx, y, bBaseZ + bz),
                                QueueEntry::IncreaseOnlyOneDirection(vb, BlockLightProperties::IsEmptyShape(s), bToA));
                        }
                    }
                }
            }
        }
    }

    // ── Block changes ────────────────────────────────────────────────────────

    void LevelLightManager::OnBlockChanged(Chunk& chunk, int x, int y, int z,
                                           BlockState oldState, BlockState newState) {
        if (!BlockLightProperties::HasDifferentLightProperties(oldState, newState)) return;
        // LevelChunk.setBlockState: the sky source column first, always (it
        // is this chunk's own data and must track its blocks)...
        chunk.light.skySources.Update(chunk, x & 15, y, z & 15);
        if (!EngineEnabled()) return;
        // ...then checkBlock.
        const int cx = x >> 4, cz = z >> 4;
        if (Lookup(cx, cz)) {
            m_engine.CheckBlock(x, y, z);
        } else {
            m_deferredChecks[Key(cx, cz)].push_back(Pos::Pack(x, y, z));
        }
    }

    // ── Tick ─────────────────────────────────────────────────────────────────

    bool LevelLightManager::HasWork() const { return m_engine.HasLightWork(); }

    int LevelLightManager::RunUpdates() {
        DrainEvictions();
        if (!EngineEnabled() || !m_engine.HasLightWork()) {
            m_missing.clear();
            return 0;
        }
        PROFILE_ZONE_N("Light.RunUpdates");
        const auto t0 = std::chrono::steady_clock::now();
        m_inRun = true;
        const int count = m_engine.RunLightUpdates();
        m_inRun = false;
        ReleaseRunLocks();
        m_missing.clear();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++m_stats.runs;
        m_stats.entries += static_cast<uint64_t>(count);
        m_stats.lastRunMs = ms;
        m_stats.maxRunMs = std::max(m_stats.maxRunMs, ms);
        PROFILE_PLOT("Light/Entries", static_cast<int64_t>(count));
        if (ms > 20.0) {
            Log::Debug("[Light] run: %d queue entries in %.1f ms", count, ms);
        }
        return count;
    }

    std::vector<int64_t> LevelLightManager::TakeAffectedSections() {
        std::vector<int64_t> out;
        out.reserve(m_affected.size());
        for (int64_t key : m_affected) {
            if (m_chunks.count(Key(SectionKey::X(key), SectionKey::Z(key)))) out.push_back(key);
        }
        m_affected.clear();
        return out;
    }

    bool LevelLightManager::DropAffectedSectionsOf(Math::ChunkPos pos) {
        bool any = false;
        for (auto it = m_affected.begin(); it != m_affected.end();) {
            if (SectionKey::X(*it) == pos.x && SectionKey::Z(*it) == pos.z) {
                it = m_affected.erase(it);
                any = true;
            } else {
                ++it;
            }
        }
        return any;
    }

    // ── Queries ──────────────────────────────────────────────────────────────

    int LevelLightManager::GetBrightness(LightLayer layer, int x, int y, int z) {
        const Chunk* chunk = Lookup(x >> 4, z >> 4);
        if (!chunk) {
            // Not (yet) taking part: a resident chunk still answers from its
            // own initial light.
            std::shared_ptr<Chunk> resident = m_lookup ? m_lookup(Math::ChunkPos{x >> 4, z >> 4}) : nullptr;
            if (resident && resident->light.lightCorrect) {
                return Lighting::GetBrightness(resident.get(), layer, x, y, z, HasSkyLight());
            }
        }
        return Lighting::GetBrightness(chunk, layer, x, y, z, HasSkyLight());
    }

    int LevelLightManager::GetRawBrightness(int x, int y, int z, int skyDarken) {
        const int sky = GetBrightness(LightLayer::Sky, x, y, z) - skyDarken;
        const int block = GetBrightness(LightLayer::Block, x, y, z);
        return std::max(block, sky);
    }

    LevelLightManager::Stats LevelLightManager::GetStats() const {
        Stats s = m_stats;
        s.registered = m_chunks.size();
        return s;
    }

} // namespace Game::Lighting
