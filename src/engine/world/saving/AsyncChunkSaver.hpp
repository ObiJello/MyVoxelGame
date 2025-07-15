// File: src/engine/world/saving/AsyncChunkSaver.hpp
#pragma once

#include "../interfaces/IChunkSaver.hpp"
#include "../../../game/WorldMath.hpp"
#include "../../../core/Log.hpp"
#include "../../../core/JobSystem.hpp"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <memory>
#include <unordered_set>

namespace Game {

    // Forward declarations
    class Chunk;

    // Compression utilities for chunk saving
    class ChunkCompressor {
    public:
        static std::vector<uint8_t> CompressChunk(const Chunk& chunk, int compressionLevel = 6);
        static std::vector<uint8_t> CompressData(const std::vector<uint8_t>& data, int compressionLevel = 6);
        static size_t EstimateCompressedSize(const Chunk& chunk);
    };

    // Chunk serialization utilities
    class ChunkSerializer {
    public:
        static std::vector<uint8_t> SerializeChunk(const Chunk& chunk);
        static bool DeserializeChunk(const std::vector<uint8_t>& data, Chunk& chunk);
        static size_t EstimateSerializedSize(const Chunk& chunk);

    private:
        static void SerializeSection(const ChunkSection* section, std::vector<uint8_t>& buffer);
        static bool DeserializeSection(const uint8_t*& data, size_t& remaining, ChunkSection& section);
    };

    // Save queue entry with priority
    struct SaveQueueEntry {
        std::shared_ptr<const Chunk> chunk;
        SaveMode saveMode;
        bool highPriority;
        std::chrono::steady_clock::time_point queueTime;
        std::shared_ptr<std::promise<ChunkSaveResult>> promise;

        SaveQueueEntry(std::shared_ptr<const Chunk> chunkPtr, SaveMode mode, bool priority = false)
            : chunk(chunkPtr), saveMode(mode), highPriority(priority)
            , queueTime(std::chrono::steady_clock::now())
            , promise(std::make_shared<std::promise<ChunkSaveResult>>()) {}
    };

    // Background save worker configuration
    struct SaveWorkerConfig {
        int workerThreads = 2;               // Number of background save threads
        int maxQueueSize = 1000;             // Maximum pending saves
        float maxSaveTimeMs = 100.0f;        // Max time per save operation
        bool enableBatching = true;          // Batch multiple saves together
        int batchSize = 10;                  // Number of chunks per batch
        float batchTimeoutMs = 50.0f;        // Max wait time for batch completion
    };

    // Async chunk saver with background worker threads and compression
    class AsyncChunkSaver : public IChunkSaver {
    public:
        explicit AsyncChunkSaver(const SaveWorkerConfig& config = SaveWorkerConfig{});
        ~AsyncChunkSaver() override;

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

        // === ASYNC-SPECIFIC FEATURES ===

        // Worker thread management
        void SetWorkerThreadCount(int count);
        int GetWorkerThreadCount() const;
        bool AreWorkersRunning() const;

        // Priority save queue
        void SaveChunkHighPriority(std::shared_ptr<const Chunk> chunk);
        size_t GetHighPriorityQueueSize() const;

        // Batch saving configuration
        void SetBatchingEnabled(bool enabled);
        bool IsBatchingEnabled() const;
        void SetBatchSize(int size);
        int GetBatchSize() const;

        // Performance monitoring
        float GetAverageSaveTime() const;
        float GetCompressionRatio() const;
        size_t GetTotalBytesWritten() const;
        size_t GetQueueMemoryUsage() const;

        // Manual flush controls
        void FlushAllQueues();
        bool WaitForQueueEmpty(float timeoutSeconds = 10.0f);

    protected:
        void UpdateStats(const ChunkSaveResult& result) override;

    private:
        // Configuration
        SaveWorkerConfig m_config;
        std::string m_destinationPath;
        bool m_compressionEnabled = true;
        int m_compressionLevel = 6;
        SaveMode m_saveMode = SaveMode::Queued;
        ErrorPolicy m_errorPolicy = ErrorPolicy::SkipFailed;
        bool m_initialized = false;

        // Auto-save settings
        bool m_autoSaveEnabled = false;
        float m_autoSaveInterval = 30.0f; // 30 seconds
        std::chrono::steady_clock::time_point m_lastAutoSave;

        // Backup settings
        bool m_backupEnabled = false;
        int m_maxBackups = 5;

        // Worker thread management
        std::vector<std::thread> m_workerThreads;
        std::atomic<bool> m_workersRunning{false};
        std::atomic<bool> m_shutdownRequested{false};

        // Save queues
        mutable std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        std::queue<SaveQueueEntry> m_normalPriorityQueue;
        std::queue<SaveQueueEntry> m_highPriorityQueue;
        std::unordered_set<Math::ChunkPos, Math::ChunkPosHash> m_pendingPositions;

        // Statistics
        mutable std::mutex m_statsMutex;
        SaverStats m_stats;

        // Error handling
        mutable std::mutex m_errorMutex;
        std::string m_lastError;

        // === WORKER THREAD IMPLEMENTATION ===

        // Main worker thread loop
        void WorkerThreadMain();

        // Process single save queue entry
        ChunkSaveResult ProcessSaveEntry(const SaveQueueEntry& entry);

        // Batch processing
        std::vector<ChunkSaveResult> ProcessBatch(const std::vector<SaveQueueEntry>& batch);

        // Get next save entry from queues (high priority first)
        bool GetNextSaveEntry(SaveQueueEntry& entry, float timeoutMs = 100.0f);

        // === CORE SAVE IMPLEMENTATION ===

        // Synchronous save implementation
        ChunkSaveResult SaveChunkInternal(const Chunk& chunk, bool compress = true);

        // Write chunk data to file
        bool WriteChunkToFile(const Chunk& chunk, const std::vector<uint8_t>& data);

        // Generate save file path for chunk
        std::string GetChunkSavePath(Math::ChunkPos position) const;

        // Ensure save directory exists
        bool EnsureSaveDirectory(Math::ChunkPos position) const;

        // === QUEUE MANAGEMENT ===

        // Add entry to appropriate queue
        void EnqueueSaveEntry(SaveQueueEntry&& entry);

        // Remove chunk from pending set
        void RemoveFromPending(Math::ChunkPos position);

        // Check if queue has space
        bool HasQueueSpace() const;

        // Get total queue size
        size_t GetTotalQueueSize() const;

        // === AUTO-SAVE SYSTEM ===

        // Check if auto-save should trigger
        bool ShouldAutoSave() const;

        // Trigger auto-save of all dirty chunks
        void TriggerAutoSave();

        // === BACKUP SYSTEM ===

        // Create backup of chunk file
        bool CreateBackup(Math::ChunkPos position);

        // Clean old backups
        void CleanOldBackups(Math::ChunkPos position);

        // Get backup file path
        std::string GetBackupPath(Math::ChunkPos position, int backupIndex) const;

        // === VALIDATION ===

        // Validate chunk data before saving
        bool ValidateChunkData(const Chunk& chunk) const;

        // Verify save file integrity
        bool VerifyFileIntegrity(const std::string& filePath) const;

        // === ERROR HANDLING ===

        void SetLastError(const std::string& error) const;
        void LogError(const std::string& operation, const std::string& error) const;
        bool ShouldRetryOnError(const std::string& error) const;

        // === STATISTICS ===

        void UpdateSaveStats(const ChunkSaveResult& result, size_t originalSize, size_t compressedSize);
        void UpdateCompressionStats(size_t originalSize, size_t compressedSize);

        // === UTILITY METHODS ===

        // Calculate memory usage of pending chunks
        size_t CalculateQueueMemoryUsage() const;

        // Get current timestamp string for logging
        std::string GetTimestamp() const;

        // File system utilities
        bool FileExists(const std::string& path) const;
        bool CreateDirectoryRecursive(const std::string& path) const;
        size_t GetFileSize(const std::string& path) const;
    };

    // === RAII SAVE OPERATION ===

    // RAII wrapper for save operations with automatic cleanup
    class SaveOperation {
    public:
        SaveOperation(AsyncChunkSaver* saver, std::shared_ptr<const Chunk> chunk, bool highPriority = false);
        ~SaveOperation();

        // Get the save future
        std::future<ChunkSaveResult> GetFuture();

        // Check if save is complete
        bool IsComplete() const;

        // Wait for completion with timeout
        bool WaitForCompletion(float timeoutSeconds = 10.0f);

        // Cancel the save operation
        void Cancel();

    private:
        AsyncChunkSaver* m_saver;
        std::shared_ptr<const Chunk> m_chunk;
        std::future<ChunkSaveResult> m_future;
        bool m_cancelled = false;
    };

    // === UTILITY FUNCTIONS ===

    // Create an AsyncChunkSaver instance
    std::unique_ptr<IChunkSaver> CreateAsyncChunkSaver(const SaveWorkerConfig& config = SaveWorkerConfig{});

    // Helper for chunk position hashing
    struct ChunkPosHash {
        std::size_t operator()(const Math::ChunkPos& pos) const {
            return std::hash<int32_t>{}(pos.x) ^ (std::hash<int32_t>{}(pos.z) << 1);
        }
    };

} // namespace Game