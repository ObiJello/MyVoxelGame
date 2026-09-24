#include "levelgen/structure/StructureSerialization.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "nbt/AllTags.h"
#include "nbt/NbtIo.h"
#include "world/IChunk.h"
#include <cstdio>
#include <sstream>

// Reference: SerializableChunkData.packStructureData / unpackStructureStart /
// unpackStructureReferences, StructureStart.createTag, StructurePiece.createTag.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace StructureSerialization {

namespace {

std::unique_ptr<nbt::Tag> boxTag(const BoundingBox& box) {
    return std::make_unique<nbt::IntArrayTag>(
        std::vector<int32_t>{box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ});
}

// StructurePiece.createTag: id, BB, O, GD, then addAdditionalSaveData.
// Orientation: this port keeps a piece's Rotation, not the Direction MC
// derives it from (SOUTH and NORTH both rotate NONE), so O is -1 (null).
std::unique_ptr<nbt::CompoundTag> pieceTag(const StructurePieceData& piece,
                                           const StructurePieceBehavior* behavior) {
    auto tag = std::make_unique<nbt::CompoundTag>();
    tag->putString("id", piece.pieceType);
    tag->put("BB", boxTag(piece.boundingBox));
    tag->putInt("O", -1);
    tag->putInt("GD", piece.genDepth);
    if (behavior != nullptr) {
        behavior->saveState(*tag);
    }
    return tag;
}

// StructureStart.createTag.
std::unique_ptr<nbt::CompoundTag> startTag(const std::string& name, const StructureStartData& start) {
    auto tag = std::make_unique<nbt::CompoundTag>();
    if (!start.isValid()) {
        tag->putString("id", "INVALID");
        return tag;
    }
    tag->putString("id", name);
    tag->putInt("ChunkX", start.startChunkPos.x());
    tag->putInt("ChunkZ", start.startChunkPos.z());
    tag->putInt("references", start.references);
    auto children = std::make_unique<nbt::ListTag>();
    for (size_t i = 0; i < start.pieces.size(); ++i) {
        const StructurePieceBehavior* behavior =
            i < start.behaviors.size() ? start.behaviors[i].get() : nullptr;
        children->add(pieceTag(start.pieces[i], behavior));
    }
    tag->put("Children", std::move(children));
    return tag;
}

} // namespace

std::unique_ptr<nbt::CompoundTag> packStructureData(const world::ChunkPos& pos,
                                                    const StructureStartMap& starts,
                                                    const StructureReferenceMap& references) {
    (void)pos;
    auto out = std::make_unique<nbt::CompoundTag>();

    auto ownStarts = std::make_unique<nbt::CompoundTag>();
    for (const auto& [name, start] : starts) {
        if (!start.isValid()) continue;
        ownStarts->put(name, startTag(name, start));
    }
    // Vanilla's key stays empty: see the header.
    out->put("starts", std::make_unique<nbt::CompoundTag>());
    if (!ownStarts->isEmpty()) {
        out->put(kStartsKey, std::move(ownStarts));
    }

    auto referencesTag = std::make_unique<nbt::CompoundTag>();
    for (const auto& [name, refs] : references) {
        if (refs.empty()) continue;
        referencesTag->putLongArray(name, refs);
    }
    out->put("References", std::move(referencesTag));
    return out;
}

std::vector<uint8_t> encodeStructureData(const world::ChunkPos& pos,
                                         const StructureStartMap& starts,
                                         const StructureReferenceMap& references) {
    std::ostringstream out;
    nbt::NbtIo::write(*packStructureData(pos, starts, references), out);
    const std::string bytes = out.str();
    // NbtIo::write puts the root's id and empty name first (3 bytes).
    return bytes.size() > 3 ? std::vector<uint8_t>(bytes.begin() + 3, bytes.end())
                            : std::vector<uint8_t>{};
}

void unpackReferences(const nbt::CompoundTag& structures, const world::ChunkPos& pos,
                      world::IChunk& chunk) {
    const nbt::CompoundTag* referencesTag = structures.getCompoundPtr("References");
    if (referencesTag == nullptr) return;
    for (const auto& [name, value] : *referencesTag) {
        if (value->getId() != nbt::TagType::TAG_LONG_ARRAY) continue;
        const std::vector<int64_t> refs = *value->asLongArray();
        for (int64_t packed : refs) {
            const world::ChunkPos refPos = world::ChunkPos::fromLong(packed);
            if (refPos.getChessboardDistance(pos) > 8) {
                std::fprintf(stderr, "[StructureSerialization] Found invalid structure reference [ %s @ %s ] for chunk %s.\n",
                             name.c_str(), refPos.toString().c_str(), pos.toString().c_str());
                continue;
            }
            chunk.addReferenceForStructure(name, packed);
        }
    }
}

const nbt::CompoundTag* savedStarts(const nbt::CompoundTag& structures) {
    const nbt::CompoundTag* starts = structures.getCompoundPtr(kStartsKey);
    return starts != nullptr && !starts->isEmpty() ? starts : nullptr;
}

void restoreStarts(const nbt::CompoundTag& saved, world::IChunk& chunk) {
    const world::ChunkPos pos = chunk.getPos();
    for (const auto& [name, value] : saved) {
        if (value->getId() != nbt::TagType::TAG_COMPOUND) continue;
        const nbt::CompoundTag& tag = *value->asCompound();
        if (tag.getStringOr("id", "") == "INVALID") continue;

        StructureStartData* start = chunk.getMutableStartForStructure(name);
        if (start == nullptr || !start->isValid()) {
            std::fprintf(stderr, "[StructureSerialization] chunk %s: saved %s start is no longer generated; its unplaced parts are lost\n",
                         pos.toString().c_str(), name.c_str());
            continue;
        }
        start->references = tag.getIntOr("references", start->references);

        const nbt::ListTag* children = tag.getListPtr("Children");
        bool matches = children != nullptr && children->size() == start->pieces.size();
        for (size_t i = 0; matches && i < start->pieces.size(); ++i) {
            const nbt::CompoundTag* child = children->getCompound(i);
            matches = child != nullptr && child->getStringOr("id", "") == start->pieces[i].pieceType;
        }
        if (!matches) {
            std::fprintf(stderr, "[StructureSerialization] chunk %s: saved %s start no longer matches its regenerated pieces; keeping the regenerated one\n",
                         pos.toString().c_str(), name.c_str());
            continue;
        }

        for (size_t i = 0; i < start->pieces.size(); ++i) {
            const nbt::CompoundTag& child = *children->getCompound(i);
            StructurePieceData& piece = start->pieces[i];
            const std::vector<int32_t> box = child.getIntArray("BB");
            if (box.size() == 6) {
                piece.boundingBox = BoundingBox(box[0], box[1], box[2], box[3], box[4], box[5]);
            }
            if (i < start->behaviors.size() && start->behaviors[i]) {
                start->behaviors[i]->loadState(child, piece);
            }
        }
    }
}

} // namespace StructureSerialization
} // namespace structure
} // namespace levelgen
} // namespace minecraft
