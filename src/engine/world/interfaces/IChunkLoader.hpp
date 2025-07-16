// File: src/engine/world/interfaces/IIChunkLoader.hpp
#pragma once

#include "../Chunk.hpp"
#include "../../../game/WorldMath.hpp"
#include <memory>
#include <string>
#include <future>

namespace Game {

    // Result of a chunk loading operation
    struct ChunkLoadResult {
        std::shared_ptr<Chunk> chunk;
        bool success = false;
        std::string errorMessage;

        // Metadata about the load operation
        bool wasFromCache = false;
        bool wasFromDisk = false;
        bool wasGenerated = false;
        float loadTimeMs = 0.0f;

        ChunkLoadResult() = default;

        ChunkLoadResult(std::shared_ptr<Chunk> chunkPtr, bool ok = true)
            : chunk(chunkPtr), success(ok) {}

        static ChunkLoadResult Success(std::shared_ptr<Chunk> chunk) {
            return ChunkLoadResult(chunk, true);
        }

        static ChunkLoadResult Failure(const std::string& error) {
            ChunkLoadResult result;
            result.success = false;
            result.errorMessage = error;
            return result;
        }
    };

    // Abstract interface for chunk loading strategies
    class IChunkLoader {
    public:
        virtual ~IChunkLoader() = default;

        // === CORE LOADING INTERFACE ===

        // Synchronously load a chunk at the given position
        virtual ChunkLoadResult LoadChunk(Math::ChunkPos position) = 0;

        // Asynchronously load a chunk (returns immediately with future)
        virtual std::future<ChunkLoadResult> LoadChunkAsync(Math::ChunkPos position) = 0;

        // Check if a chunk exists and can be loaded (without actually loading it)
        virtual bool ChunkExists(Math::ChunkPos position) const = 0;

        // Get estimated load time for a chunk (for scheduling/prioritization)
        virtual float EstimateLoadTime(Math::ChunkPos position) const = 0;

        // === BATCH OPERATIONS ===

        // Load multiple chunks in one operation (more efficient for some loaders)
        virtual std::vector<ChunkLoadResult> LoadChunks(const std::vector<Math::ChunkPos>& positions) {
            std::vector<ChunkLoadResult> results;
            results.reserve(positions.size());

            for (const auto& pos : positions) {
                results.push_back(LoadChunk(pos));
            }

            return results;
        }

        // Check existence of multiple chunks
        virtual std::vector<bool> ChunksExist(const std::vector<Math::ChunkPos>& positions) const {
            std::vector<bool> results;
            results.reserve(positions.size());

            for (const auto& pos : positions) {
                results.push_back(ChunkExists(pos));
            }

            return results;
        }

        // === CONFIGURATION ===

        // Set the source path/configuration for this loader
        virtual void SetSource(const std::string& sourcePath) = 0;
        virtual std::string GetSource() const = 0;

        // Enable/disable caching for this loader
        virtual void SetCachingEnabled(bool enabled) = 0;
        virtual bool IsCachingEnabled() const = 0;

        // === LIFECYCLE ===

        // Initialize the loader (e.g., open files, connect to database)
        virtual bool Initialize() = 0;

        // Shutdown the loader and clean up resources
        virtual void Shutdown() = 0;

        // Check if the loader is ready to use
        virtual bool IsReady() const = 0;

        // === STATISTICS ===

        // Get loader performance statistics
        struct LoaderStats {
            size_t chunksLoaded = 0;
            size_t cacheHits = 0;
            size_t cacheMisses = 0;
            float totalLoadTimeMs = 0.0f;
            float averageLoadTimeMs = 0.0f;
            size_t bytesLoaded = 0;

            void Reset() {
                chunksLoaded = cacheHits = cacheMisses = 0;
                totalLoadTimeMs = averageLoadTimeMs = 0.0f;
                bytesLoaded = 0;
            }
        };

        virtual LoaderStats GetStats() const = 0;
        virtual void ResetStats() = 0;

        // === PRIORITY AND HINTS ===

        // Priority levels for chunk loading
        enum class LoadPriority {
            Low = 0,      // Background loading
            Normal = 1,   // Standard loading
            High = 2,     // Player vicinity
            Critical = 3  // Player current chunk
        };

        // Set loading priority for future loads
        virtual void SetDefaultPriority(LoadPriority priority) = 0;
        virtual LoadPriority GetDefaultPriority() const = 0;

        // Load with specific priority (overrides default)
        virtual ChunkLoadResult LoadChunkWithPriority(Math::ChunkPos position, LoadPriority priority) {
            // Default implementation ignores priority
            return LoadChunk(position);
        }

        // === VALIDATION ===

        // Validate that a loaded chunk is consistent and usable
        virtual bool ValidateChunk(const Chunk& chunk) const {
            // Default implementation does basic checks
            return !chunk.IsEmpty() && chunk.pos.x != 0 && chunk.pos.z != 0; // Basic validation
        }

        // === ERROR HANDLING ===

        // Get the last error message from this loader
        virtual std::string GetLastError() const = 0;

        // Clear any persistent error state
        virtual void ClearErrors() = 0;

    protected:
        // Helper for implementations to update statistics
        virtual void UpdateStats(const ChunkLoadResult& result) {
            // Override in derived classes to update stats
        }
    };

    // Factory function type for creating chunk loaders
    using ChunkLoaderFactory = std::function<std::unique_ptr<IChunkLoader>()>;

} // namespace Game