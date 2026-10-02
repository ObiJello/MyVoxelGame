// File: src/server/level/maps/MapFill.cpp
#include "server/level/maps/MapFill.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/ItemEntityManager.hpp"
#include "server/entity/MobManager.hpp"
#include "server/world/ChunkProvider.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/level/maps/MapDataStore.hpp"
#include "server/level/maps/MapItemServer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/EquipmentSlot.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/inventory/AbstractContainerMenu.hpp"
#include "common/inventory/Container.hpp"
#include "common/inventory/InventoryMenu.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/level/World.hpp"
#include "common/world/map/MapItem.hpp"
#include "common/world/math/WorldMath.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Server::MapFill {

    namespace {

        using Game::Maps::MapItemSavedData;
        using Game::Math::ChunkPos;

        constexpr int kMapSize = Game::Maps::kMapSize;
        // The pixel grid carries one extra row above the map (imgY = -1):
        // MC's update samples it so row 0 is shaded against real terrain.
        constexpr int kRows = kMapSize + 1;

        // Chunk loads outstanding per fill: the pipeline stays busy without
        // one fill crowding out the players' own terrain.
        constexpr size_t kMaxInFlight = 32;
        // Chunks sampled per fill per tick, and the tick-time budget for all
        // fills together.
        constexpr int kChunksPerTick = 32;
        constexpr auto kTickBudget = std::chrono::milliseconds(4);
        // A request that neither arrived nor is still queued after this long
        // (dropped by a cancel, or loaded and unloaded between two looks) is
        // asked for again.
        constexpr int64_t kRetryTicks = 200;

        struct Pending {
            ChunkPos pos;
            int64_t requestedTick;
        };

        struct Job {
            int32_t mapId = 0;
            uint32_t playerId = 0;
            Game::DimensionId dimension = Game::DimensionId::Overworld;
            std::shared_ptr<MapItemSavedData> data;
            int scale = 1;
            std::vector<ChunkPos> order;                                     // nearest the centre first
            std::unordered_map<ChunkPos, std::vector<uint16_t>, Game::Math::ChunkPosHash> pixels;   // chunk -> grid cells
            size_t next = 0;
            std::vector<Pending> inFlight;
            std::vector<MapItems::PixelSample> samples;                      // kMapSize * kRows
            std::vector<uint8_t> sampled;
            size_t chunksDone = 0;
            int lastPercent = -1;
            // Consecutive censuses that found no live stack of this map.
            int missingChecks = 0;
        };

        // ── Is the map still an item anywhere? ─────────────────────────
        //
        // THE RULE. Maps are shared by id: a fill belongs to the map, not to
        // one stack, so it runs while ANY live item stack carries its id and
        // stops when none does — burned, blown up, fallen into the void,
        // despawned, /kill'd as an item, /clear'd, deleted in the creative
        // inventory, or consumed by crafting / the cartography table with no
        // copy left (a clone keeps it going; a zoom or lock that eats the
        // only stack stops it — the result is a new map id).
        //
        // "Live" is everything the server can look at: every online player's
        // inventory, cursor and open menus (a crafting grid, the cartography
        // inputs and result square), dropped items, block containers in
        // loaded chunks, item frames and entity equipment (armor stands),
        // and bundles inside any of those. A census runs every
        // kCensusTicks; two misses in a row stop the fill, so a stack in
        // flight between two holders within one tick is never mistaken for
        // gone.
        constexpr int64_t kCensusTicks = 10;
        constexpr int kMissesToStop = 2;

        void NoteMapIds(const Game::ItemStack& stack, std::unordered_set<int32_t>& out, int depth = 0) {
            if (stack.IsEmpty() || depth > 8) return;
            if (const auto id = stack.get(Game::DataComponents::MAP_ID)) out.insert(*id);
            if (stack.components.empty() || !stack.components.has(Game::DataComponents::BUNDLE_CONTENTS)) return;
            if (const auto bundle = stack.components.get(Game::DataComponents::BUNDLE_CONTENTS)) {
                for (const Game::ItemStack& inner : bundle->items) NoteMapIds(inner, out, depth + 1);
            }
        }

        std::unordered_set<int32_t> LiveMapIds(IntegratedServer& server) {
            std::unordered_set<int32_t> ids;
            if (auto* sessions = server.GetSessionManager()) {
                for (const auto& session : sessions->GetAllSessions()) {
                    ServerPlayer* player = session ? session->GetPlayer() : nullptr;
                    if (!player) continue;
                    const Game::Inventory& inventory = player->getInventory();
                    for (int i = 0; i < Game::Inventory::TOTAL_SIZE; ++i) NoteMapIds(inventory.GetSlot(i), ids);
                    NoteMapIds(player->getCarried(), ids);
                    auto walk = [&ids](Game::AbstractContainerMenu& menu) {
                        for (int slot = 0; slot < menu.SlotCount(); ++slot) {
                            const Game::Slot& s = menu.GetSlot(slot);
                            if (s.container) NoteMapIds(s.GetItem(), ids);
                        }
                    };
                    walk(player->inventoryMenu());
                    if (player->hasOpenContainerMenu()) walk(player->container());
                }
            }
            server.ForEachLevel([&ids](ServerLevel& level) {
                if (auto* items = level.Items()) {
                    for (const auto& [entityId, entity] : items->All()) NoteMapIds(entity.stack, ids);
                }
                if (Game::World* world = level.World()) {
                    if (Game::ChunkProvider* provider = world->GetChunkProvider()) {
                        for (const auto& [chunkPos, chunk] : provider->GetChunksWithBlockEntities()) {
                            if (!chunk) continue;
                            for (const auto& [local, be] : chunk->GetAllBlockEntities()) {
                                const auto* container = dynamic_cast<const Game::IContainer*>(be.get());
                                if (!container) continue;
                                for (int i = 0; i < container->GetContainerSize(); ++i) {
                                    NoteMapIds(container->GetItem(i), ids);
                                }
                            }
                        }
                    }
                }
                if (auto* mobs = level.Mobs()) {
                    for (Game::Mob* mob : mobs->List()) {
                        if (!mob || mob->IsRemoved()) continue;
                        if (const auto* frame = dynamic_cast<const Game::ItemFrame*>(mob)) {
                            NoteMapIds(frame->GetItem(), ids);
                            continue;
                        }
                        if (!mob->HasEquipmentSlots()) continue;
                        for (int slot = static_cast<int>(Game::EquipmentSlot::MAINHAND);
                             slot <= static_cast<int>(Game::EquipmentSlot::SADDLE); ++slot) {
                            if (const Game::ItemStack* stack =
                                    mob->EquipmentInSlot(static_cast<Game::EquipmentSlot>(slot))) {
                                NoteMapIds(*stack, ids);
                            }
                        }
                    }
                }
            });
            return ids;
        }

        std::vector<Job>& Jobs() {
            static std::vector<Job> jobs;
            return jobs;
        }

        size_t Cell(int imgX, int imgY) { return static_cast<size_t>(imgX + (imgY + 1) * kMapSize); }

        // The pixel's area corner, as MapItem.update computes it.
        void AreaOf(const MapItemSavedData& data, int scale, int imgX, int imgY, int& minX, int& minZ) {
            minX = (data.centerX / scale + imgX - 64) * scale;
            minZ = (data.centerZ / scale + imgY - 64) * scale;
        }

        // Write pixel (imgX, imgY) once it and its northern neighbour are
        // both sampled — the shading MapItem.update gives a full column pass.
        void TryApply(Job& job, int imgX, int imgY) {
            if (imgY < 0 || imgY >= kMapSize) return;
            if (!job.sampled[Cell(imgX, imgY)] || !job.sampled[Cell(imgX, imgY - 1)]) return;
            const MapItems::PixelSample& here = job.samples[Cell(imgX, imgY)];
            const double north = job.samples[Cell(imgX, imgY - 1)].averageHeight;
            job.data->UpdateColor(imgX, imgY, MapItems::PackedColorFor(here, north, imgX, imgY, job.scale));
        }

        void SampleChunk(Job& job, Game::World& world, Game::Chunk& chunk, const ChunkPos& pos) {
            const auto it = job.pixels.find(pos);
            if (it == job.pixels.end()) return;
            for (const uint16_t cell : it->second) {
                const int imgX = cell % kMapSize;
                const int imgY = cell / kMapSize - 1;
                int minX = 0, minZ = 0;
                AreaOf(*job.data, job.scale, imgX, imgY, minX, minZ);
                job.samples[cell] = MapItems::SamplePixel(world, chunk, *job.data, minX, minZ);
                job.sampled[cell] = 1;
            }
            for (const uint16_t cell : it->second) {
                const int imgX = cell % kMapSize;
                const int imgY = cell / kMapSize - 1;
                TryApply(job, imgX, imgY);
                TryApply(job, imgX, imgY + 1);
            }
            ++job.chunksDone;
        }

        ServerPlayer* PlayerOf(IntegratedServer& server, uint32_t playerId) {
            PlayerSessionManager* sessions = server.GetSessionManager();
            auto session = sessions ? sessions->GetSession(playerId) : nullptr;
            return session ? session->GetPlayer() : nullptr;
        }

        // Stop a fill: its outstanding chunk loads are let go (the ones
        // nobody else wants are cancelled, their generation tickets
        // released), the pixels written so far stay.
        void CancelJob(IntegratedServer& server, Job& job) {
            PlayerSessionManager* sessions = server.GetSessionManager();
            const auto all = sessions ? sessions->GetAllSessions() : std::vector<std::shared_ptr<PlayerSession>>{};
            for (const Pending& p : job.inFlight) server.CancelLoadIfUnwanted(job.dimension, p.pos, nullptr, all);
            job.inFlight.clear();
            Log::Info("[Maps] debug fill of map #%d cancelled (%zu of %zu chunks done)", job.mapId, job.chunksDone,
                      job.order.size());
        }

        // Cancel every fill of `mapId` (when set) or started by `playerId`.
        // Returns how many stopped.
        int CancelJobs(IntegratedServer& server, std::optional<int32_t> mapId, uint32_t playerId) {
            int cancelled = 0;
            auto& jobs = Jobs();
            for (auto it = jobs.begin(); it != jobs.end();) {
                if ((mapId && it->mapId == *mapId) || it->playerId == playerId) {
                    CancelJob(server, *it);
                    it = jobs.erase(it);
                    ++cancelled;
                } else {
                    ++it;
                }
            }
            return cancelled;
        }

        void Execute(const CommandSourceStack& source, const std::vector<std::string>& args,
                     ServerConnection& connection, PlayerSessionManager& /*sessions*/) {
            IntegratedServer* server = g_integratedServer.get();
            // The executor's map (`/execute as Steve run mapfill`).
            ServerPlayer* player = source.ExecutorPlayer();
            if (!server) return;
            if (!player) { connection.SendChatMessage(CommandSourceStack::kPlayerRequired, 1); return; }

            // `/mapfill cancel`: every fill this player started.
            if (!args.empty() && args[0] == "cancel") {
                if (CancelJobs(*server, std::nullopt, player->getPlayerId()) > 0) {
                    player->DisplayClientMessage("Map fill cancelled", true);
                } else {
                    connection.SendChatMessage("No map fill is running", 1);
                }
                return;
            }
            if (!args.empty()) {
                connection.SendChatMessage("Usage: /mapfill [cancel]", 1);
                return;
            }

            // The held map: main hand, else the off hand.
            const Game::Inventory& inventory = player->getInventory();
            std::optional<int32_t> mapId = inventory.GetSelectedStack().IsEmpty()
                ? std::nullopt : inventory.GetSelectedStack().get(Game::DataComponents::MAP_ID);
            if (!mapId) {
                const Game::ItemStack& off = inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN);
                if (!off.IsEmpty()) mapId = off.get(Game::DataComponents::MAP_ID);
            }
            // The chord is a toggle: pressed again while the held map (or
            // any fill this player started) is being filled, it cancels.
            if (CancelJobs(*server, mapId, player->getPlayerId()) > 0) {
                player->DisplayClientMessage("Map fill cancelled", true);
                return;
            }
            if (!mapId) {
                connection.SendChatMessage("Hold a filled map to fill it in", 1);
                return;
            }
            auto data = MapDataStore::Instance().Get(*mapId);
            if (!data) {
                connection.SendChatMessage("Map #" + std::to_string(*mapId) + " has no data", 1);
                return;
            }
            if (data->locked) {
                connection.SendChatMessage("Map #" + std::to_string(*mapId) + " is locked", 1);
                return;
            }
            if (std::none_of(data->colors.begin(), data->colors.end(), [](uint8_t c) { return c == 0; })) {
                connection.SendChatMessage("Map #" + std::to_string(*mapId) + " is already fully explored", 1);
                return;
            }
            ServerLevel* level = server->GetLevel(data->dimension);
            if (!level || !level->World()) {
                connection.SendChatMessage("The map's dimension is not loaded", 1);
                return;
            }

            Job job;
            job.mapId = *mapId;
            job.playerId = player->getPlayerId();
            job.dimension = data->dimension;
            job.data = data;
            job.scale = 1 << data->scale;
            job.samples.resize(static_cast<size_t>(kMapSize * kRows));
            job.sampled.assign(static_cast<size_t>(kMapSize * kRows), 0);
            for (int imgY = -1; imgY < kMapSize; ++imgY) {
                for (int imgX = 0; imgX < kMapSize; ++imgX) {
                    int minX = 0, minZ = 0;
                    AreaOf(*data, job.scale, imgX, imgY, minX, minZ);
                    const ChunkPos pos{minX >> 4, minZ >> 4};
                    auto [slot, inserted] = job.pixels.try_emplace(pos);
                    if (inserted) job.order.push_back(pos);
                    slot->second.push_back(static_cast<uint16_t>(Cell(imgX, imgY)));
                }
            }
            const ChunkPos centre{data->centerX >> 4, data->centerZ >> 4};
            std::stable_sort(job.order.begin(), job.order.end(), [&centre](const ChunkPos& a, const ChunkPos& b) {
                const int64_t da = int64_t(a.x - centre.x) * (a.x - centre.x) + int64_t(a.z - centre.z) * (a.z - centre.z);
                const int64_t db = int64_t(b.x - centre.x) * (b.x - centre.x) + int64_t(b.z - centre.z) * (b.z - centre.z);
                return da < db;
            });
            connection.SendChatMessage("Filling in map #" + std::to_string(*mapId) + " (1:" +
                                       std::to_string(job.scale) + ", " + std::to_string(job.order.size()) +
                                       " chunks)", 1);
            Log::Info("[Maps] debug fill of map #%d: %zu chunks at scale %d", *mapId, job.order.size(),
                      static_cast<int>(data->scale));
            Jobs().push_back(std::move(job));
        }
    }

    void Register(CommandDispatcher& dispatcher) {
        dispatcher.RegisterCommand("mapfill", &Execute, Game::Cmd::Root().Executes());
    }

    void Clear() {
        // World closing: the pending loads die with the server's queues.
        Jobs().clear();
    }

    void Tick(IntegratedServer& server, int64_t serverTick) {
        auto& jobs = Jobs();
        if (jobs.empty()) return;
        const auto deadline = std::chrono::steady_clock::now() + kTickBudget;

        // Stop what lost its reason to run: the player who started it left,
        // or no live stack carries the map any more (see THE RULE above).
        {
            std::unordered_set<int32_t> live;
            const bool census = serverTick % kCensusTicks == 0;
            if (census) live = LiveMapIds(server);
            for (auto it = jobs.begin(); it != jobs.end();) {
                const bool playerGone = PlayerOf(server, it->playerId) == nullptr;
                if (census) it->missingChecks = live.count(it->mapId) ? 0 : it->missingChecks + 1;
                if (playerGone || it->missingChecks >= kMissesToStop) {
                    if (!playerGone) {
                        if (ServerPlayer* player = PlayerOf(server, it->playerId)) {
                            player->DisplayClientMessage("Map fill stopped: the map is gone", true);
                        }
                    }
                    Log::Info("[Maps] debug fill of map #%d stopped (%s)", it->mapId,
                              playerGone ? "its player left" : "no stack of the map exists");
                    CancelJob(server, *it);
                    it = jobs.erase(it);
                    continue;
                }
                ++it;
            }
            if (jobs.empty()) return;
        }

        for (auto it = jobs.begin(); it != jobs.end();) {
            Job& job = *it;
            ServerLevel* level = server.GetLevel(job.dimension);
            Game::World* world = level ? level->World() : nullptr;
            if (!world || job.data->locked) {
                it = jobs.erase(it);
                continue;
            }

            int sampledThisTick = 0;
            // What arrived.
            for (auto p = job.inFlight.begin(); p != job.inFlight.end();) {
                if (sampledThisTick >= kChunksPerTick || std::chrono::steady_clock::now() >= deadline) break;
                if (auto chunk = world->GetLoadedChunk(p->pos.x, p->pos.z)) {
                    SampleChunk(job, *world, *chunk, p->pos);
                    ++sampledThisTick;
                    p = job.inFlight.erase(p);
                    continue;
                }
                if (serverTick - p->requestedTick >= kRetryTicks && level->pendingChunkLoads.count(p->pos) == 0) {
                    server.RequestChunkLoad(job.dimension, p->pos, 0);
                    p->requestedTick = serverTick;
                }
                ++p;
            }
            // What comes next: sampled at once when already loaded, asked for
            // otherwise.
            while (job.next < job.order.size() && job.inFlight.size() < kMaxInFlight &&
                   sampledThisTick < kChunksPerTick && std::chrono::steady_clock::now() < deadline) {
                const ChunkPos pos = job.order[job.next++];
                if (auto chunk = world->GetLoadedChunk(pos.x, pos.z)) {
                    SampleChunk(job, *world, *chunk, pos);
                    ++sampledThisTick;
                } else {
                    server.RequestChunkLoad(job.dimension, pos, 0);
                    job.inFlight.push_back({pos, serverTick});
                }
            }

            const bool finished = job.next >= job.order.size() && job.inFlight.empty();
            ServerPlayer* player = PlayerOf(server, job.playerId);
            const int percent = static_cast<int>(job.chunksDone * 100 / std::max<size_t>(1, job.order.size()));
            if (player && !finished && percent != job.lastPercent && serverTick % 10 == 0) {
                job.lastPercent = percent;
                player->DisplayClientMessage("Filling in map #" + std::to_string(job.mapId) + ": " +
                                             std::to_string(percent) + "%", true);
            }
            if (finished) {
                if (player) {
                    player->DisplayClientMessage("Map #" + std::to_string(job.mapId) + " filled in", true);
                }
                Log::Info("[Maps] debug fill of map #%d done (%zu chunks)", job.mapId, job.chunksDone);
                it = jobs.erase(it);
                continue;
            }
            ++it;
        }
    }

} // namespace Server::MapFill
