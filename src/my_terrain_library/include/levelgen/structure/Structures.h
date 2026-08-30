#pragma once

#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/StructureStartData.h"
#include "random/LegacyRandomSource.h"
#include "world/biome/BiomeSource.h"
#include "world/biome/Climate.h"
#include <cstdint>
#include <string>
#include <unordered_set>

// Reference: net/minecraft/world/level/levelgen/structure/Structure.java and
// the per-structure classes under .../structure/structures/.
//
// Behavioral layer: given a placement-positive chunk, run the structure's
// findGenerationPoint + piece layout and produce a StructureStartData.
// Batches add structures incrementally; Structures::isImplemented gates which
// ones generate (unimplemented ones fail like an invalid start, and the parity
// gates compare Java dumps filtered to the implemented subset).

namespace minecraft {
namespace levelgen {
class ChunkGenerator;
class RandomState;
} // namespace levelgen
} // namespace minecraft

namespace minecraft {
namespace levelgen {
namespace structure {

/**
 * Reference: Structure.GenerationContext. The random is seeded by makeRandom:
 * WorldgenRandom(LegacyRandomSource(0)).setLargeFeatureSeed(seed, cx, cz),
 * and is threaded through findGenerationPoint AND piece building. Piece
 * building runs AFTER the biome validity check (GenerationStub holds a lazy
 * consumer) - draw order depends on this.
 */
struct GenerationContext {
    ChunkGenerator* generator;
    RandomState* randomState;
    world::biome::BiomeSource* biomeSource;
    const world::biome::Climate::Sampler* sampler;
    LegacyRandomSource random{0};
    int64_t seed;
    int32_t chunkX;
    int32_t chunkZ;
    const std::unordered_set<std::string>* validBiomes;

    GenerationContext(ChunkGenerator* gen, RandomState* rs,
                      world::biome::BiomeSource* source,
                      const world::biome::Climate::Sampler* smp,
                      int64_t worldSeed, int32_t cx, int32_t cz,
                      const std::unordered_set<std::string>* biomes)
        : generator(gen), randomState(rs), biomeSource(source), sampler(smp),
          seed(worldSeed), chunkX(cx), chunkZ(cz), validBiomes(biomes) {
        random.setLargeFeatureSeed(worldSeed, cx, cz);
    }
};

namespace Structures {

/** True when the structure's findGenerationPoint + layout is ported. */
bool isImplemented(const StructureInfo& info);

/**
 * Reference: Structure.generate(). Returns true and fills `out` when the
 * structure produced a valid start (biome check passed, pieces non-empty).
 * `references` is the fetchReferences value (existing start's counter or 0).
 */
bool generate(const StructureInfo& info, GenerationContext& context,
              int32_t references, StructureStartData& out);

} // namespace Structures

} // namespace structure
} // namespace levelgen
} // namespace minecraft
