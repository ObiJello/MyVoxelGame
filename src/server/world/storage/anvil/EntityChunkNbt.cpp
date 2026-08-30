// File: src/server/world/storage/anvil/EntityChunkNbt.cpp
#include "server/world/storage/anvil/EntityChunkNbt.hpp"

#include "server/world/storage/anvil/EntityNbt.hpp"

#include "common/core/Log.hpp"
#include "common/entity/EntityType.hpp"
#include "common/entity/Mob.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

namespace Game::Anvil {

    bool SerialiseEntityChunk(const EntityChunkContents& contents,
                              Math::ChunkPos pos, int dataVersion,
                              std::vector<uint8_t>& out, std::string& error) {
        if (contents.Empty()) {
            error.clear();
            return false;   // caller must CLEAR the chunk, not write an empty list
        }

        Nbt::Writer w;
        w.BeginRootCompound();
        w.Int("DataVersion", dataVersion);

        size_t written = 0;
        {
            auto list = w.BeginList("Entities", Nbt::TagType::Compound);
            // A rider is written INSIDE its vehicle's compound by WriteMob, so
            // emitting it here as well would restore two of it.
            //
            // The test is "its vehicle is a MOB", not "it is a passenger":
            // a mob riding a player has no vehicle in this file at all (players
            // live in playerdata), so skipping it would lose it outright.
            // Written as a root it merely arrives dismounted, which is what
            // vanilla's own eject-on-save leaves behind too.
            for (const Mob* mob : contents.mobs) {
                if (!mob) continue;
                if (dynamic_cast<const Mob*>(mob->GetVehicle())) continue;
                if (WriteMob(w, list, *mob)) ++written;
            }
            for (const ItemEntity* item : contents.items) if (item && WriteItem(w, list, *item)) ++written;
            for (const ExperienceOrb* orb : contents.orbs) if (orb && WriteOrb (w, list, *orb))  ++written;
            w.EndList(list);
        }

        // An INT ARRAY of two, not a compound — measured against real files.
        {
            const int32_t position[2] = {pos.x, pos.z};
            w.IntArray("Position", position, 2);
        }

        w.EndRootCompound();
        if (!w.ok()) { error = "NBT writer refused the entity chunk"; return false; }

        // Everything was filtered out (all projectiles, all dead). Same
        // outcome as an empty chunk: clear it rather than write an empty list.
        if (written == 0) { error.clear(); return false; }

        out = w.TakeBytes();
        return true;
    }

    namespace {

        // Pull the nested "Passengers" list out of one entity compound.
        //
        // Only MOBS are accepted as riders: an item or an orb in a Passengers
        // list is malformed (vanilla can never produce one), and accepting it
        // would need a mount call neither manager offers. Anything else counts
        // as skipped rather than failing the chunk, matching the top level.
        void ReadPassengers(const ::World::NBTTagCompound& tag, LoadedEntity& into,
                            size_t& outSkipped) {
            auto list = std::dynamic_pointer_cast<::World::NBTTagList>(tag.GetTag("Passengers"));
            if (!list) return;
            for (const auto& element : list->value) {
                auto child = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
                if (!child) { ++outSkipped; continue; }

                EntityTypeId type{};
                if (ClassifyEntity(*child, type) != EntityKind::Mob) { ++outSkipped; continue; }

                LoadedEntity rider;
                rider.kind    = LoadedEntity::Kind::Mob;
                rider.mobType = type;
                rider.mobTag  = child;
                ReadPassengers(*child, rider, outSkipped);   // stacked jockeys
                into.passengers.push_back(std::move(rider));
            }
        }

    } // namespace

    bool DeserialiseEntityChunk(const std::vector<uint8_t>& nbt, Math::ChunkPos expected,
                                Game::EntityLevel* level,
                                std::vector<LoadedEntity>& out, size_t& outSkipped,
                                std::string& error) {
        (void)level;   // construction is the caller's job — see ApplyMobNbt
        outSkipped = 0;

        ::World::NBTTagPtr root;
        try {
            root = ::World::NBTParser::Parse(nbt);
        } catch (const std::exception& e) {
            error = std::string("entity chunk parse failed: ") + e.what();
            return false;
        }
        auto rootC = std::dynamic_pointer_cast<::World::NBTTagCompound>(root);
        if (!rootC) { error = "entity chunk root is not a compound"; return false; }

        // A mismatch only warns. Vanilla relocates rather than dropping, and
        // refusing here would cost the player every entity in the chunk.
        if (auto position = std::dynamic_pointer_cast<::World::NBTTagIntArray>(rootC->GetTag("Position"));
            position && position->value.size() == 2) {
            if (position->value[0] != expected.x || position->value[1] != expected.z) {
                Log::Warning("[Anvil] entity chunk at (%d,%d) says it is (%d,%d)",
                             expected.x, expected.z, position->value[0], position->value[1]);
            }
        }

        auto list = std::dynamic_pointer_cast<::World::NBTTagList>(rootC->GetTag("Entities"));
        if (!list) { error.clear(); return true; }   // nothing to do, not a failure

        out.reserve(out.size() + list->value.size());
        for (const auto& element : list->value) {
            auto tag = std::dynamic_pointer_cast<::World::NBTTagCompound>(element);
            if (!tag) { ++outSkipped; continue; }

            EntityTypeId type{};
            switch (ClassifyEntity(*tag, type)) {
                case EntityKind::Item: {
                    LoadedEntity loaded;
                    loaded.kind = LoadedEntity::Kind::Item;
                    if (!ReadItem(*tag, loaded.item)) { ++outSkipped; break; }
                    out.push_back(std::move(loaded));
                    break;
                }
                case EntityKind::Orb: {
                    LoadedEntity loaded;
                    loaded.kind = LoadedEntity::Kind::Orb;
                    if (!ReadOrb(*tag, loaded.orb)) { ++outSkipped; break; }
                    out.push_back(std::move(loaded));
                    break;
                }
                case EntityKind::Mob: {
                    // The caller constructs (the factory lives in the server,
                    // beside the 71-case MakeMob) and then calls ApplyMobNbt.
                    // Recording the type and the tag keeps that split clean.
                    LoadedEntity loaded;
                    loaded.kind = LoadedEntity::Kind::Mob;
                    loaded.mobType = type;
                    loaded.mobTag  = tag;
                    ReadPassengers(*tag, loaded, outSkipped);
                    out.push_back(std::move(loaded));
                    break;
                }
                case EntityKind::Unknown:
                default:
                    // One entity this build does not know must never cost the
                    // player the rest of the chunk — vanilla logs "Skipping
                    // Entity with id" and carries on.
                    ++outSkipped;
                    break;
            }
        }

        error.clear();
        return true;
    }

} // namespace Game::Anvil
