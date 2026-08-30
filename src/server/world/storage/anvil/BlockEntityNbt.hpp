// File: src/server/world/storage/anvil/BlockEntityNbt.hpp
//
// BlockEntity <-> vanilla NBT.
//
//   { id: "minecraft:chest", x, y, z, <type-specific fields> }
//
// COORDINATES. x/z are ABSOLUTE world coordinates and need the chunk origin
// added; y is ALREADY absolute, because the in-memory map is keyed
// (localX, worldY, localZ). Re-deriving y from a section index is the obvious
// mistake here and it is worth 64 blocks of vertical offset.
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "common/world/block/Blocks.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/math/WorldMath.hpp"
#include "server/world/storage/NBTParser.hpp"

#include <memory>

namespace Game::Anvil {

    // Appends one element to an open `block_entities` list. Returns false —
    // writing nothing — for a block entity with no vanilla-known type, so a
    // half-formed entry never reaches the file.
    bool WriteBlockEntity(Nbt::Writer& w, Nbt::Writer::ListScope& list,
                          const BlockEntity& entity, Math::ChunkPos chunkPos);

    // Rebuilds one. `blockAt` is the block actually sitting at that position,
    // which the caller reads from the already-decoded sections — vanilla
    // validates the same pairing (a `minecraft:chest` tag on a furnace is
    // rejected), and it is also how the concrete class gets its block id.
    //
    // Returns null for an id this build does not know, a block/type mismatch,
    // or a position outside `chunkPos`. `outLocal` receives the
    // (localX, worldY, localZ) key the chunk map uses.
    std::unique_ptr<BlockEntity> ReadBlockEntity(const ::World::NBTTagCompound& tag,
                                                 Math::ChunkPos chunkPos,
                                                 BlockID blockAt,
                                                 glm::ivec3& outLocal);

} // namespace Game::Anvil
