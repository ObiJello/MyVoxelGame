// File: src/engine/world/tracking/DirtyTracker.cpp
#include "DirtyTracker.hpp"
#include "../../../game/WorldCoordinates.hpp"
#include "../../../core/Log.hpp"
#include <algorithm>
#include <cmath>

namespace Game {

    DirtyTracker::DirtyTracker(const DirtyTrackerConfig& config)
        : m_config(config) {

        Log::Debug("DirtyTracker created with neighbor invalidation=%s",
                  config.enableNeighborInvalidation ? "enabled" : "disabled");
    }

    DirtyTracker::~DirtyTracker() {
        Shutdown();
    }

    // === CORE TRACKING INTERFACE ===

    void DirtyTracker::MarkSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        if (!m_initialized || !IsValidSectionIndex(sectionIndex)) {
            return;
        }

        DirtySection section(chunkPos, sectionIndex);
        MarkSectionDirtyInternal(section);

        // Mark neighbors if enabled
        if (m_config.enableNeighborInvalidation) {
            MarkNeighborsDirty(chunkPos, sectionIndex);
        }
    }

    void DirtyTracker::MarkChunkDirty(Math::ChunkPos chunkPos) {
        if (!m_initialized) {
            return;
        }

        // Mark all sections in the chunk
        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            MarkSectionDirty(chunkPos, sectionY);
        }
    }

    void DirtyTracker::MarkRegionDirty(int minWorldX, int minWorldY, int minWorldZ,
                                     int maxWorldX, int maxWorldY, int maxWorldZ) {
        if (!m_initialized) {
            return;
        }

        // Convert world coordinates to chunk/section ranges
        Math::ChunkPos minChunk = WorldToChunkPos(minWorldX, minWorldZ);
        Math::ChunkPos maxChunk = WorldToChunkPos(maxWorldX, maxWorldZ);

        int minSection = WorldYToSectionIndex(minWorldY);
        int maxSection = WorldYToSectionIndex(maxWorldZ);

        // Mark all affected sections
        for (int chunkX = minChunk.x; chunkX <= maxChunk.x; ++chunkX) {
            for (int chunkZ = minChunk.z; chunkZ <= maxChunk.z; ++chunkZ) {
                for (int sectionY = minSection; sectionY <= maxSection; ++sectionY) {
                    if (IsValidSectionIndex(sectionY)) {
                        MarkSectionDirty(Math::ChunkPos{chunkX, chunkZ}, sectionY);
                    }
                }
            }
        }
    }

    bool DirtyTracker::IsSectionDirty(Math::ChunkPos chunkPos, int sectionIndex) const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        DirtySection section(chunkPos, sectionIndex);
        return m_dirtySections.find(section) != m_dirtySections.end();
    }

    bool DirtyTracker::IsChunkDirty(Math::ChunkPos chunkPos) const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            DirtySection section(chunkPos, sectionY);
            if (m_dirtySections.find(section) != m_dirtySections.end()) {
                return true;
            }
        }
        return false;
    }

    size_t DirtyTracker::GetDirtyCount() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        return m_dirtySections.size();
    }

    bool DirtyTracker::HasDirtySections() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);
        return !m_dirtySections.empty();
    }

    // === RETRIEVAL ===

    std::vector<DirtySection> DirtyTracker::GetAndClearAllDirtySections() {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        std::vector<DirtySection> result;
        result.reserve(m_dirtySections.size());

        for (const auto& section : m_dirtySections) {
            result.push_back(section);
        }

        // Clear all sections
        size_t clearedCount = m_dirtySections.size();
        m_dirtySections.clear();

        // Update statistics
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSectionsCleared += clearedCount;
            m_stats.currentDirtySections = 0;
        }

        return result;
    }

    std::vector<DirtySection> DirtyTracker::GetDirtySections(size_t maxCount) {
        if (maxCount == 0) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        std::vector<DirtySection> result;
        result.reserve(std::min(maxCount, m_dirtySections.size()));

        auto it = m_dirtySections.begin();
        for (size_t i = 0; i < maxCount && it != m_dirtySections.end(); ++i, ++it) {
            result.push_back(*it);
        }

        return result;
    }

    void DirtyTracker::ClearDirtySections(const std::vector<DirtySection>& sections) {
        if (sections.empty()) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        size_t clearedCount = 0;
        for (const auto& section : sections) {
            if (m_dirtySections.erase(section)) {
                clearedCount++;
            }
        }

        // Update statistics
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSectionsCleared += clearedCount;
            m_stats.currentDirtySections = m_dirtySections.size();
        }
    }

    void DirtyTracker::ClearAllDirtySections() {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        size_t clearedCount = m_dirtySections.size();
        m_dirtySections.clear();

        // Update statistics
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSectionsCleared += clearedCount;
            m_stats.currentDirtySections = 0;
        }
    }

    // === CONFIGURATION ===

    void DirtyTracker::SetConfig(const DirtyTrackerConfig& config) {
        m_config = config;
        Log::Debug("DirtyTracker config updated");
    }

    DirtyTrackerConfig DirtyTracker::GetConfig() const {
        return m_config;
    }

    void DirtyTracker::SetNeighborInvalidationEnabled(bool enabled) {
        m_config.enableNeighborInvalidation = enabled;
    }

    bool DirtyTracker::IsNeighborInvalidationEnabled() const {
        return m_config.enableNeighborInvalidation;
    }

    // === LIFECYCLE ===

    bool DirtyTracker::Initialize() {
        if (m_initialized) {
            return true;
        }

        Log::Info("Initializing DirtyTracker...");

        // Reset statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.Reset();
        }

        // Clear any existing state
        ClearAllDirtySections();

        m_initialized = true;

        Log::Info("DirtyTracker initialized successfully");
        return true;
    }

    void DirtyTracker::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down DirtyTracker...");

        // Clear all state
        ClearAllDirtySections();

        m_initialized = false;
        Log::Info("DirtyTracker shutdown complete");
    }

    void DirtyTracker::Update(float deltaTime) {
        // No complex processing needed in simplified version
    }

    // === STATISTICS ===

    DirtyTrackerStats DirtyTracker::GetStats() const {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        DirtyTrackerStats stats = m_stats;
        stats.currentDirtySections = GetDirtyCount();

        return stats;
    }

    void DirtyTracker::ResetStats() {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.Reset();
    }

    size_t DirtyTracker::GetMemoryUsage() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        size_t usage = 0;
        usage += m_dirtySections.size() * sizeof(DirtySection);
        usage += m_dirtySections.bucket_count() * sizeof(void*);

        return usage;
    }

    // === DEBUGGING ===

    std::vector<DirtySection> DirtyTracker::GetAllDirtySections() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        std::vector<DirtySection> result;
        result.reserve(m_dirtySections.size());

        for (const auto& section : m_dirtySections) {
            result.push_back(section);
        }

        return result;
    }

    void DirtyTracker::LogDirtySections(const std::string& prefix) const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        Log::Info("%s: %zu dirty sections", prefix.c_str(), m_dirtySections.size());

        if (m_dirtySections.size() <= 10) {
            for (const auto& section : m_dirtySections) {
                Log::Debug("  %s", section.ToString().c_str());
            }
        }
    }

    bool DirtyTracker::ValidateState() const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        // Validate each section
        for (const auto& section : m_dirtySections) {
            if (!IsValidDirtySection(section)) {
                Log::Error("DirtyTracker: Invalid section found: %s", section.ToString().c_str());
                return false;
            }
        }

        return true;
    }

    // === FILTERING ===

    std::vector<DirtySection> DirtyTracker::GetDirtySectionsForChunk(Math::ChunkPos chunkPos) const {
        std::lock_guard<std::mutex> lock(m_dirtyMutex);

        std::vector<DirtySection> result;

        for (const auto& section : m_dirtySections) {
            if (section.chunkPos.x == chunkPos.x && section.chunkPos.z == chunkPos.z) {
                result.push_back(section);
            }
        }

        return result;
    }

    // === INTERNAL IMPLEMENTATION ===

    void DirtyTracker::MarkSectionDirtyInternal(const DirtySection& section) {
        if (!IsValidDirtySection(section)) {
            return;
        }

        bool wasNew = false;
        {
            std::lock_guard<std::mutex> lock(m_dirtyMutex);
            auto [it, inserted] = m_dirtySections.insert(section);
            wasNew = inserted;
        }

        // Update statistics
        UpdateStats(section, wasNew);
    }

    void DirtyTracker::MarkNeighborsDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        if (!m_config.enableNeighborInvalidation) {
            return;
        }

        std::vector<DirtySection> neighbors = CalculateNeighboringSections(chunkPos, sectionIndex);

        for (const auto& neighbor : neighbors) {
            MarkSectionDirtyInternal(neighbor);
        }
    }

    std::vector<DirtySection> DirtyTracker::CalculateNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const {
        std::vector<DirtySection> neighbors;
        neighbors.reserve(6);

        // Neighboring chunks (same section Y)
        neighbors.emplace_back(Math::ChunkPos{chunkPos.x + 1, chunkPos.z}, sectionIndex);
        neighbors.emplace_back(Math::ChunkPos{chunkPos.x - 1, chunkPos.z}, sectionIndex);
        neighbors.emplace_back(Math::ChunkPos{chunkPos.x, chunkPos.z + 1}, sectionIndex);
        neighbors.emplace_back(Math::ChunkPos{chunkPos.x, chunkPos.z - 1}, sectionIndex);

        // Neighboring sections (same chunk)
        if (IsValidSectionIndex(sectionIndex + 1)) {
            neighbors.emplace_back(chunkPos, sectionIndex + 1);
        }
        if (IsValidSectionIndex(sectionIndex - 1)) {
            neighbors.emplace_back(chunkPos, sectionIndex - 1);
        }

        return neighbors;
    }

    // === COORDINATE UTILITIES ===

    Math::ChunkPos DirtyTracker::WorldToChunkPos(int worldX, int worldZ) const {
        return Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
    }

    int DirtyTracker::WorldYToSectionIndex(int worldY) const {
        return Math::WorldCoordinates::WorldYToSectionIndex(worldY);
    }

    bool DirtyTracker::IsValidSectionIndex(int sectionIndex) const {
        return sectionIndex >= 0 && sectionIndex < Math::SECTIONS_PER_CHUNK;
    }

    // === STATISTICS UPDATES ===

    void DirtyTracker::UpdateStats(const DirtySection& section, bool added) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        if (added) {
            m_stats.totalSectionsMarked++;
            m_stats.currentDirtySections++;
        }
    }

    // === VALIDATION HELPERS ===

    bool DirtyTracker::IsValidChunkPos(Math::ChunkPos pos) const {
        const int MAX_CHUNK_COORD = 1000000;
        return std::abs(pos.x) < MAX_CHUNK_COORD && std::abs(pos.z) < MAX_CHUNK_COORD;
    }

    bool DirtyTracker::IsValidDirtySection(const DirtySection& section) const {
        return IsValidChunkPos(section.chunkPos) && IsValidSectionIndex(section.sectionIndex);
    }

    // === UTILITY FUNCTIONS ===

    std::unique_ptr<DirtyTracker> CreateDirtyTracker(const DirtyTrackerConfig& config) {
        return std::make_unique<DirtyTracker>(config);
    }

    DirtySection WorldCoordinatesToDirtySection(int worldX, int worldY, int worldZ) {
        Math::ChunkPos chunkPos = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ);
        int sectionIndex = Math::WorldCoordinates::WorldYToSectionIndex(worldY);
        return DirtySection(chunkPos, sectionIndex);
    }

    std::vector<DirtySection> GetAffectedSections(int worldX, int worldY, int worldZ, bool includeNeighbors) {
        std::vector<DirtySection> sections;

        // Primary section
        DirtySection primary = WorldCoordinatesToDirtySection(worldX, worldY, worldZ);
        sections.push_back(primary);

        if (includeNeighbors) {
            // Check if on chunk boundaries
            int localX = worldX & 15;
            int localZ = worldZ & 15;

            if (localX == 0) {
                Math::ChunkPos westChunk = Math::WorldCoordinates::WorldToChunkPos(worldX - 1, worldZ);
                sections.emplace_back(westChunk, primary.sectionIndex);
            }
            if (localX == 15) {
                Math::ChunkPos eastChunk = Math::WorldCoordinates::WorldToChunkPos(worldX + 1, worldZ);
                sections.emplace_back(eastChunk, primary.sectionIndex);
            }
            if (localZ == 0) {
                Math::ChunkPos northChunk = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ - 1);
                sections.emplace_back(northChunk, primary.sectionIndex);
            }
            if (localZ == 15) {
                Math::ChunkPos southChunk = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ + 1);
                sections.emplace_back(southChunk, primary.sectionIndex);
            }

            // Check if on section boundaries
            int localY = worldY - (primary.sectionIndex * 16);

            if (localY == 0 && primary.sectionIndex > 0) {
                sections.emplace_back(primary.chunkPos, primary.sectionIndex - 1);
            }
            if (localY == 15 && primary.sectionIndex < Math::SECTIONS_PER_CHUNK - 1) {
                sections.emplace_back(primary.chunkPos, primary.sectionIndex + 1);
            }
        }

        return sections;
    }

} // namespace Game