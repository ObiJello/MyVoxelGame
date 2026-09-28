// File: src/server/level/maps/MapItemServer.hpp
//
// The server's map behaviour — MC MapItem's static and ticking halves:
//
//   MapItem.create / applyNewSavedData   a new map id + MapItemSavedData
//   MapItem.update                       the exploration pass: as a carrier
//                                        walks with the map in hand, one
//                                        column in sixteen per tick is
//                                        resampled from the terrain
//   MapItem.renderBiomePreviewMap        the explorer map's ocean/land
//                                        outline, drawn from biomes
//   MapItem.inventoryTick +              every carrier's maps ticked, then
//   ServerPlayer.synchronizeSpecial…     each carrier sent what changed
//   ServerEntity.sendChanges (frames)    framed maps every 10 ticks
//
// and the common-side bridges (Game::MapItemBridge) the item behaviours,
// loot tables, trades and crafting call.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/map/MapItemSavedData.hpp"

#include <cstdint>

namespace Game { class Chunk; class World; }

namespace Server {

    class IntegratedServer;
    class ServerLevel;

    namespace MapItems {

        // MapItem.create(level, x, z, scale, trackPosition, unlimitedTracking):
        // a filled_map with a fresh map id.
        Game::ItemStack Create(ServerLevel& level, int originX, int originZ, int scale,
                               bool trackPosition, bool unlimitedTracking);

        // MapItem.applyNewSavedData: give `stack` a fresh map id.
        void ApplyNewSavedData(ServerLevel& level, Game::ItemStack& stack, int originX, int originZ, int scale,
                               bool trackPosition, bool unlimitedTracking);

        // MapItem.renderBiomePreviewMap.
        void RenderBiomePreviewMap(ServerLevel& level, const Game::ItemStack& stack);

        // MapItemSavedData.addTargetDecoration: pin a marker to the stack.
        void AddTargetDecoration(Game::ItemStack& stack, const glm::ivec3& pos, const std::string& key,
                                 Game::Maps::DecorationType type);

        // One map pixel's column sample (MapItem.update's inner loop): the
        // most common MapColor over the pixel's area, its average height and
        // water depth. `chunk` holds the area (every scale fits one chunk).
        struct PixelSample {
            uint8_t color = 0;
            double averageHeight = 0.0;
            int waterDepth = 0;
        };
        PixelSample SamplePixel(Game::World& world, Game::Chunk& chunk,
                                Game::Maps::MapItemSavedData& data, int areaMinX, int areaMinZ);
        // MapItem.update's brightness: water by depth, land by the slope
        // against the pixel to the north, both dithered by parity.
        uint8_t PackedColorFor(const PixelSample& sample, double previousAverageHeight, int imgX, int imgY,
                               int scale);

        // MapItem.update — `player` must be in `level`.
        void Update(ServerLevel& level, const Game::Maps::MapHolderSnapshot& player,
                    Game::Maps::MapItemSavedData& data);

        // Once per server tick, after the player sessions: every online
        // player's maps (inventoryTick + synchronizeSpecialItemUpdates), and
        // every 10 ticks the framed maps.
        void Tick(IntegratedServer& server, int64_t serverTick);

    } // namespace MapItems

} // namespace Server
