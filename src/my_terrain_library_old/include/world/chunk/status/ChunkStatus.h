#pragma once

#include "world/chunk/status/ChunkType.h"
#include "levelgen/Heightmap.h"
#include <cstdint>
#include <string>
#include <vector>
#include <set>

// Reference: net/minecraft/world/level/chunk/status/ChunkStatus.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

/**
 * ChunkStatus - Represents the generation stage of a chunk
 *
 * The 12 stages in order (Java reference lines 19-30, 113-124):
 *   EMPTY -> STRUCTURE_STARTS -> STRUCTURE_REFERENCES -> BIOMES ->
 *   NOISE -> SURFACE -> CARVERS -> FEATURES ->
 *   INITIALIZE_LIGHT -> LIGHT -> SPAWN -> FULL
 *
 * Each status tracks:
 *   - Parent status (what must be done before)
 *   - Heightmaps to generate
 *   - Whether result is ProtoChunk or LevelChunk
 *
 * Reference: ChunkStatus.java
 */
class ChunkStatus {
public:
    /**
     * Maximum distance for structure references
     * Reference: ChunkStatus.java line 16
     */
    static constexpr int32_t MAX_STRUCTURE_DISTANCE = 8;

    /**
     * Status index values - matches Java static initialization order
     * Reference: ChunkStatus.java lines 113-124
     */
    enum Index : int32_t {
        EMPTY_INDEX = 0,
        STRUCTURE_STARTS_INDEX = 1,
        STRUCTURE_REFERENCES_INDEX = 2,
        BIOMES_INDEX = 3,
        NOISE_INDEX = 4,
        SURFACE_INDEX = 5,
        CARVERS_INDEX = 6,
        FEATURES_INDEX = 7,
        INITIALIZE_LIGHT_INDEX = 8,
        LIGHT_INDEX = 9,
        SPAWN_INDEX = 10,
        FULL_INDEX = 11,
        STATUS_COUNT = 12
    };

private:
    int32_t m_index;
    const ChunkStatus* m_parent;
    ChunkType m_chunkType;
    std::set<levelgen::Heightmap::Types> m_heightmapsAfter;
    std::string m_name;

    /**
     * Private constructor for static registration
     * Reference: ChunkStatus.java lines 55-60
     */
    ChunkStatus(
        const std::string& name,
        const ChunkStatus* parent,
        std::set<levelgen::Heightmap::Types> heightmapsAfter,
        ChunkType chunkType
    );

public:
    // Static status instances (defined in cpp)
    // Reference: ChunkStatus.java lines 19-30, 113-124
    static const ChunkStatus EMPTY;
    static const ChunkStatus STRUCTURE_STARTS;
    static const ChunkStatus STRUCTURE_REFERENCES;
    static const ChunkStatus BIOMES;
    static const ChunkStatus NOISE;
    static const ChunkStatus SURFACE;
    static const ChunkStatus CARVERS;
    static const ChunkStatus FEATURES;
    static const ChunkStatus INITIALIZE_LIGHT;
    static const ChunkStatus LIGHT;
    static const ChunkStatus SPAWN;
    static const ChunkStatus FULL;

    /**
     * Worldgen heightmaps - used during generation
     * Reference: ChunkStatus.java line 111
     */
    static std::set<levelgen::Heightmap::Types> WORLDGEN_HEIGHTMAPS;

    /**
     * Final heightmaps - kept after worldgen
     * Reference: ChunkStatus.java line 112
     */
    static std::set<levelgen::Heightmap::Types> FINAL_HEIGHTMAPS;

    /**
     * Get the status index (0 = EMPTY, 11 = FULL)
     * Reference: ChunkStatus.java lines 62-64
     */
    int32_t getIndex() const { return m_index; }

    /**
     * Get the parent status (EMPTY returns itself)
     * Reference: ChunkStatus.java lines 66-68
     */
    const ChunkStatus& getParent() const { return *m_parent; }

    /**
     * Get the chunk type this status produces
     * Reference: ChunkStatus.java lines 70-72
     */
    ChunkType getChunkType() const { return m_chunkType; }

    /**
     * Get the heightmaps to be populated at this status
     * Reference: ChunkStatus.java lines 78-80
     */
    const std::set<levelgen::Heightmap::Types>& heightmapsAfter() const {
        return m_heightmapsAfter;
    }

    /**
     * Get the name of this status
     * Reference: ChunkStatus.java lines 106-108
     */
    const std::string& getName() const { return m_name; }

    /**
     * Check if this status is at or after the given status
     * Reference: ChunkStatus.java lines 82-84
     */
    bool isOrAfter(const ChunkStatus& step) const {
        return m_index >= step.m_index;
    }

    /**
     * Check if this status is strictly after the given status
     * Reference: ChunkStatus.java lines 86-88
     */
    bool isAfter(const ChunkStatus& step) const {
        return m_index > step.m_index;
    }

    /**
     * Check if this status is at or before the given status
     * Reference: ChunkStatus.java lines 90-92
     */
    bool isOrBefore(const ChunkStatus& step) const {
        return m_index <= step.m_index;
    }

    /**
     * Check if this status is strictly before the given status
     * Reference: ChunkStatus.java lines 94-96
     */
    bool isBefore(const ChunkStatus& step) const {
        return m_index < step.m_index;
    }

    /**
     * Return the later of two statuses
     * Reference: ChunkStatus.java lines 98-100
     */
    static const ChunkStatus& max(const ChunkStatus& a, const ChunkStatus& b) {
        return a.isAfter(b) ? a : b;
    }

    /**
     * Get ordered list of all statuses from EMPTY to FULL
     * Reference: ChunkStatus.java lines 41-52
     */
    static std::vector<const ChunkStatus*> getStatusList();

    /**
     * Get a status by name
     * Reference: ChunkStatus.java lines 74-76
     */
    static const ChunkStatus* byName(const std::string& name);

    /**
     * Get a status by index
     */
    static const ChunkStatus* byIndex(int32_t index);

    /**
     * Equality comparison (by index)
     */
    bool operator==(const ChunkStatus& other) const {
        return m_index == other.m_index;
    }

    bool operator!=(const ChunkStatus& other) const {
        return m_index != other.m_index;
    }
};

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
