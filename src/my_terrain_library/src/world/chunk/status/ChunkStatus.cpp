#include "world/chunk/status/ChunkStatus.h"
#include <algorithm>

// Reference: net/minecraft/world/level/chunk/status/ChunkStatus.java

namespace minecraft {
namespace world {
namespace chunk {
namespace status {

// Static heightmap sets initialization
// Reference: ChunkStatus.java lines 111-112
std::set<levelgen::Heightmap::Types> ChunkStatus::WORLDGEN_HEIGHTMAPS = {
    levelgen::Heightmap::Types::OCEAN_FLOOR_WG,
    levelgen::Heightmap::Types::WORLD_SURFACE_WG
};

std::set<levelgen::Heightmap::Types> ChunkStatus::FINAL_HEIGHTMAPS = {
    levelgen::Heightmap::Types::OCEAN_FLOOR,
    levelgen::Heightmap::Types::WORLD_SURFACE,
    levelgen::Heightmap::Types::MOTION_BLOCKING,
    levelgen::Heightmap::Types::MOTION_BLOCKING_NO_LEAVES
};

// Private constructor
// Reference: ChunkStatus.java lines 55-60
ChunkStatus::ChunkStatus(
    const std::string& name,
    const ChunkStatus* parent,
    std::set<levelgen::Heightmap::Types> heightmapsAfter,
    ChunkType chunkType
)
    : m_index(parent == nullptr ? 0 : parent->m_index + 1)
    , m_parent(parent == nullptr ? this : parent)
    , m_chunkType(chunkType)
    , m_heightmapsAfter(std::move(heightmapsAfter))
    , m_name(name)
{
}

// Static status definitions
// Reference: ChunkStatus.java lines 113-124
// Order is critical for correct index assignment!

const ChunkStatus ChunkStatus::EMPTY(
    "empty",
    nullptr,  // No parent - this is the first status
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::STRUCTURE_STARTS(
    "structure_starts",
    &EMPTY,
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::STRUCTURE_REFERENCES(
    "structure_references",
    &STRUCTURE_STARTS,
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::BIOMES(
    "biomes",
    &STRUCTURE_REFERENCES,
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::NOISE(
    "noise",
    &BIOMES,
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::SURFACE(
    "surface",
    &NOISE,
    WORLDGEN_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::CARVERS(
    "carvers",
    &SURFACE,
    FINAL_HEIGHTMAPS,  // Switches to final heightmaps here
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::FEATURES(
    "features",
    &CARVERS,
    FINAL_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::INITIALIZE_LIGHT(
    "initialize_light",
    &FEATURES,
    FINAL_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::LIGHT(
    "light",
    &INITIALIZE_LIGHT,
    FINAL_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::SPAWN(
    "spawn",
    &LIGHT,
    FINAL_HEIGHTMAPS,
    ChunkType::PROTOCHUNK
);

const ChunkStatus ChunkStatus::FULL(
    "full",
    &SPAWN,
    FINAL_HEIGHTMAPS,
    ChunkType::LEVELCHUNK  // Final status produces LevelChunk
);

// Static lookup table for all statuses (by index)
static const ChunkStatus* ALL_STATUSES[] = {
    &ChunkStatus::EMPTY,
    &ChunkStatus::STRUCTURE_STARTS,
    &ChunkStatus::STRUCTURE_REFERENCES,
    &ChunkStatus::BIOMES,
    &ChunkStatus::NOISE,
    &ChunkStatus::SURFACE,
    &ChunkStatus::CARVERS,
    &ChunkStatus::FEATURES,
    &ChunkStatus::INITIALIZE_LIGHT,
    &ChunkStatus::LIGHT,
    &ChunkStatus::SPAWN,
    &ChunkStatus::FULL
};

// Get ordered list of all statuses from EMPTY to FULL
// Reference: ChunkStatus.java lines 41-52
std::vector<const ChunkStatus*> ChunkStatus::getStatusList() {
    std::vector<const ChunkStatus*> list;
    list.reserve(STATUS_COUNT);

    // Walk backwards from FULL to build list, then reverse
    const ChunkStatus* status = &FULL;
    while (&status->getParent() != status) {
        list.push_back(status);
        status = &status->getParent();
    }
    list.push_back(status);  // Add EMPTY

    std::reverse(list.begin(), list.end());
    return list;
}

// Get status by name
// Reference: ChunkStatus.java lines 74-76
const ChunkStatus* ChunkStatus::byName(const std::string& name) {
    for (int32_t i = 0; i < STATUS_COUNT; ++i) {
        if (ALL_STATUSES[i]->getName() == name) {
            return ALL_STATUSES[i];
        }
    }
    return nullptr;
}

// Get status by index
const ChunkStatus* ChunkStatus::byIndex(int32_t index) {
    if (index >= 0 && index < STATUS_COUNT) {
        return ALL_STATUSES[index];
    }
    return nullptr;
}

} // namespace status
} // namespace chunk
} // namespace world
} // namespace minecraft
