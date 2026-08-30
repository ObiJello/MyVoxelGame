// File: src/server/world/ChunkProvider.cpp
#include "ChunkProvider.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "storage/MinecraftChunkLoaderImpl.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "platform/GameDirectory.hpp"
#include <algorithm>

// ========================================================================
// TERRAIN GENERATION: Using custom terrain library
// ========================================================================
#include "MyTerrainGenerator.hpp"

// OLD TERRAIN SYSTEM (commented out - replaced by MyTerrainGenerator)
// #include "common/world/gen/ProceduralChunkGenerator.hpp"

namespace Game {

    // === CONSTRUCTION ===

    ChunkProvider::ChunkProvider(const ChunkProviderConfig& config)
        : m_config(config) {

        if (!m_config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, using defaults");
            m_config = ChunkProviderConfig{};
        }

        Log::Debug("ChunkProvider created");
    }

    ChunkProvider::~ChunkProvider() {
        Shutdown();
    }

    ChunkProvider::ChunkProvider(ChunkProvider&& other) noexcept {
        MoveFrom(std::move(other));
    }

    ChunkProvider& ChunkProvider::operator=(ChunkProvider&& other) noexcept {
        if (this != &other) {
            Shutdown();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    // === LIFECYCLE ===

    bool ChunkProvider::Initialize() {
        Log::Info("=== SIMPLIFIED CHUNKPROVIDER INITIALIZATION START ===");

        if (m_initialized) {
            Log::Warning("ChunkProvider already initialized");
            return true;
        }

        try {
            // Create core components
            Log::Info("Creating ChunkCache...");
            ChunkCacheConfig cacheConfig;
            // The LRU cap is a SAFETY NET, not the unload mechanism — that is
            // IntegratedServer::UnloadUnwatchedChunks, which drops exactly the
            // chunks no player's tracking view holds, the way MC's tickets do.
            // It used to be 5120 ("one view distance 32 plus margin"), which
            // is less than ONE player's view plus the previous area still
            // waiting for the sweep: measured 2026-08-29, a far teleport put
            // 5,120 chunks in the cache two seconds after arrival and the LRU
            // evicted ~800 freshly loaded, WATCHED chunks — whose results had
            // already been processed, so nothing ever re-requested them and
            // the player was left with holes in the world for the rest of the
            // session. Sized so several players at view distance 32 (3,725
            // chunks each) plus one stale area fit without ever evicting.
            cacheConfig.maxSize = 32768;
            m_chunkCache = std::make_unique<ChunkCache>(cacheConfig);
            Log::Info("ChunkCache created with size %zu", cacheConfig.maxSize);

            // ========================================================================
            // TERRAIN GENERATION: Using MyTerrainGenerator (custom library)
            // ========================================================================
            //
            // ChunkProviderConfig::dimension is the single source of truth and
            // is pushed down here, because the generator is constructed from
            // the nested GenerationConfig and cannot change dimension
            // afterwards. Warn loudly if the two disagree rather than silently
            // generating the wrong dimension's terrain into this provider.
            if (m_config.generationConfig.dimension != m_config.dimension) {
                if (m_config.generationConfig.dimension != "overworld") {
                    Log::Warning("generationConfig.dimension '%s' overridden by"
                                 " ChunkProviderConfig::dimension '%s' — set the"
                                 " provider's field, not the generator's",
                                 m_config.generationConfig.dimension.c_str(),
                                 m_config.dimension.c_str());
                }
                m_config.generationConfig.dimension = m_config.dimension;
            }

            Log::Info("Creating MyTerrainGenerator (custom terrain library) for dimension '%s'...",
                      m_config.generationConfig.dimension.c_str());
            if (!m_config.savePath.empty()) {
                std::string reason;
                if (auto root = Anvil::SaveRoot::Open(m_config.savePath, reason)) {
                    if (!std::getenv("OBEY_NO_LIB_DISK")) m_config.generationConfig.storagePath = root->Dimension(m_config.dimensionId).string();
                }
            }
            m_chunkGenerator = std::make_unique<MyTerrainGenerator>(m_config.generationConfig);
            Log::Info("MyTerrainGenerator created");

            // OLD TERRAIN SYSTEM (commented out - replaced by MyTerrainGenerator)
            // Log::Info("Creating ProceduralChunkGenerator...");
            // m_chunkGenerator = std::make_unique<ProceduralChunkGenerator>(m_config.generationConfig);
            // Log::Info("ProceduralChunkGenerator created");

            // Create Minecraft loader if world path specified
            if (!m_config.minecraftWorldPath.empty()) {
                Log::Info("Creating MinecraftChunkLoaderImpl for: %s", m_config.minecraftWorldPath.c_str());
                MinecraftLoaderConfig loaderConfig;
                loaderConfig.worldPath = m_config.minecraftWorldPath;
                loaderConfig.enableFallbackGeneration = m_config.enableFallbackGeneration;
                m_chunkLoader = std::make_unique<MinecraftChunkLoaderImpl>(loaderConfig);
                Log::Info("MinecraftChunkLoaderImpl created");
            }

            // Create chunk saver — unless this world is read-only.
            //
            // Not constructing one at all is the enforcement, deliberately:
            // guarding ChunkProvider::SaveChunk would not be enough, because
            // ChunkCache saves through its OWN saver reference on eviction and
            // in its destructor (ChunkCache.cpp:20/30/214), which never passes
            // back through here. With no saver to hand it, every one of those
            // paths becomes a no-op and there is no way to write a byte to the
            // player's real Minecraft world by accident.
            // Persistence is opt-in and gated on a SaveRoot, which is a
            // capability token: its factory refuses any path that is not
            // under obeycraft/saves, so a real Minecraft world cannot produce
            // one and therefore cannot be written to.
            //
            // NOT constructing a saver is the enforcement, deliberately.
            // Guarding ChunkProvider::SaveChunk would not be enough, because
            // ChunkCache saves through its OWN saver reference on eviction
            // (ChunkCache::EvictChunk) and in its destructor, neither of which
            // passes back through here. With no saver to hand it, every one of
            // those paths becomes a no-op.
            if (m_config.readOnly) {
                Log::Info("Chunk saver DISABLED — world opened read-only");
            } else if (m_config.savePath.empty()) {
                Log::Info("Chunk saver DISABLED — world has no save folder (regenerates from seed)");
            } else {
                std::string reason;
                auto root = Anvil::SaveRoot::Open(m_config.savePath, reason);
                if (!root) {
                    Log::Error("Chunk saver DISABLED — refusing to write to '%s': %s",
                               m_config.savePath.c_str(), reason.c_str());
                } else {
                    m_anvilIo = std::make_shared<Anvil::AnvilChunkIo>(*root);
                    auto storage = std::make_shared<Anvil::AnvilChunkStorage>(
                        m_anvilIo, m_config.dimensionId, Save::DataVersion());
                    m_chunkSaver   = storage;
                    m_anvilStorage = storage;
                    m_anvilLoader = std::make_unique<Anvil::AnvilChunkLoader>(
                        m_anvilIo, m_config.dimensionId);
                    Log::Info("Anvil chunk storage enabled: %s", root->Root().string().c_str());
                }
            }

            // Create dirty tracker
            Log::Info("Creating DirtyTracker...");
            m_dirtyTracker = std::make_unique<DirtyTracker>(m_config.dirtyConfig);
            Log::Info("DirtyTracker created");

            // Initialize components
            if (m_chunkLoader && !m_chunkLoader->Initialize()) {
                Log::Warning("Failed to initialize chunk loader, will use generation only");
                m_chunkLoader.reset();
            }

            if (!m_chunkGenerator->Initialize()) {
                Log::Error("Failed to initialize chunk generator - this is critical!");
                return false;
            }

            if (m_chunkSaver && !m_chunkSaver->Initialize()) {
                Log::Error("Failed to initialize chunk saver");
                return false;
            }

            if (!m_dirtyTracker->Initialize()) {
                Log::Error("Failed to initialize dirty tracker");
                return false;
            }

            // Reset statistics
            ResetProviderStats();

            // MUST come before SetupComponentDependencies: that function's
            // first statement is `if (!m_initialized) return;`, so calling it
            // any earlier made it a silent no-op — and it is what hands the
            // chunk cache its saver.
            //
            // That is exactly what happened. ChunkCache::m_chunkSaver stayed
            // null, so SaveAllDirty returned at its own null check and
            // EvictChunk's `wasDirty && m_chunkSaver` never fired: every chunk
            // write in the engine was quietly discarded. It went unnoticed
            // because the old saver wrote a format nothing read, to a
            // CWD-relative path nobody looked at.
            m_initialized = true;

            // Set up component dependencies
            SetupComponentDependencies();
            ConfigureComponents();

            Log::Info("✓ ChunkProvider initialized successfully");
            Log::Info("=== SIMPLIFIED CHUNKPROVIDER INITIALIZATION COMPLETE ===");

            return true;

        } catch (const std::exception& e) {
            Log::Error("ChunkProvider initialization failed with exception: %s", e.what());
            Shutdown();
            return false;
        }
    }

    void ChunkProvider::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down ChunkProvider...");

        // ORDER MATTERS, and it used to be wrong.
        //
        // 1. Queue every dirty chunk.
        // 2. DRAIN and join the writer, so the queued chunks actually reach
        //    disk. The old code destroyed the saver here, whose worker loop
        //    exited on a flag and discarded whatever was still queued.
        // 3. Only then destroy the cache. ~ChunkCache calls SaveAllDirty
        //    again; with the saver already closed those calls now fail
        //    loudly instead of being silently dropped, and with a real
        //    shared_ptr they are not a use-after-free either.
        if (m_chunkSaver && m_chunkCache) {
            Log::Info("Saving all dirty chunks before shutdown...");
            m_chunkCache->SaveAllDirty();
            m_chunkSaver->FlushAndJoin();
        }

        // Shutdown components in reverse order
        if (m_dirtyTracker) {
            m_dirtyTracker->Shutdown();
            m_dirtyTracker.reset();
        }

        if (m_chunkCache) {
            m_chunkCache.reset();
        }

        if (m_chunkSaver) {
            m_chunkSaver->Shutdown();
            m_chunkSaver.reset();
        }
        // Released after m_chunkSaver: it is the SAME object, and dropping the
        // concrete alias first would leave m_chunkSaver holding the last
        // reference to something we then Shutdown() through a base pointer.
        m_anvilStorage.reset();
        m_anvilLoader.reset();
        m_anvilIo.reset();

        if (m_chunkGenerator) {
            m_chunkGenerator->Shutdown();
            m_chunkGenerator.reset();
        }

        if (m_chunkLoader) {
            m_chunkLoader->Shutdown();
            m_chunkLoader.reset();
        }

        m_initialized = false;
        Log::Info("ChunkProvider shutdown complete");
    }

    // === CORE CHUNK OPERATIONS ===

    std::shared_ptr<Chunk> ChunkProvider::GetChunk(Math::ChunkPos position) {
        PROFILE_ZONE;
        if (!m_initialized) {
            Log::Error("ChunkProvider::GetChunk: Not initialized");
            return nullptr;
        }

        if (!ValidateChunkPosition(position)) {
            Log::Error("ChunkProvider::GetChunk: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        // Always check cache first
        std::shared_ptr<Chunk> chunk = TryLoadFromCache(position);
        if (chunk) {
            return chunk;
        }

        // Load from disk or generate
        bool wasGenerated = false;
        chunk = LoadChunkInternal(position, wasGenerated);

        if (chunk) {
            chunk = CompleteChunkLoad(chunk, wasGenerated);
            //Log::Info("Successfully loaded/generated chunk (%d, %d)", position.x, position.z);
        } else {
            if (m_chunkGenerator && m_chunkGenerator->IsAbortRequested()) {
                Log::Debug("Chunk (%d, %d) cancelled (shutting down)", position.x, position.z);
            } else {
                Log::Warning("Failed to load/generate chunk (%d, %d)", position.x, position.z);
            }
        }

        return chunk;
    }


    // ── Per-thread chunk memo (MC ServerChunkCache's lastChunk[4]) ─────────
    //
    // ChunkCache::Get is the single hottest call in the engine: a process-wide
    // mutex, two hash lookups, an LRU list erase + push_front (one free, one
    // alloc) and a shared_ptr returned by value — two atomic refcount ops, the
    // second when the caller drops it a line later. Measured 79.1 ns, versus
    // 5.2 ns through a memo. An explosion does 34,019 of them; 94.5% target the
    // same chunk as the previous call, and a radius-4 blast touches four
    // columns in total.
    //
    // MC keeps a 4-entry direct-mapped array with a plain key compare and no
    // lock (ServerChunkCache.lastChunkPos/lastChunk). This is that, with two
    // differences forced by this engine:
    //
    //   * thread_local, not a member. MC's chunk source is main-thread-only;
    //     ours is read from the server tick, the worker pool and the client.
    //     A shared memo would be a data race on the shared_ptr control block.
    //   * validated against ChunkCache::Generation(). MC invalidates by hand
    //     at each mutation site; a counter cannot be forgotten at a site nobody
    //     thought of, which matters more here because eviction is automatic.
    //
    // Caching a NULL is correct and deliberate — "this column is not loaded"
    // is exactly the answer a blast reaching past the loaded region needs, and
    // the generation guard retires it the instant a load lands.
    namespace {
        struct ChunkMemo {
            const void*            owner = nullptr;   // which ChunkProvider
            uint64_t               putGeneration = 0;
            uint64_t               removeGeneration = 0;
            static constexpr int kSlots = 64;
            Math::ChunkPos         pos[kSlots]{};
            std::shared_ptr<Chunk> chunk[kSlots];
            bool                   valid[kSlots]{};

            void Reset(const void* newOwner, uint64_t putGen, uint64_t removeGen) {
                owner = newOwner;
                putGeneration = putGen;
                removeGeneration = removeGen;
                for (int i = 0; i < kSlots; ++i) {
                    valid[i] = false;
                    chunk[i].reset();   // drop the strong ref promptly
                }
            }
            // A chunk APPEARED somewhere: only the slots that answered "not
            // resident" can be wrong now. The resident ones stay — that is
            // what keeps nine physics workers off the cache mutex while the
            // chunk pipeline streams terrain in around them.
            void ForgetAbsent(uint64_t putGen) {
                putGeneration = putGen;
                for (int i = 0; i < kSlots; ++i) {
                    if (valid[i] && !chunk[i]) valid[i] = false;
                }
            }
        };
        // One memo PER OWNER per thread. The tick pool's workers serve the
        // server's provider and the client's (and the other dimensions') in
        // interleaved batches, and a single memo keyed on "owner changed →
        // reset" was wiped at every switch — measured as half the falling-
        // block physics time sitting in ChunkCache::Get's mutex even with
        // nothing streaming. Three is enough for the providers a thread can
        // alternate between in one tick; a fourth evicts round-robin.
        struct ChunkMemoSet {
            static constexpr int kOwners = 3;
            ChunkMemo memos[kOwners];
            int       next = 0;
            ChunkMemo& For(const void* owner) {
                for (ChunkMemo& m : memos) if (m.owner == owner) return m;
                ChunkMemo& m = memos[next];
                next = (next + 1) % kOwners;
                m.owner = nullptr;   // forces the Reset in the caller
                return m;
            }
        };
        thread_local ChunkMemoSet t_chunkMemos;

        // Direct-mapped on the low three bits of each axis: the chunks around
        // any boundary crossing land in distinct slots, and — the reason for
        // 64 rather than 4 — a worker servicing entities scattered over a
        // blast-spread pile keeps its recent columns resident instead of
        // falling through to ChunkCache::Get, whose mutex eight threads were
        // measured spending a fifth of the server thread waiting on.
        inline int MemoSlot(Math::ChunkPos p) {
            return ((p.x & 7) << 3) | (p.z & 7);
        }
    } // namespace

    // Borrowed, non-owning. The returned reference is the memo's own strong
    // reference, so it is valid only until the next chunk fetch ON THIS THREAD:
    // MemoSlot is direct-mapped on the low bit of each axis, so a read one
    // chunk away in x or z reuses the slot and may drop the last strong ref.
    // Callers must finish with the pointer before fetching another chunk.
    //
    // This exists because the owning form copied a shared_ptr on EVERY block
    // read — two atomic read-modify-writes on the same cache line, ~4 ns, on a
    // path the explosion exposure raycast walks billions of times.
    const std::shared_ptr<Chunk>&
    ChunkProvider::GetCachedChunkRef(Math::ChunkPos position) const {
        static const std::shared_ptr<Chunk> kNull;
        if (!m_chunkCache) return kNull;

        const uint64_t putGen = m_chunkCache->PutGeneration();
        const uint64_t remGen = m_chunkCache->RemoveGeneration();
        ChunkMemo& memo = t_chunkMemos.For(this);

        // The owner check is not optional: one thread serves three dimensions,
        // and without it a Nether read could be answered with an Overworld
        // chunk at the same coordinates. A removal invalidates everything (a
        // slot may hold the chunk that just left); a load invalidates only the
        // "absent" slots — see ForgetAbsent.
        if (memo.owner != this || memo.removeGeneration != remGen) {
            memo.Reset(this, putGen, remGen);
        } else if (memo.putGeneration != putGen) {
            memo.ForgetAbsent(putGen);
        }

        const int slot = MemoSlot(position);
        if (memo.valid[slot] && memo.pos[slot] == position) {
            return memo.chunk[slot];
        }

        // Move-assign straight into the slot. libc++'s operator= is
        // copy-and-swap, so the new value is installed before the previous
        // occupant is released — no temporary, and two fewer refcount ops on
        // the miss path than a `chunk` local costs.
        memo.pos[slot]   = position;
        memo.chunk[slot] = m_chunkCache->Get(position);
        memo.valid[slot] = true;
        return memo.chunk[slot];
    }

    // Owning form, for callers whose use outlives the next fetch on this thread.
    std::shared_ptr<Chunk> ChunkProvider::GetCachedChunk(Math::ChunkPos position) const {
        return GetCachedChunkRef(position);
    }

    std::shared_ptr<Chunk> ChunkProvider::GetLoadedChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) {
            return nullptr;
        }
        return GetCachedChunk(position);
    }

    bool ChunkProvider::IsChunkLoaded(Math::ChunkPos position) const {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        return m_chunkCache->Contains(position);
    }

    bool ChunkProvider::UnloadChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache) {
            return false;
        }

        return m_chunkCache->Remove(position);
    }

    // === BLOCK ACCESS ===

    BlockID ChunkProvider::GetBlock(int worldX, int worldY, int worldZ) const {
        if (!ValidateWorldPosition(worldX, worldY, worldZ)) {
            return BlockID::Air;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        const Chunk* chunk = GetCachedChunkRef(chunkPos).get();
        if (!chunk) {
            return BlockID::Air;
        }

        return chunk->GetBlock(localX, localY, localZ);
    }

    BlockState ChunkProvider::GetBlockState(int worldX, int worldY, int worldZ) const {
        if (!ValidateWorldPosition(worldX, worldY, worldZ)) {
            return BlockState{};
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        const Chunk* chunk = GetCachedChunkRef(chunkPos).get();
        if (!chunk) {
            return BlockState{};
        }

        return chunk->StateAt(localX, localY, localZ);
    }

    // Whole-box collision mask, walked one (chunk column, section) tile at a
    // time instead of one cell at a time. The explosion exposure raycast reads
    // the same ~9k-cell neighbourhood a thousand times per blast; building this
    // once turns each of those reads into a bit test.
    //
    // Two cases are NOT the same and must not be conflated:
    //
    //   outside the build range  -> bit stays CLEAR. There is nothing there and
    //                               nothing can arrive; air has no collision.
    //   column/section absent    -> bit is SET, meaning UNKNOWN. Chunks are
    //                               installed from worker threads and that does
    //                               not bump the block-write epoch, so a column
    //                               that streams in mid-blast would otherwise
    //                               keep answering "no collision" from this
    //                               snapshot and let rays through a wall.
    //
    // A set bit only costs the read the uncached path would have done anyway,
    // and while the column really is absent the accessor returns air — the same
    // answer a clear bit would have given. This is where the fast path
    // deliberately differs from the generic IBlockAccess default, which cannot
    // tell "absent" from "air" because it only has GetBlockState.
    bool ChunkProvider::IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                                       bool absentIsAir) const {
        if (min.x > max.x || min.y > max.y || min.z > max.z) return true;
        // Section index range. Y outside the build range holds nothing, so it
        // is air for this purpose; the clamp keeps the loop inside [0, 24).
        int s0, s1, unusedY;
        Math::WorldCoordinates::WorldYToSectionCoords(min.y, s0, unusedY);
        Math::WorldCoordinates::WorldYToSectionCoords(max.y, s1, unusedY);
        s0 = std::max(s0, 0);
        s1 = std::min(s1, Math::SECTIONS_PER_CHUNK - 1);
        if (s0 > s1) return true;

        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                const Chunk* chunk = GetCachedChunkRef(Math::ChunkPos{cx, cz}).get();
                if (!chunk) {
                    if (absentIsAir) continue;   // nothing to collide with
                    return false;                // not resident: unknown, not air
                }
                for (int si = s0; si <= s1; ++si) {
                    const ChunkSection* section = chunk->GetSection(si);
                    if (!section || !section->IsAllAir()) return false;
                }
            }
        }
        return true;
    }

    uint64_t ChunkProvider::RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const {
        uint64_t sum = 0;
        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                const Chunk* chunk = GetCachedChunkRef(Math::ChunkPos{cx, cz}).get();
                if (chunk) sum += chunk->blockWriteCounter.load(std::memory_order_acquire);
            }
        }
        return sum;
    }

    void ChunkProvider::GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                            BlockState* out) const {
        if (min.x > max.x || min.y > max.y || min.z > max.z) return;
        const int ny = max.y - min.y + 1, nz = max.z - min.z + 1;
        const size_t total = static_cast<size_t>(max.x - min.x + 1) * ny * nz;
        for (size_t i = 0; i < total; ++i) out[i] = BlockState{};
        const auto at = [&](int x, int y, int z) -> BlockState& {
            return out[(static_cast<size_t>(x - min.x) * ny + (y - min.y)) * nz + (z - min.z)];
        };

        const int y0 = std::max(min.y, Math::WorldCoordinates::MIN_WORLD_Y);
        const int y1 = std::min(max.y, Math::WorldCoordinates::MIN_WORLD_Y +
                                       Math::SECTIONS_PER_CHUNK * Math::SECTION_HEIGHT - 1);
        if (y0 > y1) return;

        for (int cx = min.x >> 4; cx <= (max.x >> 4); ++cx) {
            for (int cz = min.z >> 4; cz <= (max.z >> 4); ++cz) {
                const Chunk* chunk = GetCachedChunkRef(Math::ChunkPos{cx, cz}).get();
                if (!chunk) continue;   // absent column reads as air
                const int bx0 = std::max(min.x, cx << 4), bx1 = std::min(max.x, (cx << 4) + 15);
                const int bz0 = std::max(min.z, cz << 4), bz1 = std::min(max.z, (cz << 4) + 15);
                // No LockShared: this reads exactly what Chunk::StateAt reads,
                // and StateAt takes no lock either. Taking the chunk's shared
                // mutex here put eight physics workers on one lock (a mass
                // detonation is one chunk) and made the bulk read slower than
                // the per-cell path it replaced.
                for (int wy = y0; wy <= y1; ) {
                    int sectionIndex, sectionY;
                    Math::WorldCoordinates::WorldYToSectionCoords(wy, sectionIndex, sectionY);
                    if (sectionIndex < 0 || sectionIndex >= Math::SECTIONS_PER_CHUNK) break;
                    const int syTop = std::min(15, sectionY + (y1 - wy));
                    const ChunkSection* sec = chunk->GetSection(sectionIndex);
                    if (sec && !sec->IsAllAir()) {
                        const PalettedContainer& states = sec->States();
                        if (states.IsSingleValue()) {
                            const BlockState one = BlockState::FromRawId(states.SingleValue());
                            for (int sy = sectionY; sy <= syTop; ++sy)
                                for (int wz = bz0; wz <= bz1; ++wz)
                                    for (int wx = bx0; wx <= bx1; ++wx)
                                        at(wx, wy + (sy - sectionY), wz) = one;
                        } else {
                            for (int sy = sectionY; sy <= syTop; ++sy)
                                for (int wz = bz0; wz <= bz1; ++wz)
                                    for (int wx = bx0; wx <= bx1; ++wx)
                                        at(wx, wy + (sy - sectionY), wz) =
                                            sec->StateAt(wx & 15, sy, wz & 15);
                        }
                    }
                    wy += (syTop - sectionY) + 1;
                }
            }
        }
    }

    void ChunkProvider::FillCollisionMaskFast(const glm::ivec3& origin,
                                              const glm::ivec3& size,
                                              int shiftZ, int shiftY,
                                              uint64_t* out) const {
        if (size.x <= 0 || size.y <= 0 || size.z <= 0) return;

        const int y0 = std::max(origin.y, Math::WorldCoordinates::MIN_WORLD_Y);
        const int y1 = std::min(origin.y + size.y - 1,
                                Math::WorldCoordinates::MIN_WORLD_Y +
                                    Chunk::SECTION_COUNT * Math::SECTION_HEIGHT - 1);
        if (y0 > y1) return;

        const int x1 = origin.x + size.x - 1;
        const int z1 = origin.z + size.z - 1;

        // Cache HasCollision per distinct raw state id within a section. A
        // 16^3 section rarely holds more than a handful.
        for (int cx = origin.x >> 4; cx <= (x1 >> 4); ++cx) {
            for (int cz = origin.z >> 4; cz <= (z1 >> 4); ++cz) {
                const int bx0 = std::max(origin.x, cx << 4);
                const int bx1 = std::min(x1, (cx << 4) + 15);
                const int bz0 = std::max(origin.z, cz << 4);
                const int bz1 = std::min(z1, (cz << 4) + 15);

                const Chunk* chunk =
                    GetCachedChunkRef(Math::ChunkPos{cx, cz}).get();
                if (!chunk) {
                    // A column that is not resident is UNKNOWN, not empty. Set
                    // its bits so every cell in it falls through to the live
                    // accessor instead of being answered from this snapshot.
                    //
                    // Leaving it clear would be a real bug rather than a
                    // conservative one: chunk installs come from worker threads
                    // (CompleteChunkLoad -> ChunkCache::Put) and do NOT bump the
                    // block-write epoch, so a column that streams in during the
                    // blast would keep answering "no collision" and victims
                    // behind its wall would take full damage through it.
                    //
                    // A set bit costs only the read it would have done anyway,
                    // and the accessor returns air for a still-absent column —
                    // which is exactly what the uncached path answers today.
                    for (int wy = y0; wy <= y1; ++wy) {
                        const size_t rowY = static_cast<size_t>(wy - origin.y) << shiftY;
                        for (int wz = bz0; wz <= bz1; ++wz) {
                            const size_t row = rowY |
                                (static_cast<size_t>(wz - origin.z) << shiftZ);
                            for (int wx = bx0; wx <= bx1; ++wx) {
                                const size_t i = row | static_cast<size_t>(wx - origin.x);
                                out[i >> 6] |= (1ull << (i & 63));
                            }
                        }
                    }
                    continue;
                }

                // Shared lock for the whole column's scan: the palette can be
                // reallocated by a concurrent write, and holding the lock over
                // the scan is what makes reading it safe.
                auto guard = chunk->LockShared();

                for (int wy = y0; wy <= y1; ) {
                    int sectionIndex, sectionY;
                    Math::WorldCoordinates::WorldYToSectionCoords(wy, sectionIndex, sectionY);
                    if (sectionIndex < 0 || sectionIndex >= Chunk::SECTION_COUNT) break;
                    const ChunkSection* sec = chunk->GetSection(sectionIndex);

                    // How much of this section the box covers.
                    const int syTop = std::min(15, sectionY + (y1 - wy));

                    if (!sec) {
                        // Same reasoning as an absent column.
                        for (int sy = sectionY; sy <= syTop; ++sy) {
                            const int dy = (wy + (sy - sectionY)) - origin.y;
                            for (int wz = bz0; wz <= bz1; ++wz) {
                                const size_t row =
                                    (static_cast<size_t>(dy) << shiftY) |
                                    (static_cast<size_t>(wz - origin.z) << shiftZ);
                                for (int wx = bx0; wx <= bx1; ++wx) {
                                    const size_t i =
                                        row | static_cast<size_t>(wx - origin.x);
                                    out[i >> 6] |= (1ull << (i & 63));
                                }
                            }
                        }
                    } else {
                        const PalettedContainer& states = sec->States();
                        // A single-value section answers all 4096 cells with
                        // one lookup. In a crater this is most of them.
                        if (states.IsSingleValue()) {
                            const bool solid = BlockRegistry::HasCollision(
                                BlockState::FromRawId(states.SingleValue()).Block());
                            if (solid) {
                                for (int sy = sectionY; sy <= syTop; ++sy) {
                                    const int dy = (wy + (sy - sectionY)) - origin.y;
                                    for (int wz = bz0; wz <= bz1; ++wz) {
                                        const size_t row =
                                            (static_cast<size_t>(dy) << shiftY) |
                                            (static_cast<size_t>(wz - origin.z) << shiftZ);
                                        for (int wx = bx0; wx <= bx1; ++wx) {
                                            const size_t i =
                                                row | static_cast<size_t>(wx - origin.x);
                                            out[i >> 6] |= (1ull << (i & 63));
                                        }
                                    }
                                }
                            }
                        } else {
                            // Per-cell through PalettedContainer::Get, which is
                            // correct for BOTH a local palette and a global
                            // one. Reading m_palette directly would be wrong on
                            // the global path: m_palette is EMPTY when m_global
                            // is set, so an index scan would report every cell
                            // of a >=257-state section as clear and let rays
                            // sail through solid obsidian.
                            for (int sy = sectionY; sy <= syTop; ++sy) {
                                const int dy = (wy + (sy - sectionY)) - origin.y;
                                for (int wz = bz0; wz <= bz1; ++wz) {
                                    const size_t row =
                                        (static_cast<size_t>(dy) << shiftY) |
                                        (static_cast<size_t>(wz - origin.z) << shiftZ);
                                    const int lz = wz & 15;
                                    for (int wx = bx0; wx <= bx1; ++wx) {
                                        const uint32_t raw = states.Get(
                                            Math::LocalIndex(wx & 15, sy, lz));
                                        if (BlockRegistry::HasCollision(
                                                BlockState::FromRawId(raw).Block())) {
                                            const size_t i =
                                                row | static_cast<size_t>(wx - origin.x);
                                            out[i >> 6] |= (1ull << (i & 63));
                                        }
                                    }
                                }
                            }
                        }
                    }
                    wy += (syTop - sectionY) + 1;
                }
            }
        }
    }

    uint16_t ChunkProvider::GetBiome(int worldX, int worldY, int worldZ) const {
        if (!ValidateWorldPosition(worldX, worldY, worldZ)) {
            return kFallbackBiomeId;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        const Chunk* chunk = GetCachedChunkRef(chunkPos).get();
        if (!chunk) {
            return kFallbackBiomeId;
        }

        // Chunk::GetBiome takes a WORLD y (it rebases against MIN_WORLD_Y
        // itself), unlike the section-local y the block accessors above use.
        return chunk->GetBiome(localX, worldY, localZ);
    }

    void ChunkProvider::SetBlock(int worldX, int worldY, int worldZ, BlockID block) {
        SetBlock(worldX, worldY, worldZ, block, 0);
    }

    void ChunkProvider::SetBlock(int worldX, int worldY, int worldZ, BlockID block, BlockStateIndex stateIndex) {
        if (!ValidateWorldPosition(worldX, worldY, worldZ) || !IsValidBlockID(block)) {
            return;
        }

        Math::ChunkPos chunkPos;
        int localX, localY, localZ;
        WorldToLocalCoords(worldX, worldY, worldZ, chunkPos, localX, localY, localZ);

        auto chunk = GetChunk(chunkPos);
        if (!chunk) {
            Log::Warning("Cannot set block at (%d, %d, %d) - chunk not available", worldX, worldY, worldZ);
            return;
        }

        chunk->SetBlock(localX, localY, localZ, block, stateIndex);

        // Mark chunk as dirty for saving
        if (m_chunkCache) {
            m_chunkCache->MarkDirty(chunkPos);
        }

        // NOT marked dirty here. World::SetBlock does it, gated on
        // UpdateFlags::MarkDirty (World.cpp -> OnBlockChanged ->
        // World::MarkSectionDirty -> ChunkProvider::MarkBlockDirty), and this
        // call was the identical work done a second time: each MarkBlockDirty
        // fans one section out to seven, so a single block write cost 14
        // MarkSectionDirtyInternal calls and 28 lock/unlock pairs across two
        // mutexes, exactly half of it a literal repeat.
        //
        // Deleting THIS one rather than World's is the deliberate choice: this
        // one was unconditional, so keeping it would have made
        // UpdateFlags::MarkDirty meaningless. Writes that deliberately omit
        // that bit now genuinely skip the marking, which is what the flag is
        // for. Every ordinary write — including every explosion — passes
        // UpdateFlags::All, which includes it.
    }

    // === INEIGHBORPROVIDER IMPLEMENTATION ===

    bool ChunkProvider::IsChunkLoaded(int chunkX, int chunkZ) const {
        return IsChunkLoaded(Math::ChunkPos{chunkX, chunkZ});
    }

    bool ChunkProvider::IsPositionLoaded(int worldX, int worldY, int worldZ) const {
        Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        return IsChunkLoaded(chunkPos);
    }

    bool ChunkProvider::IsBlockSolid(int worldX, int worldY, int worldZ) const {
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isSolid;
    }

    bool ChunkProvider::IsBlockFluid(int worldX, int worldY, int worldZ) const {
        // MC `!state.getFluidState().isEmpty()` — see ClientBlockAccess for why
        // this cannot be a block-id test once waterlogging exists.
        if (ContainsWater(worldX, worldY, worldZ)) return true;
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isFluid;
    }

    bool ChunkProvider::IsBlockTransparent(int worldX, int worldY, int worldZ) const {
        BlockID block = GetBlock(worldX, worldY, worldZ);
        bool isSolid, isFluid, isTransparent;
        GetBlockProperties(block, isSolid, isFluid, isTransparent);
        return isTransparent;
    }

    INeighborProvider::NeighborStats ChunkProvider::GetStats() const {
        NeighborStats stats;
        auto providerStats = GetProviderStats();
        stats.totalQueries = providerStats.chunksLoaded * 100;
        return stats;
    }

    void ChunkProvider::ResetStats() {
        ResetProviderStats();
    }

    // === DIRTY TRACKING ===

    void ChunkProvider::MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->MarkSectionDirty(chunkPos, sectionIndex);
    }

    void ChunkProvider::MarkChunkDirty(Math::ChunkPos chunkPos) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->MarkChunkDirty(chunkPos);
    }

    void ChunkProvider::MarkChunkForSave(Math::ChunkPos chunkPos) {
        if (!m_initialized || !m_chunkCache) return;
        m_chunkCache->MarkDirty(chunkPos);
    }

    void ChunkProvider::MarkBlockDirty(int worldX, int worldY, int worldZ) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        // Same-section memo, ahead of the vector GetAffectedSections builds:
        // a landing sand column writes the same section thousands of times a
        // tick, and each call here was an allocation plus the tracker's mutex
        // before the tracker's own memo could say "already dirty". Valid
        // within one clear epoch; a boundary cell (local x/z 0 or 15) is not
        // memoised because it marks a neighbour chunk too.
        {
            struct Last { const void* owner; int cx, cz, sy; uint64_t epoch; };
            thread_local Last t_last{nullptr, 0, 0, -1, 0};
            const int cx = worldX >> 4, cz = worldZ >> 4;
            const int sy = Math::WorldCoordinates::WorldYToSectionIndex(worldY);
            const int lx = worldX & 15, lz = worldZ & 15;
            const bool interior = lx != 0 && lx != 15 && lz != 0 && lz != 15;
            const uint64_t epoch = m_dirtyTracker->ClearEpoch();
            if (interior && t_last.owner == this && t_last.epoch == epoch &&
                t_last.cx == cx && t_last.cz == cz && t_last.sy == sy) {
                return;
            }
            if (interior) t_last = Last{this, cx, cz, sy, epoch};
            else          t_last.owner = nullptr;
        }

        auto affectedSections = GetAffectedSections(worldX, worldY, worldZ, true);

        for (const auto& section : affectedSections) {
            m_dirtyTracker->MarkSectionDirty(section.chunkPos, section.sectionIndex);
        }
    }

    std::vector<DirtySection> ChunkProvider::GetDirtySections() {
        if (!m_initialized || !m_dirtyTracker) {
            return {};
        }

        return m_dirtyTracker->GetDirtySections();
    }

    std::vector<DirtySection> ChunkProvider::GetAndClearDirtySections() {
        if (!m_initialized || !m_dirtyTracker) {
            return {};
        }

        return m_dirtyTracker->GetAndClearAllDirtySections();
    }

    void ChunkProvider::ClearDirtySections(const std::vector<DirtySection>& sections) {
        if (!m_initialized || !m_dirtyTracker) {
            return;
        }

        m_dirtyTracker->ClearDirtySections(sections);
    }

    bool ChunkProvider::IsChunkDirty(Math::ChunkPos chunkPos) const {
        if (!m_initialized || !m_dirtyTracker) {
            return false;
        }

        return m_dirtyTracker->IsChunkDirty(chunkPos);
    }

    bool ChunkProvider::IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const {
        if (!m_initialized || !m_dirtyTracker) {
            return false;
        }

        return m_dirtyTracker->IsSectionDirty(chunkPos, sectionIndex);
    }

    size_t ChunkProvider::GetDirtyCount() const {
        if (!m_initialized || !m_dirtyTracker) {
            return 0;
        }

        return m_dirtyTracker->GetDirtyCount();
    }

    // === SAVING ===

    void ChunkProvider::SaveChunk(Math::ChunkPos position) {
        if (!m_initialized || !m_chunkCache || !m_chunkSaver) {
            return;
        }

        auto chunk = m_chunkCache->Get(position);
        if (chunk && m_chunkCache->IsDirty(position)) {
            m_chunkSaver->SaveChunkAsync(*chunk);
            m_chunkCache->ClearDirtyFlag(position);
        }
    }

    void ChunkProvider::SaveAllDirtyChunks() {
        if (!m_initialized || !m_chunkCache) {
            return;
        }

        m_chunkCache->SaveAllDirty();
    }

    // === CONFIGURATION ===

    void ChunkProvider::SetConfig(const ChunkProviderConfig& config) {
        if (!config.IsValid()) {
            Log::Warning("Invalid ChunkProvider configuration, ignoring");
            return;
        }

        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;

        if (m_initialized) {
            ConfigureComponents();
        }
    }

    ChunkProviderConfig ChunkProvider::GetConfig() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }

    void ChunkProvider::SetWorldPath(const std::string& path) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.minecraftWorldPath = path;
        }

        if (m_initialized && m_chunkLoader) {
            m_chunkLoader->SetSource(path);
        }
    }

    std::string ChunkProvider::GetWorldPath() const {
        if (m_chunkLoader) {
            return m_chunkLoader->GetSource();
        }

        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.minecraftWorldPath;
    }

    void ChunkProvider::SetMaxLoadedChunks(size_t maxChunks) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        // Cache size is now managed directly by ChunkCache
        if (m_chunkCache) {
            ChunkCacheConfig cacheConfig;
            cacheConfig.maxSize = std::max(size_t(16), maxChunks);
            // Note: Would need to recreate cache to change size
        }
    }

    size_t ChunkProvider::GetMaxLoadedChunks() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        // Return actual cache size
        if (m_chunkCache) {
            return m_chunkCache->GetStats().maxSize;
        }
        return 2048; // Default
    }

    void ChunkProvider::SetGameTime(int64_t gameTime) {
        // Both halves of the Anvil stack keep their own atomic copy rather than
        // reaching back here: Encode() and LoadChunk() run on the worker pool
        // and the IO thread, and a pointer back into the provider would be one
        // more lifetime to reason about for a value that is four aligned bytes.
        if (m_anvilStorage) m_anvilStorage->SetGameTime(gameTime);
        if (m_anvilLoader)  m_anvilLoader->SetGameTime(gameTime);
    }

    void ChunkProvider::SetGenerationSeed(int64_t seed) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.generationConfig.seed = seed;
        }

        if (m_initialized && m_chunkGenerator) {
            m_chunkGenerator->SetSeed(seed);
        }
    }

    int64_t ChunkProvider::GetGenerationSeed() const {
        if (m_chunkGenerator) {
            return m_chunkGenerator->GetSeed();
        }

        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.generationConfig.seed;
    }

    void ChunkProvider::SetGenerateStructures(bool enabled) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.generationConfig.generateStructures = enabled;

        // Push the updated config to the generator (it re-reads it on its
        // lazy Initialize). SetConfig replaces the whole GenerationConfig, so
        // this must come after SetGenerationSeed — PlatformMain calls them in
        // that order.
        if (m_initialized && m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }
    }

    bool ChunkProvider::GetGenerateStructures() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.generationConfig.generateStructures;
    }

    void ChunkProvider::SetWorldGenOptions(const std::string& worldType,
                                           const std::string& flatPreset,
                                           const std::string& flatLayers,
                                           const std::string& singleBiome) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.generationConfig.worldType = worldType;
        m_config.generationConfig.flatPreset = flatPreset;
        m_config.generationConfig.flatLayers = flatLayers;
        m_config.generationConfig.singleBiome = singleBiome;

        // Same push mechanism as SetGenerateStructures — SetConfig replaces
        // the whole GenerationConfig, so call after SetGenerationSeed.
        if (m_initialized && m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }
    }

    void ChunkProvider::SetWorldGenTweaks(const std::string& tweaksJson) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.generationConfig.worldgenTweaks = tweaksJson;

        if (m_initialized && m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }
    }

    // === STATISTICS ===

    ChunkProviderStats ChunkProvider::GetProviderStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ChunkProviderStats stats = m_stats;

        // Update real-time values
        if (m_chunkCache) {
            auto cacheStats = m_chunkCache->GetStats();
            stats.memoryUsage = m_chunkCache->GetMemoryUsageBytes();
        }

        if (m_dirtyTracker) {
            stats.dirtySections = m_dirtyTracker->GetDirtyCount();
        }

        return stats;
    }

    void ChunkProvider::ResetProviderStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();

        if (m_chunkCache) m_chunkCache->ResetStats();
        if (m_chunkLoader) m_chunkLoader->ResetStats();
        if (m_chunkGenerator) m_chunkGenerator->ResetStats();
        if (m_dirtyTracker) m_dirtyTracker->ResetStats();
    }

    ChunkCache::CacheStats ChunkProvider::GetCacheStats() const {
        return m_chunkCache ? m_chunkCache->GetStats() : ChunkCache::CacheStats{};
    }

    IChunkLoader::LoaderStats ChunkProvider::GetLoaderStats() const {
        return m_chunkLoader ? m_chunkLoader->GetStats() : IChunkLoader::LoaderStats{};
    }

    ProceduralChunkGenerator::GeneratorStats ChunkProvider::GetGeneratorStats() const {
        return m_chunkGenerator ? m_chunkGenerator->GetStats() : ProceduralChunkGenerator::GeneratorStats{};
    }

    DirtyTrackerStats ChunkProvider::GetDirtyTrackerStats() const {
        return m_dirtyTracker ? m_dirtyTracker->GetStats() : DirtyTrackerStats{};
    }

    // === DIAGNOSTICS ===

    size_t ChunkProvider::GetMemoryUsage() const {
        size_t total = sizeof(ChunkProvider);

        if (m_chunkCache) total += m_chunkCache->GetMemoryUsageBytes();
        if (m_dirtyTracker) total += m_dirtyTracker->GetMemoryUsage();

        return total;
    }

    size_t ChunkProvider::GetLoadedChunkCount() const {
        return m_chunkCache ? m_chunkCache->GetStats().currentSize : 0;
    }

    std::vector<Math::ChunkPos> ChunkProvider::GetLoadedChunkPositions() const {
        if (!m_initialized || !m_chunkCache) {
            return {};
        }
        return m_chunkCache->GetLoadedChunkPositions();
    }

    void ChunkProvider::LogPerformanceStats() const {
        auto stats = GetProviderStats();

        Log::Info("ChunkProvider Performance:");
        Log::Info("  Loaded: %zu, Generated: %zu, Saved: %zu, Evicted: %zu",
                 stats.chunksLoaded, stats.chunksGenerated, stats.chunksSaved, stats.chunksEvicted);
        Log::Info("  Memory: %zu KB, Dirty Sections: %zu",
                 stats.memoryUsage / 1024, stats.dirtySections);
    }

    bool ChunkProvider::ValidateState() const {
        if (!m_initialized) {
            return false;
        }

        if (m_dirtyTracker && !m_dirtyTracker->ValidateState()) {
            Log::Error("DirtyTracker validation failed");
            return false;
        }

        return true;
    }

    // === INTERNAL WORKFLOWS ===

    std::shared_ptr<Chunk> ChunkProvider::LoadChunkInternal(Math::ChunkPos position,
                                                            bool& outWasGenerated) {
        PROFILE_ZONE;
        if (!ValidateChunkPosition(position)) {
            Log::Error("LoadChunkInternal: Invalid chunk position (%d, %d)", position.x, position.z);
            return nullptr;
        }

        //Log::Debug("LoadChunkInternal: Loading chunk (%d, %d)", position.x, position.z);

        // Try loading from disk first
        std::shared_ptr<Chunk> chunk = TryLoadFromDisk(position);
        if (chunk) {
            Log::Debug("Loaded chunk (%d, %d) from disk", position.x, position.z);
            return chunk;
        }

        // Generate new chunk as fallback
        //Log::Debug("Generating chunk (%d, %d)", position.x, position.z);
        chunk = TryGenerateChunk(position);
        if (chunk) {
            // A newly generated chunk is UNSAVED, and MC treats it that way
            // (ChunkAccess.markUnsaved once its status advances) — the chunk it
            // just generated is the chunk it writes.
            //
            // Without this, only chunks the player EDITED would ever reach
            // disk: nothing else marks a chunk dirty. The world would then be
            // mostly holes, and opening it in Minecraft would let MC generate
            // the gaps with ITS generator, which does not match ours at the
            // seams. Costs one write per generated chunk, which is what
            // vanilla pays too.
            outWasGenerated = true;
            return chunk;
        } else {
            if (m_chunkGenerator && m_chunkGenerator->IsAbortRequested()) {
                Log::Debug("Chunk (%d, %d) generation cancelled (shutting down)",
                           position.x, position.z);
            } else {
                Log::Error("Failed to generate chunk (%d, %d)", position.x, position.z);
            }
        }

        if (m_chunkGenerator && m_chunkGenerator->IsAbortRequested()) {
            Log::Debug("LoadChunkInternal: chunk (%d, %d) cancelled (shutting down)",
                       position.x, position.z);
        } else {
            Log::Warning("LoadChunkInternal: All methods failed for chunk (%d, %d)",
                         position.x, position.z);
        }
        return nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::TryLoadFromCache(Math::ChunkPos position) {
        if (!m_chunkCache) {
            return nullptr;
        }

        return m_chunkCache->Get(position);
    }

    std::shared_ptr<Chunk> ChunkProvider::TryLoadFromDisk(Math::ChunkPos position) {
        PROFILE_ZONE_N("LoadFromDisk");

        // An ObeyCraft save, in vanilla Anvil format. Read and write share one
        // AnvilChunkIo, so this can never observe a sector table the writer has
        // already moved on from.
        if (m_anvilLoader) {
            auto chunk = std::make_shared<Chunk>();
            std::string error;
            if (m_anvilLoader->LoadChunk(position, *chunk, error)) {
                // Appointments that came off disk have to announce themselves —
                // see SetChunkTicksLoadedCallback. No lock needed on the chunk:
                // it is still local to this function and nothing else can see it.
                if (m_onChunkTicksLoaded && !chunk->BlockTicks().Empty()) {
                    m_onChunkTicksLoaded(position);
                }
                return chunk;
            }
            if (!error.empty()) {
                // A REAL read failure, not "not saved yet". Returning nullptr
                // here would send the caller to the generator, which would
                // overwrite a player's build with fresh terrain the next time
                // the chunk was saved. Refuse the chunk instead.
                Log::Error("Refusing to regenerate chunk (%d, %d): %s",
                           position.x, position.z, error.c_str());
                return nullptr;
            }
            // Empty error: the chunk has simply never been saved. Fall through
            // to generation.
        }

        if (!m_chunkLoader) {
            return nullptr;
        }

        auto result = m_chunkLoader->LoadChunk(position);
        return result.success ? result.chunk : nullptr;
    }

    std::shared_ptr<Chunk> ChunkProvider::TryGenerateChunk(Math::ChunkPos position) {
        PROFILE_ZONE_N("GenerateChunk");
        if (!m_chunkGenerator) {
            Log::Error("TryGenerateChunk: No chunk generator available");
            return nullptr;
        }

        try {
            //Log::Debug("Generating chunk (%d, %d) using generator", position.x, position.z);
            auto result = m_chunkGenerator->GenerateChunk(position);

            if (!result.success) {
                // A cancelled generation is not a failure. On shutdown every
                // in-flight chunk aborts at once — ~1,000 of them on a world
                // still streaming — and logging those at ERROR made a perfectly
                // normal quit look like a corrupted world.
                if (m_chunkGenerator->IsAbortRequested()) {
                    Log::Debug("Chunk (%d, %d) generation cancelled (shutting down)",
                               position.x, position.z);
                } else {
                    Log::Error("TryGenerateChunk: Generator failed for chunk (%d, %d): %s",
                               position.x, position.z, result.errorMessage.c_str());
                }
                return nullptr;
            }

            if (!result.chunk) {
                Log::Error("TryGenerateChunk: Generator returned null chunk for (%d, %d)",
                          position.x, position.z);
                return nullptr;
            }

            result.chunk->pos = position;

            //Log::Info("Successfully generated chunk (%d, %d)", position.x, position.z);
            return result.chunk;

        } catch (const std::exception& e) {
            Log::Error("TryGenerateChunk: Exception generating chunk (%d, %d): %s",
                      position.x, position.z, e.what());
            return nullptr;
        }
    }

    std::shared_ptr<Chunk> ChunkProvider::CompleteChunkLoad(std::shared_ptr<Chunk> chunk,
                                                            bool wasGenerated) {
        PROFILE_ZONE_N("CompleteChunkLoad");
        if (!chunk || !m_chunkCache) {
            return chunk;
        }

        // Validate chunk
        if (!ValidateChunk(chunk)) {
            Log::Warning("Loaded chunk failed validation: (%d, %d)", chunk->pos.x, chunk->pos.z);
            return nullptr;
        }

        // Check if chunk is already in cache before adding
        if (m_chunkCache->Contains(chunk->pos)) {
            Log::Debug("Chunk (%d, %d) already in cache during CompleteChunkLoad, returning existing",
                      chunk->pos.x, chunk->pos.z);
            return m_chunkCache->Get(chunk->pos);
        }

        // Add to cache
        m_chunkCache->Put(chunk->pos, chunk);
        //Log::Debug("Added chunk (%d, %d) to cache", chunk->pos.x, chunk->pos.z);

        // Now that the cache knows about it, mark a freshly generated chunk as
        // needing a write. Only when this world persists at all — an imported
        // read-only world has no saver, and a seed-only world has nowhere to
        // put it.
        if (wasGenerated && m_chunkSaver) {
            m_chunkCache->MarkDirty(chunk->pos);
        }

        return chunk;
    }

    std::shared_ptr<Chunk> ChunkProvider::LoadWithoutGenerating(Math::ChunkPos position) {
        PROFILE_ZONE_N("LoadWithoutGenerating");
        if (!m_initialized || !ValidateChunkPosition(position)) return nullptr;
        if (auto chunk = TryLoadFromCache(position)) return chunk;
        auto chunk = TryLoadFromDisk(position);
        if (!chunk) return nullptr;
        return CompleteChunkLoad(chunk, /*wasGenerated=*/false);
    }

    std::shared_ptr<Chunk> ChunkProvider::StoreChunkInCache(std::shared_ptr<Chunk> chunk) {
        // No callers today. `true` is the conservative answer for a chunk
        // arriving by an unknown route: an unnecessary rewrite costs a little
        // I/O, a missed one loses the chunk.
        return CompleteChunkLoad(std::move(chunk), /*wasGenerated=*/true);
    }

    // ── entities/*.mca ──────────────────────────────────────────────────────
    //
    // The same AnvilChunkIo the terrain uses, with RegionKind::Entities. One
    // store serving both families is deliberate: it is what keeps a relocating
    // write from leaving any reader pointing at a freed sector.

    bool ChunkProvider::ReadEntityChunkNbt(Math::ChunkPos pos, std::vector<uint8_t>& out,
                                           std::string& error) {
        if (!m_anvilIo) { error.clear(); return false; }
        return m_anvilIo->ReadChunkNbt(m_config.dimensionId, Anvil::RegionKind::Entities,
                                       pos, out, error);
    }

    bool ChunkProvider::WriteEntityChunkNbt(Math::ChunkPos pos, const std::vector<uint8_t>& payload,
                                            std::string& error) {
        if (!m_anvilIo) { error.clear(); return false; }
        return m_anvilIo->WriteChunkNbt(m_config.dimensionId, Anvil::RegionKind::Entities,
                                        pos, payload, error);
    }

    bool ChunkProvider::ClearEntityChunk(Math::ChunkPos pos, std::string& error) {
        if (!m_anvilIo) { error.clear(); return false; }
        return m_anvilIo->ClearChunk(m_config.dimensionId, Anvil::RegionKind::Entities, pos, error);
    }

    // === COORDINATION ===

    void ChunkProvider::SetupComponentDependencies() {
        if (!m_initialized) {
            return;
        }

        // Set up chunk cache dependencies
        if (m_chunkCache && m_chunkSaver) {
            // A real shared_ptr, not an aliasing one with a no-op deleter.
            // Shutdown() destroys the saver before the cache, and ~ChunkCache
            // then calls SaveAllDirty guarded only by a null check — with a
            // non-owning pointer that was a use-after-free.
            m_chunkCache->SetChunkSaver(m_chunkSaver);
            Log::Info("Chunk cache wired to the saver — this world will persist");
        } else if (m_chunkSaver) {
            Log::Error("Chunk saver exists but the cache never received it — nothing will be written");
        }

        // Set up loader fallback generation
        if (m_chunkLoader && m_chunkGenerator) {
            std::shared_ptr<IChunkGenerator> generatorPtr(m_chunkGenerator.get(), [](IChunkGenerator*){});
            m_chunkLoader->SetFallbackGenerator(generatorPtr);
        }

        // Set up callbacks
        if (m_chunkCache) {
            m_chunkCache->SetEvictionCallback(
                [this](Math::ChunkPos pos, std::shared_ptr<Chunk> chunk, bool wasDirty) {
                    OnChunkEvicted(pos, chunk, wasDirty);
                });
        }
    }

    void ChunkProvider::ConfigureComponents() {
        std::lock_guard<std::mutex> lock(m_configMutex);

        // Configure loader
        if (m_chunkLoader) {
            m_chunkLoader->SetSource(m_config.minecraftWorldPath);
        }

        // Configure generator
        if (m_chunkGenerator) {
            m_chunkGenerator->SetConfig(m_config.generationConfig);
        }

        // Configure dirty tracker
        if (m_dirtyTracker) {
            m_dirtyTracker->SetConfig(m_config.dirtyConfig);
        }
    }

    void ChunkProvider::OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty) {
        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.chunksEvicted++;
        }

        Log::Debug("Chunk (%d, %d) evicted from cache%s", position.x, position.z, wasDirty ? " (was dirty)" : "");
    }

    // === VALIDATION ===

    bool ChunkProvider::ValidateChunk(const std::shared_ptr<Chunk>& chunk) const {
        if (!chunk) {
            return false;
        }

        if (chunk->IsEmpty()) {
            return false;
        }

        return ValidateChunkPosition(chunk->pos);
    }

    bool ChunkProvider::ValidateChunkPosition(Math::ChunkPos position) const {
        const int MAX_CHUNK_COORD = 1000000;
        bool valid = std::abs(position.x) < MAX_CHUNK_COORD && std::abs(position.z) < MAX_CHUNK_COORD;

        if (!valid) {
            Log::Error("Invalid chunk position: (%d, %d) exceeds maximum coordinate limit",
                      position.x, position.z);
        }

        return valid;
    }

    bool ChunkProvider::ValidateWorldPosition(int worldX, int worldY, int worldZ) const {
        return Math::WorldCoordinates::IsValidWorldY(worldY) &&
               std::abs(worldX) < 30000000 && std::abs(worldZ) < 30000000;
    }

    // === HELPERS ===

    void ChunkProvider::WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                                          int& localX, int& localY, int& localZ) const {
        chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        localX = worldX - (chunkPos.x * Math::CHUNK_SIZE_X);
        localY = worldY;
        localZ = worldZ - (chunkPos.z * Math::CHUNK_SIZE_Z);

        if (localX < 0) localX += Math::CHUNK_SIZE_X;
        if (localZ < 0) localZ += Math::CHUNK_SIZE_Z;
    }

    bool ChunkProvider::IsValidBlockID(BlockID block) const {
        // Range check against the actual enum extent — NOT a hardcoded
        // constant. The old `< 1000` bound silently rejected every BlockID
        // added after the first ~1000, including SnowGrass/Lilac/leaf-litter
        // variants and the entire SlabTop family. SetBlock would silently
        // no-op for those, so the server's chunk stayed Air at the cell
        // while the BlockChangeS2C broadcast still told the client to
        // render the new block — producing the "slab renders but has no
        // collision" failure mode (player physics reads the server world).
        return static_cast<unsigned>(block) < static_cast<unsigned>(BlockID::Count);
    }

    bool ChunkProvider::GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const {
        switch (block) {
            case BlockID::Air:
                isSolid = false;
                isFluid = false;
                isTransparent = true;
                break;
            case BlockID::Water:
                isSolid = false;
                isFluid = true;
                isTransparent = true;
                break;
            case BlockID::Glass:
                isSolid = true;
                isFluid = false;
                isTransparent = true;
                break;
            case BlockID::OakLeaves:
            case BlockID::BirchLeaves:
                isSolid = true;
                isFluid = false;
                isTransparent = true;
                break;
            default:
                isSolid = true;
                isFluid = false;
                isTransparent = false;
                break;
        }
        return true;
    }

    void ChunkProvider::LogError(const std::string& operation, const std::string& error) const {
        Log::Error("ChunkProvider %s: %s", operation.c_str(), error.c_str());
    }

    // === MOVE SEMANTICS ===

    void ChunkProvider::MoveFrom(ChunkProvider&& other) noexcept {
        m_config = std::move(other.m_config);
        m_chunkCache = std::move(other.m_chunkCache);
        m_chunkLoader = std::move(other.m_chunkLoader);
        m_chunkGenerator = std::move(other.m_chunkGenerator);
        m_chunkSaver = std::move(other.m_chunkSaver);
        m_dirtyTracker = std::move(other.m_dirtyTracker);
        m_stats = other.m_stats;
        m_initialized = other.m_initialized.load();

        // Clear other's state
        other.m_chunkCache.reset();
        other.m_chunkLoader.reset();
        other.m_chunkGenerator.reset();
        other.m_chunkSaver.reset();
        other.m_dirtyTracker.reset();
        other.m_stats.Reset();
        other.m_initialized = false;
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config) {
        return std::make_unique<ChunkProvider>(config);
    }

    ChunkProviderConfig CreateDefaultConfig() {
        ChunkProviderConfig config;
        // ChunkCache now defaults to 2048

        // Set up generation config
        // NOTE: Seed uses default from GenerationConfig struct (IChunkGenerator.hpp)
        // Change the seed there for a single source of truth
        config.dimension = "overworld";
        config.generationConfig.worldType = "default";
        config.generationConfig.generateOres = true;
        config.generationConfig.generateCaves = true;
        config.generationConfig.generateStructures = true;
        config.generationConfig.generateVegetation = true;

        // Set up dirty tracking config
        config.dirtyConfig.enableNeighborInvalidation = true;

        return config;
    }

} // namespace Game