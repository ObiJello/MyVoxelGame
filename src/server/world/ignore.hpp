/* File: src/server/world/ServerChunkProvider.hpp
#pragma once

#include "ChunkProvider.hpp"
#include "ServerWorkerPool.hpp"
#include "client/NetworkManager.hpp"
#include <unordered_set>
#include <unordered_map>

namespace Game {

    // Enhanced chunk provider for server-side operation
    // Coordinates with ServerWorkerPool for async chunk loading/generation
    class ServerChunkProvider : public ChunkProvider {
    public:
        explicit ServerChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});
        ~ServerChunkProvider();

        // ========================================================================
        // LIFECYCLE (ENHANCED)
        // ========================================================================

        bool Initialize();
        void Shutdown();

        // ========================================================================
        // SERVER-SIDE CHUNK COORDINATION
        // ========================================================================

        // Request chunk loading (may be async via ServerWorkerPool)
        void RequestChunkLoad(Math::ChunkPos position, int priority = 0);

        // Process completed chunk generation results from ServerWorkerPool
        void ProcessChunkGenResults();

        // Get chunks ready to be sent to clients
        std::vector<Math::ChunkPos> GetChunksReadyForClient() const;

        // Mark chunk as sent to client
        void MarkChunkSentToClient(Math::ChunkPos position);

        // Check if chunk is ready to send to client
        bool IsChunkReadyForClient(Math::ChunkPos position) const;

        // ========================================================================
        // ASYNC LOADING COORDINATION
        // ========================================================================

        // Enable/disable async loading via ServerWorkerPool
        void SetAsyncLoadingEnabled(bool enabled) { m_asyncLoadingEnabled = enabled; }
        bool IsAsyncLoadingEnabled() const { return m_asyncLoadingEnabled; }

        // Set priority for chunk loading
        void SetChunkLoadPriority(Math::ChunkPos position, int priority);

        // Get pending chunk load requests
        std::vector<Math::ChunkPos> GetPendingChunkLoads() const;

        // Cancel pending chunk load
        void CancelChunkLoad(Math::ChunkPos position);

        // ========================================================================
        // CLIENT MANAGEMENT
        // ========================================================================

        // Track which chunks each client needs (for multiplayer support)
        void AddClientChunkInterest(uint32_t clientId, Math::ChunkPos position);
        void RemoveClientChunkInterest(uint32_t clientId, Math::ChunkPos position);
        
        // Get chunks interested by clients
        std::vector<Math::ChunkPos> GetChunksWithClientInterest() const;

        // ========================================================================
        // ENHANCED STATISTICS
        // ========================================================================

        struct ServerChunkStats {
            size_t chunksRequested = 0;
            size_t chunksLoadedSync = 0;
            size_t chunksLoadedAsync = 0;
            size_t chunksGenerated = 0;
            size_t chunksSentToClients = 0;
            size_t pendingAsyncLoads = 0;
            size_t failedAsyncLoads = 0;
            
            void Reset() {
                chunksRequested = chunksLoadedSync = chunksLoadedAsync = chunksGenerated = 0;
                chunksSentToClients = pendingAsyncLoads = failedAsyncLoads = 0;
            }
        };

        ServerChunkStats GetServerStats() const;
        void ResetServerStats();
        void LogServerStats() const;

    private:
        // Server-side configuration
        bool m_asyncLoadingEnabled = true;

        // Chunk load tracking
        mutable std::mutex m_loadTrackingMutex;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_pendingAsyncLoads;
        std::unordered_map<Math::ChunkPos, int, Math::ChunkPosHash> m_chunkLoadPriorities;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_chunksReadyForClient;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_chunksSentToClient;

        // Client interest tracking (for multiplayer support)
        mutable std::mutex m_clientInterestMutex;
        std::unordered_map<uint32_t, std::unordered_set<Math::ChunkPos, Math::ChunkPosHash>> m_clientChunkInterest;

        // Server statistics
        mutable std::mutex m_serverStatsMutex;
        ServerChunkStats m_serverStats;

        // ========================================================================
        // OVERRIDDEN METHODS
        // ========================================================================

        // Override GetChunk to support async loading
        std::shared_ptr<Chunk> GetChunk(Math::ChunkPos position);

        // ========================================================================
        // SERVER-SIDE INTERNAL METHODS
        // ========================================================================

        // Try to load chunk synchronously (fallback)
        std::shared_ptr<Chunk> LoadChunkSync(Math::ChunkPos position);

        // Submit async chunk load/generation job
        void SubmitAsyncChunkLoad(Math::ChunkPos position, int priority);

        // Process single chunk generation result
        void ProcessChunkGenResult(const Network::ChunkGenResult& result);

        // Check if chunk should be loaded asynchronously
        bool ShouldLoadAsync(Math::ChunkPos position) const;

        // Get load priority for chunk
        int GetChunkLoadPriority(Math::ChunkPos position) const;

        // Update server statistics
        void UpdateServerStats(const std::string& operation, bool success = true);

        // Validate server state
        bool ValidateServerState() const;
    };

    // ========================================================================
    // FACTORY FUNCTIONS
    // ========================================================================

    // Create server-enhanced chunk provider
    std::unique_ptr<ServerChunkProvider> CreateServerChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});

    // ========================================================================
    // GLOBAL ACCESS FOR SERVER
    // ========================================================================

    // Global server chunk provider instance
    extern std::unique_ptr<ServerChunkProvider> g_serverChunkProvider;

    // Convenience functions for server integration
    void InitializeServerChunkProvider(const ChunkProviderConfig& config = ChunkProviderConfig{});
    void ShutdownServerChunkProvider();

    // Server chunk operations
    void RequestServerChunkLoad(Math::ChunkPos position, int priority = 0);
    void ProcessServerChunkGenResults();
    std::vector<Math::ChunkPos> GetServerChunksReadyForClient();

} // namespace Game*/