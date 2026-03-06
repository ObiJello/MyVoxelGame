#include "server/level/ChunkLevel.h"
#include "world/chunk/status/ChunkStep.h"
#include "world/chunk/status/ChunkPyramid.h"
#include "world/chunk/status/ChunkStatus.h"
#include <stdexcept>
#include <iostream>

// Reference: net/minecraft/server/level/ChunkLevel.java

namespace minecraft {
namespace server {
namespace level {

// Static member initialization
bool ChunkLevel::s_initialized = false;
const world::chunk::status::ChunkStep* ChunkLevel::s_fullChunkStep = nullptr;
int ChunkLevel::s_radiusAroundFullChunk = 0;
int ChunkLevel::s_maxLevel = 0;

void ChunkLevel::initialize() {
    if (s_initialized) {
        return;
    }

    // Reference: ChunkLevel.java lines 73-77
    // FULL_CHUNK_STEP = ChunkPyramid.GENERATION_PYRAMID.getStepTo(ChunkStatus.FULL);
    // RADIUS_AROUND_FULL_CHUNK = FULL_CHUNK_STEP.accumulatedDependencies().getRadius();
    // MAX_LEVEL = 33 + RADIUS_AROUND_FULL_CHUNK;

    using ChunkPyramid = world::chunk::status::ChunkPyramid;
    using ChunkStatus = world::chunk::status::ChunkStatus;


    // Ensure ChunkPyramid is initialized before we access it
    // This handles the case where ChunkLevel is accessed before ChunkPyramid::initialize() is called
    if (!ChunkPyramid::isInitialized()) {
        ChunkPyramid::initialize();
    }

    // Get the FULL chunk step from the generation pyramid
    const auto& pyramid = ChunkPyramid::getGenerationPyramid();

    s_fullChunkStep = &pyramid.getStepTo(ChunkStatus::FULL);

    // Get the radius of accumulated dependencies for FULL status
    const auto& deps = s_fullChunkStep->accumulatedDependencies();

    s_radiusAroundFullChunk = deps.getRadius();

    // MAX_LEVEL = 33 + radius (e.g., 33 + 11 = 44)
    s_maxLevel = FULL_CHUNK_LEVEL + s_radiusAroundFullChunk;

    s_initialized = true;
}

bool ChunkLevel::isInitialized() {
    return s_initialized;
}

int ChunkLevel::getRadiusAroundFullChunk() {
    if (!s_initialized) {
        initialize();
    }
    return s_radiusAroundFullChunk;
}

int ChunkLevel::getMaxLevel() {
    if (!s_initialized) {
        initialize();
    }
    return s_maxLevel;
}

const world::chunk::status::ChunkStatus* ChunkLevel::generationStatus(int level) {
    // Reference: ChunkLevel.java lines 17-19
    return getStatusAroundFullChunk(level - FULL_CHUNK_LEVEL, nullptr);
}

const world::chunk::status::ChunkStatus* ChunkLevel::getStatusAroundFullChunk(
    int distanceToFullChunk,
    const world::chunk::status::ChunkStatus* defaultValue
) {
    // Reference: ChunkLevel.java lines 22-28
    if (!s_initialized) {
        initialize();
    }

    if (distanceToFullChunk > s_radiusAroundFullChunk) {
        return defaultValue;
    }

    if (distanceToFullChunk <= 0) {
        return &world::chunk::status::ChunkStatus::FULL;
    }

    // Return the status at that distance from FULL using the actual pyramid dependencies
    return &s_fullChunkStep->accumulatedDependencies().get(distanceToFullChunk);
}

const world::chunk::status::ChunkStatus* ChunkLevel::getStatusAroundFullChunkOrEmpty(
    int distanceToFullChunk
) {
    // Reference: ChunkLevel.java lines 30-32
    return getStatusAroundFullChunk(distanceToFullChunk, &world::chunk::status::ChunkStatus::EMPTY);
}

int ChunkLevel::byStatus(const world::chunk::status::ChunkStatus& status) {
    // Reference: ChunkLevel.java lines 34-36
    // return 33 + FULL_CHUNK_STEP.getAccumulatedRadiusOf(status);

    if (!s_initialized) {
        initialize();
    }

    // Use the actual accumulated radius from the FULL chunk step
    return FULL_CHUNK_LEVEL + s_fullChunkStep->getAccumulatedRadiusOf(status);
}

FullChunkStatus ChunkLevel::fullStatus(int level) {
    // Reference: ChunkLevel.java lines 38-46
    if (level <= ENTITY_TICKING_LEVEL) {  // <= 31
        return FullChunkStatus::ENTITY_TICKING;
    } else if (level <= BLOCK_TICKING_LEVEL) {  // <= 32
        return FullChunkStatus::BLOCK_TICKING;
    } else if (level <= FULL_CHUNK_LEVEL) {  // <= 33
        return FullChunkStatus::FULL;
    } else {
        return FullChunkStatus::INACCESSIBLE;
    }
}

int ChunkLevel::byStatus(FullChunkStatus status) {
    // Reference: ChunkLevel.java lines 48-59
    if (!s_initialized) {
        initialize();
    }

    switch (status) {
        case FullChunkStatus::INACCESSIBLE:
            return s_maxLevel;
        case FullChunkStatus::FULL:
            return FULL_CHUNK_LEVEL;  // 33
        case FullChunkStatus::BLOCK_TICKING:
            return BLOCK_TICKING_LEVEL;  // 32
        case FullChunkStatus::ENTITY_TICKING:
            return ENTITY_TICKING_LEVEL;  // 31
        default:
            throw std::runtime_error("Unknown FullChunkStatus");
    }
}

bool ChunkLevel::isEntityTicking(int level) {
    // Reference: ChunkLevel.java lines 61-63
    return level <= ENTITY_TICKING_LEVEL;  // <= 31
}

bool ChunkLevel::isBlockTicking(int level) {
    // Reference: ChunkLevel.java lines 65-67
    return level <= BLOCK_TICKING_LEVEL;  // <= 32
}

bool ChunkLevel::isLoaded(int level) {
    // Reference: ChunkLevel.java lines 69-71
    if (!s_initialized) {
        initialize();
    }
    return level <= s_maxLevel;
}

} // namespace level
} // namespace server
} // namespace minecraft
