#include "levelgen/structure/StructureEntities.h"

#include "levelgen/WorldGenLevel.h"
#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "world/IChunk.h"

#include <cmath>
#include <utility>

// Reference: WorldGenRegion.addFreshEntity and the structure pieces'
// EntityTypes.X.create(level, STRUCTURE) + snapTo sequence. See the header.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace StructureEntities {

float wrapDegrees(float angle) {
    // Reference: Mth.wrapDegrees(float) - Java's float % keeps the sign of
    // the dividend, as std::fmod does.
    float normalized = std::fmod(angle, 360.0f);
    if (normalized >= 180.0f) normalized -= 360.0f;
    if (normalized < -180.0f) normalized += 360.0f;
    return normalized;
}

void addFreshEntity(WorldGenLevel* level, std::shared_ptr<nbt::CompoundTag> tag,
                    bool finalizeSpawn) {
    if (level == nullptr || !tag) return;
    const nbt::ListTag* pos = tag->getListPtr("Pos");
    if (pos == nullptr || pos->size() != 3) return;
    // Reference: entity.getBlockX/Z() = Mth.floor(position), then
    // SectionPos.blockToSectionCoord.
    const int blockX = Mth::floor(pos->getDouble(0));
    const int blockZ = Mth::floor(pos->getDouble(2));
    if (::world::IChunk* chunk = level->getChunk(blockX >> 4, blockZ >> 4)) {
        chunk->addEntity({std::move(tag), finalizeSpawn});
    }
}

std::shared_ptr<nbt::CompoundTag> mobTag(const std::string& entityId, double x, double y,
                                         double z, float yRot, float xRot,
                                         bool persistenceRequired) {
    auto tag = std::make_shared<nbt::CompoundTag>();
    tag->putString("id", entityId);
    auto pos = std::make_unique<nbt::ListTag>();
    pos->add(nbt::DoubleTag::valueOf(x));
    pos->add(nbt::DoubleTag::valueOf(y));
    pos->add(nbt::DoubleTag::valueOf(z));
    tag->put("Pos", std::move(pos));
    auto rotation = std::make_unique<nbt::ListTag>();
    rotation->add(nbt::FloatTag::valueOf(yRot));
    rotation->add(nbt::FloatTag::valueOf(xRot));
    tag->put("Rotation", std::move(rotation));
    if (persistenceRequired) tag->putBoolean("PersistenceRequired", true);
    return tag;
}

} // namespace StructureEntities
} // namespace structure
} // namespace levelgen
} // namespace minecraft
