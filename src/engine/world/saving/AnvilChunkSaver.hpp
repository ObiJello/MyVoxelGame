// File: src/engine/world/saving/AnvilChunkSaver.hpp
#pragma once

#include "../interfaces/IChunkSaver.hpp"
#include "AnvilRegionWriter.hpp"
#include "../../../game/WorldMath.hpp"
#include "../../../core/Log.hpp"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <unordered_set>

#include "engine/world/ChunkProvider.hpp"

namespace Game {

    // Configuration for Anvil format saving
    struct AnvilSaverConfig {
        std::string worldPath;                    // Path to Minecraft world directory
        bool enableCompression = true;            // Use Zlib compression (always true for Anvil)
        bool createWorldStructure = true;        // Auto-create world directories
        bool enableRegionCaching = true;         // Cache region writers
        size_t maxCachedRegions = 16;            // Maximum region files to keep open
        float regionCacheTimeoutSeconds = 60.0f; // Close regions after this time
        bool enableAsyncSaving = true;           // Use background threads
        bool strictAnvilCompliance = true;       // Ensure strict Minecraft compatibility
    };

    // Anvil-format chunk saver that creates true .mca files
    class AnvilChunkSaver : public IChunkSaver {
    public:
        explicit AnvilChunkSaver(const AnvilSaverConfig& config = AnvilSaverConfig{});
        ~AnvilChunkSaver() override;

        // === CORE SAVING INTERFACE ===
        ChunkSaveResult SaveChunk(const Chunk& chunk) override;
        std::future<ChunkSaveResult> SaveChunkAsync(const Chunk& chunk) override;
        void QueueChunkForSave(std::shared_ptr<const Chunk> chunk) override;
        std::vector<ChunkSaveResult> FlushPendingSaves() override;
        std::future<std::vector<ChunkSaveResult>> FlushPendingSavesAsync() override;

        // === BATCH OPERATIONS ===
        std::vector<ChunkSaveResult> SaveChunks(const std::vector<std::shared_ptr<const Chunk>>& chunks) override;

        // === CONFIGURATION ===
        void SetDestination(const std::string& destinationPath) override;
        std::string GetDestination() const override;
        void SetCompressionEnabled(bool enabled) override;
        bool IsCompressionEnabled() const override;
        void SetCompressionLevel(int level) override;
        int GetCompressionLevel() const override;

        // === SAVE POLICIES ===
        void SetSaveMode(SaveMode mode) override;
        SaveMode GetSaveMode() const override;
        void SetAutoSaveEnabled(bool enabled) override;
        void SetAutoSaveInterval(float seconds) override;
        bool IsAutoSaveEnabled() const override;
        float GetAutoSaveInterval() const override;

        // === LIFECYCLE ===
        bool Initialize() override;
        void Shutdown() override;
        bool IsReady() const override;

        // === QUEUE MANAGEMENT ===
        size_t GetPendingChunkCount() const override;
        void ClearPendingChunks() override;
        bool IsChunkPending(Math::ChunkPos position) const override;
        bool RemovePendingChunk(Math::ChunkPos position) override;

        // === STATISTICS ===
        SaverStats GetStats() const override;
        void ResetStats() override;

        // === VALIDATION ===
        bool CanSaveChunk(const Chunk& chunk) const override;
        bool VerifySavedChunk(Math::ChunkPos position) override;

        // === ERROR HANDLING ===
        std::string GetLastError() const override;
        void ClearErrors() override;
        void SetErrorPolicy(ErrorPolicy policy) override;
        ErrorPolicy GetErrorPolicy() const override;

        // === BACKUP AND RECOVERY ===
        void SetBackupEnabled(bool enabled) override;
        bool IsBackupEnabled() const override;
        void SetMaxBackups(int maxBackups) override;
        int GetMaxBackups() const override;

        // === ANVIL-SPECIFIC FEATURES ===

        // Region file management
        void SetRegionCacheSize(size_t maxRegions);
        size_t GetRegionCacheSize() const;
        void FlushRegionCache();

        // Force close specific region file
        void CloseRegionFile(int regionX, int regionZ);

        // Get list of currently cached regions
        std::vector<std::pair<int, int>> GetCachedRegions() const;

        // Anvil format validation
        bool ValidateAnvilFormat(const std::string& regionFilePath) const;

        // World structure management
        bool CreateWorldStructure(const std::string& worldPath) const;

        // Set strict Minecraft compatibility mode
        void SetStrictCompliance(bool enabled);
        bool IsStrictCompliance() const;

    protected:
        void UpdateStats(const ChunkSaveResult& result) override;

    private:
        // Configuration
        AnvilSaverConfig m_config;
        mutable std::mutex m_configMutex;

        // Region file cache
        mutable std::mutex m_regionCacheMutex;
        std::unordered_map<uint64_t, std::shared_ptr<AnvilRegionWriter>> m_regionCache;
        std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> m_regionAccessTimes;

        // Save queue for async operations
        mutable std::mutex m_queueMutex;
        std::queue<std::shared_ptr<const Chunk>> m_saveQueue;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_pendingChunks;

        // Statistics
        mutable std::mutex m_statsMutex;
        SaverStats m_stats;

        // Error handling
        mutable std::mutex m_errorMutex;
        mutable std::string m_lastError;
        ErrorPolicy m_errorPolicy = ErrorPolicy::SkipFailed;

        // State
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_shutdownRequested{false};

        // === REGION FILE MANAGEMENT ===

        // Get or create region writer for chunk position
        std::shared_ptr<AnvilRegionWriter> GetRegionWriter(Math::ChunkPos chunkPos);

        // Create region writer for specific region coordinates
        std::shared_ptr<AnvilRegionWriter> CreateRegionWriter(int regionX, int regionZ);

        // Generate cache key for region coordinates
        uint64_t GetRegionKey(int regionX, int regionZ) const;

        // Cleanup expired region files from cache
        void CleanupRegionCache();

        // Close and remove region from cache
        void CloseRegion(uint64_t regionKey);

        // === CORE SAVE IMPLEMENTATION ===

        // Internal save implementation
        ChunkSaveResult SaveChunkInternal(const Chunk& chunk);

        // Process save queue
        void ProcessSaveQueue();

        // === VALIDATION HELPERS ===

        // Validate chunk data before saving
        bool ValidateChunkForAnvil(const Chunk& chunk) const;

        // Validate world path structure
        bool ValidateWorldPath(const std::string& worldPath) const;

        // === ERROR HANDLING ===

        void SetLastError(const std::string& error) const;
        void LogError(const std::string& operation, const std::string& error) const;

        // === UTILITY METHODS ===

        // Get region coordinates for chunk
        void GetRegionCoords(Math::ChunkPos chunkPos, int& regionX, int& regionZ) const;

        // Check if region file exists
        bool RegionFileExists(int regionX, int regionZ) const;

        // Get region file path
        std::string GetRegionFilePath(int regionX, int regionZ) const;
    };

    // === UTILITY FUNCTIONS ===

    // Factory function for creating Anvil chunk savers
    std::unique_ptr<IChunkSaver> CreateAnvilChunkSaver(const AnvilSaverConfig& config = AnvilSaverConfig{});

    // Convert existing save format to Anvil format
    bool ConvertToAnvilFormat(const std::string& inputPath, const std::string& outputPath);

    // Validate entire world for Anvil compatibility
    struct AnvilValidationResult {
        bool isValid = false;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        size_t totalRegions = 0;
        size_t validRegions = 0;
        size_t totalChunks = 0;
        size_t validChunks = 0;
    };

    AnvilValidationResult ValidateAnvilWorld(const std::string& worldPath);

} // namespace Game