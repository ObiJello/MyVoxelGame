#include "levelgen/feature/TemplateFeature.h"
#include "levelgen/structure/TemplateEngine.h"
#include <limits>

namespace minecraft {
namespace levelgen {

namespace {

// Rotation.rotate(direction).getUnitVec3i() for the negative X and Z axis
// directions (WEST, NORTH), by Rotation ordinal (NONE, CLOCKWISE_90,
// CLOCKWISE_180, COUNTERCLOCKWISE_90).
core::BlockPos rotatedNegativeX(int rotation) {
    switch (rotation) {
        case 1:  return core::BlockPos(0, 0, -1);  // WEST -> NORTH
        case 2:  return core::BlockPos(1, 0, 0);   // WEST -> EAST
        case 3:  return core::BlockPos(0, 0, 1);   // WEST -> SOUTH
        default: return core::BlockPos(-1, 0, 0);  // WEST
    }
}

core::BlockPos rotatedNegativeZ(int rotation) {
    switch (rotation) {
        case 1:  return core::BlockPos(1, 0, 0);   // NORTH -> EAST
        case 2:  return core::BlockPos(0, 0, 1);   // NORTH -> SOUTH
        case 3:  return core::BlockPos(-1, 0, 0);  // NORTH -> WEST
        default: return core::BlockPos(0, 0, -1);  // NORTH
    }
}

} // namespace

bool TemplateFeature::place(FeaturePlaceContext<TemplateFeatureConfiguration>& context) {
    const TemplateFeatureConfiguration& config = context.config();
    WorldgenRandom& random = context.random();
    if (config.templates.empty()) {
        return false;
    }

    // templates.getRandomOrThrow(random): nextInt(totalWeight)
    int totalWeight = 0;
    for (const auto& entry : config.templates) totalWeight += entry.weight;
    int selection = random.nextInt(totalWeight);
    const TemplateFeatureConfiguration::Entry* chosen = &config.templates.back();
    for (const auto& entry : config.templates) {
        if (selection < entry.weight) {
            chosen = &entry;
            break;
        }
        selection -= entry.weight;
    }

    // Util.getRandom(rotations, random)
    const int rotation = chosen->rotations[static_cast<size_t>(
        random.nextInt(static_cast<int32_t>(chosen->rotations.size())))];

    const structure::FullTemplateData& data = structure::TemplateEngine::get(chosen->templateId);
    const core::BlockPos offsetX = rotatedNegativeX(rotation);
    const core::BlockPos offsetZ = rotatedNegativeZ(rotation);
    const int halfX = data.sizeX / 2;
    const int halfZ = data.sizeZ / 2;
    const core::BlockPos pos = context.origin()
        .offset(offsetX.getX() * halfX, offsetX.getY() * halfX, offsetX.getZ() * halfX)
        .offset(offsetZ.getX() * halfZ, offsetZ.getY() * halfZ, offsetZ.getZ() * halfZ);

    structure::TemplatePlaceSettings settings;
    settings.rotation = rotation;
    settings.appendLootRules = config.appendLootRules;
    // StructureTemplate.placeInWorld: settings.getRandomPalette draws
    // nextInt(paletteCount) from the settings' random - the feature random.
    if (!data.palettes.empty()) {
        settings.paletteIndex = random.nextInt(static_cast<int32_t>(data.palettes.size()));
    }

    // No settings bounding box: the level's own write guard clips.
    constexpr int kFar = std::numeric_limits<int>::max() / 4;
    const structure::BoundingBox unbounded(-kFar, -kFar, -kFar, kFar, kFar, kFar);
    return structure::TemplateEngine::placeInWorld(context.level(), chosen->templateId, pos, pos, settings,
                                                   random, unbounded);
}

} // namespace levelgen
} // namespace minecraft
