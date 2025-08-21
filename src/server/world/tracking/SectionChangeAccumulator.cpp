// File: src/server/world/tracking/SectionChangeAccumulator.cpp
#include "SectionChangeAccumulator.hpp"
#include "common/core/Log.hpp"

namespace Server {

SectionChangeAccumulator::SectionChangeAccumulator() {
    // Reserve some initial capacity for common cases
    m_buckets.reserve(64);
}

SectionChangeAccumulator::~SectionChangeAccumulator() = default;

void SectionChangeAccumulator::accumulate(const Game::Math::SectionPos& sp, uint16_t localIdx, Game::BlockID state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Get or create bucket for this section
    auto& bucket = m_buckets[sp];
    
    // Track if this is a new change or overwriting existing
    bool wasNew = (bucket.changes.find(localIdx) == bucket.changes.end());
    
    // Add/update the change (automatically dedups)
    bucket.add(localIdx, state);
    
    // Update statistics
    m_stats.totalAccumulated++;
    if (!wasNew) {
        m_stats.totalDeduplicated++;
    }
}

void SectionChangeAccumulator::accumulate(const Game::Math::SectionPos& sp, 
                                         uint8_t localX, uint8_t localY, uint8_t localZ, 
                                         Game::BlockID state) {
    uint16_t idx = packLocalIndex(localX, localY, localZ);
    accumulate(sp, idx, state);
}

std::vector<std::pair<Game::Math::SectionPos, SectionChangeAccumulator::SectionChanges>> 
SectionChangeAccumulator::drain() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::pair<Game::Math::SectionPos, SectionChanges>> result;
    result.reserve(m_buckets.size());
    
    // Materialize all buckets
    for (auto& [sectionPos, bucket] : m_buckets) {
        if (!bucket.empty()) {
            result.emplace_back(sectionPos, bucket.materialize());
        }
    }
    
    // Update statistics
    m_stats.sectionsAffected = result.size();
    m_stats.lastDrainSize = m_stats.totalAccumulated - m_stats.totalDeduplicated;
    
    // Clear all buckets for next tick
    m_buckets.clear();
    
    // Log if we had significant activity
    if (result.size() > 10) {
        Log::Debug("SectionChangeAccumulator: Drained %zu sections with %zu total changes (%zu deduped)",
                  result.size(), m_stats.totalAccumulated, m_stats.totalDeduplicated);
    }
    
    return result;
}

SectionChangeAccumulator::Stats SectionChangeAccumulator::getStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void SectionChangeAccumulator::resetStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats = Stats{};
}

} // namespace Server