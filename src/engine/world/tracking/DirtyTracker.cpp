// File: src/engine/world/tracking/DirtyTracker.cpp
#include "DirtyTracker.hpp"
#include "../../../game/WorldCoordinates.hpp"
#include "../../../core/Log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace Game {

    // === DIRTY TRACKER IMPLEMENTATION ===

    DirtyTracker::DirtyTracker(const DirtyTrackerConfig& config)
        : m_config(config)
        , m_lastBatchTime(std::chrono::steady_clock::now())
        , m_lastCleanup(std::chrono::steady_clock::now()) {

        Log::Debug("DirtyTracker created with batching=%s, maxBatch=%zu",
                  config.enableBatching ? "enabled" : "disabled", config.maxBatchSize);
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

        if (m_config.enableBatching) {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            m_pendingBatch.push_back(section);

            if (ShouldFlushBatch()) {
                FlushBatch();
            }
        } else {
            MarkSectionDirtyInternal(section);
        }

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
        int maxSection = WorldYToSectionIndex(maxWorldY);

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
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        DirtySection section(chunkPos, sectionIndex);
        return m_dirtySections.find(section) != m_dirtySections.end();
    }

    bool DirtyTracker::IsChunkDirty(Math::ChunkPos chunkPos) const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);

        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            DirtySection section(chunkPos, sectionY);
            if (m_dirtySections.find(section) != m_dirtySections.end()) {
                return true;
            }
        }
        return false;
    }

    size_t DirtyTracker::GetDirtyCount() const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        return m_dirtySections.size();
    }

    bool DirtyTracker::HasDirtySections() const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        return !m_dirtySections.empty();
    }

    // === BATCH RETRIEVAL ===

    std::vector<DirtySection> DirtyTracker::GetAndClearAllDirtySections() {
        // Flush any pending batch first
        if (m_config.enableBatching) {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            if (!m_pendingBatch.empty()) {
                FlushBatch();
            }
        }

        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);

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

        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);

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

        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);

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
        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);

        size_t clearedCount = m_dirtySections.size();
        m_dirtySections.clear();

        // Update statistics
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSectionsCleared += clearedCount;
            m_stats.currentDirtySections = 0;
        }
    }

    // === NEIGHBOR INVALIDATION ===

    void DirtyTracker::MarkNeighborsDirty(Math::ChunkPos chunkPos, int sectionIndex) {
        if (!m_config.enableNeighborInvalidation) {
            return;
        }

        std::vector<DirtySection> neighbors = GetNeighboringSections(chunkPos, sectionIndex);

        for (const auto& neighbor : neighbors) {
            MarkSectionDirtyInternal(neighbor);
        }

        // Update statistics
        {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.neighborInvalidations += neighbors.size();
        }
    }

    std::vector<DirtySection> DirtyTracker::GetNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const {
        return CalculateNeighboringSections(chunkPos, sectionIndex);
    }

    // === CONFIGURATION ===

    void DirtyTracker::SetConfig(const DirtyTrackerConfig& config) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config = config;
        Log::Debug("DirtyTracker config updated");
    }

    DirtyTrackerConfig DirtyTracker::GetConfig() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config;
    }

    void DirtyTracker::SetNeighborInvalidationEnabled(bool enabled) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.enableNeighborInvalidation = enabled;
    }

    bool DirtyTracker::IsNeighborInvalidationEnabled() const {
        std::shared_lock<std::shared_mutex> lock(m_configMutex);
        return m_config.enableNeighborInvalidation;
    }

    void DirtyTracker::SetBatchingEnabled(bool enabled) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.enableBatching = enabled;

        if (!enabled) {
            // Flush any pending batch
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            if (!m_pendingBatch.empty()) {
                FlushBatch();
            }
        }
    }

    void DirtyTracker::SetMaxBatchSize(size_t maxSize) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.maxBatchSize = std::max(size_t(1), maxSize);
    }

    void DirtyTracker::SetBatchTimeout(float timeoutMs) {
        std::lock_guard<std::shared_mutex> lock(m_configMutex);
        m_config.batchTimeoutMs = std::max(1.0f, timeoutMs);
    }

    // === CALLBACKS ===

    void DirtyTracker::SetDirtyCallback(DirtyCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_dirtyCallback = callback;
    }

    void DirtyTracker::SetBatchCallback(DirtyBatchCallback callback) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_batchCallback = callback;
    }

    void DirtyTracker::ClearCallbacks() {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_dirtyCallback = nullptr;
        m_batchCallback = nullptr;
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
        ClearCallbacks();

        m_initialized = true;
        m_shutdownRequested = false;

        Log::Info("DirtyTracker initialized successfully");
        return true;
    }

    void DirtyTracker::Shutdown() {
        if (!m_initialized) {
            return;
        }

        Log::Info("Shutting down DirtyTracker...");

        m_shutdownRequested = true;

        // Flush any pending batch
        if (m_config.enableBatching) {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            if (!m_pendingBatch.empty()) {
                FlushBatch();
            }
        }

        // Clear all state
        ClearAll();
        ClearCallbacks();

        m_initialized = false;
        Log::Info("DirtyTracker shutdown complete");
    }

    void DirtyTracker::Update(float deltaTime) {
        if (!m_initialized || m_shutdownRequested) {
            return;
        }

        // Process pending batches
        if (m_config.enableBatching) {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            if (ShouldFlushBatch()) {
                FlushBatch();
            }
        }

        // Perform cleanup if needed
        if (m_config.enableAutoCleanup && ShouldPerformCleanup()) {
            PerformCleanup();
        }

        // Enforce memory limits
        EnforceMemoryLimits();
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
        return CalculateMemoryUsage();
    }

    // === DEBUGGING ===

    std::vector<DirtySection> DirtyTracker::GetAllDirtySections() const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        
        std::vector<DirtySection> result;
        result.reserve(m_dirtySections.size());
        
        for (const auto& section : m_dirtySections) {
            result.push_back(section);
        }
        
        return result;
    }

    void DirtyTracker::LogDirtySections(const std::string& prefix) const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        
        Log::Info("%s: %zu dirty sections", prefix.c_str(), m_dirtySections.size());
        
        if (m_dirtySections.size() <= 10) {
            // Log individual sections if not too many
            for (const auto& section : m_dirtySections) {
                Log::Debug("  %s", section.ToString().c_str());
            }
        }
    }

    bool DirtyTracker::ValidateState() const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        
        // Check for duplicate entries (shouldn't happen with unordered_set)
        size_t expectedSize = m_dirtySections.size();
        
        // Validate each section
        for (const auto& section : m_dirtySections) {
            if (!IsValidDirtySection(section)) {
                Log::Error("DirtyTracker: Invalid section found: %s", section.ToString().c_str());
                return false;
            }
        }
        
        return true;
    }

    // === BULK OPERATIONS ===

    void DirtyTracker::MarkSectionsDirty(const std::vector<DirtySection>& sections) {
        if (sections.empty()) {
            return;
        }

        if (m_config.enableBatching) {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            
            for (const auto& section : sections) {
                m_pendingBatch.push_back(section);
            }
            
            if (ShouldFlushBatch()) {
                FlushBatch();
            }
        } else {
            for (const auto& section : sections) {
                MarkSectionDirtyInternal(section);
            }
        }
    }

    void DirtyTracker::ClearAll() {
        // Clear dirty sections
        ClearAllDirtySections();
        
        // Clear pending batch
        {
            std::lock_guard<std::mutex> batchLock(m_batchMutex);
            m_pendingBatch.clear();
        }
    }

    // === FILTERING ===

    std::vector<DirtySection> DirtyTracker::GetDirtySectionsForChunk(Math::ChunkPos chunkPos) const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        
        std::vector<DirtySection> result;
        
        for (const auto& section : m_dirtySections) {
            if (section.chunkPos.x == chunkPos.x && section.chunkPos.z == chunkPos.z) {
                result.push_back(section);
            }
        }
        
        return result;
    }

    std::vector<DirtySection> DirtyTracker::GetDirtySectionsInRadius(Math::ChunkPos center, int radius) const {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        
        std::vector<DirtySection> result;
        int radiusSquared = radius * radius;
        
        for (const auto& section : m_dirtySections) {
            int dx = section.chunkPos.x - center.x;
            int dz = section.chunkPos.z - center.z;
            int distanceSquared = dx * dx + dz * dz;
            
            if (distanceSquared <= radiusSquared) {
                result.push_back(section);
            }
        }
        
        return result;
    }

    size_t DirtyTracker::ClearDirtySectionsOutsideRadius(Math::ChunkPos center, int radius) {
        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);
        
        int radiusSquared = radius * radius;
        size_t clearedCount = 0;
        
        auto it = m_dirtySections.begin();
        while (it != m_dirtySections.end()) {
            int dx = it->chunkPos.x - center.x;
            int dz = it->chunkPos.z - center.z;
            int distanceSquared = dx * dx + dz * dz;
            
            if (distanceSquared > radiusSquared) {
                it = m_dirtySections.erase(it);
                clearedCount++;
            } else {
                ++it;
            }
        }
        
        // Update statistics
        if (clearedCount > 0) {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.totalSectionsCleared += clearedCount;
            m_stats.currentDirtySections = m_dirtySections.size();
        }
        
        return clearedCount;
    }

    // === INTERNAL IMPLEMENTATION ===

    void DirtyTracker::MarkSectionDirtyInternal(const DirtySection& section) {
        if (!IsValidDirtySection(section)) {
            return;
        }

        bool wasNew = false;
        {
            std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);
            auto [it, inserted] = m_dirtySections.insert(section);
            wasNew = inserted;
        }

        // Update statistics
        UpdateStats(section, wasNew);

        // Invoke callback if section was newly added
        if (wasNew) {
            InvokeDirtyCallback(section);
        }
    }

    void DirtyTracker::ProcessPendingBatch() {
        std::lock_guard<std::mutex> lock(m_batchMutex);
        
        if (!m_pendingBatch.empty()) {
            FlushBatch();
        }
    }

    void DirtyTracker::FlushBatch() {
        if (m_pendingBatch.empty()) {
            return;
        }

        std::vector<DirtySection> batch = std::move(m_pendingBatch);
        m_pendingBatch.clear();
        m_lastBatchTime = std::chrono::steady_clock::now();

        // Process batch
        for (const auto& section : batch) {
            MarkSectionDirtyInternal(section);
        }

        // Update batch statistics
        UpdateBatchStats(batch.size());

        // Invoke batch callback
        InvokeBatchCallback(batch);
    }

    bool DirtyTracker::ShouldFlushBatch() const {
        if (m_pendingBatch.empty()) {
            return false;
        }

        // Check size limit
        if (m_pendingBatch.size() >= m_config.maxBatchSize) {
            return true;
        }

        // Check time limit
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastBatchTime);
        
        return elapsed.count() >= m_config.batchTimeoutMs;
    }

    std::vector<DirtySection> DirtyTracker::CalculateNeighboringSections(Math::ChunkPos chunkPos, int sectionIndex) const {
        std::vector<DirtySection> neighbors;
        neighbors.reserve(6); // Up to 6 neighboring sections

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
            m_stats.maxDirtySections = std::max(m_stats.maxDirtySections, m_stats.currentDirtySections);
        } else {
            m_stats.duplicateMarks++;
        }
    }

    void DirtyTracker::UpdateBatchStats(size_t batchSize) {
        std::lock_guard<std::mutex> lock(m_statsMutex);

        m_stats.totalBatchesProcessed++;
        
        // Update average batch size
        float totalBatches = static_cast<float>(m_stats.totalBatchesProcessed);
        float currentAverage = m_stats.averageBatchSize;
        m_stats.averageBatchSize = (currentAverage * (totalBatches - 1) + static_cast<float>(batchSize)) / totalBatches;
    }

    // === CLEANUP OPERATIONS ===

    void DirtyTracker::PerformCleanup() {
        auto now = std::chrono::steady_clock::now();
        m_lastCleanup = now;

        // Currently no stale entries to clean up, but this could be extended
        // to remove sections for unloaded chunks, etc.
    }

    bool DirtyTracker::ShouldPerformCleanup() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastCleanup);
        
        return elapsed.count() >= m_config.cleanupIntervalSeconds;
    }

    // === VALIDATION HELPERS ===

    bool DirtyTracker::IsValidChunkPos(Math::ChunkPos pos) const {
        // Basic validation - prevent extreme coordinates
        const int MAX_CHUNK_COORD = 1000000;
        return std::abs(pos.x) < MAX_CHUNK_COORD && std::abs(pos.z) < MAX_CHUNK_COORD;
    }

    bool DirtyTracker::IsValidDirtySection(const DirtySection& section) const {
        return IsValidChunkPos(section.chunkPos) && IsValidSectionIndex(section.sectionIndex);
    }

    // === MEMORY MANAGEMENT ===

    void DirtyTracker::EnforceMemoryLimits() {
        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);

        if (m_dirtySections.size() > m_config.maxDirtySections) {
            // Remove oldest sections (simplified - could be more sophisticated)
            size_t toRemove = m_dirtySections.size() - m_config.maxDirtySections;
            auto it = m_dirtySections.begin();
            
            for (size_t i = 0; i < toRemove && it != m_dirtySections.end(); ++i) {
                it = m_dirtySections.erase(it);
            }
            
            Log::Warning("DirtyTracker: Removed %zu sections due to memory limit", toRemove);
        }
    }

    size_t DirtyTracker::CalculateMemoryUsage() const {
        std::shared_lock<std::shared_mutex> dirtyLock(m_dirtyMutex);
        std::lock_guard<std::mutex> batchLock(m_batchMutex);

        size_t usage = 0;
        
        // Dirty sections storage
        usage += m_dirtySections.size() * sizeof(DirtySection);
        
        // Pending batch storage
        usage += m_pendingBatch.capacity() * sizeof(DirtySection);
        
        // Hash table overhead (rough estimate)
        usage += m_dirtySections.bucket_count() * sizeof(void*);
        
        return usage;
    }

    // === CALLBACK INVOCATION ===

    void DirtyTracker::InvokeDirtyCallback(const DirtySection& section) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        
        if (m_dirtyCallback) {
            try {
                m_dirtyCallback(section);
            } catch (const std::exception& e) {
                Log::Error("DirtyTracker: Dirty callback exception: %s", e.what());
            }
        }
    }

    void DirtyTracker::InvokeBatchCallback(const std::vector<DirtySection>& batch) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        
        if (m_batchCallback) {
            try {
                m_batchCallback(batch);
            } catch (const std::exception& e) {
                Log::Error("DirtyTracker: Batch callback exception: %s", e.what());
            }
        }
    }

    // === TEMPLATE IMPLEMENTATIONS ===

    template<typename Func>
    auto DirtyTracker::WithReadLock(Func&& func) const -> decltype(func()) {
        std::shared_lock<std::shared_mutex> lock(m_dirtyMutex);
        return func();
    }

    template<typename Func>
    auto DirtyTracker::WithWriteLock(Func&& func) -> decltype(func()) {
        std::lock_guard<std::shared_mutex> lock(m_dirtyMutex);
        return func();
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
            // Check if on chunk boundaries (need to mark neighboring chunks)
            int localX = worldX & 15; // worldX % 16 (but handles negatives correctly)
            int localZ = worldZ & 15;
            
            if (localX == 0) {
                // Western boundary
                Math::ChunkPos westChunk = Math::WorldCoordinates::WorldToChunkPos(worldX - 1, worldZ);
                sections.emplace_back(westChunk, primary.sectionIndex);
            }
            if (localX == 15) {
                // Eastern boundary  
                Math::ChunkPos eastChunk = Math::WorldCoordinates::WorldToChunkPos(worldX + 1, worldZ);
                sections.emplace_back(eastChunk, primary.sectionIndex);
            }
            if (localZ == 0) {
                // Northern boundary
                Math::ChunkPos northChunk = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ - 1);
                sections.emplace_back(northChunk, primary.sectionIndex);
            }
            if (localZ == 15) {
                // Southern boundary
                Math::ChunkPos southChunk = Math::WorldCoordinates::WorldToChunkPos(worldX, worldZ + 1);
                sections.emplace_back(southChunk, primary.sectionIndex);
            }
            
            // Check if on section boundaries (need to mark neighboring sections)
            int localY = worldY - (primary.sectionIndex * 16);
            
            if (localY == 0 && primary.sectionIndex > 0) {
                // Bottom of section
                sections.emplace_back(primary.chunkPos, primary.sectionIndex - 1);
            }
            if (localY == 15 && primary.sectionIndex < Math::SECTIONS_PER_CHUNK - 1) {
                // Top of section
                sections.emplace_back(primary.chunkPos, primary.sectionIndex + 1);
            }
        }
        
        return sections;
    }

    // === DIRTY BATCH OPERATOR IMPLEMENTATION ===

    DirtyBatchOperator::DirtyBatchOperator(DirtyTracker& tracker)
        : m_tracker(tracker) {
        m_batch.reserve(100); // Reserve space for efficient batching
    }

    DirtyBatchOperator::~DirtyBatchOperator() {
        if (!m_flushed && !m_batch.empty()) {
            Flush();
        }
    }

    void DirtyBatchOperator::AddSection(Math::ChunkPos chunkPos, int sectionIndex) {
        if (m_flushed) {
            return;
        }
        
        m_batch.emplace_back(chunkPos, sectionIndex);
    }

    void DirtyBatchOperator::AddChunk(Math::ChunkPos chunkPos) {
        if (m_flushed) {
            return;
        }
        
        for (int sectionY = 0; sectionY < Math::SECTIONS_PER_CHUNK; ++sectionY) {
            m_batch.emplace_back(chunkPos, sectionY);
        }
    }

    void DirtyBatchOperator::AddRegion(int minWorldX, int minWorldY, int minWorldZ,
                                     int maxWorldX, int maxWorldY, int maxWorldZ) {
        if (m_flushed) {
            return;
        }
        
        // Convert world coordinates to chunk/section ranges
        Math::ChunkPos minChunk = Math::WorldCoordinates::WorldToChunkPos(minWorldX, minWorldZ);
        Math::ChunkPos maxChunk = Math::WorldCoordinates::WorldToChunkPos(maxWorldX, maxWorldZ);
        
        int minSection = Math::WorldCoordinates::WorldYToSectionIndex(minWorldY);
        int maxSection = Math::WorldCoordinates::WorldYToSectionIndex(maxWorldZ);

        // Add all affected sections to batch
        for (int chunkX = minChunk.x; chunkX <= maxChunk.x; ++chunkX) {
            for (int chunkZ = minChunk.z; chunkZ <= maxChunk.z; ++chunkZ) {
                for (int sectionY = minSection; sectionY <= maxSection; ++sectionY) {
                    if (sectionY >= 0 && sectionY < Math::SECTIONS_PER_CHUNK) {
                        m_batch.emplace_back(Math::ChunkPos{chunkX, chunkZ}, sectionY);
                    }
                }
            }
        }
    }

    void DirtyBatchOperator::Flush() {
        if (m_flushed || m_batch.empty()) {
            return;
        }
        
        m_tracker.MarkSectionsDirty(m_batch);
        m_batch.clear();
        m_flushed = true;
    }

} // namespace Game