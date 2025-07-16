// File: src/engine/world/tracking/DirtyTracker.hpp
#pragma once

#include "../../../game/WorldMath.hpp"
#include <unordered_set>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <functional>

namespace Game {

    // Represents a dirty chunk section that needs mesh rebuilding
    struct DirtySection {
        Math::ChunkPos chunkPos;
        int sectionIndex;

        DirtySection() = default;
        DirtySection(Math::ChunkPos pos, int section) : chunkPos(pos), sectionIndex(section) {}

        bool operator==(const DirtySection& other) const {
            return chunkPos.x == other.chunkPos.x &&
                   chunkPos.z == other.chunkPos.z &&
                   sectionIndex == other.sectionIndex;
        }

        bool operator!=(const DirtySection& other) const {
            return !(*this == other);
        }

        // For debugging/logging
        std::string ToString() const {
            return "(" + std::to_string(chunkPos.x) + ", " + std::to_string(chunkPos.z) + 
                   ", section=" + std::to_string(sectionIndex) + ")";
        }
    };

    // Hash function for DirtySection
    struct DirtySectionHash {
        std::size_t operator()(const DirtySection& ds) const {
            std::size_t h1 = std::hash<int32_t>{}(ds.chunkPos.x);
            std::size_t h2 = std::hash<int32_t>{}(ds.chunkPos.z);
            std::size_t h3 = std::hash<int>{}(ds.sectionIndex);
            
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    // Callback function types for dirty notifications
    using DirtyCallback = std::function<void(const DirtySection&)>;
    using DirtyBatchCallback = std::function<void(const std::vector<DirtySection>&)>;

    // Configuration for dirty tracking behavior
    struct DirtyTrackerConfig {
        bool enableBatching = true;           // Batch multiple dirty sections together
        size_t maxBatchSize = 50;             // Maximum sections per batch
        float batchTimeoutMs = 10.0f;         // Max time to wait for batch completion
        bool enableNeighborInvalidation = true; // Mark neighboring sections dirty when needed
        bool enableAutoCleanup = true;        // Automatically remove stale entries
        float cleanupIntervalSeconds = 30.0f; // How often to run cleanup
        size_t maxDirtySections = 10000;      // Maximum dirty sections to track
    };

    // Statistics about dirty tracking performance
    struct DirtyTrackerStats {
        size_t totalSectionsMarked = 0;       // Total sections marked dirty
        size_t totalSectionsCleared = 0;      // Total sections cleared
        size_t totalBatchesProcessed = 0;     // Number of batches processed
        size_t currentDirtySections = 0;      // Currently dirty sections
        size_t maxDirtySections = 0;          // Peak dirty sections
        size_t neighborInvalidations = 0;     // Neighbor sections invalidated
        float averageBatchSize = 0.0f;        // Average batch size
        size_t duplicateMarks = 0;             // Attempts to mark already dirty sections

        void Reset() {
            totalSectionsMarked = totalSectionsCleared = totalBatchesProcessed = 0;
            currentDirtySections = maxDirtySections = neighborInvalidations = 0;
            averageBatchSize = 0.0f;
            duplicateMarks = 0;
        }
    };

    // Thread-safe dirty section tracker for mesh invalidation
    class DirtyTracker {
    public:
        explicit DirtyTracker(const DirtyTrackerConfig& config = DirtyTrackerConfig{});
        ~DirtyTracker();

        // === CORE TRACKING INTERFACE ===

        // Mark a specific section as dirty
        void MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex);

        // Mark all sections in a chunk as dirty
        void MarkChunkDirty(Math::ChunkPos chunkPos);

        // Mark sections in a world coordinate range as dirty
        void MarkRegionDirty(int minWorldX, int minWorldY, int minWorldZ,
                           int maxWorldX, int maxWorldY, int maxWorldZ);

        // Check if a section is dirty
        bool IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const;

        // Check if any section in a chunk is dirty
        bool IsChunkDirty(Math::ChunkPos chunkPos) const;

        // Get count of dirty sections
        size_t GetDirtyCount() const;

        // Check if there are any dirty sections
        bool HasDirtySections() const;

        // === BATCH RETRIEVAL ===

        // Get and clear all dirty sections in one operation
        std::vector<DirtySection> GetAndClearAllDirtySections();

        // Get dirty sections up to a maximum count (leaves remaining dirty)
        std::vector<DirtySection> GetDirtySections(size_t maxCount);

        // Clear specific dirty sections
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Clear all dirty sections without returning them
        void ClearAllDirtySections();

        // === NEIGHBOR INVALIDATION ===

        // Mark neighboring sections dirty when a section changes
        void MarkNeighborsDirty(Math::ChunkPos chunkPos, int sectionIndex);

        // Get sections that neighbor the given section
        std::vector<DirtySection> GetNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const;

        // === CONFIGURATION ===

        void SetConfig(const DirtyTrackerConfig& config);
        DirtyTrackerConfig GetConfig() const;

        // Enable/disable neighbor invalidation
        void SetNeighborInvalidationEnabled(bool enabled);
        bool IsNeighborInvalidationEnabled() const;

        // Set batching parameters
        void SetBatchingEnabled(bool enabled);
        void SetMaxBatchSize(size_t maxSize);
        void SetBatchTimeout(float timeoutMs);

        // === CALLBACKS ===

        // Register callback for when sections become dirty
        void SetDirtyCallback(DirtyCallback callback);

        // Register callback for dirty section batches
        void SetBatchCallback(DirtyBatchCallback callback);

        // Clear callbacks
        void ClearCallbacks();

        // === LIFECYCLE ===

        // Initialize the tracker
        bool Initialize();

        // Shutdown and cleanup
        void Shutdown();

        // Update (call periodically for batching and cleanup)
        void Update(float deltaTime);

        // === STATISTICS ===

        DirtyTrackerStats GetStats() const;
        void ResetStats();

        // Get memory usage estimate
        size_t GetMemoryUsage() const;

        // === DEBUGGING ===

        // Get all dirty sections (for debugging - doesn't clear them)
        std::vector<DirtySection> GetAllDirtySections() const;

        // Log current dirty sections
        void LogDirtySections(const std::string& prefix = "DirtyTracker") const;

        // Validate internal state consistency
        bool ValidateState() const;

        // === BULK OPERATIONS ===

        // Mark multiple sections dirty at once
        void MarkSectionsDirty(const std::vector<DirtySection>& sections);

        // Efficient bulk clearing for shutdown
        void ClearAll();

        // === FILTERING ===

        // Get dirty sections for a specific chunk
        std::vector<DirtySection> GetDirtySectionsForChunk(Math::ChunkPos chunkPos) const;

        // Get dirty sections in a chunk radius around a center point
        std::vector<DirtySection> GetDirtySectionsInRadius(Math::ChunkPos center, int radius) const;

        // Remove dirty sections outside a given area (for unloading distant chunks)
        size_t ClearDirtySectionsOutsideRadius(Math::ChunkPos center, int radius);

    private:
        // Configuration
        DirtyTrackerConfig m_config;
        mutable std::shared_mutex m_configMutex;

        // Core dirty section storage
        mutable std::shared_mutex m_dirtyMutex;
        std::unordered_set<DirtySection, DirtySectionHash> m_dirtySections;

        // Batching support
        mutable std::mutex m_batchMutex;
        std::vector<DirtySection> m_pendingBatch;
        std::chrono::steady_clock::time_point m_lastBatchTime;

        // Callbacks
        mutable std::mutex m_callbackMutex;
        DirtyCallback m_dirtyCallback;
        DirtyBatchCallback m_batchCallback;

        // Statistics
        mutable std::mutex m_statsMutex;
        DirtyTrackerStats m_stats;

        // Lifecycle
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_shutdownRequested{false};

        // Cleanup timing
        std::chrono::steady_clock::time_point m_lastCleanup;

        // === INTERNAL IMPLEMENTATION ===

        // Core dirty marking (assumes lock is held)
        void MarkSectionDirtyInternal(const DirtySection& section);

        // Batch processing
        void ProcessPendingBatch();
        void FlushBatch();
        bool ShouldFlushBatch() const;

        // Neighbor calculation
        std::vector<DirtySection> CalculateNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const;

        // Coordinate utilities
        Math::ChunkPos WorldToChunkPos(int worldX, int worldZ) const;
        int WorldYToSectionIndex(int worldY) const;
        bool IsValidSectionIndex(int sectionIndex) const;

        // Statistics updates
        void UpdateStats(const DirtySection& section, bool added);
        void UpdateBatchStats(size_t batchSize);

        // Cleanup operations
        void PerformCleanup();
        bool ShouldPerformCleanup() const;

        // Validation helpers
        bool IsValidChunkPos(Math::ChunkPos pos) const;
        bool IsValidDirtySection(const DirtySection& section) const;

        // Memory management
        void EnforceMemoryLimits();
        size_t CalculateMemoryUsage() const;

        // Callback invocation
        void InvokeDirtyCallback(const DirtySection& section);
        void InvokeBatchCallback(const std::vector<DirtySection>& batch);

        // Thread safety helpers
        template<typename Func>
        auto WithReadLock(Func&& func) const -> decltype(func());

        template<typename Func>
        auto WithWriteLock(Func&& func) -> decltype(func());
    };

    // === UTILITY FUNCTIONS ===

    // Factory function for creating dirty trackers
    std::unique_ptr<DirtyTracker> CreateDirtyTracker(const DirtyTrackerConfig& config = DirtyTrackerConfig{});

    // Helper to convert world coordinates to dirty section
    DirtySection WorldCoordinatesToDirtySection(int worldX, int worldY, int worldZ);

    // Helper to get all sections affected by a block change
    std::vector<DirtySection> GetAffectedSections(int worldX, int worldY, int worldZ, bool includeNeighbors = true);

    // Bulk operation helper
    class DirtyBatchOperator {
    public:
        explicit DirtyBatchOperator(DirtyTracker& tracker);
        ~DirtyBatchOperator();

        // Add sections to batch
        void AddSection(Math::ChunkPos chunkPos, int sectionIndex);
        void AddChunk(Math::ChunkPos chunkPos);
        void AddRegion(int minWorldX, int minWorldY, int minWorldZ,
                      int maxWorldX, int maxWorldY, int maxWorldZ);

        // Manually flush the batch
        void Flush();

    private:
        DirtyTracker& m_tracker;
        std::vector<DirtySection> m_batch;
        bool m_flushed = false;
    };

} // namespace Game