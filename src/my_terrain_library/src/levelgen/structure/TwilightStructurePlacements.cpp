#include "levelgen/structure/TwilightStructurePlacements.h"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/TwilightLandmarks.h"

#include <cstdint>
#include <cstdlib>
#include <string>

// Twilight Forest 4.9 — structures/placements/LandmarkGridPlacement.java and
// AvoidLandmarkGridPlacement.java.

namespace minecraft {
namespace levelgen {
namespace structure {

bool TwilightLandmarkGridPlacement::isPlacementChunk(const ChunkGeneratorStructureState& state,
                                                     int32_t sourceX, int32_t sourceZ) const {
    if (!twilight_landmarks::chunkHasLandmarkCenter(sourceX, sourceZ)) return false;
    return !m_gridLock.has_value()
        || twilight_landmarks::pickVarietyLandmark(sourceX, sourceZ, state.getLevelSeed()) == *m_gridLock;
}

bool TwilightAvoidLandmarkGridPlacement::isPlacementChunk(const ChunkGeneratorStructureState& state,
                                                          int32_t sourceX, int32_t sourceZ) const {
    // copy of super
    const auto chunkPos = getPotentialStructureChunk(state.getLevelSeed(), sourceX, sourceZ);
    if (chunkPos.first != sourceX || chunkPos.second != sourceZ) return false;

    // Feature centre -> offset from the candidate chunk's world position.
    const core::BlockPos featurePos = twilight_landmarks::getNearestCenterXZ(sourceX, sourceZ);
    const int32_t offsetX = std::abs(featurePos.getX() - (chunkPos.first << 4));
    const int32_t offsetZ = std::abs(featurePos.getZ() - (chunkPos.second << 4));
    constexpr int32_t kSize = 80;
    return offsetX >= kSize || offsetZ >= kSize;
}

bool TwilightAvoidLandmarkGridPlacement::applyInteractionsWithOtherStructures(
        const ChunkGeneratorStructureState& state, int32_t sourceX, int32_t sourceZ) const {
    if (!StructurePlacement::applyInteractionsWithOtherStructures(state, sourceX, sourceZ)) return false;
    // AvoidAdditionalStructures.isPlacementForbidden.
    for (const auto& [setName, range] : m_avoidAdditional) {
        if (state.hasStructureChunkInRange(setName, sourceX, sourceZ, range)) return false;
    }
    return true;
}

} // namespace structure
} // namespace levelgen
} // namespace minecraft
