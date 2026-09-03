// File: src/client/world/ClientChunkManager.hpp
#pragma once
#include <list>

#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/network/PacketTypes.hpp"
#include "../renderer/core/Frustum.hpp"
#include "PendingDiffsManager.hpp"
#include "BlockStatePrediction.hpp"
#include <glad/glad.h>

// Include mesh job data types
#include "../renderer/mesh/MeshJobData.hpp"
#include "../renderer/mesh/RenderRegionCache.hpp"
#include "../renderer/mesh/SectionMesh.hpp"  // For GPUSectionData
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>
#include <array>
#include <vector>
#include <glm/glm.hpp>
#include "common/world/level/DimensionId.hpp"

namespace Render {
    class ClientMeshManager;
    class ChunkRenderer;
}

namespace Client {

    // Main thread assertion for debug builds
    #ifdef DEBUG
    #define ASSERT_MAIN_THREAD() do { \
        static std::thread::id s_mainThreadId = std::this_thread::get_id(); \
        assert(std::this_thread::get_id() == s_mainThreadId && "ClientChunkManager must be accessed from main thread only"); \
    } while(0)
    #else
    #define ASSERT_MAIN_THREAD()
    #endif

    // Simplified chunk state machine for client-side chunks
    enum class ChunkState {
        UNLOADED,   // Not present on client
        LOADED,     // ChunkDataS2CPacket received, chunk data available
    };
    
    // Section state tracking for mesh building
    enum class SectionState {
        LOADED,    // Has block data, can be meshed
        MESHING,   // Snapshot sent to worker
        READY      // Mesh built and uploaded
    };
    
    // Mesh acceptance decisions
    enum class MeshApplyAction {
        Upload,              // Good to upload to GPU
        Drop_StaleVersion,   // Version mismatch - will reschedule
        Drop_Unloaded,       // Chunk/section gone
        Drop_Replaced        // Superseded by newer mesh
    };
    
    // Result of accepting a mesh build
    struct MeshAcceptance {
        MeshApplyAction action;
        // Additional data could go here for upload
    };
    
    // Mesh job types for different processing paths
    enum class MeshJobType {
        Full,        // Normal meshing for non-empty sections
        BorderOnly   // Fast path for empty sections - only compute neighbor mask
    };
    
    // Per-section tracking info
    struct SectionInfo {
        SectionState state = SectionState::LOADED;
        uint32_t version = 0;         // Incremented on block changes
        uint32_t meshingVersion = 0;  // Version being meshed
        bool dirty = false;           // Needs remeshing
        // Version of the section CONTENT the currently-uploaded GPU mesh (and
        // its visibility mask) was built from — set in FinalizeSectionUpload
        // from MeshBuildResult::generation. version != uploadedVersion means
        // whatever is on the GPU (mask included) describes blocks that no
        // longer exist. Unlike `dirty`, which is cleared when the mesh JOB is
        // *scheduled* (seconds before its result uploads during a cascade),
        // this only advances when the result actually lands, so the occlusion
        // BFS keys its stale-mask-is-see-through rule off this. Keying it off
        // `dirty` made rebuilds flap between see-through and stale-solid while
        // remeshes were in flight — whole regions flashing during a TNT
        // cascade.
        uint32_t uploadedVersion = 0;
        bool hasCpuData = false;      // True when we have valid CPU view
        // MIRROR of the live ChunkSection's hasOnlyAir. MC reads
        // LevelChunk.getSection(i).hasOnlyAir() directly; this is a cached copy
        // because SectionOcclusionGraph reads it once per cell over the whole
        // render-distance cube every rebuild, and chasing chunkData ->
        // GetSection there costs more than keeping the copy true.
        //
        // INVARIANT: every path that writes a block into ClientChunk::chunkData
        // must re-derive this — call ClientChunkManager::RefreshSectionEmptiness.
        // It gates `cell.renderable` in SectionOcclusionGraph, so a section left
        // wrongly marked all-air is meshed, uploaded, and then never emitted
        // into the draw list: the block is solid, collides, and is invisible.
        bool isAllAir = true;         // True when section contains only air

        // MC CompiledSectionMesh.EMPTY, as distinct from UNCOMPILED.
        //
        // An all-air section is deliberately never compiled — there is nothing
        // to draw and queueing it would mean 24 mesh jobs per chunk instead of
        // the ~8 that can produce geometry. MC makes the same call, and then
        // compensates for it: SectionOcclusionGraph.java:254-260 skips adding
        // an empty section to the draw tree but does
        //     sectionMesh.compareAndSet(UNCOMPILED, EMPTY)
        // in the else branch, so LevelRenderer.isSectionCompiledAndVisible's
        // `!= UNCOMPILED` test still passes for it.
        //
        // We had the skip without the compensation, and it deadlocked the join:
        // a player who spawns in mid-air stands in an all-air section, which is
        // therefore never dirty, never scheduled and never builtOnce — so
        // LevelLoadTracker waited its full 30 s timeout before letting them
        // move. Reproduced exactly: spawn at y=163.54 in a column whose
        // section 160..175 is a single-value air palette.
        bool meshResolvedEmpty = false;
        uint8_t lastNeighborMask = 0; // Which neighbors were present during last mesh (PX=1, NX=2, PZ=4, NZ=8)
        // Greedy-debug palette generation the uploaded mesh was built under
        // (Mesher::GreedyPaletteGen at build start). A parked chunk revived
        // with a stale stamp is re-dirtied in RestoreRetainedChunk — the
        // toggle's RemeshAll only reaches ACTIVE sections, and this is what
        // kept debug-colored meshes alive past the toggle on revisits.
        uint32_t builtPaletteGen = 0;
        // Total order over this section's mesh jobs, independent of `version`.
        // Two jobs CAN legitimately share a version: every re-dirty that
        // resets meshingVersion (convergence nets, NoteMeshBuildFailed,
        // ApplyChunkData resends) reschedules at the same content version, so
        // the strictly-less generation guard in AcceptMeshResult cannot order
        // them — whichever finished LAST won, even when built from older
        // data. That race is the 2026-08-30 hole/ghost bug: an empty result
        // landing after a solid one leaves a hole; the reverse leaves ghost
        // terrain. lastJobSeq increments per schedule (main thread), a result
        // carries its seq, and accept drops anything older than uploadedJobSeq.
        uint32_t lastJobSeq = 0;
        uint32_t uploadedJobSeq = 0;
        bool builtOnce = false;       // True after first successful build
        // MC's RenderSection.isDirtyFromPlayer — set when THIS client edited a
        // block in the section, cleared when it is scheduled. Drives the
        // "Semi Blocking" / "Fully Blocking" chunk-builder modes, which compile
        // player edits on the main thread so they appear the same frame.
        bool dirtyFromPlayer = false;
        
        // Per-task cancellation: reference to last submitted mesh job
        std::shared_ptr<::Client::Render::MeshJobData> lastMeshJob;

        // NEW: Direct GPU data ownership (render thread only)
        std::atomic<::Render::GPUSectionData*> gpuData{nullptr};
        
    };

    // Client-side chunk data
    struct ClientChunk {
        Game::Math::ChunkPos position;
        std::shared_ptr<Game::Chunk> chunkData;
        ChunkState state = ChunkState::UNLOADED;
        
        // Generation tracking for staleness control
        uint32_t generation = 0;
        
        // Timing information
        std::chrono::steady_clock::time_point loadTime;
        
        // Per-section state tracking (24 sections per chunk)
        std::array<SectionInfo, 24> sectionInfos;
        
        // Legacy dirty section tracking (to be phased out)
        std::unordered_set<int> dirtySections;

        // WORLD-space positions of every BlockID::EndPortal in this chunk.
        //
        // The end portal's model has no elements — vanilla draws it from a
        // dedicated screen-projected pass, not from the chunk mesh — so
        // Render::EndPortalRenderer needs some way to find the blocks, and it
        // cannot use the block-entity map the other BE renderers walk: block
        // entities here are only ever created by World::SetBlock, so a
        // stronghold portal that arrived inside a chunk packet has none.
        //
        // Maintained entirely by ClientChunkManager: rebuilt wholesale when
        // chunk data lands, patched in place by SetBlockLocal. Empty for all
        // but a handful of chunks in a world, which is what makes the
        // renderer's per-chunk cull a size() check.
        std::vector<glm::ivec3> endPortals;
        // Same contract for END GATEWAYS — the block renders invisible in the
        // chunk mesh (MC RenderShape.INVISIBLE) and EndPortalRenderer draws
        // its starfield cube off this index.
        std::vector<glm::ivec3> endGateways;

        ClientChunk(Game::Math::ChunkPos pos) 
            : position(pos), loadTime(std::chrono::steady_clock::now())
            {
        }
        bool IsLoaded() const { return state == ChunkState::LOADED; }
    };

    // Client chunk manager (mirrors Minecraft's ClientChunkManager)
    class ClientChunkManager {
    public:
        ClientChunkManager();
        ~ClientChunkManager();

        // Non-copyable, non-movable
        ClientChunkManager(const ClientChunkManager&) = delete;
        ClientChunkManager& operator=(const ClientChunkManager&) = delete;

        // ========================================================================
        // LIFECYCLE
        // ========================================================================

        void Initialize();
        void Shutdown();

        // Which dimension this manager holds. Stamped onto every mesh job so
        // the result comes back to THIS level's mesh manager and not to
        // whichever level the globals point at when it lands.
        void SetDimension(Game::DimensionId d) { m_dimension = d; }
        Game::DimensionId Dimension() const { return m_dimension; }

        // The mesh manager and renderer of the SAME level. Set once by
        // ClientLevel after all three exist; never the globals, which may be
        // bound to another level while a packet or portal view is applied.
        void SetPeers(::Render::ClientMeshManager* meshes, ::Render::ChunkRenderer* renderer) {
            m_meshes = meshes;
            m_renderer = renderer;
        }

        // ========================================================================
        // CHUNK STATE MANAGEMENT
        // ========================================================================

        // Process ChunkDataS2CPacket (UNLOADED → LOADED)
        void ProcessChunkDataS2CPacket(const Network::ChunkDataS2CPacket& packet);
        // Unpack the wire containers into a Game::Chunk. Pure and thread-safe;
        // called on the network I/O thread by ClientConnection::DecodePacket.
        static std::shared_ptr<Game::Chunk> PrebuildChunk(const Network::ChunkDataS2CPacket& packet);
        
        // Apply chunk data with generation tracking
        void ApplyChunkData(Game::Math::ChunkPos chunkPos, const Network::ChunkDataS2CPacket& packet);
        
        // Load chunk from serialized data
        
        // Process block change packet
        void ProcessBlockChange(const Network::BlockChangeS2CPacket& packet);

        // Clear all chunks
        void ClearAllChunks();

        // ========================================================================
        // CLIENT-SIDE BLOCK PREDICTION (MC ClientLevel + BlockStatePredictionHandler)
        // ========================================================================

        // Apply a locally-predicted block change immediately (world write +
        // remesh) and remember the state to roll back to if the server
        // disagrees. `sequence` must be the same sequence carried by the C2S
        // packet for this interaction — that's what the server's
        // BlockChangedAckS2C refers to.
        void PredictBlockChange(const glm::ivec3& pos, Game::BlockID newBlock, uint32_t sequence,
                                Game::BlockStateIndex stateIndex = 0);

        // BlockChangedAckS2C arrived: retire predictions up to `sequence`,
        // snapping back anywhere the server disagreed with us.
        void HandleBlockChangedAck(uint32_t sequence);

        // Read the live client block state (Air when the chunk isn't loaded).
        Game::BlockID GetBlockAt(const glm::ivec3& pos) const;

        // Block + block-state index together (0 when the chunk isn't loaded).
        std::pair<Game::BlockID, Game::BlockStateIndex> GetBlockAndStateAt(const glm::ivec3& pos) const;

        // Write a block into the client's chunk store and mark the affected
        // sections dirty. This is the shared body behind ProcessBlockChange,
        // predictions and rollbacks.
        // fromPlayer marks the touched sections as MC's isDirtyFromPlayer, which
        // the "Semi Blocking"/"Fully Blocking" chunk-builder modes compile on the
        // main thread so the edit shows up the same frame.
        void SetBlockLocal(const glm::ivec3& pos, Game::BlockID blockId, Game::BlockStateIndex stateIndex = 0,
                           bool fromPlayer = false);
        

        // Re-derive SectionInfo::isAllAir for one section from the live
        // ChunkSection, and seed an occlusion-graph propagation if it changed.
        //
        // MUST be called by every path that writes a block into
        // ClientChunk::chunkData. The flag is the renderer's only record of
        // whether a section is worth drawing (SectionOcclusionGraph gates
        // `cell.renderable` on it) and it used to be written ONLY at chunk-load
        // time, so the first block placed into a section that was air when the
        // chunk arrived was stored, collided, and then never meshed OR drawn.
        void RefreshSectionEmptiness(ClientChunk& chunk, int sectionY);

        // Mark individual section dirty for mesh rebuilding
        void MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY, bool fromPlayer = false);

        // Mark entire chunk dirty (all 24 sections) for mesh rebuilding
        void MarkChunkDirty(Game::Math::ChunkPos chunkPos);
        
        // Clear dirty flag for a section (called when mesh build completes)
        void ClearSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY);

        // Mark border sections of neighbor chunks as dirty when a chunk loads/unloads
        void MarkNeighborSectionsDirty(Game::Math::ChunkPos chunkPos);

        // ========================================================================
        // CHUNK ACCESS
        // ========================================================================

        // Get client chunk by position
        ClientChunk* GetChunk(Game::Math::ChunkPos chunkPos);

        // Biome id at a world position, across chunk borders. 0 = fallback.
        uint16_t BiomeAtWorld(int worldX, int worldY, int worldZ);
        const ClientChunk* GetChunk(Game::Math::ChunkPos chunkPos) const;

        // Check chunk state
        ChunkState GetChunkState(Game::Math::ChunkPos chunkPos) const;
        bool IsChunkLoaded(Game::Math::ChunkPos chunkPos) const;
        
        // NEW: Direct section access for lock-free rendering
        SectionInfo* GetSectionInfo(Game::Math::ChunkPos chunkPos, int sectionY);
        const SectionInfo* GetSectionInfo(Game::Math::ChunkPos chunkPos, int sectionY) const;


        // ========================================================================
        // CHUNK UNLOADING
        // ========================================================================

        // Force unload specific chunk
        void UnloadChunk(Game::Math::ChunkPos chunkPos);

        // Retention cache (instant revisits). UnloadChunk parks the chunk
        // (CPU data + GPU meshes) instead of freeing it; a later
        // ChunkUnchangedS2C with a matching Chunk::modStamp revives it with
        // no transfer and no meshing. Bounded by m_retainBudgetBytes, LRU.
        bool RestoreRetainedChunk(Game::Math::ChunkPos pos, uint64_t modStamp);
        size_t GetRetainedChunkCount() const { return m_retained.size(); }
        size_t GetRetainedBytes() const { return m_retainedBytes; }
        size_t m_retainRestored = 0;


        // ========================================================================
        // BASIC STATISTICS
        // ========================================================================

        size_t GetLoadedChunkCount() const;
        void GetSectionStats(size_t& totalSections, size_t& readySections, 
                            size_t& meshingSections, size_t& dirtySections) const;

        struct ClientChunkStats {
            size_t totalChunks = 0;
            size_t loadedChunks = 0;
            
            void Reset() {
                totalChunks = loadedChunks = 0;
            }
        };
        
        // ========================================================================
        // RENDER GRID SYNCHRONIZATION
        // ========================================================================
        
        // Thread-safe snapshot of loaded chunks for initial RenderGrid sync
        void SnapshotLoadedChunks(std::vector<std::pair<Game::Math::ChunkPos, ClientChunk*>>& out) const;
        
        // Notify RenderGrid when chunks/sections change
        void NotifyRenderGridChunkLoaded(Game::Math::ChunkPos pos, ClientChunk* chunk);
        void NotifyRenderGridChunkUnloaded(Game::Math::ChunkPos pos);
        void NotifyRenderGridSectionUpdated(Game::Math::ChunkPos pos, int sectionY, ::Render::GPUSectionData* gpu);

    private:
        // Chunk storage - main thread only, no mutex needed
        std::unordered_map<Game::Math::ChunkPos, std::unique_ptr<ClientChunk>, Game::Math::ChunkPosHash> m_chunks;

        // Index of chunks that (may) have dirty sections — lets the mesh
        // scheduler iterate only chunks with work instead of every loaded chunk
        // at 30Hz. Inserted wherever dirtySections gains an entry; entries whose
        // chunk is gone or fully clean are lazily erased during scheduling, so
        // erase sites don't need to maintain it.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> m_chunksWithDirtySections;
        struct Retained {
            std::unique_ptr<ClientChunk> chunk;
            size_t bytes = 0;
            std::list<Game::Math::ChunkPos>::iterator lru;
        };
        std::unordered_map<Game::Math::ChunkPos, Retained, Game::Math::ChunkPosHash> m_retained;
        std::list<Game::Math::ChunkPos> m_retainedLru;   // front = most recent
        size_t m_retainedBytes = 0;
        // Sized from physical RAM (see ComputeRetainBudgetBytes): 1.5 GB on
        // a 16 GB+ machine, a tenth of RAM below that. A parked chunk holds
        // its GPU meshes, and on every Mac and every integrated-GPU PC the
        // GPU's memory IS this RAM — a flat 1.5 GB was most of an 8 GB
        // Intel MacBook's graphics budget spent on chunks the player had
        // walked away from.
        const size_t m_retainBudgetBytes = ComputeRetainBudgetBytes();
        static size_t ComputeRetainBudgetBytes();
        void DiscardRetained(Game::Math::ChunkPos pos);
        uint32_t m_schedulerSkip = 0;   // idle backoff, see ScheduleMeshBuildsWithSnapshots
        // See MarkSectionDirty: counts section dirties toward a forced
        // full visibility rebuild during mass destruction.
        int m_dirtyMarksSinceRebuild = 0;

        // Pending diffs for chunks that haven't arrived yet
        std::unique_ptr<PendingDiffsManager> m_pendingDiffs;

        // Outstanding client-side block predictions (MC ClientLevel's
        // blockStatePredictionHandler). Hooks are bound in Initialize().
        BlockStatePrediction m_prediction;
        
        // Generation counter for staleness control
        std::atomic<uint32_t> m_nextGeneration{1};

        // (No mesh-schedule throttle any more — matching MC, which compiles
        // sections every frame. Rate limiting is the upload permit pool, taken
        // by the mesh workers when a job starts. See
        // ScheduleMeshBuildsWithSnapshots.)

        // Persistent candidate buffer — reused across calls to avoid per-call heap allocation
        struct SectionCandidate {
            Game::Math::ChunkPos chunkPos;
            int sectionY;
            float effectiveDistSq;  // Squared distance (no sqrt needed for sorting)
            ClientChunk* chunk;     // Cached pointer — avoids redundant m_chunks.find() in submission
        };
        std::vector<SectionCandidate> m_meshCandidates;


        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Deserialize chunk data from packet

        // Transition chunk state
        void TransitionChunkState(ClientChunk* chunk, ChunkState newState);
        
        // Apply pending diffs after chunk load
        void ApplyPendingDiffsForChunk(Game::Math::ChunkPos chunkPos, ClientChunk* chunk);

        // Rescan a chunk and rebuild ClientChunk::endPortals from scratch.
        // Cheap despite being a full scan: each section is skipped on a
        // palette-membership test, so only a section that genuinely contains
        // portal blocks pays the 4096-voxel walk.
        static void RebuildEndPortalIndex(ClientChunk& chunk);
        
        // Schedule mesh build for dirty sections
        void ScheduleDirtySectionMeshes();
        
    public:
        // Schedule mesh builds using snapshots (main thread only) - public for ClientMeshManager
        void ScheduleMeshBuildsWithSnapshots(const glm::vec3& playerPosition);

        // MC RenderSection.hasAllNeighbors — are all 8 surrounding chunk columns
        // loaded? A never-compiled section waits for this before it is meshed.
        bool HasAllNeighborChunks(Game::Math::ChunkPos pos) const;
        
        // Build the 3x3x3 region a section is meshed against, with version
        // checking (MC RenderSection.createCompileTask). `regionCache` is shared
        // across one scheduling pass so neighbouring sections reuse each other's
        // copies. Returns false if the section is missing or its version changed
        // during capture.
        bool BuildSectionRegion(Game::Math::ChunkPos chunkPos, int sectionY,
                                uint32_t expectedVersion,
                                Render::RenderRegionCache& regionCache,
                                std::shared_ptr<Render::MeshJobData>& outSnapshot);
        
        // Accept or reject mesh build result based on version and state
        MeshAcceptance AcceptMeshResult(const Network::MeshBuildResult& result);
        
        // Finalize section after successful GPU upload
        void FinalizeSectionUpload(Game::Math::ChunkPos chunkPos, int sectionY, uint8_t neighborMask = 0,
                                   uint32_t builtVersion = 0, uint32_t paletteGen = 0,
                                   uint32_t jobSeq = 0);

        // A mesh result for this section failed or was rejected before upload.
        // If it was the LATEST job for the section, re-dirty so the scheduler
        // retries — otherwise the section reads as "in flight" forever.
        void NoteMeshBuildFailed(Game::Math::ChunkPos chunkPos, int sectionY, uint32_t builtVersion);

    private:
        // Which of the four horizontal neighbour chunks are LOADED right now,
        // in the same bit layout BuildSectionRegion derives from the region
        // (+X=1, -X=2, +Z=4, -Z=8) so it is directly comparable with
        // SectionInfo::lastNeighborMask / MeshBuildResult::neighborMask.
        uint8_t CurrentNeighborMask(Game::Math::ChunkPos pos) const;

        // ── Stale-border-mesh instrumentation ────────────────────────────────
        // A mesh built while a horizontal neighbour chunk was absent treats the
        // missing chunk as air, so solid underground sections grow a wall of
        // border quads (nearly 2x GPU sections in the worst orderings). These
        // count, cumulatively per session:
        //   m_partialNeighborBuilds — uploads whose mesh was built with >=1
        //     horizontal neighbour missing (plotted as
        //     "Mesh/PartialNeighborBuilds" in Tracy).
        //   m_staleBorderRemeshes — sections re-dirtied because a neighbour
        //     that was missing when they were meshed is present now (the
        //     convergence paths in FinalizeSectionUpload and
        //     RestoreRetainedChunk). Summarised by Shutdown().
        size_t m_partialNeighborBuilds = 0;
        size_t m_staleBorderRemeshes = 0;

        Game::DimensionId          m_dimension = Game::DimensionId::Overworld;
        ::Render::ClientMeshManager* m_meshes   = nullptr;
        ::Render::ChunkRenderer*     m_renderer = nullptr;
    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // The chunk manager of the level the globals are BOUND to — see
    // ClientLevel.hpp. Not owned here: ClientLevel owns it, ClientLevels
    // rebinds this pointer. Null outside a session.
    extern ClientChunkManager* g_clientChunkManager;

    // Direct access functions
    ClientChunk* GetClientChunk(Game::Math::ChunkPos chunkPos);
    ChunkState GetClientChunkState(Game::Math::ChunkPos chunkPos);
    bool IsClientChunkLoaded(Game::Math::ChunkPos chunkPos);

} // namespace Client