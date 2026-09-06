// File: src/server/world/storage/anvil/EntityChunkNbt.hpp
//
// entities/r.X.Z.mca — the 1.17+ split entity storage.
//
// Root NBT, verified against the user's real 1.21.8 and 26.1 worlds rather
// than taken from the decompile:
//
//   { DataVersion: int,
//     Entities:    [ <entity compound>, ... ],
//     Position:    [I; chunkX, chunkZ] }        <- an INT ARRAY of two, not a compound
//
// A chunk whose entity list would be empty is DELETED from the region file
// rather than written as an empty list — measured across 415 entity chunks in
// a real world, not one held an empty Entities list. AnvilRegion::Clear is
// what performs that deletion.
//
// Only ROOT entities appear here; a rider is nested inside its vehicle's
// "Passengers" list and never stored separately.
#pragma once

#include "common/world/math/WorldMath.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/entity/ExperienceOrb.hpp"
#include "common/entity/ItemEntity.hpp"

#include "common/entity/EntityType.hpp"
#include "server/world/storage/NBTParser.hpp"

namespace Game {
    class Mob;
    struct EntityLevel;
}

namespace Game::Anvil {

    // Everything belonging to one chunk, gathered by the caller from the three
    // managers. Raw pointers: the caller owns the entities and outlives the
    // call, which is the same contract the chunk serialiser has with Chunk.
    struct EntityChunkContents {
        std::vector<const Game::Mob*>           mobs;
        std::vector<const Game::ItemEntity*>    items;
        std::vector<const Game::ExperienceOrb*> orbs;

        bool Empty() const { return mobs.empty() && items.empty() && orbs.empty(); }
        size_t Count() const { return mobs.size() + items.size() + orbs.size(); }
    };

    // Uncompressed root NBT. The caller compresses and hands the bytes to the
    // region writer, exactly as the chunk path does.
    //
    // Returns false with `out` untouched when there is nothing to write —
    // check Empty() first if you need to distinguish "nothing" from "failed",
    // because an empty chunk must be CLEARED rather than written.
    bool SerialiseEntityChunk(const EntityChunkContents& contents,
                              Math::ChunkPos pos, int dataVersion,
                              std::vector<uint8_t>& out, std::string& error);

    // One entity restored from disk. The variant is discriminated by `kind`
    // because the three managers are separate and take different types.
    struct LoadedEntity {
        enum class Kind : uint8_t { Mob, Item, Orb };
        Kind kind = Kind::Mob;

        // Kind::Mob — the TYPE plus its saved compound. Construction happens
        // in the caller, which is where the EntityTypeId -> Mob factory lives;
        // duplicating that 71-case switch here would be a second thing to keep
        // in sync with every new mob.
        Game::EntityTypeId          mobType{};
        ::World::NBTTagPtr          mobTag;

        Game::ItemEntity            item;   // Kind::Item
        Game::ExperienceOrb         orb;    // Kind::Orb

        // Kind::Mob — the riders nested in this entity's "Passengers" list,
        // already classified. They are NOT in the top-level `out` vector: a
        // rider that appeared there too would be constructed twice, and the
        // second copy would sit unmounted inside the first.
        //
        // The caller builds the vehicle, then each of these, then calls
        // StartRiding(vehicle, force=true) — see Entity.hpp's jockey note.
        std::vector<LoadedEntity> passengers;
    };

    // NEVER fails the chunk for one bad entity: vanilla logs "Skipping Entity
    // with id {}" and carries on, so a mob type this build does not know must
    // not cost the player the rest of the chunk. `outSkipped` counts them.
    //
    // A Position mismatch only warns, matching EntityStorage.
    bool DeserialiseEntityChunk(const std::vector<uint8_t>& nbt, Math::ChunkPos expected,
                                Game::EntityLevel* level,
                                std::vector<LoadedEntity>& out, size_t& outSkipped,
                                std::string& error);

} // namespace Game::Anvil
