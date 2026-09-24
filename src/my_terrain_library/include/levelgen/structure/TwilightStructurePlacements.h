#pragma once

#include "levelgen/structure/StructurePlacement.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 structure placements —
// world/components/structures/placements/LandmarkGridPlacement.java
// ("twilightforest:landmark_grid") and AvoidLandmarkGridPlacement.java
// ("twilightforest:avoid_landmark_grid"). Parsed by StructureSets.cpp from
// data/twilightforest/worldgen/structure_set/*.json.

namespace minecraft {
namespace levelgen {
namespace structure {

/**
 * LandmarkGridPlacement: a placement chunk is a landmark-centre chunk
 * (LegacyLandmarkPlacements.chunkHasLandmarkCenter) and, with a
 * structure_grid_lock, one whose variety slot holds that structure
 * (pickVarietyLandmark). Without a lock (forceStructureForCenters) every
 * centre qualifies and the structure's biome check decides. The base
 * placement fields are fixed (Vec3i.ZERO, DEFAULT, 1f, salt 0, no exclusion).
 */
class TwilightLandmarkGridPlacement : public StructurePlacement {
public:
    explicit TwilightLandmarkGridPlacement(std::optional<std::string> gridLock)
        : StructurePlacement(0, 0, 0, FrequencyReductionMethod::DEFAULT, 1.0f, 0, std::nullopt),
          m_gridLock(std::move(gridLock)) {}

    const std::optional<std::string>& gridLock() const { return m_gridLock; }

    bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                          int32_t sourceX, int32_t sourceZ) const override;

private:
    std::optional<std::string> m_gridLock;   // "twilightforest:<structure>"
};

/**
 * AvoidLandmarkGridPlacement: a random_spread placement whose chunk must also
 * lie at least 80 blocks (on either axis) from the nearest landmark centre,
 * plus an optional map of other structure sets to keep `range` chunks from
 * (avoid_additional_structures).
 */
class TwilightAvoidLandmarkGridPlacement : public RandomSpreadStructurePlacement {
public:
    TwilightAvoidLandmarkGridPlacement(int32_t locateOffsetX, int32_t locateOffsetY, int32_t locateOffsetZ,
                                       FrequencyReductionMethod frequencyReductionMethod, float frequency,
                                       int32_t salt, std::optional<ExclusionZone> exclusionZone,
                                       int32_t spacing, int32_t separation, RandomSpreadType spreadType,
                                       std::vector<std::pair<std::string, int32_t>> avoidAdditional)
        : RandomSpreadStructurePlacement(locateOffsetX, locateOffsetY, locateOffsetZ,
                                         frequencyReductionMethod, frequency, salt,
                                         std::move(exclusionZone), spacing, separation, spreadType),
          m_avoidAdditional(std::move(avoidAdditional)) {}

    bool isPlacementChunk(const ChunkGeneratorStructureState& state,
                          int32_t sourceX, int32_t sourceZ) const override;

    bool applyInteractionsWithOtherStructures(const ChunkGeneratorStructureState& state,
                                              int32_t sourceX, int32_t sourceZ) const override;

private:
    // (structure set id, chunk range) in the JSON object's order.
    std::vector<std::pair<std::string, int32_t>> m_avoidAdditional;
};

} // namespace structure
} // namespace levelgen
} // namespace minecraft
