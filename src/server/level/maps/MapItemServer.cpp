// File: src/server/level/maps/MapItemServer.cpp
#include "server/level/maps/MapItemServer.hpp"

#include "server/IntegratedServer.hpp"
#include "server/entity/MobManager.hpp"
#include "server/level/LocateFinder.hpp"
#include "server/level/ServerLevel.hpp"
#include "server/level/maps/MapDataStore.hpp"
#include "server/level/maps/MapFill.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/Log.hpp"
#include "common/data/DataComponents.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Inventory.hpp"
#include "common/entity/decoration/ItemFrame.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/MapItemDataS2CPacket.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/world/level/World.hpp"
#include "common/world/map/MapColor.hpp"
#include "common/world/map/MapItem.hpp"
#include "common/world/tags/DataTags.hpp"

#include <array>
#include <cmath>
#include <thread>
#include <unordered_set>

namespace Server::MapItems {

    namespace {

        using Game::ItemStack;
        using Game::Maps::MapHolderSnapshot;
        using Game::Maps::MapHolderTable;
        using Game::Maps::MapItemSavedData;
        namespace MapColors = Game::MapColors;

        int FloorToInt(double v) { return static_cast<int>(std::floor(v)); }

        // Mth.sin's 65536-entry table (MC keeps the table for bit-exact
        // patterns; the preview's wavy shoreline reads it).
        float MthSin(double value) {
            static const std::array<float, 65536> table = [] {
                std::array<float, 65536> t{};
                for (int i = 0; i < 65536; ++i) {
                    t[static_cast<size_t>(i)] = static_cast<float>(std::sin(static_cast<double>(i) * 3.141592653589793 * 2.0 / 65536.0));
                }
                return t;
            }();
            return table[static_cast<size_t>(static_cast<int64_t>(value * 10430.378350470453) & 65535)];
        }

        // MapBanner.fromWorld. The banner's colour is its block's; this
        // engine's banners carry no block entity, so a banner has no custom
        // name to show.
        std::optional<Game::Maps::MapBanner> BannerFromWorld(const Game::World& world, const glm::ivec3& pos) {
            const Game::BlockState state = world.GetBlockState(pos.x, pos.y, pos.z);
            const std::string_view slug = Game::BlockRegistry::Get(state.Block()).registrySlug;
            if (slug.empty() || !Game::DataTags::HasTag(Game::DataTags::Registry::Block, slug, "minecraft:banners")) {
                return std::nullopt;
            }
            std::string_view color = slug;
            for (std::string_view suffix : {std::string_view("_wall_banner"), std::string_view("_banner")}) {
                if (color.size() > suffix.size() && color.substr(color.size() - suffix.size()) == suffix) {
                    color = color.substr(0, color.size() - suffix.size());
                    break;
                }
            }
            Game::Maps::MapBanner banner;
            banner.pos = pos;
            banner.color = std::string(color);
            return banner;
        }

        IntegratedServer* CurrentServer() { return g_integratedServer.get(); }

        ServerLevel* LevelOf(int dimensionId) {
            IntegratedServer* server = CurrentServer();
            return server ? server->GetLevel(Game::DimensionFromRaw(dimensionId)) : nullptr;
        }

        bool OnServerThread() { return std::this_thread::get_id() == Server::g_serverThreadId; }

        std::shared_ptr<MapItemSavedData> SavedDataOf(const ItemStack& stack) {
            const auto id = Game::MapItem::GetMapId(stack);
            return id ? MapDataStore::Instance().Get(*id) : nullptr;
        }

        // Player.getInventory().contains(mapMatcher) and the armour check,
        // for one player, once per tick.
        MapHolderSnapshot SnapshotOf(const ServerPlayer& player) {
            MapHolderSnapshot s;
            s.key = static_cast<int32_t>(player.getPlayerId());
            s.name = player.getName();
            s.dimension = Game::DimensionFromRaw(player.getDimensionId());
            s.x = player.getPosition().x;
            s.z = player.getPosition().z;
            s.yRot = player.getYaw();
            const Game::Inventory& inv = player.getInventory();
            // Inventory.contains: the 36 item slots, the armour and the
            // offhand (not the crafting grid).
            for (int i = Game::Inventory::ARMOR_BEGIN; i < Game::Inventory::TOTAL_SIZE; ++i) {
                const ItemStack& stack = inv.GetSlot(i);
                if (stack.IsEmpty()) continue;
                if (const auto mapId = stack.get(Game::DataComponents::MAP_ID)) {
                    s.carriedMaps.insert(MapHolderSnapshot::CarryKey(stack.itemId, *mapId));
                }
                // hasMapInvisibilityItemEquipped: any slot but the hands.
                if (Game::Inventory::IsArmorSlot(i) &&
                    Game::DataTags::HasTag(Game::DataTags::Registry::Item, Game::ItemRegistry::Slug(stack.itemId),
                                           "minecraft:map_invisibility_equipment")) {
                    s.mapInvisible = true;
                }
            }
            return s;
        }

        void SendUpdate(PlayerSession& session, int32_t mapId, Game::Maps::MapUpdate update) {
            ServerConnection* conn = session.GetConnection();
            if (!conn) return;
            Network::MapItemDataS2CPacket packet;
            packet.mapId = mapId;
            packet.scale = update.scale;
            packet.locked = update.locked;
            packet.decorations = std::move(update.decorations);
            packet.colorPatch = std::move(update.patch);
            conn->SendPacket(static_cast<uint8_t>(Network::PacketId::MapItemDataS2C),
                             Network::Serialization::Serialize(packet));
        }

        // MapItem.getCorrectStateForFluidBlock.
        Game::BlockState CorrectStateForFluidBlock(Game::BlockState state) {
            const Game::FluidState fluid = Game::FluidStateOf(state);
            if (!fluid.IsEmpty() && !Game::IsStateFaceSturdy(state, Game::Direction::Up)) {
                return Game::FluidLegacyBlock(fluid);
            }
            return state;
        }
    }

    // ── Creating maps ───────────────────────────────────────────────────

    void ApplyNewSavedData(ServerLevel& level, ItemStack& stack, int originX, int originZ, int scale,
                           bool trackPosition, bool unlimitedTracking) {
        auto data = MapItemSavedData::CreateFresh(originX, originZ, static_cast<int8_t>(scale), trackPosition,
                                                  unlimitedTracking, level.Dimension());
        MapDataStore& store = MapDataStore::Instance();
        const int32_t id = store.GetFreeMapId();
        store.Set(id, std::move(data));
        stack.components.set(Game::DataComponents::MAP_ID, id);
    }

    ItemStack Create(ServerLevel& level, int originX, int originZ, int scale, bool trackPosition,
                     bool unlimitedTracking) {
        ItemStack map(Game::Items::FilledMap, 1);
        ApplyNewSavedData(level, map, originX, originZ, scale, trackPosition, unlimitedTracking);
        return map;
    }

    void AddTargetDecoration(ItemStack& stack, const glm::ivec3& pos, const std::string& key,
                             Game::Maps::DecorationType type) {
        Game::Maps::MapDecorations::Entry entry;
        entry.type = type;
        entry.x = pos.x;
        entry.z = pos.z;
        entry.rotation = 180.0f;
        const Game::Maps::MapDecorations current =
            stack.get(Game::DataComponents::MAP_DECORATIONS).value_or(Game::Maps::MapDecorations{});
        stack.components.set(Game::DataComponents::MAP_DECORATIONS, current.WithDecoration(key, entry));
    }

    // ── MapItem.update ──────────────────────────────────────────────────

    PixelSample SamplePixel(Game::World& world, Game::Chunk& chunk, MapItemSavedData& data,
                            int areaMinX, int areaMinZ) {
        const bool hasCeiling = Game::DimensionHasCeiling(data.dimension);
        const int minY = Game::DimensionMinY(data.dimension);
        const int scale = 1 << data.scale;

        // LinkedHashMultiset + copyHighestCountFirst: the most common colour,
        // ties to the first seen.
        std::array<uint8_t, 64> colorOrder{};
        std::array<int, 64> colorCount{};
        int distinctColors = 0;
        auto addColor = [&](uint8_t color, int n) {
            if (colorCount[color] == 0) colorOrder[static_cast<size_t>(distinctColors++)] = color;
            colorCount[color] += n;
        };

        int waterDepth = 0;
        double averageAreaHeight = 0.0;
        if (hasCeiling) {
            // Java int arithmetic, wrapping.
            uint32_t h = static_cast<uint32_t>(areaMinX) + static_cast<uint32_t>(areaMinZ) * 231871u;
            h = h * h * 31287121u + h * 11u;
            const int hash = static_cast<int>(h);
            if ((hash >> 20 & 1) == 0) {
                addColor(MapColors::GetBlockMapColor(Game::BlockStates::Default(Game::BlockID::Dirt)), 10);
            } else {
                addColor(MapColors::GetBlockMapColor(Game::BlockStates::Default(Game::BlockID::Stone)), 100);
            }
            averageAreaHeight = 100.0;
        } else {
            if (!chunk.AreHeightmapsPrimed()) chunk.PrimeHeightmaps();
            const Game::BlockState bedrock = Game::BlockStates::Default(Game::BlockID::Bedrock);
            for (int ax = 0; ax < scale; ++ax) {
                for (int az = 0; az < scale; ++az) {
                    const int x = areaMinX + ax, z = areaMinZ + az;
                    const int lx = x & 15, lz = z & 15;
                    int columnY = chunk.GetSurfaceHeight(lx, lz, Game::HeightmapType::WorldSurface) + 1;
                    Game::BlockState state;
                    if (columnY <= minY) {
                        state = bedrock;
                    } else {
                        do {
                            --columnY;
                            state = chunk.StateAt(lx, columnY, lz);
                        } while (MapColors::GetBlockMapColor(state) == MapColors::NONE && columnY > minY);

                        if (columnY > minY && !Game::FluidStateOf(state).IsEmpty()) {
                            int solidY = columnY - 1;
                            Game::BlockState below;
                            do {
                                below = chunk.StateAt(lx, solidY--, lz);
                                ++waterDepth;
                            } while (solidY > minY && !Game::FluidStateOf(below).IsEmpty());
                            state = CorrectStateForFluidBlock(state);
                        }
                    }
                    if (data.HasBanners()) {
                        data.CheckBanners(x, z, [&world](const glm::ivec3& p) { return BannerFromWorld(world, p); });
                    }
                    averageAreaHeight += static_cast<double>(columnY) / static_cast<double>(scale * scale);
                    addColor(MapColors::GetBlockMapColor(state), 1);
                }
            }
        }

        PixelSample out;
        out.waterDepth = waterDepth / (scale * scale);
        out.averageHeight = averageAreaHeight;
        int best = 0;
        for (int i = 0; i < distinctColors; ++i) {
            const uint8_t c = colorOrder[static_cast<size_t>(i)];
            if (colorCount[c] > best) { best = colorCount[c]; out.color = c; }
        }
        return out;
    }

    uint8_t PackedColorFor(const PixelSample& sample, double previousAverageHeight, int imgX, int imgY, int scale) {
        MapColors::Brightness brightness;
        const int parity = (imgX + imgY) & 1;
        if (sample.color == MapColors::WATER) {
            const double diff = static_cast<double>(sample.waterDepth) * 0.1 + static_cast<double>(parity) * 0.2;
            if (diff < 0.5)      brightness = MapColors::Brightness::HIGH;
            else if (diff > 0.9) brightness = MapColors::Brightness::LOW;
            else                 brightness = MapColors::Brightness::NORMAL;
        } else {
            const double diff = (sample.averageHeight - previousAverageHeight) * 4.0 / static_cast<double>(scale + 4) +
                                (static_cast<double>(parity) - 0.5) * 0.4;
            if (diff > 0.6)       brightness = MapColors::Brightness::HIGH;
            else if (diff < -0.6) brightness = MapColors::Brightness::LOW;
            else                  brightness = MapColors::Brightness::NORMAL;
        }
        return MapColors::PackedId(sample.color, brightness);
    }

    void Update(ServerLevel& level, const MapHolderSnapshot& player, MapItemSavedData& data) {
        Game::World* world = level.World();
        if (!world || level.Dimension() != data.dimension || player.dimension != data.dimension) return;

        const bool hasCeiling = Game::DimensionHasCeiling(data.dimension);
        const int scale = 1 << data.scale;
        const int centerX = data.centerX;
        const int centerZ = data.centerZ;
        const int playerImgX = FloorToInt(player.x - centerX) / scale + 64;
        const int playerImgY = FloorToInt(player.z - centerZ) / scale + 64;
        int radius = 128 / scale;
        if (hasCeiling) radius /= 2;

        MapItemSavedData::HoldingPlayer& holding = data.GetHoldingPlayer(player);
        ++holding.step;
        const int step = holding.step;

        std::shared_ptr<Game::Chunk> chunk;
        int chunkX = INT32_MIN, chunkZ = INT32_MIN;
        bool foundConsecutiveChanges = false;

        for (int imgX = playerImgX - radius + 1; imgX < playerImgX + radius; ++imgX) {
            if ((imgX & 15) != (step & 15) && !foundConsecutiveChanges) continue;
            foundConsecutiveChanges = false;
            double previousAverageAreaHeight = 0.0;

            for (int imgY = playerImgY - radius - 1; imgY < playerImgY + radius; ++imgY) {
                if (imgX < 0 || imgY < -1 || imgX >= 128 || imgY >= 128) continue;
                const int dx = imgX - playerImgX, dy = imgY - playerImgY;
                const int distanceToPlayerSqr = dx * dx + dy * dy;
                const bool ditherBlack = distanceToPlayerSqr > (radius - 2) * (radius - 2);
                const int areaMinX = (centerX / scale + imgX - 64) * scale;
                const int areaMinZ = (centerZ / scale + imgY - 64) * scale;

                // level.getChunk(areaMin >> 4): the area lies inside one
                // chunk at every scale. MC loads the chunk if it has to; the
                // map here reads only loaded terrain (chunk.isEmpty()).
                const int cx = areaMinX >> 4, cz = areaMinZ >> 4;
                if (cx != chunkX || cz != chunkZ) {
                    chunk = world->GetLoadedChunk(cx, cz);
                    chunkX = cx;
                    chunkZ = cz;
                }
                if (!chunk) continue;

                const PixelSample sample = SamplePixel(*world, *chunk, data, areaMinX, areaMinZ);
                const uint8_t packed = PackedColorFor(sample, previousAverageAreaHeight, imgX, imgY, scale);
                previousAverageAreaHeight = sample.averageHeight;

                const int parity = (imgX + imgY) & 1;
                if (imgY >= 0 && distanceToPlayerSqr < radius * radius && (!ditherBlack || parity != 0)) {
                    foundConsecutiveChanges |= data.UpdateColor(imgX, imgY, packed);
                }
            }
        }
    }

    // ── MapItem.renderBiomePreviewMap ───────────────────────────────────

    void RenderBiomePreviewMap(ServerLevel& level, const ItemStack& stack) {
        auto data = SavedDataOf(stack);
        if (!data || level.Dimension() != data->dimension) return;

        static const std::unordered_set<std::string> watery =
            ResolveBiomeIdOrTag("#minecraft:water_on_map_outlines");

        const int scale = 1 << data->scale;
        const int unscaledStartX = data->centerX / scale - 64;
        const int unscaledStartZ = data->centerZ / scale - 64;
        const int biomeSampleY = level.SeaLevel();

        std::vector<uint8_t> isBiomeWatery(16384, 0);
        for (int mx = 0; mx < 128; ++mx) {
            for (int mz = 0; mz < 128; ++mz) {
                const glm::ivec3 pos((unscaledStartX + mz) * scale, biomeSampleY, (unscaledStartZ + mx) * scale);
                const std::string biome = ZoomedBiomeAt(level, pos);
                isBiomeWatery[static_cast<size_t>(mx * 128 + mz)] = watery.count(biome) ? 1 : 0;
            }
        }
        auto wateryAt = [&isBiomeWatery](int x, int z) { return isBiomeWatery[static_cast<size_t>(z * 128 + x)] != 0; };

        for (int mx = 1; mx < 127; ++mx) {
            for (int mz = 1; mz < 127; ++mz) {
                int waterCount = 0;
                for (int dx = -1; dx < 2; ++dx) {
                    for (int dz = -1; dz < 2; ++dz) {
                        if ((dx != 0 || dz != 0) && wateryAt(mx + dx, mz + dz)) ++waterCount;
                    }
                }
                MapColors::Brightness brightness = MapColors::Brightness::LOWEST;
                uint8_t newColor = MapColors::NONE;
                if (wateryAt(mx, mz)) {
                    newColor = MapColors::COLOR_ORANGE;
                    if (waterCount > 7 && mz % 2 == 0) {
                        switch ((mx + static_cast<int>(MthSin(static_cast<double>(static_cast<float>(mz) + 0.0f)) * 7.0f)) / 8 % 5) {
                            case 0: case 4: brightness = MapColors::Brightness::LOW;    break;
                            case 1: case 3: brightness = MapColors::Brightness::NORMAL; break;
                            case 2:         brightness = MapColors::Brightness::HIGH;   break;
                            default: break;
                        }
                    } else if (waterCount > 7) {
                        newColor = MapColors::NONE;
                    } else if (waterCount > 5) {
                        brightness = MapColors::Brightness::NORMAL;
                    } else if (waterCount > 3) {
                        brightness = MapColors::Brightness::LOW;
                    } else if (waterCount > 1) {
                        brightness = MapColors::Brightness::LOW;
                    }
                } else if (waterCount > 0) {
                    newColor = MapColors::COLOR_BROWN;
                    brightness = waterCount > 3 ? MapColors::Brightness::NORMAL : MapColors::Brightness::LOWEST;
                }
                if (newColor != MapColors::NONE) data->SetColor(mx, mz, MapColors::PackedId(newColor, brightness));
            }
        }
    }

    // ── The tick ────────────────────────────────────────────────────────

    void Tick(IntegratedServer& server, int64_t serverTick) {
        // Debug fills (/mapfill) first, so their pixels ride this tick's
        // update packets.
        MapFill::Tick(server, serverTick);
        PlayerSessionManager* sessions = server.GetSessionManager();
        if (!sessions) return;
        MapDataStore& store = MapDataStore::Instance();

        const auto all = sessions->GetAllSessions();
        MapHolderTable table;
        table.reserve(all.size());
        for (const auto& session : all) {
            if (ServerPlayer* player = session ? session->GetPlayer() : nullptr) {
                MapHolderSnapshot s = SnapshotOf(*player);
                table.emplace(s.key, std::move(s));
            }
        }

        for (const auto& session : all) {
            ServerPlayer* player = session ? session->GetPlayer() : nullptr;
            if (!player) continue;
            const auto self = table.find(static_cast<int32_t>(player->getPlayerId()));
            if (self == table.end()) continue;
            ServerLevel* level = server.GetLevel(self->second.dimension);
            const int64_t gameTime = level && level->World() ? level->World()->GetGameTime() : 0;
            Game::Inventory& inv = player->getInventory();
            const int selected = Game::Inventory::HotbarToIndex(inv.GetSelectedSlot());

            // Inventory.tick → MapItem.inventoryTick for every map carried:
            // tickCarriedBy always, update when held in a hand.
            for (int i = Game::Inventory::ARMOR_BEGIN; i < Game::Inventory::TOTAL_SIZE; ++i) {
                // A crafted map that left its result square by a path that
                // bypassed the slot's own post-process (a hotbar-key swap)
                // is locked / scaled here, before anything reads its id.
                if (inv.GetSlot(i).components.has(Game::DataComponents::MAP_POST_PROCESSING)) {
                    Game::MapItemBridge::OnCraftedPostProcess(inv.GetItem(i));
                }
                const ItemStack& stack = inv.GetSlot(i);
                if (stack.IsEmpty() || !Game::MapItem::IsMapItem(stack.itemId)) continue;
                const auto mapId = stack.get(Game::DataComponents::MAP_ID);
                if (!mapId) continue;
                auto data = store.Get(*mapId);
                if (!data) continue;
                data->TickCarriedBy(self->second, table, stack.itemId, *mapId,
                                    stack.get(Game::DataComponents::MAP_DECORATIONS), nullptr, gameTime);
                const bool inHand = i == selected || Game::Inventory::IsOffhandSlot(i);
                if (!data->locked && inHand && level) Update(*level, self->second, *data);
            }

            // ServerPlayer.synchronizeSpecialItemUpdates, every slot.
            for (int i = Game::Inventory::ARMOR_BEGIN; i < Game::Inventory::TOTAL_SIZE; ++i) {
                const ItemStack& stack = inv.GetSlot(i);
                if (stack.IsEmpty()) continue;
                const auto mapId = stack.get(Game::DataComponents::MAP_ID);
                if (!mapId) continue;
                auto data = store.Get(*mapId);
                if (!data) continue;
                if (auto update = data->NextUpdateFor(self->second.key)) SendUpdate(*session, *mapId, std::move(*update));
            }
        }

        // ServerEntity.sendChanges: a framed map, every 10 ticks, to every
        // player in the frame's level.
        if (serverTick % 10 != 0) return;
        server.ForEachLevel([&](ServerLevel& level) {
            MobManager* mobs = level.Mobs();
            if (!mobs || !level.World()) return;
            const int64_t gameTime = level.World()->GetGameTime();
            for (Game::Mob* mob : mobs->List()) {
                auto* frame = dynamic_cast<Game::ItemFrame*>(mob);
                if (!frame || frame->IsRemoved()) continue;
                const ItemStack& item = frame->GetItem();
                const auto mapId = item.get(Game::DataComponents::MAP_ID);
                if (!mapId) continue;
                auto data = store.Get(*mapId);
                if (!data) continue;
                Game::Maps::MapFrameInfo info;
                info.pos = frame->GetBlockPos();
                const Game::Direction dir = frame->GetDirection();
                info.direction2D = dir == Game::Direction::South ? 0 : dir == Game::Direction::West ? 1
                                 : dir == Game::Direction::North ? 2 : dir == Game::Direction::East ? 3 : -1;
                info.entityId = frame->GetId();
                for (const auto& session : all) {
                    ServerPlayer* player = session ? session->GetPlayer() : nullptr;
                    if (!player || Game::DimensionFromRaw(player->getDimensionId()) != level.Dimension()) continue;
                    const auto snap = table.find(static_cast<int32_t>(player->getPlayerId()));
                    if (snap == table.end()) continue;
                    data->TickCarriedBy(snap->second, table, item.itemId, *mapId,
                                        item.get(Game::DataComponents::MAP_DECORATIONS), &info, gameTime);
                    if (auto update = data->NextUpdateFor(snap->second.key)) {
                        SendUpdate(*session, *mapId, std::move(*update));
                    }
                }
            }
        });
    }

} // namespace Server::MapItems

// ── Common bridges (common/world/map/MapItem.hpp) ────────────────────────────

namespace Game::MapItemBridge {

    using Server::MapItems::ApplyNewSavedData;

    ItemStack CreateMapAt(int dimensionId, const glm::dvec3& playerPos) {
        Server::ServerLevel* level = Server::MapItems::LevelOf(dimensionId);
        if (!level) return {};
        // MapItem.create(serverLevel, player.getBlockX(), player.getBlockZ(), 0, true, false).
        return Server::MapItems::Create(*level, static_cast<int>(std::floor(playerPos.x)),
                                        static_cast<int>(std::floor(playerPos.z)), 0, true, false);
    }

    void GiveOrDrop(IUsePlayer& user, const ItemStack& stack) {
        auto* player = dynamic_cast<Server::ServerPlayer*>(&user);
        if (!player || stack.IsEmpty()) return;
        const int leftover = player->getInventory().AddStack(stack);
        if (leftover > 0) {
            ItemStack rest = stack;
            rest.count = leftover;
            player->queuePendingDrop(rest);
        }
    }

    bool ToggleBanner(int dimensionId, const ItemStack& map, const glm::ivec3& bannerPos) {
        Server::ServerLevel* level = Server::MapItems::LevelOf(dimensionId);
        auto data = Server::MapItems::SavedDataOf(map);
        if (!level || !level->World() || !data) return true;   // no data: MC returns SUCCESS
        return data->ToggleBanner(Server::MapItems::BannerFromWorld(*level->World(), bannerPos), bannerPos,
                                  level->World()->GetGameTime());
    }

    bool MapTrackedCountOverLimit(const ItemStack& map, int limit) {
        if (!Server::MapItems::OnServerThread()) return false;
        auto data = Server::MapItems::SavedDataOf(map);
        return data && data->IsTrackedCountOverLimit(limit);
    }

    void RemovedFromFrame(const ItemStack& framed, const glm::ivec3& framePos, int frameEntityId) {
        if (!Server::MapItems::OnServerThread()) return;
        if (auto data = Server::MapItems::SavedDataOf(framed)) data->RemovedFromFrame(framePos, frameEntityId);
    }

    void OnCraftedPostProcess(ItemStack& stack) {
        if (!Server::MapItems::OnServerThread()) return;
        const auto post = stack.components.get(DataComponents::MAP_POST_PROCESSING);
        if (!post) return;
        stack.components.remove(DataComponents::MAP_POST_PROCESSING);
        auto original = Server::MapItems::SavedDataOf(stack);
        if (!original) return;
        Server::MapDataStore& store = Server::MapDataStore::Instance();
        const int32_t id = store.GetFreeMapId();
        // MapItem.lockMap / scaleMap.
        store.Set(id, *post == Maps::MapPostProcessing::Lock ? original->Locked() : original->Scaled());
        stack.components.set(DataComponents::MAP_ID, id);
    }

    std::optional<int> MapScale(const ItemStack& map) {
        const auto id = MapItem::GetMapId(map);
        if (!id) return std::nullopt;
        if (!Server::MapItems::OnServerThread()) {
            int scale = 0;
            bool locked = false;
            if (auto fn = detail::ClientMapInfoSource(); fn && fn(*id, scale, locked)) return scale;
            return std::nullopt;
        }
        auto data = Server::MapDataStore::Instance().Get(*id);
        return data ? std::optional<int>(data->scale) : std::nullopt;
    }

    bool MapLocked(const ItemStack& map) {
        const auto id = MapItem::GetMapId(map);
        if (!id) return false;
        if (!Server::MapItems::OnServerThread()) {
            int scale = 0;
            bool locked = false;
            if (auto fn = detail::ClientMapInfoSource(); fn && fn(*id, scale, locked)) return locked;
            return false;
        }
        auto data = Server::MapDataStore::Instance().Get(*id);
        return data && data->locked;
    }

    ItemStack ApplyExplorationMap(const ItemStack& input, int dimensionId, const glm::dvec3& origin,
                                  const ExplorationMapParams& params) {
        // ExplorationMapFunction.run.
        if (input.IsEmpty()) return input;
        Server::ServerLevel* level = Server::MapItems::LevelOf(dimensionId);
        if (!level) return input;
        const glm::ivec3 lootPos(static_cast<int>(std::floor(origin.x)), static_cast<int>(std::floor(origin.y)),
                                 static_cast<int>(std::floor(origin.z)));
        const std::vector<std::string> structures = Server::ResolveStructureIdOrTag(params.destination);
        if (structures.empty()) {
            Log::Warning("[Maps] exploration_map: unknown destination '%s'", params.destination.c_str());
            return input;
        }
        Server::MapDataStore& store = Server::MapDataStore::Instance();
        const Game::DimensionId dim = level->Dimension();
        const Server::StructureReferenceFn tryAddReference = [&store, dim](const Server::StructureStartKey& key) {
            return store.TryAddStructureReference(dim, key.structure, key.chunkX, key.chunkZ);
        };
        // claimStructureAt: the structure the loot sits in cannot be the one
        // its map leads to.
        if (params.skipKnownStructures) {
            if (auto here = Server::FindStructureStartAt(*level, structures, lootPos)) tryAddReference(*here);
        }
        const auto found = Server::FindNearestStructure(*level, structures, lootPos, params.searchRadius,
                                                        params.skipKnownStructures ? &tryAddReference : nullptr);
        if (!found) return input;

        ItemStack stack = input;
        ApplyNewSavedData(*level, stack, found->pos.x, found->pos.z, std::clamp(params.zoom, 0, Maps::kMaxScale),
                          true, true);
        Server::MapItems::RenderBiomePreviewMap(*level, stack);
        Server::MapItems::AddTargetDecoration(stack, found->pos, "+", params.decoration);
        return stack;
    }

    bool BiomeIn(int dimensionId, const glm::ivec3& pos, const std::vector<std::string>& biomes) {
        Server::ServerLevel* level = Server::MapItems::LevelOf(dimensionId);
        if (!level || biomes.empty()) return false;
        // level.getBiome(pos): the loaded chunk's (zoomed) biome, else the
        // generator's.
        std::string here;
        if (World* world = level->World(); world && world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) {
            const std::string_view name = BiomeRegistry::Get(world->GetBiome(pos.x, pos.y, pos.z)).name;
            here = name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : std::string(name);
        } else {
            here = Server::ZoomedBiomeAt(*level, pos);
        }
        if (here.empty()) return false;
        for (const std::string& entry : biomes) {
            if (!entry.empty() && entry[0] == '#') {
                if (Server::ResolveBiomeIdOrTag(entry).count(here)) return true;
            } else {
                const std::string id = entry.find(':') == std::string::npos ? "minecraft:" + entry : entry;
                if (id == here) return true;
            }
        }
        return false;
    }

} // namespace Game::MapItemBridge
