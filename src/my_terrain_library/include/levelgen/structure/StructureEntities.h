#pragma once

#include <memory>
#include <string>

// Reference: the entity half of structure placement -
// WorldGenRegion.addFreshEntity(WithPassengers) and the EntityTypes.X.create /
// snapTo / setPersistenceRequired / finalizeSpawn(STRUCTURE) sequence that
// SwampHutPiece, OceanMonumentPieces, OceanRuinPieces, WoodlandMansionPieces
// and EndCityPieces run. The library has no Entity classes: an entity is
// recorded as the compound EntityType.create would have loaded (id, Pos,
// Rotation, PersistenceRequired, ...) and filed under the ProtoChunk its
// position falls in (IChunk::addEntity). The engine builds the mob, applies
// the compound, and runs finalizeSpawn when the chunk is promoted.

namespace minecraft {
namespace nbt {
class CompoundTag;
}
namespace levelgen {
class WorldGenLevel;
namespace structure {
namespace StructureEntities {

/**
 * Reference: WorldGenRegion.addFreshEntity - the entity joins the chunk at
 * SectionPos.blockToSectionCoord(entity.getBlockX/Z()), read from the tag's
 * Pos. `finalizeSpawn` = Mob.finalizeSpawn(level, difficulty, STRUCTURE,
 * null) is owed (it runs engine-side, before the mob is added).
 */
void addFreshEntity(WorldGenLevel* level, std::shared_ptr<nbt::CompoundTag> tag,
                    bool finalizeSpawn);

/**
 * The compound for EntityTypes.<id>.create + snapTo(x, y, z, yRot, xRot):
 * {id, Pos:[x,y,z], Rotation:[yRot,xRot]}, plus PersistenceRequired:1b when
 * the piece calls setPersistenceRequired(). Callers add type-specific keys.
 */
std::shared_ptr<nbt::CompoundTag> mobTag(const std::string& entityId, double x, double y,
                                         double z, float yRot, float xRot,
                                         bool persistenceRequired);

/** Reference: Mth.wrapDegrees(float). */
float wrapDegrees(float angle);

} // namespace StructureEntities
} // namespace structure
} // namespace levelgen
} // namespace minecraft
