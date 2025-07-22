// File: src/engine/world/tracking/DirtyTracker.hpp
#pragma once

#include "../../../game/WorldMath.hpp"
#include <unordered_set>
#include <vector>
#include <mutex>
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

    // Simple configuration
    struct DirtyTrackerConfig {
        bool enableNeighborInvalidation = true;
    };

    // Simple statistics
    struct DirtyTrackerStats {
        size_t totalSectionsMarked = 0;
        size_t totalSectionsCleared = 0;
        size_t currentDirtySections = 0;

        void Reset() {
            totalSectionsMarked = totalSectionsCleared = 0;
            currentDirtySections = 0;
        }
    };

    // Simplified dirty section tracker
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

        // === RETRIEVAL ===

        // Get and clear all dirty sections in one operation
        std::vector<DirtySection> GetAndClearAllDirtySections();

        // Get dirty sections up to a maximum count (leaves remaining dirty)
        std::vector<DirtySection> GetDirtySections(size_t maxCount);

        // Clear specific dirty sections
        void ClearDirtySections(const std::vector<DirtySection>& sections);

        // Clear all dirty sections without returning them
        void ClearAllDirtySections();

        // === CONFIGURATION ===

        void SetConfig(const DirtyTrackerConfig& config);
        DirtyTrackerConfig GetConfig() const;

        // Enable/disable neighbor invalidation
        void SetNeighborInvalidationEnabled(bool enabled);
        bool IsNeighborInvalidationEnabled() const;

        // === LIFECYCLE ===

        bool Initialize();
        void Shutdown();
        void Update(float deltaTime);

        // === STATISTICS ===

        DirtyTrackerStats GetStats() const;
        void ResetStats();
        size_t GetMemoryUsage() const;

        // === DEBUGGING ===

        std::vector<DirtySection> GetAllDirtySections() const;
        void LogDirtySections(const std::string& prefix = "DirtyTracker") const;
        bool ValidateState() const;

        // === FILTERING ===

        std::vector<DirtySection> GetDirtySectionsForChunk(Math::ChunkPos chunkPos) const;

    private:
        // Configuration
        DirtyTrackerConfig m_config;

        // Core dirty section storage
        mutable std::mutex m_dirtyMutex;
        std::unordered_set<DirtySection, DirtySectionHash> m_dirtySections;

        // Statistics
        mutable std::mutex m_statsMutex;
        DirtyTrackerStats m_stats;

        // Lifecycle
        std::atomic<bool> m_initialized{false};

        // === INTERNAL IMPLEMENTATION ===

        void MarkSectionDirtyInternal(const DirtySection& section);
        void MarkNeighborsDirty(Math::ChunkPos chunkPos, int sectionIndex);
        std::vector<DirtySection> CalculateNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const;

        // Coordinate utilities
        Math::ChunkPos WorldToChunkPos(int worldX, int worldZ) const;
        int WorldYToSectionIndex(int worldY) const;
        bool IsValidSectionIndex(int sectionIndex) const;

        // Statistics updates
        void UpdateStats(const DirtySection& section, bool added);

        // Validation helpers
        bool IsValidChunkPos(Math::ChunkPos pos) const;
        bool IsValidDirtySection(const DirtySection& section) const;
    };

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<DirtyTracker> CreateDirtyTracker(const DirtyTrackerConfig& config = DirtyTrackerConfig{});

    DirtySection WorldCoordinatesToDirtySection(int worldX, int worldY, int worldZ);

    std::vector<DirtySection> GetAffectedSections(int worldX, int worldY, int worldZ, bool includeNeighbors = true);

} // namespace Game