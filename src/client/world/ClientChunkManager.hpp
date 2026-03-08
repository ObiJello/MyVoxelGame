// File: src/client/world/ClientChunkManager.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/network/PacketTypes.hpp"
#include "../renderer/core/Frustum.hpp"
#include "PendingDiffsManager.hpp"
#include <glad/glad.h>

// Include mesh job data types
#include "../renderer/mesh/MeshJobData.hpp"
#include "../renderer/mesh/SectionMesh.hpp"  // For GPUSectionData
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <thread>
#include <cassert>
#include <array>

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
        bool hasCpuData = false;      // True when we have valid CPU view
        bool isAllAir = true;         // True when section contains only air
        uint8_t lastNeighborMask = 0; // Which neighbors were present during last mesh (PX=1, NX=2, PZ=4, NZ=8)
        bool builtOnce = false;       // True after first successful build
        
        // Per-task cancellation: reference to last submitted mesh job
        std::shared_ptr<::Client::Render::MeshJobData> lastMeshJob;

        // NEW: Direct GPU data ownership (render thread only)
        std::atomic<::Render::GPUSectionData*> gpuData{nullptr};
        
        // NEW: For deferred deletion (deleted after 2 frames to avoid use-after-free)
        std::atomic<::Render::GPUSectionData*> pendingDelete{nullptr};
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
        std::chrono::steady_clock::time_point lastAccessTime;
        
        // Per-section state tracking (24 sections per chunk)
        std::array<SectionInfo, 24> sectionInfos;
        
        // Legacy dirty section tracking (to be phased out)
        std::unordered_set<int> dirtySections;
        
        ClientChunk(Game::Math::ChunkPos pos) 
            : position(pos), loadTime(std::chrono::steady_clock::now())
            , lastAccessTime(std::chrono::steady_clock::now()) {
        }
        bool IsLoaded() const { return state == ChunkState::LOADED; }
        void UpdateAccessTime() { lastAccessTime = std::chrono::steady_clock::now(); }
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

        // ========================================================================
        // CHUNK STATE MANAGEMENT
        // ========================================================================

        // Process ChunkDataS2CPacket (UNLOADED → LOADED)
        void ProcessChunkDataS2CPacket(const Network::ChunkDataS2CPacket& packet);
        
        // Apply chunk data with generation tracking
        void ApplyChunkData(Game::Math::ChunkPos chunkPos, const Network::ChunkDataS2CPacket& packet);
        
        // Load chunk from serialized data
        void LoadChunk(Game::Math::ChunkPos chunkPos, const Network::SerializedChunkData& serializedData);
        
        // Process block change packet
        void ProcessBlockChange(const Network::BlockChangeS2CPacket& packet);
        
        // Clear all chunks
        void ClearAllChunks();
        

        // Mark individual section dirty for mesh rebuilding
        void MarkSectionDirty(Game::Math::ChunkPos chunkPos, int sectionY);

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
        
        // Pending diffs for chunks that haven't arrived yet
        std::unique_ptr<PendingDiffsManager> m_pendingDiffs;
        
        // Generation counter for staleness control
        std::atomic<uint32_t> m_nextGeneration{1};


        // ========================================================================
        // INTERNAL METHODS
        // ========================================================================

        // Deserialize chunk data from packet
        std::shared_ptr<Game::Chunk> DeserializeChunkData(const Network::SerializedChunkData& serializedData);

        // Transition chunk state
        void TransitionChunkState(ClientChunk* chunk, ChunkState newState);
        
        // Apply pending diffs after chunk load
        void ApplyPendingDiffsForChunk(Game::Math::ChunkPos chunkPos, ClientChunk* chunk);
        
        // Schedule mesh build for dirty sections
        void ScheduleDirtySectionMeshes();
        
    public:
        // Schedule mesh builds using snapshots (main thread only) - public for ClientMeshManager
        void ScheduleMeshBuildsWithSnapshots(const glm::vec3& playerPosition);
        
        // Build snapshot for a single section with version checking (Minecraft-style)
        // Returns false if section missing or version changed during capture
        bool BuildSectionSnapshot(Game::Math::ChunkPos chunkPos, int sectionY, 
                                 uint32_t expectedVersion, 
                                 std::shared_ptr<Render::MeshJobData>& outSnapshot);
        
        // Helper to copy neighbor data for a section
        void CopyNeighborData(Game::Math::ChunkPos chunkPos, int sectionY, 
                            Render::SectionSnapshot& snapshot);
        
        // Accept or reject mesh build result based on version and state
        MeshAcceptance AcceptMeshResult(const Network::MeshBuildResult& result);
        
        // Finalize section after successful GPU upload
        void FinalizeSectionUpload(Game::Math::ChunkPos chunkPos, int sectionY, uint8_t neighborMask = 0);
        
    private:

    };

    // ========================================================================
    // GLOBAL ACCESS
    // ========================================================================

    // Global client chunk manager instance
    extern std::unique_ptr<ClientChunkManager> g_clientChunkManager;

    // Convenience functions
    void InitializeClientChunkManager();
    void ShutdownClientChunkManager();

    // Direct access functions
    ClientChunk* GetClientChunk(Game::Math::ChunkPos chunkPos);
    ChunkState GetClientChunkState(Game::Math::ChunkPos chunkPos);
    bool IsClientChunkLoaded(Game::Math::ChunkPos chunkPos);

} // namespace Client