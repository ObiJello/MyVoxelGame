#include "levelgen/placement/PlacementContext.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include <algorithm>

// Reference: net/minecraft/world/level/levelgen/placement/PlacementContext.java

namespace minecraft {
namespace levelgen {
namespace placement {

PlacementContext::PlacementContext(
    WorldGenLevel* level,
    ChunkGenerator* generator,
    std::optional<const PlacedFeature*> topFeature
)
    : WorldGenerationContext(
        // Java: this.minY = Math.max(heightAccessor.getMinY(), generator.getMinY());
        // Java: this.height = Math.min(heightAccessor.getHeight(), generator.getGenDepth());
        std::max(level->getMinY(), generator->getMinY()),
        std::min(level->getMaxY() - level->getMinY(), generator->getGenDepth())
    )
    , m_level(level)
    , m_generator(generator)
    , m_topFeature(topFeature)
{
}

} // namespace placement
} // namespace levelgen
} // namespace minecraft
