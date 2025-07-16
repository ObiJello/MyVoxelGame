// File: src/engine/world/interfaces/IIChunkSaver.hpp
#pragma once

#include "../Chunk.hpp"
#include "../../../game/WorldMath.hpp"
#include <memory>
#include <string>
#include <future>
#include <vector>
#include <queue>

namespace Game {

    // Result of a chunk saving operation
    struct ChunkSaveResult {
        Math::ChunkPos position;
        bool success = false;
        std::string errorMessage;

        // Metadata about the save operation
        size_t bytesWritten = 0;
        float saveTimeMs = 0.0f;
        bool wasCompressed = false;

        ChunkSaveResult() = default;

        ChunkSaveResult(Math::ChunkPos pos, bool ok = true)
            : position(pos), success(ok) {}

        static ChunkSaveResult Success(Math::ChunkPos position, size_t bytes = 0) {
            ChunkSaveResult result(position, true);
            result.bytesWritten = bytes;
            return result;
        }

        static ChunkSaveResult Failure(Math::ChunkPos position, const std::string& error) {
            ChunkSaveResult result(position, false);
            result.errorMessage = error;
            return result;
        }
    };

    // Abstract interface for chunk saving strategies
    class IChunkSaver {
    public:
        virtual ~IChunkSaver() = default;

        // === CORE SAVING INTERFACE ===

        // Synchronously save a chunk
        virtual ChunkSaveResult SaveChunk(const Chunk& chunk) = 0;

        // Asynchronously save a chunk (returns immediately with future)
        virtual std::future<ChunkSaveResult> SaveChunkAsync(const Chunk& chunk) = 0;

        // Queue a chunk for saving (will be saved when FlushPendingSaves is called)
        virtual void QueueChunkForSave(std::shared_ptr<const Chunk> chunk) = 0;

        // Save all queued chunks and wait for completion
        virtual std::vector<ChunkSaveResult> FlushPendingSaves() = 0;

        // Save all queued chunks asynchronously
        virtual std::future<std::vector<ChunkSaveResult>> FlushPendingSavesAsync() = 0;

        // === BATCH OPERATIONS ===

        // Save multiple chunks in one operation
        virtual std::vector<ChunkSaveResult> SaveChunks(const std::vector<std::shared_ptr<const Chunk>>& chunks) {
            std::vector<ChunkSaveResult> results;
            results.reserve(chunks.size());

            for (const auto& chunk : chunks) {
                if (chunk) {
                    results.push_back(SaveChunk(*chunk));
                } else {
                    results.push_back(ChunkSaveResult::Failure({0, 0}, "Null chunk pointer"));
                }
            }

            return results;
        }

        // === CONFIGURATION ===

        // Set the destination path/configuration for this saver
        virtual void SetDestination(const std::string& destinationPath) = 0;
        virtual std::string GetDestination() const = 0;

        // Enable/disable compression for saved chunks
        virtual void SetCompressionEnabled(bool enabled) = 0;
        virtual bool IsCompressionEnabled() const = 0;

        // Set compression level (0-9, where 9 is maximum compression)
        virtual void SetCompressionLevel(int level) = 0;
        virtual int GetCompressionLevel() const = 0;

        // === SAVE POLICIES ===

        // Save modes for different use cases
        enum class SaveMode {
            Immediate,      // Save immediately, blocking
            Queued,         // Add to queue for later batch save
            Background,     // Save in background thread
            OnShutdown      // Only save during shutdown
        };

        virtual void SetSaveMode(SaveMode mode) = 0;
        virtual SaveMode GetSaveMode() const = 0;

        // Auto-save configuration
        virtual void SetAutoSaveEnabled(bool enabled) = 0;
        virtual void SetAutoSaveInterval(float seconds) = 0;
        virtual bool IsAutoSaveEnabled() const = 0;
        virtual float GetAutoSaveInterval() const = 0;

        // === LIFECYCLE ===

        // Initialize the saver (e.g., create directories, open files)
        virtual bool Initialize() = 0;

        // Shutdown the saver and save any pending chunks
        virtual void Shutdown() = 0;

        // Check if the saver is ready to use
        virtual bool IsReady() const = 0;

        // === QUEUE MANAGEMENT ===

        // Get number of chunks waiting to be saved
        virtual size_t GetPendingChunkCount() const = 0;

        // Clear the save queue without saving (use with caution!)
        virtual void ClearPendingChunks() = 0;

        // Check if a specific chunk is in the save queue
        virtual bool IsChunkPending(Math::ChunkPos position) const = 0;

        // Remove a specific chunk from the save queue
        virtual bool RemovePendingChunk(Math::ChunkPos position) = 0;

        // === STATISTICS ===

        // Get saver performance statistics
        struct SaverStats {
            size_t chunksQueued = 0;
            size_t chunksSaved = 0;
            size_t saveFailures = 0;
            float totalSaveTimeMs = 0.0f;
            float averageSaveTimeMs = 0.0f;
            size_t totalBytesWritten = 0;
            size_t compressionRatio = 100; // Percentage (100 = no compression)

            void Reset() {
                chunksQueued = chunksSaved = saveFailures = 0;
                totalSaveTimeMs = averageSaveTimeMs = 0.0f;
                totalBytesWritten = 0;
                compressionRatio = 100;
            }
        };

        virtual SaverStats GetStats() const = 0;
        virtual void ResetStats() = 0;

        // === VALIDATION ===

        // Validate that a chunk can be saved
        virtual bool CanSaveChunk(const Chunk& chunk) const {
            // Default implementation does basic checks
            return !chunk.IsEmpty();
        }

        // Verify that a saved chunk can be loaded back correctly
        virtual bool VerifySavedChunk(Math::ChunkPos position) = 0;

        // === ERROR HANDLING ===

        // Get the last error message from this saver
        virtual std::string GetLastError() const = 0;

        // Clear any persistent error state
        virtual void ClearErrors() = 0;

        // Set error handling policy
        enum class ErrorPolicy {
            FailFast,       // Stop saving on first error
            SkipFailed,     // Skip failed chunks and continue
            RetryFailed     // Retry failed chunks up to a limit
        };

        virtual void SetErrorPolicy(ErrorPolicy policy) = 0;
        virtual ErrorPolicy GetErrorPolicy() const = 0;

        // === BACKUP AND RECOVERY ===

        // Create a backup before saving (for safety)
        virtual void SetBackupEnabled(bool enabled) = 0;
        virtual bool IsBackupEnabled() const = 0;

        // Set maximum number of backups to keep
        virtual void SetMaxBackups(int maxBackups) = 0;
        virtual int GetMaxBackups() const = 0;

    protected:
        // Helper for implementations to update statistics
        virtual void UpdateStats(const ChunkSaveResult& result) {
            // Override in derived classes to update stats
        }
    };

    // Factory function type for creating chunk savers
    using ChunkSaverFactory = std::function<std::unique_ptr<IChunkSaver>()>;

} // namespace Game