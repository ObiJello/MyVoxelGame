// File: src/server/world/ChunkProvider.hpp
#pragma once

#include "common/world/chunk/Chunk.hpp"
#include "common/world/math/WorldMath.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/core/Config.hpp"

// Dependencies
#include "cache/ChunkCache.hpp"
#include "common/world/gen/ProceduralChunkGenerator.hpp"
#include "storage/anvil/AnvilChunkStorage.hpp"
#include "interfaces/IChunkSaver.hpp"
#include "tracking/DirtyTracker.hpp"
#include "interfaces/INeighborProvider.hpp"
#include "interfaces/IChunkLoader.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace Game {

    // Forward declarations
    class IChunkSaver;
    class IChunkGenerator;
    class MinecraftChunkLoaderImpl;

    // Simple configuration
    struct ChunkProviderConfig {
        // Cache settings removed - now uses ChunkCache directly

        // Loading settings
        std::string minecraftWorldPath;   // an IMPORTED Minecraft world (read-only)
        bool enableFallbackGeneration = true;

        // This dimension's folder inside an ObeyCraft save. Non-empty means
        // "this world persists": chunks are read from here before generating
        // and written back. Empty keeps the historical behaviour of
        // regenerating from the seed every session.
        //
        // Distinct from minecraftWorldPath on purpose. That one is somebody's
        // real Minecraft world, and it must never be written; this one is
        // ours. Conflating them is what let the old writer aim itself at a
        // player's save.
        std::string savePath;

        // Which dimension's sub-folder inside savePath. Overworld sits at the
        // world root, nether under DIM-1/, end under DIM1/.
        DimensionId dimensionId = DimensionId::Overworld;

        // Load the world but never write to it. Set for Anvil worlds imported
        // from the player's real Minecraft installation. Enforced by simply not
        // creating a chunk saver — see ChunkProvider::Initialize.
        bool readOnly = false;

        // Which dimension this provider generates: "overworld", "nether" or
        // "end". THIS is the knob to set — Initialize copies it into
        // generationConfig.dimension, so setting the inner one instead has no
        // effect (it warns and is overwritten). Must be set before Initialize;
        // a generator's dimension is fixed for its lifetime.
        std::string dimension = "overworld";

        // Generation settings
        GenerationConfig generationConfig;

        // Dirty tracking settings
        DirtyTrackerConfig dirtyConfig;

        bool IsValid() const {
            return generationConfig.IsValid();
        }
    };

    // Simple statistics
    struct ChunkProviderStats {
        size_t chunksLoaded = 0;
        size_t chunksGenerated = 0;
        size_t chunksSaved = 0;
        size_t chunksEvicted = 0;
        size_t dirtySections = 0;
        size_t memoryUsage = 0;

        void Reset() {
            chunksLoaded = chunksGenerated = chunksSaved = chunksEvicted = 0;
            dirtySections = memoryUsage = 0;
        }
    };

    // Simplified ChunkProvider using composition pattern
    class ChunkProvider : public INeighborProvider {
    public:
        explicit ChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});
        ~ChunkProvider();

        // Non-copyable, movable
        ChunkProvider(const ChunkProvider&) = delete;
        ChunkProvider& operator=(const ChunkProvider&) = delete;
        ChunkProvider(ChunkProvider&& other) noexcept;
        ChunkProvider& operator=(ChunkProvider&& other) noexcept;

        // === LIFECYCLE ===

        bool Initialize();
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }

        // === CORE CHUNK OPERATIONS ===

        // Get chunk (loads if necessary, returns null if not available).
        //
        // BLOCKS. A cache miss goes to disk and then to the generator, and the
        // generator call blocks the calling thread until that chunk reaches
        // FULL. On the server thread that means the tick does not end until
        // generation finishes — never call this from per-tick simulation.
        std::shared_ptr<Chunk> GetChunk(Math::ChunkPos position);

        // Cache-only lookup: null when the chunk is not resident. Never loads,
        // never generates, never blocks. MC ServerChunkCache.getChunkNow.
        //
        // This is what per-tick simulation wants: MC only ever ticks chunks
        // that are already loaded (ChunkMap.forEachBlockTickingChunk walks
        // existing holders), and a chunk that is not there yet is simply not
        // ticked this tick.
        std::shared_ptr<Chunk> GetLoadedChunk(Math::ChunkPos position);

    private:
        // Cache-only chunk fetch with a per-thread 4-entry memo in front of it.
        // See the long note at its definition. Deliberately routed to the
        // CACHE-ONLY accessor, never to GetChunk — memoizing the load-or-
        // generate path would let a blast reaching an unloaded column stall the
        // server tick on terrain generation.
        std::shared_ptr<Chunk> GetCachedChunk(Math::ChunkPos position) const;

        // Borrowing form of the same fetch, for the hot block accessors. The
        // reference is the thread's memo slot: valid only until the next chunk
        // fetch ON THIS THREAD, because MemoSlot is direct-mapped on the low
        // bit of each axis and a chunk one step away in x or z reuses the slot.
        // Never expose this outside the class.
        const std::shared_ptr<Chunk>& GetCachedChunkRef(Math::ChunkPos position) const;
    public:

        // Section-aware fill of a collision bitset over one box. Backs
        // World::FillCollisionMask; see IBlockAccess for the bit layout.
        // A PLAIN member, not an override: ChunkProvider derives from
        // INeighborProvider, which has no IBlockAccess base.
        void FillCollisionMaskFast(const glm::ivec3& origin, const glm::ivec3& size,
                                   int shiftZ, int shiftY, uint64_t* out) const;

        // World::IsRegionAllAir — section-emptiness walk over the box.
        bool IsRegionAllAir(const glm::ivec3& min, const glm::ivec3& max,
                            bool absentIsAir) const;
        // World::GetBlockStatesInBox — one (column, section) tile at a time.
        void GetBlockStatesInBox(const glm::ivec3& min, const glm::ivec3& max,
                                 BlockState* out) const;

        // World::RegionWriteStamp — per-column write counters, summed.
        uint64_t RegionWriteStamp(const glm::ivec3& min, const glm::ivec3& max) const;

        // Check if chunk is loaded
        bool IsChunkLoaded(Math::ChunkPos position) const;

        // Unload chunk and save if dirty
        bool UnloadChunk(Math::ChunkPos position);

        // === BLOCK ACCESS ===

        // Get/set blocks using world coordinates
        BlockID GetBlock(int worldX, int worldY, int worldZ) const override;
        BlockState GetBlockState(int worldX, int worldY, int worldZ) const override;
        uint16_t GetBiome(int worldX, int worldY, int worldZ) const override;
        void SetBlock(int worldX, int worldY, int worldZ, BlockID block);
        // stateIndex is the index into the block's own state list (MC
        // BlockState.getId()); 0 = the block's default state.
        void SetBlock(int worldX, int worldY, int worldZ, BlockID block, BlockStateIndex stateIndex);

        // === INEIGHBORPROVIDER IMPLEMENTATION ===

        bool IsChunkLoaded(int chunkX, int chunkZ) const override;
        bool IsPositionLoaded(int worldX, int worldY, int worldZ) const override;
        bool IsBlockSolid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockFluid(int worldX, int worldY, int worldZ) const override;
        bool IsBlockTransparent(int worldX, int worldY, int worldZ) const override;

        NeighborStats GetStats() const override;
        void ResetStats() override;

        // === DIRTY TRACKING ===

        void MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex);
        void MarkChunkDirty(Math::ChunkPos chunkPos);

        // Mark a chunk as needing to be WRITTEN.
        //
        // Distinct from MarkChunkDirty above, which means "the mesh is stale"
        // and routes to the DirtyTracker. Overloading that name for saving
        // would have marked meshes dirty and saved nothing — a chest edit
        // still would not have persisted, which is the exact bug this exists
        // to fix.
        void MarkChunkForSave(Math::ChunkPos chunkPos);
        void MarkBlockDirty(int worldX, int worldY, int worldZ);

        std::vector<DirtySection> GetDirtySections();
        std::vector<DirtySection> GetAndClearDirtySections();
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        bool IsChunkDirty(Math::ChunkPos chunkPos) const;
        bool IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const;
        size_t GetDirtyCount() const;

        // === SAVING ===

        void SaveChunk(Math::ChunkPos position);
        void SaveAllDirtyChunks();

        // === CONFIGURATION ===

        void SetConfig(const ChunkProviderConfig& config);
        ChunkProviderConfig GetConfig() const;

        void SetWorldPath(const std::string& path);
        std::string GetWorldPath() const;

        void SetMaxLoadedChunks(size_t maxChunks);
        size_t GetMaxLoadedChunks() const;

        void SetGenerationSeed(int64_t seed);
        int64_t GetGenerationSeed() const;

        // World-creation "Generate Structures" option. Must be set before the
        // generator lazily initializes (same timing rule as SetGenerationSeed).
        void SetGenerateStructures(bool enabled);
        bool GetGenerateStructures() const;

        // World type (MC WorldPresets id: "default", "flat", "large_biomes",
        // "amplified", "single_biome_surface") + the flat/single-biome
        // customization. Same timing rule as SetGenerationSeed.
        void SetWorldGenOptions(const std::string& worldType,
                                const std::string& flatPreset,
                                const std::string& flatLayers,
                                const std::string& singleBiome);
        void SetWorldGenTweaks(const std::string& tweaksJson);

        // === SCHEDULED BLOCK TICKS ===

        // Push the world clock down to the Anvil stack. Scheduled ticks are
        // stored as DELAYS relative to it (MC SavedTick), so the saver and the
        // loader both need it. Refreshed once per server tick from
        // World::WorldLoop; one tick of staleness is harmless.
        void SetGameTime(int64_t gameTime);

        // Called on the loading thread, once, for every chunk that came off
        // disk carrying pending scheduled ticks. World points this at
        // LevelTicks::NoteChunkWithTicks — the drain only visits chunks it
        // already knows about, so a chunk whose appointments arrived from a
        // save rather than from a ScheduleTick call has to announce itself or
        // its sand never falls.
        //
        // MUST be thread-safe; TryLoadFromDisk runs on the worker pool.
        void SetChunkTicksLoadedCallback(std::function<void(Math::ChunkPos)> cb) {
            m_onChunkTicksLoaded = std::move(cb);
        }

        // === STATISTICS ===

        ChunkProviderStats GetProviderStats() const;
        void ResetProviderStats();

        ChunkCache::CacheStats GetCacheStats() const;
        IChunkLoader::LoaderStats GetLoaderStats() const;
        ProceduralChunkGenerator::GeneratorStats GetGeneratorStats() const;
        DirtyTrackerStats GetDirtyTrackerStats() const;

        // === DIAGNOSTICS ===

        size_t GetMemoryUsage() const;
        size_t GetLoadedChunkCount() const;

        // Get all loaded chunk positions (for iterating loaded chunks)
        std::vector<Math::ChunkPos> GetLoadedChunkPositions() const;

        void LogPerformanceStats() const;
        bool ValidateState() const;

        // Get the underlying generator (for async API access)
        IChunkGenerator* GetGenerator() const { return m_chunkGenerator.get(); }

        // Store a loaded/generated chunk in cache (validates + caches)
        std::shared_ptr<Chunk> StoreChunkInCache(std::shared_ptr<Chunk> chunk);

        // Cache, then disk — never generates. Worker threads use this so a
        // missing chunk becomes a generation REQUEST to the terrain library
        // instead of a worker parked inside it (see IntegratedServer
        // PumpChunkPipeline). Null when the chunk is not on disk.
        std::shared_ptr<Chunk> LoadWithoutGenerating(Math::ChunkPos position);

        // ── entities/*.mca for THIS provider's dimension ─────────────────────
        //
        // Three narrow forwarders rather than an accessor for m_anvilIo: a raw
        // getter would hand out the write capability that SaveRoot exists to
        // contain. All three are no-ops returning false when persistence is
        // off (read-only world, or no save folder), because m_anvilIo is then
        // simply null — the same shape the terrain path uses.
        //
        // Callable from any thread: AnvilChunkIo holds the mutex.
        bool ReadEntityChunkNbt (Math::ChunkPos pos, std::vector<uint8_t>& out, std::string& error);
        bool WriteEntityChunkNbt(Math::ChunkPos pos, const std::vector<uint8_t>& payload,
                                 std::string& error);
        bool ClearEntityChunk   (Math::ChunkPos pos, std::string& error);
        bool EntityPersistenceEnabled() const { return m_anvilIo != nullptr; }

    private:
        // Configuration
        ChunkProviderConfig m_config;
        mutable std::mutex m_configMutex;

        // Core components
        std::unique_ptr<ChunkCache> m_chunkCache;
        std::unique_ptr<MinecraftChunkLoaderImpl> m_chunkLoader;
        std::unique_ptr<IChunkGenerator> m_chunkGenerator;  // Changed to interface type to support different generators
        std::shared_ptr<IChunkSaver> m_chunkSaver;

        // The Anvil stack for an ObeyCraft save. One AnvilChunkIo serves both
        // directions so a relocating write can never leave the reader looking
        // at a freed sector.
        std::shared_ptr<Anvil::AnvilChunkIo>    m_anvilIo;
        std::unique_ptr<Anvil::AnvilChunkLoader> m_anvilLoader;
        // The same object m_chunkSaver points at, kept concretely so
        // SetGameTime does not need a dynamic_cast through IChunkSaver. Null
        // when the world has no save path (m_chunkSaver may still be a
        // different implementation).
        std::shared_ptr<Anvil::AnvilChunkStorage> m_anvilStorage;

        // See SetChunkTicksLoadedCallback. Assigned once during world setup,
        // before any load can run, so it needs no synchronisation of its own.
        std::function<void(Math::ChunkPos)> m_onChunkTicksLoaded;

        std::unique_ptr<DirtyTracker> m_dirtyTracker;

        // State
        std::atomic<bool> m_initialized{false};

        // Statistics
        mutable std::mutex m_statsMutex;
        ChunkProviderStats m_stats;

        // === INTERNAL WORKFLOWS ===

        // `outWasGenerated` says whether this came from the generator rather
        // than from disk. An out-param rather than a member: GetChunk runs on
        // every chunk worker at once, so a shared flag would be both a data
        // race and, worse, one thread consuming another thread's answer.
        std::shared_ptr<Chunk> LoadChunkInternal(Math::ChunkPos position, bool& outWasGenerated);
        std::shared_ptr<Chunk> TryLoadFromCache(Math::ChunkPos position);
        std::shared_ptr<Chunk> TryLoadFromDisk(Math::ChunkPos position);
        std::shared_ptr<Chunk> TryGenerateChunk(Math::ChunkPos position);
        std::shared_ptr<Chunk> CompleteChunkLoad(std::shared_ptr<Chunk> chunk, bool wasGenerated);

        // === COORDINATION ===

        void SetupComponentDependencies();
        void ConfigureComponents();

        void OnChunkEvicted(Math::ChunkPos position, std::shared_ptr<Chunk> chunk, bool wasDirty);

        // === VALIDATION ===

        bool ValidateChunk(const std::shared_ptr<Chunk>& chunk) const;
        bool ValidateChunkPosition(Math::ChunkPos position) const;
        bool ValidateWorldPosition(int worldX, int worldY, int worldZ) const;

        // === HELPERS ===

        void WorldToLocalCoords(int worldX, int worldY, int worldZ, Math::ChunkPos& chunkPos,
                               int& localX, int& localY, int& localZ) const;

        bool IsValidBlockID(BlockID block) const;
        bool GetBlockProperties(BlockID block, bool& isSolid, bool& isFluid, bool& isTransparent) const;

        void LogError(const std::string& operation, const std::string& error) const;

        // === MOVE SEMANTICS ===

        void MoveFrom(ChunkProvider&& other) noexcept;
    };

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<ChunkProvider> CreateChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});

    ChunkProviderConfig CreateDefaultConfig();

} // namespace Game