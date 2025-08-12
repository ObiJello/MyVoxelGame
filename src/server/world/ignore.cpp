/* File: src/server/world/ServerChunkProvider.cpp
#include "ServerChunkProvider.hpp"
#include "common/core/Log.hpp"
#include "ServerWorkerPool.hpp"
#include "client/NetworkManager.hpp"
#include <algorithm>

namespace Game {

    // Global instance
    std::unique_ptr<ServerChunkProvider> g_serverChunkProvider = nullptr;

    ServerChunkProvider::ServerChunkProvider(const ChunkProviderConfig& config)
        : ChunkProvider(config) {
        Log::Info("ServerChunkProvider created with async loading %s", 
                 m_asyncLoadingEnabled ? "enabled" : "disabled");
    }

    ServerChunkProvider::~ServerChunkProvider() {
        Shutdown();
        Log::Info("ServerChunkProvider destroyed");
    }

    bool ServerChunkProvider::Initialize() {
        Log::Info("Initializing ServerChunkProvider...");
        
        // Initialize base ChunkProvider first
        if (!ChunkProvider::Initialize()) {
            Log::Error("Failed to initialize base ChunkProvider");
            return false;
        }
        
        // Reset server-specific state
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            m_pendingAsyncLoads.clear();
            m_chunkLoadPriorities.clear();
            m_chunksReadyForClient.clear();
            m_chunksSentToClient.clear();
        }
        
        {
            std::lock_guard<std::mutex> lock(m_clientInterestMutex);
            m_clientChunkInterest.clear();
        }
        
        ResetServerStats();
        
        Log::Info("ServerChunkProvider initialized successfully");
        return true;
    }

    void ServerChunkProvider::Shutdown() {
        Log::Info("Shutting down ServerChunkProvider...");
        
        // Cancel all pending async loads
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            for (const auto& chunkPos : m_pendingAsyncLoads) {
                if (Threading::g_serverWorkerPool) {
                    Threading::g_serverWorkerPool->CancelChunkJobs(chunkPos);
                }
            }
            m_pendingAsyncLoads.clear();
        }
        
        // Log final statistics
        LogServerStats();
        
        // Shutdown base ChunkProvider
        ChunkProvider::Shutdown();
        
        Log::Info("ServerChunkProvider shutdown complete");
    }

    // ========================================================================
    // SERVER-SIDE CHUNK COORDINATION
    // ========================================================================

    void ServerChunkProvider::RequestChunkLoad(Math::ChunkPos position, int priority) {
        UpdateServerStats("chunk_requested");
        
        // Check if chunk is already loaded
        if (IsChunkLoaded(position)) {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            m_chunksReadyForClient.insert(position);
            Log::Debug("Chunk (%d, %d) already loaded, marked ready for client", position.x, position.z);
            return;
        }
        
        // Check if already pending
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            if (m_pendingAsyncLoads.find(position) != m_pendingAsyncLoads.end()) {
                // Update priority if higher
                auto it = m_chunkLoadPriorities.find(position);
                if (it != m_chunkLoadPriorities.end() && priority > it->second) {
                    it->second = priority;
                    Log::Debug("Updated priority for pending chunk (%d, %d) to %d", 
                              position.x, position.z, priority);
                }
                return;
            }
        }
        
        if (m_asyncLoadingEnabled && ShouldLoadAsync(position)) {
            // Submit async load job
            SubmitAsyncChunkLoad(position, priority);
        } else {
            // Load synchronously (like vanilla Minecraft integrated server)
            auto chunk = LoadChunkSync(position);
            if (chunk) {
                std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
                m_chunksReadyForClient.insert(position);
                UpdateServerStats("chunk_loaded_sync");
                Log::Debug("Synchronously loaded chunk (%d, %d)", position.x, position.z);
            } else {
                UpdateServerStats("chunk_loaded_sync", false);
                Log::Warning("Failed to synchronously load chunk (%d, %d)", position.x, position.z);
            }
        }
    }

    void ServerChunkProvider::ProcessChunkGenResults() {
        auto& chunkGenQueue = Network::GetChunkGenResultQueue();
        auto results = chunkGenQueue.DrainAll();
        
        for (const auto& result : results) {
            ProcessChunkGenResult(result);
        }
        
        chunkGenQueue.ResetProcessedCount();
    }

    std::vector<Math::ChunkPos> ServerChunkProvider::GetChunksReadyForClient() const {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        
        std::vector<Math::ChunkPos> readyChunks;
        readyChunks.reserve(m_chunksReadyForClient.size());
        
        for (const auto& chunkPos : m_chunksReadyForClient) {
            if (m_chunksSentToClient.find(chunkPos) == m_chunksSentToClient.end()) {
                readyChunks.push_back(chunkPos);
            }
        }
        
        return readyChunks;
    }

    void ServerChunkProvider::MarkChunkSentToClient(Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        m_chunksSentToClient.insert(position);
        UpdateServerStats("chunk_sent_to_client");
        Log::Debug("Marked chunk (%d, %d) as sent to client", position.x, position.z);
    }

    bool ServerChunkProvider::IsChunkReadyForClient(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        return m_chunksReadyForClient.find(position) != m_chunksReadyForClient.end() &&
               m_chunksSentToClient.find(position) == m_chunksSentToClient.end();
    }

    // ========================================================================
    // ASYNC LOADING COORDINATION
    // ========================================================================

    void ServerChunkProvider::SetChunkLoadPriority(Math::ChunkPos position, int priority) {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        m_chunkLoadPriorities[position] = priority;
    }

    std::vector<Math::ChunkPos> ServerChunkProvider::GetPendingChunkLoads() const {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        
        std::vector<Math::ChunkPos> pending;
        pending.reserve(m_pendingAsyncLoads.size());
        for (const auto& chunkPos : m_pendingAsyncLoads) {
            pending.push_back(chunkPos);
        }
        
        return pending;
    }

    void ServerChunkProvider::CancelChunkLoad(Math::ChunkPos position) {
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            m_pendingAsyncLoads.erase(position);
            m_chunkLoadPriorities.erase(position);
        }
        
        // Cancel in ServerWorkerPool
        if (Threading::g_serverWorkerPool) {
            Threading::g_serverWorkerPool->CancelChunkJobs(position);
        }
        
        Log::Debug("Cancelled chunk load for (%d, %d)", position.x, position.z);
    }

    // ========================================================================
    // CLIENT MANAGEMENT
    // ========================================================================

    void ServerChunkProvider::AddClientChunkInterest(uint32_t clientId, Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_clientInterestMutex);
        m_clientChunkInterest[clientId].insert(position);
        Log::Debug("Added client %u interest in chunk (%d, %d)", clientId, position.x, position.z);
    }

    void ServerChunkProvider::RemoveClientChunkInterest(uint32_t clientId, Math::ChunkPos position) {
        std::lock_guard<std::mutex> lock(m_clientInterestMutex);
        auto it = m_clientChunkInterest.find(clientId);
        if (it != m_clientChunkInterest.end()) {
            it->second.erase(position);
            if (it->second.empty()) {
                m_clientChunkInterest.erase(it);
            }
        }
        Log::Debug("Removed client %u interest in chunk (%d, %d)", clientId, position.x, position.z);
    }

    std::vector<Math::ChunkPos> ServerChunkProvider::GetChunksWithClientInterest() const {
        std::lock_guard<std::mutex> lock(m_clientInterestMutex);
        
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> interestedChunks;
        for (const auto& [clientId, chunks] : m_clientChunkInterest) {
            for (const auto& chunkPos : chunks) {
                interestedChunks.insert(chunkPos);
            }
        }
        
        std::vector<Math::ChunkPos> result;
        result.reserve(interestedChunks.size());
        for (const auto& chunkPos : interestedChunks) {
            result.push_back(chunkPos);
        }
        
        return result;
    }

    // ========================================================================
    // ENHANCED STATISTICS
    // ========================================================================

    ServerChunkProvider::ServerChunkStats ServerChunkProvider::GetServerStats() const {
        std::lock_guard<std::mutex> lock(m_serverStatsMutex);
        auto stats = m_serverStats;
        stats.pendingAsyncLoads = m_pendingAsyncLoads.size();
        return stats;
    }

    void ServerChunkProvider::ResetServerStats() {
        std::lock_guard<std::mutex> lock(m_serverStatsMutex);
        m_serverStats.Reset();
    }

    void ServerChunkProvider::LogServerStats() const {
        auto stats = GetServerStats();
        
        Log::Info("ServerChunkProvider Statistics:");
        Log::Info("  Chunks Requested: %zu", stats.chunksRequested);
        Log::Info("  Chunks Loaded (Sync): %zu", stats.chunksLoadedSync);
        Log::Info("  Chunks Loaded (Async): %zu", stats.chunksLoadedAsync);
        Log::Info("  Chunks Generated: %zu", stats.chunksGenerated);
        Log::Info("  Chunks Sent to Clients: %zu", stats.chunksSentToClients);
        Log::Info("  Pending Async Loads: %zu", stats.pendingAsyncLoads);
        Log::Info("  Failed Async Loads: %zu", stats.failedAsyncLoads);
    }

    // ========================================================================
    // OVERRIDDEN METHODS
    // ========================================================================

    std::shared_ptr<Chunk> ServerChunkProvider::GetChunk(Math::ChunkPos position) {
        // For server-side, we might want to request async loading if chunk is not available
        auto chunk = ChunkProvider::GetChunk(position);
        
        if (!chunk && m_asyncLoadingEnabled) {
            // Submit async load request
            RequestChunkLoad(position, 5); // Medium priority
        }
        
        return chunk;
    }

    // ========================================================================
    // SERVER-SIDE INTERNAL METHODS
    // ========================================================================

    std::shared_ptr<Chunk> ServerChunkProvider::LoadChunkSync(Math::ChunkPos position) {
        // Use the base ChunkProvider's synchronous loading mechanism
        return ChunkProvider::GetChunk(position);
    }

    void ServerChunkProvider::SubmitAsyncChunkLoad(Math::ChunkPos position, int priority) {
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            m_pendingAsyncLoads.insert(position);
            m_chunkLoadPriorities[position] = priority;
        }
        
        // Try to load from disk first, then generate if not found
        Threading::SubmitServerChunkLoading(position, priority);
        
        Log::Debug("Submitted async chunk load for (%d, %d) with priority %d", 
                  position.x, position.z, priority);
    }

    void ServerChunkProvider::ProcessChunkGenResult(const Network::ChunkGenResult& result) {
        {
            std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
            m_pendingAsyncLoads.erase(result.position);
            m_chunkLoadPriorities.erase(result.position);
        }
        
        if (result.success && result.chunk) {
            // Add chunk to cache through base ChunkProvider
            // This is a simplified approach - in reality we'd need to coordinate with the cache
            
            {
                std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
                m_chunksReadyForClient.insert(result.position);
            }
            
            UpdateServerStats("chunk_loaded_async");
            Log::Debug("Successfully processed async chunk generation for (%d, %d)", 
                      result.position.x, result.position.z);
        } else {
            UpdateServerStats("chunk_loaded_async", false);
            Log::Warning("Failed async chunk generation for (%d, %d): %s", 
                        result.position.x, result.position.z, result.errorMessage.c_str());
        }
    }

    bool ServerChunkProvider::ShouldLoadAsync(Math::ChunkPos position) const {
        // Simple heuristic: always use async loading for server
        // In a real implementation, this might consider server load, priority, etc.
        return m_asyncLoadingEnabled;
    }

    int ServerChunkProvider::GetChunkLoadPriority(Math::ChunkPos position) const {
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        auto it = m_chunkLoadPriorities.find(position);
        return (it != m_chunkLoadPriorities.end()) ? it->second : 0;
    }

    void ServerChunkProvider::UpdateServerStats(const std::string& operation, bool success) {
        std::lock_guard<std::mutex> lock(m_serverStatsMutex);
        
        if (operation == "chunk_requested") {
            m_serverStats.chunksRequested++;
        } else if (operation == "chunk_loaded_sync") {
            if (success) m_serverStats.chunksLoadedSync++;
        } else if (operation == "chunk_loaded_async") {
            if (success) {
                m_serverStats.chunksLoadedAsync++;
            } else {
                m_serverStats.failedAsyncLoads++;
            }
        } else if (operation == "chunk_generated") {
            if (success) m_serverStats.chunksGenerated++;
        } else if (operation == "chunk_sent_to_client") {
            m_serverStats.chunksSentToClients++;
        }
    }

    bool ServerChunkProvider::ValidateServerState() const {
        // Validate server-specific state consistency
        std::lock_guard<std::mutex> lock(m_loadTrackingMutex);
        
        // Check for consistency between ready chunks and sent chunks
        for (const auto& sentChunk : m_chunksSentToClient) {
            if (m_chunksReadyForClient.find(sentChunk) == m_chunksReadyForClient.end()) {
                Log::Warning("Chunk (%d, %d) marked as sent but not ready", sentChunk.x, sentChunk.z);
                return false;
            }
        }
        
        return true;
    }

    // ========================================================================
    // FACTORY FUNCTIONS
    // ========================================================================

    std::unique_ptr<ServerChunkProvider> CreateServerChunkProvider(const ChunkProviderConfig& config) {
        return std::make_unique<ServerChunkProvider>(config);
    }

    // ========================================================================
    // GLOBAL FUNCTIONS
    // ========================================================================

    void InitializeServerChunkProvider(const ChunkProviderConfig& config) {
        if (g_serverChunkProvider) {
            Log::Warning("ServerChunkProvider already initialized");
            return;
        }

        g_serverChunkProvider = CreateServerChunkProvider(config);
        g_serverChunkProvider->Initialize();
    }

    void ShutdownServerChunkProvider() {
        if (g_serverChunkProvider) {
            g_serverChunkProvider->Shutdown();
            g_serverChunkProvider.reset();
        }
    }

    void RequestServerChunkLoad(Math::ChunkPos position, int priority) {
        if (g_serverChunkProvider) {
            g_serverChunkProvider->RequestChunkLoad(position, priority);
        }
    }

    void ProcessServerChunkGenResults() {
        if (g_serverChunkProvider) {
            g_serverChunkProvider->ProcessChunkGenResults();
        }
    }

    std::vector<Math::ChunkPos> GetServerChunksReadyForClient() {
        return g_serverChunkProvider ? g_serverChunkProvider->GetChunksReadyForClient() : std::vector<Math::ChunkPos>{};
    }

} // namespace Game*/