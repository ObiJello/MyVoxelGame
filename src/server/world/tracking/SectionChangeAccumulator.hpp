// File: src/server/world/tracking/SectionChangeAccumulator.hpp
#pragma once

#include "common/world/math/WorldMath.hpp"
#include "common/world/block/Blocks.hpp"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

namespace Server {

// Accumulates block changes per section during a server tick
// Thread-safe for concurrent world updates
class SectionChangeAccumulator {
public:
    // Change record: packed local index -> final block state
    using ChangeRecord = std::pair<uint16_t, Game::BlockID>;
    using SectionChanges = std::vector<ChangeRecord>;
    
    SectionChangeAccumulator();
    ~SectionChangeAccumulator();
    
    // === ACCUMULATION ===
    
    // Called by World::SetBlock to record a block change
    // Thread-safe: can be called from multiple threads
    void accumulate(const Game::Math::SectionPos& sp, uint16_t localIdx, Game::BlockID state);
    
    // Convenience overload with unpacked coordinates
    void accumulate(const Game::Math::SectionPos& sp, uint8_t localX, uint8_t localY, uint8_t localZ, Game::BlockID state);
    
    // === FLUSHING ===
    
    // Called once per tick by ChunkDeltaBroadcaster
    // Returns all accumulated changes and clears the accumulator
    // Thread-safe: blocks new accumulations during drain
    std::vector<std::pair<Game::Math::SectionPos, SectionChanges>> drain();
    
    // === HELPERS ===
    
    // Pack local coordinates (0-15 each) into a 16-bit index
    // Layout: xxxxyyyy zzzz0000 (4 bits each for x,y,z, 4 bits unused)
    static constexpr uint16_t packLocalIndex(uint8_t x, uint8_t y, uint8_t z) {
        return (static_cast<uint16_t>(x & 0xF) << 12) |
               (static_cast<uint16_t>(y & 0xF) << 8) |
               (static_cast<uint16_t>(z & 0xF) << 4);
        // Lower 4 bits reserved for future use
    }
    
    // Unpack a 16-bit index back to local coordinates
    static constexpr void unpackLocalIndex(uint16_t idx, uint8_t& x, uint8_t& y, uint8_t& z) {
        x = (idx >> 12) & 0xF;
        y = (idx >> 8) & 0xF;
        z = (idx >> 4) & 0xF;
    }
    
    // === STATISTICS ===
    
    struct Stats {
        size_t totalAccumulated = 0;
        size_t totalDeduplicated = 0;
        size_t sectionsAffected = 0;
        size_t lastDrainSize = 0;
    };
    
    Stats getStats() const;
    void resetStats();
    
private:
    // Internal bucket for deduplicating changes within a section
    struct DeltaBucket {
        // Map of packed index to final block state
        // If same block is changed multiple times in a tick, keep only the last state
        std::unordered_map<uint16_t, Game::BlockID> changes;
        
        // Add or update a change
        void add(uint16_t idx, Game::BlockID state) {
            changes[idx] = state;  // Overwrites if exists (dedup)
        }
        
        // Convert to vector for transmission
        SectionChanges materialize() const {
            SectionChanges result;
            result.reserve(changes.size());
            for (const auto& [idx, state] : changes) {
                result.emplace_back(idx, state);
            }
            return result;
        }
        
        size_t size() const { return changes.size(); }
        bool empty() const { return changes.empty(); }
    };
    
    // Main storage: section -> bucket of changes
    std::unordered_map<Game::Math::SectionPos, DeltaBucket, Game::Math::SectionPosHash> m_buckets;
    
    // Thread safety
    mutable std::mutex m_mutex;
    
    // Statistics
    mutable Stats m_stats;
};

} // namespace Server