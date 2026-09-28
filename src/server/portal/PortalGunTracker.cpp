// File: src/server/portal/PortalGunTracker.cpp
//
// See PortalGunTracker.hpp for the rule. This TU holds the whereabouts check,
// the live census and the one-time orphan sweep.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "PortalGunTracker.hpp"
#include "PortalRegistry.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/inventory/AbstractContainerMenu.hpp"
#include "common/inventory/Container.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "server/IntegratedServer.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/world/storage/anvil/AnvilRegion.hpp"
#include "server/world/storage/anvil/EntityNbt.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Game::Portal {

    namespace {

        using Kind = GunWhereabouts::Kind;

        // Tracker cadence, in server ticks.
        constexpr int64_t kCheckInterval        = 10;    // re-check every pair's last location
        constexpr int64_t kLooseCensusInterval  = 40;    // look for guns whose location is unknown
        constexpr int64_t kEntityCensusInterval = 200;   // ...including every entity's NBT
        // A player's inventory is only trusted to be complete this long after
        // they joined (their saved data is loaded during the join, but a
        // miss this early costs nothing to wait out).
        constexpr int64_t kJoinGraceTicks       = 60;
        // Consecutive live misses that prove a gun gone. Two checks half a
        // second apart: nothing legitimately holds a stack out of sight that
        // long.
        constexpr int     kMissesToClose        = 2;

        std::string Lower(std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // ── Holders ─────────────────────────────────────────────────────────

        void ForEachGunInImpl(const ItemStack& stack, const std::function<void(uint64_t)>& fn, int depth) {
            if (stack.IsEmpty()) return;
            if (stack.itemId == Items::PortalGun) {
                const uint64_t id = GunInstanceOf(stack);
                if (id != 0) fn(id);
                return;
            }
            // Only a stack with its own components can nest others (a bundle).
            if (stack.components.empty() || depth > 8) return;
            if (!stack.components.has(DataComponents::BUNDLE_CONTENTS)) return;
            if (const auto bundle = stack.components.get(DataComponents::BUNDLE_CONTENTS)) {
                for (const ItemStack& inner : bundle->items) ForEachGunInImpl(inner, fn, depth + 1);
            }
        }

        template <typename Fn>
        void ForEachPlayerStack(Server::ServerPlayer& player, Fn&& fn) {
            const Inventory& inventory = player.getInventory();
            for (int i = 0; i < Inventory::TOTAL_SIZE; ++i) fn(inventory.GetSlot(i));
            fn(player.getCarried());
            auto walk = [&](AbstractContainerMenu& menu) {
                for (int s = 0; s < menu.SlotCount(); ++s) {
                    const Slot& slot = menu.GetSlot(s);
                    if (slot.container) fn(slot.GetItem());
                }
            };
            // The player's own menu (its crafting grid is not part of the
            // inventory proper), and whatever block menu is open on top (an
            // anvil's input, a crafting table's grid, the chest itself).
            walk(player.inventoryMenu());
            if (player.hasOpenContainerMenu()) walk(player.container());
        }

        bool PlayerHolds(Server::ServerPlayer& player, uint64_t gunId) {
            bool found = false;
            ForEachPlayerStack(player, [&](const ItemStack& stack) {
                if (!found) ForEachGunIn(stack, [&](uint64_t id) { if (id == gunId) found = true; });
            });
            return found;
        }

        bool StackHolds(const ItemStack& stack, uint64_t gunId) {
            bool found = false;
            ForEachGunIn(stack, [&](uint64_t id) { if (id == gunId) found = true; });
            return found;
        }

        // Every gun an entity carries. Only three kinds of entity can hold an
        // arbitrary stack in this engine: item frames (their item), anything
        // with equipment slots (armor stands; LivingEntity::EquipmentInSlot),
        // and the villager family (inventory and trade offers, read through
        // their saved NBT — whatever the save keeps, the census sees).
        // Everything else (projectiles, TNT, falling blocks, plain mobs, which
        // carry no equipment here) is skipped without a look, so a census
        // stays cheap in a world full of entities.
        void ForEachGunOnMob(Mob& mob, Nbt::Writer& scratch,
                             const std::function<void(uint64_t)>& fn) {
            if (mob.IsRemoved()) return;
            if (const auto* frame = dynamic_cast<const ItemFrame*>(&mob)) {
                ForEachGunIn(frame->GetItem(), fn);
                return;
            }
            if (mob.HasEquipmentSlots()) {
                for (int slot = static_cast<int>(EquipmentSlot::MAINHAND);
                     slot <= static_cast<int>(EquipmentSlot::SADDLE); ++slot) {
                    if (const ItemStack* stack = mob.EquipmentInSlot(static_cast<EquipmentSlot>(slot))) {
                        ForEachGunIn(*stack, fn);
                    }
                }
            }
            const EntityTypeId type = mob.GetType();
            if (type != EntityTypeId::Villager && type != EntityTypeId::WanderingTrader &&
                type != EntityTypeId::ZombieVillager) {
                return;
            }
            scratch.Reset();
            scratch.BeginRootCompound();
            auto list = scratch.BeginList("Entities", Nbt::TagType::Compound);
            Anvil::WriteMob(scratch, list, mob);
            scratch.EndList(list);
            scratch.EndRootCompound();
            const std::vector<uint8_t>& bytes = scratch.Bytes();
            ScanNbtForGunIds(bytes.data(), bytes.size(), fn);
        }

        Server::ServerPlayer* FindOnlinePlayer(Server::IntegratedServer& server, const std::string& name) {
            auto* sessions = server.GetSessionManager();
            if (!sessions || name.empty()) return nullptr;
            const std::string want = Lower(name);
            for (const auto& session : sessions->GetAllSessions()) {
                if (!session || !session->GetPlayer()) continue;
                if (Lower(session->GetPlayer()->getName()) == want) return session->GetPlayer();
            }
            return nullptr;
        }

        bool ChunkLoadedAt(Server::ServerLevel* level, const glm::ivec3& pos) {
            if (!level || !level->World()) return false;
            return level->World()->IsChunkLoaded(pos.x >> 4, pos.z >> 4);
        }

        glm::ivec3 BlockOf(const glm::dvec3& p) {
            return glm::ivec3(static_cast<int>(std::floor(p.x)),
                              static_cast<int>(std::floor(p.y)),
                              static_cast<int>(std::floor(p.z)));
        }

        // ── Census ──────────────────────────────────────────────────────────

        // Where each gun id was found live this pass (first sighting wins).
        using Census = std::unordered_map<uint64_t, GunWhereabouts>;

        void RunCensus(Server::IntegratedServer& server, bool includeEntities, Census& out) {
            auto note = [&](uint64_t id, const GunWhereabouts& where) { out.emplace(id, where); };

            if (auto* sessions = server.GetSessionManager()) {
                for (const auto& session : sessions->GetAllSessions()) {
                    Server::ServerPlayer* player = session ? session->GetPlayer() : nullptr;
                    if (!player) continue;
                    GunWhereabouts w;
                    w.kind      = Kind::Player;
                    w.player    = player->getName();
                    w.dimension = DimensionFromRaw(player->getDimensionId());
                    ForEachPlayerStack(*player, [&](const ItemStack& stack) {
                        ForEachGunIn(stack, [&](uint64_t id) { note(id, w); });
                    });
                }
            }

            Nbt::Writer scratch;
            server.ForEachLevel([&](Server::ServerLevel& level) {
                const DimensionId dim = level.Dimension();
                if (auto* items = level.Items()) {
                    for (const auto& [entityId, entity] : items->All()) {
                        if (entity.stack.IsEmpty()) continue;
                        GunWhereabouts w;
                        w.kind      = Kind::ItemEntity;
                        w.dimension = dim;
                        w.entityId  = entityId;
                        w.pos       = BlockOf(entity.pos);
                        ForEachGunIn(entity.stack, [&](uint64_t id) { note(id, w); });
                    }
                }
                if (World* world = level.World()) {
                    if (ChunkProvider* provider = world->GetChunkProvider()) {
                        for (const auto& [chunkPos, chunk] : provider->GetChunksWithBlockEntities()) {
                            if (!chunk) continue;
                            for (const auto& [local, be] : chunk->GetAllBlockEntities()) {
                                const auto* container = dynamic_cast<const IContainer*>(be.get());
                                if (!container) continue;
                                GunWhereabouts w;
                                w.kind      = Kind::Container;
                                w.dimension = dim;
                                w.pos       = glm::ivec3(chunkPos.x * 16 + local.x, local.y,
                                                         chunkPos.z * 16 + local.z);
                                const int n = container->GetContainerSize();
                                for (int i = 0; i < n; ++i) {
                                    ForEachGunIn(container->GetItem(i), [&](uint64_t id) { note(id, w); });
                                }
                            }
                        }
                    }
                }
                if (includeEntities) {
                    if (auto* mobs = level.Mobs()) {
                        for (Mob* mob : mobs->List()) {
                            if (!mob) continue;
                            GunWhereabouts w;
                            w.kind      = Kind::Entity;
                            w.dimension = dim;
                            w.entityId  = mob->GetId();
                            w.pos       = mob->BlockPosition();
                            ForEachGunOnMob(*mob, scratch, [&](uint64_t id) { note(id, w); });
                        }
                    }
                }
            });
        }

        // ── Quick check of a pair's last known location ─────────────────────

        enum class Check : uint8_t {
            Present,        // still there
            Missing,        // a live location that no longer holds it
            Unverifiable,   // offline player / unloaded chunk — cannot tell
            Loose,          // location unknown, or stored in a chunk that is loaded again
        };

        // ── Orphan sweep job ────────────────────────────────────────────────

        struct SweepDimension {
            DimensionId    id;
            ChunkProvider* provider = nullptr;   // null or without persistence: read the files directly
        };

        struct SweepJob {
            std::filesystem::path       root;
            std::vector<SweepDimension> dimensions;

            // Results (written by the worker, read after `finished`).
            std::unordered_set<uint64_t> found;
            std::unordered_map<uint64_t, std::pair<DimensionId, glm::ivec3>> foundInChunk;
            size_t files  = 0;
            size_t chunks = 0;
            size_t errors = 0;
            double seconds = 0.0;
            bool   cancelled = false;

            std::atomic<bool> cancel{false};
            std::atomic<bool> finished{false};
        };

        bool ParseRegionName(const std::string& name, int& rx, int& rz) {
            // r.<x>.<z>.mca
            return std::sscanf(name.c_str(), "r.%d.%d.mca", &rx, &rz) == 2 &&
                   name.size() > 4 && name.compare(name.size() - 4, 4, ".mca") == 0;
        }

        bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return !f.bad();
        }

        void RunSweep(SweepJob& job) {
            namespace fs = std::filesystem;
            const auto started = std::chrono::steady_clock::now();
            std::vector<uint8_t> nbt;
            std::string error;
            std::error_code ec;

            // 1. Every saved chunk and entity chunk, dimension by dimension.
            for (const SweepDimension& dim : job.dimensions) {
                const std::string_view sub = DimensionSaveSubdir(dim.id);
                const fs::path dimRoot = sub.empty() ? job.root : job.root / fs::path(std::string(sub));
                const bool viaGame = dim.provider && dim.provider->EntityPersistenceEnabled();
                for (const bool entities : {false, true}) {
                    const fs::path dir = dimRoot / (entities ? "entities" : "region");
                    if (!fs::is_directory(dir, ec)) continue;
                    std::vector<fs::path> regionFiles;
                    for (const auto& entry : fs::directory_iterator(dir, ec)) {
                        regionFiles.push_back(entry.path());
                    }
                    for (const fs::path& file : regionFiles) {
                        if (job.cancel.load(std::memory_order_relaxed)) { job.cancelled = true; return; }
                        int rx = 0, rz = 0;
                        if (!ParseRegionName(file.filename().string(), rx, rz)) continue;
                        ++job.files;
                        // A level that saves reads through its own AnvilChunkIo:
                        // one lock with the saver, so a chunk being relocated by
                        // a concurrent write is never read half-moved. A
                        // dimension nobody writes is read straight off disk.
                        std::unique_ptr<Anvil::AnvilRegion> region;
                        if (!viaGame) {
                            error.clear();
                            region = Anvil::AnvilRegion::Open(file, /*writable=*/false, error);
                            if (!region) {
                                if (!error.empty()) {
                                    ++job.errors;
                                    Log::Warning("[PortalGun] sweep: cannot open %s: %s",
                                                 file.string().c_str(), error.c_str());
                                }
                                continue;
                            }
                        }
                        for (int lz = 0; lz < 32; ++lz) {
                            if (job.cancel.load(std::memory_order_relaxed)) { job.cancelled = true; return; }
                            for (int lx = 0; lx < 32; ++lx) {
                                nbt.clear();
                                error.clear();
                                const Math::ChunkPos pos{rx * 32 + lx, rz * 32 + lz};
                                bool ok = false;
                                if (viaGame) {
                                    ok = entities ? dim.provider->ReadEntityChunkNbt(pos, nbt, error)
                                                  : dim.provider->ReadTerrainChunkNbt(pos, nbt, error);
                                } else {
                                    if (!region->Has(lx, lz)) continue;
                                    ok = region->Read(lx, lz, nbt, error);
                                }
                                if (!ok) {
                                    if (!error.empty()) {
                                        ++job.errors;
                                        Log::Warning("[PortalGun] sweep: unreadable chunk %d,%d in %s: %s",
                                                     pos.x, pos.z, file.string().c_str(), error.c_str());
                                    }
                                    continue;
                                }
                                ++job.chunks;
                                ScanNbtForGunIds(nbt.data(), nbt.size(), [&](uint64_t id) {
                                    job.found.insert(id);
                                    job.foundInChunk.emplace(
                                        id, std::make_pair(dim.id, glm::ivec3(pos.x * 16 + 8, 0, pos.z * 16 + 8)));
                                });
                            }
                        }
                    }
                }
            }

            // 2. Every .dat file: level.dat, playerdata/<uuid>.dat (offline
            //    players' inventories and ender chests), data/*.dat (command
            //    storage can hold items too). The .dat_old generations are
            //    stale by definition and skipped.
            fs::recursive_directory_iterator it(job.root, fs::directory_options::skip_permission_denied, ec);
            for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (job.cancel.load(std::memory_order_relaxed)) { job.cancelled = true; return; }
                const fs::path& path = it->path();
                if (it->is_directory(ec)) {
                    const std::string name = path.filename().string();
                    if (name == "region" || name == "entities" || name == "poi") it.disable_recursion_pending();
                    continue;
                }
                if (path.extension() != ".dat") continue;
                // Only the player files and level.dat can make the sweep
                // inconclusive; any other .dat that fails is not a holder.
                const std::string parent = path.parent_path().filename().string();
                const bool holder = path.filename() == "level.dat" || parent == "playerdata" || parent == "players";
                std::vector<uint8_t> raw;
                if (!ReadWholeFile(path, raw)) {
                    if (holder) ++job.errors;
                    continue;
                }
                ++job.files;
                if (raw.size() >= 2 && ((raw[0] == 0x1f && raw[1] == 0x8b) || raw[0] == 0x78)) {
                    nbt.clear();
                    if (!Nbt::GzipDecompress(raw, nbt, 256u * 1024u * 1024u)) {
                        if (holder) {
                            ++job.errors;
                            Log::Warning("[PortalGun] sweep: cannot decompress %s", path.string().c_str());
                        }
                        continue;
                    }
                    ScanNbtForGunIds(nbt.data(), nbt.size(), [&](uint64_t id) { job.found.insert(id); });
                } else {
                    ScanNbtForGunIds(raw.data(), raw.size(), [&](uint64_t id) { job.found.insert(id); });
                }
            }
            if (ec) {
                ++job.errors;
                Log::Warning("[PortalGun] sweep: could not walk %s: %s",
                             job.root.string().c_str(), ec.message().c_str());
            }

            job.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        }

        // ── Tracker state (server thread) ───────────────────────────────────

        struct TrackerState {
            int64_t tick = 0;
            bool    censusDue = false;
            bool    autoSweepChecked = false;

            // Guns whose dropped stack was destroyed since the last check.
            // Guarded: filled from ItemEntityManager, which runs on the server
            // thread today; the lock keeps it right should levels ever tick
            // in parallel.
            std::mutex                   eventMutex;
            std::unordered_set<uint64_t> destroyed;
            std::vector<std::pair<uint64_t, GunWhereabouts>> relocations;

            std::unordered_map<std::string, int64_t> joinTick;   // lower-cased name

            // The orphan sweep in flight.
            std::shared_ptr<SweepJob>    sweep;
            std::thread                  sweepThread;
            uint32_t                     sweepRequester = 0;
            uint64_t                     sweepIdFence   = 0;
            std::unordered_set<uint64_t> seenDuringSweep;
        };

        TrackerState& State() {
            static TrackerState state;
            return state;
        }

        bool InJoinGrace(const TrackerState& st, const std::string& name) {
            auto it = st.joinTick.find(Lower(name));
            return it != st.joinTick.end() && st.tick - it->second < kJoinGraceTicks;
        }

        Check QuickCheck(Server::IntegratedServer& server, TrackerState& st, uint64_t gunId,
                         GunWhereabouts& w) {
            switch (w.kind) {
                case Kind::Player: {
                    Server::ServerPlayer* player = FindOnlinePlayer(server, w.player);
                    if (!player) return Check::Unverifiable;   // offline: their saved file has it
                    if (PlayerHolds(*player, gunId)) {
                        w.dimension = DimensionFromRaw(player->getDimensionId());
                        return Check::Present;
                    }
                    return InJoinGrace(st, w.player) ? Check::Unverifiable : Check::Missing;
                }
                case Kind::ItemEntity: {
                    Server::ServerLevel* level = server.GetLevel(w.dimension);
                    if (level && level->Items()) {
                        if (const ItemEntity* e = level->Items()->Find(w.entityId)) {
                            if (StackHolds(e->stack, gunId)) {
                                w.pos = BlockOf(e->pos);
                                return Check::Present;
                            }
                        }
                    }
                    if (!ChunkLoadedAt(level, w.pos)) {
                        w.kind = Kind::Stored;
                        return Check::Unverifiable;
                    }
                    return Check::Missing;
                }
                case Kind::Container: {
                    Server::ServerLevel* level = server.GetLevel(w.dimension);
                    if (!ChunkLoadedAt(level, w.pos)) return Check::Unverifiable;
                    if (BlockEntity* be = level->World()->GetBlockEntity(w.pos)) {
                        if (const auto* container = dynamic_cast<const IContainer*>(be)) {
                            const int n = container->GetContainerSize();
                            for (int i = 0; i < n; ++i) {
                                if (StackHolds(container->GetItem(i), gunId)) return Check::Present;
                            }
                        }
                    }
                    return Check::Missing;
                }
                case Kind::Entity: {
                    Server::ServerLevel* level = server.GetLevel(w.dimension);
                    if (level && level->Mobs()) {
                        if (Mob* mob = level->Mobs()->Find(w.entityId)) {
                            Nbt::Writer scratch;
                            bool found = false;
                            ForEachGunOnMob(*mob, scratch, [&](uint64_t id) { if (id == gunId) found = true; });
                            if (found) {
                                w.pos = mob->BlockPosition();
                                return Check::Present;
                            }
                        }
                    }
                    if (!ChunkLoadedAt(level, w.pos)) {
                        w.kind = Kind::Stored;
                        return Check::Unverifiable;
                    }
                    return Check::Missing;
                }
                case Kind::Stored: {
                    Server::ServerLevel* level = server.GetLevel(w.dimension);
                    return ChunkLoadedAt(level, w.pos) ? Check::Loose : Check::Unverifiable;
                }
                case Kind::Unknown:
                default:
                    return Check::Loose;
            }
        }

        void CloseForLostGun(uint64_t gunId, const char* why) {
            Log::Info("[PortalGun] gun=%llu %s - closing its portals",
                      static_cast<unsigned long long>(gunId), why);
            ServerRegistry().ClearPair(gunId);
        }

        void SendToPlayer(Server::IntegratedServer& server, uint32_t playerId, const std::string& text) {
            if (playerId == 0) return;
            auto* sessions = server.GetSessionManager();
            if (!sessions) return;
            auto session = sessions->GetSession(playerId);
            if (session && session->GetConnection()) session->GetConnection()->SendChatMessage(text, 1);
        }

        void FinishSweep(Server::IntegratedServer& server, TrackerState& st) {
            if (st.sweepThread.joinable()) st.sweepThread.join();
            std::shared_ptr<SweepJob> job = std::move(st.sweep);
            st.sweep.reset();
            const uint32_t requester = st.sweepRequester;
            st.sweepRequester = 0;
            if (!job) return;
            if (job->cancelled) {
                st.seenDuringSweep.clear();
                return;
            }

            // One last look at everything live, so a gun that moved between
            // the disk read and now is still seen.
            Census census;
            RunCensus(server, /*includeEntities=*/true, census);

            PortalRegistry& registry = ServerRegistry();
            std::vector<uint64_t> orphans;
            size_t kept = 0;
            std::vector<uint64_t> gunIds;
            for (const auto& entry : registry.All()) gunIds.push_back(entry.first);
            for (uint64_t gunId : gunIds) {
                if (gunId >= st.sweepIdFence) continue;   // minted after the sweep began: tracked live
                PortalPair* pairPtr = registry.TryGetPairMutable(gunId);
                if (!pairPtr) continue;
                PortalPair& pair = *pairPtr;
                auto live = census.find(gunId);
                if (live != census.end()) {
                    pair.seen = live->second;
                    ++kept;
                    continue;
                }
                if (st.seenDuringSweep.count(gunId) || job->found.count(gunId)) {
                    if (pair.seen.kind == Kind::Unknown) {
                        auto at = job->foundInChunk.find(gunId);
                        if (at != job->foundInChunk.end()) {
                            pair.seen.kind      = Kind::Stored;
                            pair.seen.dimension = at->second.first;
                            pair.seen.pos       = at->second.second;
                        }
                    }
                    ++kept;
                    continue;
                }
                orphans.push_back(gunId);
            }
            st.seenDuringSweep.clear();

            char summary[256];
            if (job->errors > 0) {
                std::snprintf(summary, sizeof(summary),
                              "Portal gun sweep: %zu chunk(s) and %zu file(s) read in %.1f s, but %zu could not be "
                              "read - nothing closed (%zu pair(s) without a gun found). It runs again next load; "
                              "/portalgun close closes pairs by hand.",
                              job->chunks, job->files, job->seconds, job->errors, orphans.size());
                Log::Warning("[PortalGun] %s", summary);
                SendToPlayer(server, requester, summary);
                return;
            }
            for (uint64_t gunId : orphans) CloseForLostGun(gunId, "exists nowhere (orphan sweep)");
            registry.SetOrphanSweepDone(true);
            registry.Save();
            std::snprintf(summary, sizeof(summary),
                          "Portal gun sweep: %zu chunk(s) and %zu file(s) read in %.1f s - closed %zu orphaned "
                          "pair(s), %zu gun(s) accounted for.",
                          job->chunks, job->files, job->seconds, orphans.size(), kept);
            Log::Info("[PortalGun] %s", summary);
            SendToPlayer(server, requester, summary);
        }

    } // namespace

    // ── Public helpers ──────────────────────────────────────────────────────

    uint64_t GunInstanceOf(const ItemStack& stack) {
        if (stack.IsEmpty() || stack.itemId != Items::PortalGun) return 0;
        if (stack.components.empty()) return 0;
        return stack.components.get(DataComponents::PORTAL_GUN_INSTANCE_ID).value_or(uint64_t{0});
    }

    void ForEachGunIn(const ItemStack& stack, const std::function<void(uint64_t)>& fn) {
        ForEachGunInImpl(stack, fn, 0);
    }

    bool StripGunInstance(ItemStack& stack) {
        if (GunInstanceOf(stack) == 0) return false;
        stack.components.remove(DataComponents::PORTAL_GUN_INSTANCE_ID);
        return true;
    }

    void ScanNbtForGunIds(const uint8_t* data, size_t size, const std::function<void(uint64_t)>& fn) {
        // A named TAG_Long: type 4, big-endian u16 name length, the name,
        // then the big-endian value — exactly ItemStackNbt's
        // w.Long("obeycraft:portal_gun_instance_id", id).
        static const std::string needle = [] {
            constexpr std::string_view name = "obeycraft:portal_gun_instance_id";
            std::string n;
            n.push_back(static_cast<char>(4));
            n.push_back(static_cast<char>((name.size() >> 8) & 0xFF));
            n.push_back(static_cast<char>(name.size() & 0xFF));
            n.append(name);
            return n;
        }();
        if (!data || size < needle.size() + 8) return;
        const std::string_view hay(reinterpret_cast<const char*>(data), size);
        size_t from = 0;
        while (true) {
            const size_t at = hay.find(needle, from);
            if (at == std::string_view::npos || at + needle.size() + 8 > size) return;
            const uint8_t* v = data + at + needle.size();
            uint64_t id = 0;
            for (int i = 0; i < 8; ++i) id = (id << 8) | v[i];
            if (id != 0) fn(id);
            from = at + needle.size() + 8;
        }
    }

    void NoteGunItemEntity(uint64_t gunId, GunItemFate fate, DimensionId dimension, const glm::dvec3& pos) {
        if (gunId == 0) return;
        TrackerState& st = State();
        std::lock_guard<std::mutex> lock(st.eventMutex);
        switch (fate) {
            case GunItemFate::Destroyed:
                st.destroyed.insert(gunId);
                break;
            case GunItemFate::Collected: {
                // A player, hopper or mob has it now; the next census finds
                // which. Until then it is not "missing" — a holder we cannot
                // inspect must never close the pair.
                GunWhereabouts w;
                w.kind = Kind::Unknown;
                st.relocations.emplace_back(gunId, w);
                break;
            }
            case GunItemFate::Stored: {
                GunWhereabouts w;
                w.kind      = Kind::Stored;
                w.dimension = dimension;
                w.pos       = BlockOf(pos);
                st.relocations.emplace_back(gunId, w);
                break;
            }
        }
    }

    void NoteGunTrackerPlayerJoined(const std::string& playerName) {
        TrackerState& st = State();
        st.joinTick[Lower(playerName)] = st.tick;
        st.censusDue = true;
    }

    void TickGunTracker(Server::IntegratedServer& server) {
        TrackerState& st = State();
        ++st.tick;
        PortalRegistry& registry = ServerRegistry();

        // The one-time sweep, first tick after the world opened.
        if (!st.autoSweepChecked) {
            st.autoSweepChecked = true;
            if (!registry.OrphanSweepDone()) {
                if (registry.All().empty()) {
                    // Nothing to reconcile; every pair from now on is tracked.
                    registry.SetOrphanSweepDone(true);
                } else {
                    StartOrphanSweep(0);
                }
            }
        }
        if (st.sweep && st.sweep->finished.load(std::memory_order_acquire)) FinishSweep(server, st);

        // Events from the item managers.
        std::unordered_set<uint64_t> destroyed;
        std::vector<std::pair<uint64_t, GunWhereabouts>> relocations;
        {
            std::lock_guard<std::mutex> lock(st.eventMutex);
            destroyed.swap(st.destroyed);
            relocations.swap(st.relocations);
        }
        for (auto& [gunId, where] : relocations) {
            if (PortalPair* pair = registry.TryGetPairMutable(gunId)) {
                pair->seen = where;
                if (st.sweep) st.seenDuringSweep.insert(gunId);
                // Collected: find out by whom at once.
                if (where.kind == Kind::Unknown) st.censusDue = true;
            }
        }

        if (registry.All().empty()) {
            st.censusDue = false;
            return;
        }

        const bool checkDue = (st.tick % kCheckInterval) == 0;
        if (destroyed.empty() && !checkDue && !st.censusDue) return;

        // 1. Where each pair's gun was last seen — still there?
        std::vector<uint64_t> missing;
        bool looseGuns = false;
        std::vector<uint64_t> gunIds;
        gunIds.reserve(registry.All().size());
        for (const auto& [gunId, pair] : registry.All()) gunIds.push_back(gunId);
        for (uint64_t gunId : gunIds) {
            PortalPair* pair = registry.TryGetPairMutable(gunId);
            if (!pair) continue;
            if (destroyed.count(gunId)) {
                missing.push_back(gunId);
                continue;
            }
            if (!checkDue) {
                if (pair->seen.kind == Kind::Unknown || pair->seen.kind == Kind::Stored) looseGuns = true;
                continue;
            }
            switch (QuickCheck(server, st, gunId, pair->seen)) {
                case Check::Present:
                    pair->seen.misses = 0;
                    if (st.sweep) st.seenDuringSweep.insert(gunId);
                    break;
                case Check::Missing:
                    missing.push_back(gunId);
                    break;
                case Check::Loose:
                    looseGuns = true;
                    break;
                case Check::Unverifiable:
                    break;
            }
        }

        // 2. Anything to look for?
        const bool looseCensus = looseGuns &&
                                 (st.censusDue || (st.tick % kLooseCensusInterval) == 0);
        // While the orphan sweep runs, every live sighting counts — look
        // regularly even when nothing is missing.
        const bool sweepCensus = st.sweep && checkDue && (st.tick % kLooseCensusInterval) == 0;
        if (missing.empty() && !looseCensus && !sweepCensus) {
            if (checkDue) st.censusDue = false;
            return;
        }
        const bool includeEntities = !missing.empty() || st.censusDue ||
                                     (st.tick % kEntityCensusInterval) == 0;
        Census census;
        RunCensus(server, includeEntities, census);
        st.censusDue = false;
        if (st.sweep) {
            for (const auto& [gunId, where] : census) st.seenDuringSweep.insert(gunId);
        }

        // 3. Move every found gun's whereabouts to where it is now.
        for (uint64_t gunId : gunIds) {
            PortalPair* pair = registry.TryGetPairMutable(gunId);
            if (!pair) continue;
            auto found = census.find(gunId);
            if (found != census.end()) pair->seen = found->second;   // misses reset with it
        }

        // 4. Missing from a live location and found nowhere live: gone.
        for (uint64_t gunId : missing) {
            if (census.count(gunId)) continue;
            PortalPair* pair = registry.TryGetPairMutable(gunId);
            if (!pair) continue;
            if (destroyed.count(gunId)) {
                CloseForLostGun(gunId, "was destroyed as a dropped item");
                continue;
            }
            if (++pair->seen.misses >= kMissesToClose) {
                CloseForLostGun(gunId, pair->seen.kind == Kind::Player
                                           ? "left its holder's inventory and exists nowhere"
                                           : "is gone from where it was and exists nowhere");
            }
        }
    }

    void ShutdownGunTracker() {
        TrackerState& st = State();
        if (st.sweep) st.sweep->cancel.store(true);
        if (st.sweepThread.joinable()) st.sweepThread.join();
        st.sweep.reset();
        st.sweepRequester = 0;
        st.sweepIdFence = 0;
        st.seenDuringSweep.clear();
        {
            std::lock_guard<std::mutex> lock(st.eventMutex);
            st.destroyed.clear();
            st.relocations.clear();
        }
        st.joinTick.clear();
        st.censusDue = false;
        st.autoSweepChecked = false;
        st.tick = 0;
    }

    SweepStart StartOrphanSweep(uint32_t requesterPlayerId) {
        TrackerState& st = State();
        if (st.sweep) return SweepStart::AlreadyRunning;
        Server::IntegratedServer* server = Server::g_integratedServer.get();
        if (!server) return SweepStart::NoSaveFolder;
        PortalRegistry& registry = ServerRegistry();
        if (registry.All().empty()) return SweepStart::NothingToCheck;
        Server::ServerLevel* overworld = server->GetLevel(DimensionId::Overworld);
        if (!overworld || overworld->Config().savePath.empty()) return SweepStart::NoSaveFolder;

        auto job = std::make_shared<SweepJob>();
        job->root = std::filesystem::path(overworld->Config().savePath);
        for (const DimensionId dim : kAllDimensions) {
            SweepDimension d;
            d.id = dim;
            if (Server::ServerLevel* level = server->GetLevel(dim)) {
                if (World* world = level->World()) d.provider = world->GetChunkProvider();
            }
            job->dimensions.push_back(d);
        }

        st.sweep          = job;
        st.sweepRequester = requesterPlayerId;
        st.sweepIdFence   = registry.PeekNextId();
        st.seenDuringSweep.clear();
        Log::Info("[PortalGun] orphan sweep started over %s (%zu pair(s) to account for)",
                  job->root.string().c_str(), registry.All().size());
        st.sweepThread = std::thread([job] {
            RunSweep(*job);
            job->finished.store(true, std::memory_order_release);
        });
        return SweepStart::Started;
    }

    bool OrphanSweepRunning() {
        return State().sweep != nullptr;
    }

    std::vector<uint64_t> GunsOfPlayer(const std::string& playerName) {
        const std::string want = Lower(playerName);
        std::vector<uint64_t> out;
        for (const auto& [gunId, pair] : ServerRegistry().All()) {
            const bool owns    = !pair.owner.empty() && Lower(pair.owner) == want;
            const bool carries = pair.seen.kind == Kind::Player && Lower(pair.seen.player) == want;
            if (owns || carries) out.push_back(gunId);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string DescribeWhereabouts(uint64_t gunId) {
        const PortalPair* pair = ServerRegistry().TryGetPair(gunId);
        if (!pair) return "no portals";
        const GunWhereabouts& w = pair->seen;
        char buf[160];
        const std::string dim(DimensionName(w.dimension));
        switch (w.kind) {
            case Kind::Player:
                return "carried by " + w.player;
            case Kind::ItemEntity:
                std::snprintf(buf, sizeof(buf), "dropped at %d %d %d (%s)", w.pos.x, w.pos.y, w.pos.z, dim.c_str());
                return buf;
            case Kind::Container:
                std::snprintf(buf, sizeof(buf), "in a container at %d %d %d (%s)", w.pos.x, w.pos.y, w.pos.z, dim.c_str());
                return buf;
            case Kind::Entity:
                std::snprintf(buf, sizeof(buf), "held by an entity at %d %d %d (%s)", w.pos.x, w.pos.y, w.pos.z, dim.c_str());
                return buf;
            case Kind::Stored:
                std::snprintf(buf, sizeof(buf), "stored with the chunk at %d %d (%s)", w.pos.x, w.pos.z, dim.c_str());
                return buf;
            case Kind::Unknown:
            default:
                return "location unknown";
        }
    }

} // namespace Game::Portal

#endif // ENABLE_PORTAL_GUN
