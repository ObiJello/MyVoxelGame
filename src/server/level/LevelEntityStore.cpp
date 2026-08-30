// File: src/server/level/LevelEntityStore.cpp
#include "server/level/LevelEntityStore.hpp"

#include "server/level/ServerLevel.hpp"
#include "server/entity/ExperienceOrbManager.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/entity/ServerLevelBridge.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/storage/anvil/EntityChunkNbt.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/entity/Mob.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/level/World.hpp"

#include <cmath>

namespace Server {

    namespace {

        Game::Math::ChunkPos ChunkOf(const glm::dvec3& pos) {
            return Game::Math::ChunkPos(
                static_cast<int>(std::floor(pos.x)) >> 4,
                static_cast<int>(std::floor(pos.z)) >> 4);
        }

        Game::ChunkProvider* ProviderOf(ServerLevel& level) {
            Game::World* world = level.World();
            return world ? world->GetChunkProvider() : nullptr;
        }

        // Build one saved mob and everything riding it, mounting as it goes.
        // Returns the mob it added, or nullptr if the mob itself could not be
        // restored (a rider that fails is counted as skipped and the vehicle
        // still loads — losing a jockey must not lose the chicken).
        //
        // The order is vanilla's and it matters: a rider is positioned off its
        // vehicle's LIVE position on the next tick, so the vehicle has to exist
        // and be placed before StartRiding runs. `force` is passed because the
        // seat rules were already satisfied when this jockey was created — a
        // later tightening of CanRide must never silently unstack saved mobs.
        Game::Mob* RestoreMobTree(Game::Anvil::LoadedEntity& entry,
                                  MobManager& mobs, ServerLevelBridge& bridge,
                                  size_t& restored, size_t& skipped) {
            auto tag = std::dynamic_pointer_cast<::World::NBTTagCompound>(entry.mobTag);
            if (!tag) return nullptr;

            std::unique_ptr<Game::Mob> mob =
                MakeMobForLoad(entry.mobType, static_cast<Game::EntityLevel*>(&bridge));
            if (!mob) return nullptr;

            Game::Anvil::ApplyMobNbt(*tag, *mob);

            // Already here? A chunk can be asked to load while its entities are
            // live — a re-entry, or two code paths both making it live. Without
            // this the world fills with clones.
            if (!Game::UuidIsNil(mob->GetUuid()) && mobs.HasUuid(mob->GetUuid())) {
                return nullptr;
            }

            const int32_t id = mobs.Add(std::move(mob));
            if (id == 0) return nullptr;
            Game::Mob* vehicle = mobs.Find(id);
            if (!vehicle) return nullptr;
            ++restored;

            for (auto& rider : entry.passengers) {
                Game::Mob* mounted = RestoreMobTree(rider, mobs, bridge, restored, skipped);
                if (!mounted) { ++skipped; continue; }
                mounted->StartRiding(*vehicle, /*force=*/true);
            }
            return vehicle;
        }

    } // namespace

    void LevelEntityStore::RequestLoad(Game::Math::ChunkPos pos) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) return;

        // Already asked, or already ours.
        const auto it = m_state.find(pos);
        if (it != m_state.end() && it->second != State::Absent) return;

        m_state[pos] = State::Pending;

        // Read on THIS thread for now. The read is a few kilobytes and a zlib
        // inflate; moving it to a worker is a later optimisation, and doing it
        // here keeps the state machine honest — Pending never outlives the call.
        std::vector<uint8_t> nbt;
        std::string error;
        bool ok = false;
        {
            // Split from the deserialise below because a single call here was
            // measured at 20.4 SECONDS on the server thread at a hundred
            // thousand entities, and the enclosing ChunkResult.EntityLoad zone
            // cannot say whether that is the disk read, the inflate, the NBT
            // walk or the adopt loop. Those want completely different fixes.
            PROFILE_ZONE_N("EntityLoad.Read");
            ok = provider->ReadEntityChunkNbt(pos, nbt, error);
        }
        if (!ok && !error.empty()) {
            // Say the consequence, not just the failure. This path is
            // deliberately non-fatal — the chunk still loads and the world
            // still plays — but the entities that were on disk are now dropped,
            // and the next save of this chunk overwrites them. A player who
            // sees only "read failed" has no way to know they are one autosave
            // away from losing whatever was in there.
            Log::Error("[Anvil] entity read failed for chunk (%d,%d): %s", pos.x, pos.z,
                       error.c_str());
            Log::Error("[Anvil]   chunk (%d,%d) loads with NO entities; anything stored "
                       "there is dropped and will be overwritten on the next save of it",
                       pos.x, pos.z);
        }
        ApplyLoadResult(pos, nbt, ok);
    }

    void LevelEntityStore::ApplyLoadResult(Game::Math::ChunkPos pos,
                                           const std::vector<uint8_t>& nbt,
                                           bool readSucceeded) {
        // Whatever happens below, the managers are authoritative from here on.
        // Leaving it Pending would mean this chunk could never be saved.
        m_state[pos] = State::Loaded;

        if (!readSucceeded || nbt.empty()) return;   // nothing on disk yet
        m_onDisk.insert(pos);

        MobManager*           mobs  = m_level.Mobs();
        ItemEntityManager*    items = m_level.Items();
        ExperienceOrbManager* orbs  = m_level.Orbs();
        ServerLevelBridge*    bridge = m_level.MobLevel();
        if (!mobs || !items || !orbs || !bridge) return;

        std::vector<Game::Anvil::LoadedEntity> loaded;
        size_t skipped = 0;
        std::string error;
        // Own scope: two ZoneScopedN in one scope is a redefinition of
        // ___tracy_scoped_zone, and the adopt loop below needs its own.
        bool deserialised = false;
        {
            PROFILE_ZONE_N("EntityLoad.Deserialise");
            deserialised = Game::Anvil::DeserialiseEntityChunk(
                nbt, pos, static_cast<Game::EntityLevel*>(bridge), loaded, skipped, error);
        }
        if (!deserialised) {
            Log::Error("[Anvil] entity chunk (%d,%d): %s", pos.x, pos.z, error.c_str());
            return;
        }

        size_t restored = 0;
        PROFILE_ZONE_N("EntityLoad.Adopt");
        for (auto& entry : loaded) {
            switch (entry.kind) {
                case Game::Anvil::LoadedEntity::Kind::Item:
                    items->Adopt(std::move(entry.item));
                    ++restored;
                    break;
                case Game::Anvil::LoadedEntity::Kind::Orb:
                    orbs->Adopt(std::move(entry.orb));
                    ++restored;
                    break;
                case Game::Anvil::LoadedEntity::Kind::Mob:
                    // Recursive: a vehicle brings its riders (and theirs) with
                    // it, mounted, before the next root entity is touched.
                    if (!RestoreMobTree(entry, *mobs, *bridge, restored, skipped)) {
                        ++skipped;
                    }
                    break;
            }
        }

        if (restored > 0 || skipped > 0) {
            Log::Info("[Anvil] chunk (%d,%d): restored %zu entities%s",
                      pos.x, pos.z, restored,
                      skipped ? (" (" + std::to_string(skipped) + " skipped)").c_str() : "");
        }
    }

    bool LevelEntityStore::SaveChunk(Game::Math::ChunkPos pos, std::string& error) {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) { error.clear(); return false; }

        // Gather FIRST, then decide. What we hold for this chunk is the thing
        // that determines whether refusing to write is safe.
        auto gather = [&](Game::Anvil::EntityChunkContents& out) {
            out = {};
            if (MobManager* mobs = m_level.Mobs()) {
                for (const auto& [id, mob] : mobs->All()) {
                    if (mob && ChunkOf(mob->position) == pos) out.mobs.push_back(mob.get());
                }
            }
            if (ItemEntityManager* items = m_level.Items()) {
                for (const auto& [id, item] : items->All()) {
                    if (ChunkOf(item.pos) == pos) out.items.push_back(&item);
                }
            }
            if (ExperienceOrbManager* orbs = m_level.Orbs()) {
                for (const auto& [id, orb] : orbs->All()) {
                    if (ChunkOf(orb.pos) == pos) out.orbs.push_back(&orb);
                }
            }
        };

        Game::Anvil::EntityChunkContents contents;
        gather(contents);

        auto claimed = [&] {
            const auto it = m_state.find(pos);
            return it != m_state.end() && it->second == State::Loaded;
        };

        if (!claimed()) {
            // An UNCLAIMED chunk is one whose entities/*.mca entry we have
            // never read. Writing it blind would stamp whatever we happen to
            // hold over the top of entities still on disk.
            //
            // But refusing outright is what silently ate dropped items: a
            // chunk can acquire live entities without ever having been
            // claimed (it reached the cache without going through the async
            // result queue, so nothing called RequestLoad), and then every
            // save politely declined to write them.
            //
            // So: if we hold nothing, decline — there is nothing to lose and
            // clearing unread data would be wrong. If we DO hold something,
            // claim the chunk now. RequestLoad reads the file and merges what
            // was there into the managers, after which writing is correct
            // rather than destructive, and the gather has to be redone
            // because it just added entities.
            if (contents.Empty()) { error.clear(); return false; }

            Log::Warning("[Anvil] chunk (%d,%d) holds %zu live entities but was never claimed "
                         "— claiming it now so they are not lost",
                         pos.x, pos.z, contents.Count());
            RequestLoad(pos);
            if (!claimed()) { error.clear(); return false; }
            gather(contents);
        }

        std::vector<uint8_t> nbt;
        if (!Game::Anvil::SerialiseEntityChunk(contents, pos, Game::Save::DataVersion(),
                                               nbt, error)) {
            if (!error.empty()) return false;

            // Nothing saveable here. Vanilla DELETES the entry rather than
            // writing an empty list — measured across 415 real entity chunks,
            // not one held an empty Entities list.
            if (m_onDisk.erase(pos) > 0) {
                return provider->ClearEntityChunk(pos, error);
            }
            return true;
        }

        std::vector<uint8_t> packed;
        if (!Game::Nbt::ZlibCompress(nbt, packed)) { error = "zlib failed"; return false; }
        if (!provider->WriteEntityChunkNbt(pos, packed, error)) return false;

        m_onDisk.insert(pos);
        return true;
    }

    void LevelEntityStore::SaveAllLoaded() {
        Game::ChunkProvider* provider = ProviderOf(m_level);
        if (!provider || !provider->EntityPersistenceEnabled()) return;

        // Walk the entities, not the chunk list: entities are not dirty-tracked
        // (a mob walking changes a chunk's entity list without touching a
        // block), so every chunk that holds one has to be considered.
        std::unordered_set<Game::Math::ChunkPos, Game::Math::ChunkPosHash> occupied;
        if (MobManager* mobs = m_level.Mobs()) {
            for (const auto& [id, mob] : mobs->All()) if (mob) occupied.insert(ChunkOf(mob->position));
        }
        if (ItemEntityManager* items = m_level.Items()) {
            for (const auto& [id, item] : items->All()) occupied.insert(ChunkOf(item.pos));
        }
        if (ExperienceOrbManager* orbs = m_level.Orbs()) {
            for (const auto& [id, orb] : orbs->All()) occupied.insert(ChunkOf(orb.pos));
        }

        // Plus every chunk that USED to hold something: those need clearing.
        std::vector<Game::Math::ChunkPos> targets(occupied.begin(), occupied.end());
        for (const auto& pos : m_onDisk) {
            if (!occupied.count(pos)) targets.push_back(pos);
        }

        // Counted so the summary says what actually went to disk. A count of
        // chunks alone hid the bug this pass exists to prevent: "saved
        // entities for 68 chunks" looks healthy whether or not a single item
        // entity was among them.
        size_t mobCount = 0, itemCount = 0, orbCount = 0;
        if (MobManager* mobs = m_level.Mobs())          mobCount  = mobs->All().size();
        if (ItemEntityManager* items = m_level.Items()) itemCount = items->All().size();
        if (ExperienceOrbManager* orbs = m_level.Orbs()) orbCount = orbs->All().size();

        size_t saved = 0;
        for (const auto& pos : targets) {
            std::string error;
            if (SaveChunk(pos, error)) ++saved;
            else if (!error.empty()) {
                Log::Error("[Anvil] entity save failed for chunk (%d,%d): %s",
                           pos.x, pos.z, error.c_str());
            }
        }
        if (saved > 0) {
            Log::Info("[Anvil] saved entities for %zu chunks (%zu mobs, %zu items, %zu orbs live)",
                      saved, mobCount, itemCount, orbCount);
        }
    }

    void LevelEntityStore::SaveAndForget(Game::Math::ChunkPos pos) {
        std::string error;
        if (!SaveChunk(pos, error) && !error.empty()) {
            Log::Error("[Anvil] entity save failed on unload of chunk (%d,%d): %s",
                       pos.x, pos.z, error.c_str());
        }
        m_state.erase(pos);
    }

    size_t LevelEntityStore::PendingCount() const {
        size_t n = 0;
        for (const auto& [pos, state] : m_state) if (state == State::Pending) ++n;
        return n;
    }

} // namespace Server
