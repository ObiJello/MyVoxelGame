// File: src/common/world/map/MapItem.hpp
//
// The item half of MC's map system:
//
//   EmptyMapItem.use         right-click a blank map: a new filled map
//                            centred on the player (scale 0).
//   MapItem.useOn            a filled map clicked on a banner toggles its
//                            marker.
//   MapItem (the class)      filled_map and 26.3's per-structure map items
//                            (ocean_monument_map … warm_ocean_ruins_map) are
//                            all MapItem: they tick, draw in hand and in
//                            frames, and carry a MAP_ID once filled.
//
// The saved data lives on the server (Server::MapDataStore); what needs it
// goes through the bridge functions below, which server/level/maps/
// MapItemServer.cpp defines — the same common→server seam ender eyes and the
// Hush items use.
#pragma once

#include "common/entity/Item.hpp"
#include "common/world/level/DimensionId.hpp"
#include "MapTypes.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Game {

    class IUsePlayer;

    namespace MapItem {

        // `instanceof MapItem`: filled_map and the 26.3 structure maps.
        bool IsMapItem(ItemID id);

        // DataComponents.MAP_ID, when the stack carries one.
        std::optional<int32_t> GetMapId(const ItemStack& stack);

        // Wire EmptyMapItem.use onto `map` and MapItem.useOn onto every map
        // item. Called from ItemRegistry_RegisterBehaviors.
        void RegisterBehaviors(std::unordered_map<ItemID, Item>& pureItems);

    } // namespace MapItem

    // MC ExplorationMapFunction's fields (loot function / trade modifier
    // "minecraft:exploration_map").
    struct ExplorationMapParams {
        std::string destination;               // structure tag "#ns:path" or id
        Maps::DecorationType decoration = Maps::DecorationType::Mansion;   // DEFAULT_DECORATION
        int  zoom = 2;                         // DEFAULT_ZOOM
        int  searchRadius = 50;                // DEFAULT_SEARCH_RADIUS
        bool skipKnownStructures = true;       // DEFAULT_SKIP_EXISTING
    };

    // ── Server bridges (server/level/maps/MapItemServer.cpp) ──────────────
    namespace MapItemBridge {

        // ExplorationMapFunction.run: the nearest structure of the
        // destination found from `origin` in the level, the stack given a new
        // map there (zoom, tracking, unlimited tracking), the biome preview
        // drawn and the target "+" decoration set. The stack comes back
        // unchanged when nothing is found (the trades / tables then discard
        // it through their map_id filter).
        ItemStack ApplyExplorationMap(const ItemStack& stack, int dimensionId, const glm::dvec3& origin,
                                      const ExplorationMapParams& params);

        // LocationPredicate's biome clause: is the biome at `pos` one of
        // `biomes` (ids, or "#tags")? The location_check the camp tables'
        // map entries use. False when the level cannot answer.
        bool BiomeIn(int dimensionId, const glm::ivec3& pos, const std::vector<std::string>& biomes);

        // EmptyMapItem.use, server half: MapItem.create(level, blockX,
        // blockZ, 0, true, false) for the player's level. Empty stack when
        // the level has no map storage.
        ItemStack CreateMapAt(int dimensionId, const glm::dvec3& playerPos);

        // Inventory.add, else Player.drop — the player's inventory is the
        // server's.
        void GiveOrDrop(IUsePlayer& player, const ItemStack& stack);

        // MapItem.useOn on a banner: MapItemSavedData.toggleBanner. False
        // when the map has no data or nothing toggled (MC: FAIL).
        bool ToggleBanner(int dimensionId, const ItemStack& map, const glm::ivec3& bannerPos);

        // MapItemSavedData.isTrackedCountOverLimit for the stack's map
        // (ItemFrame.interact refuses a map already carrying 256 tracked
        // markers). False when the stack has no map data.
        bool MapTrackedCountOverLimit(const ItemStack& map, int limit);

        // ItemFrame.removeFramedMap.
        void RemovedFromFrame(const ItemStack& framed, const glm::ivec3& framePos, int frameEntityId);

        // MapItem.onCraftedPostProcess: LOCK / SCALE consume the stack's
        // MAP_POST_PROCESSING and give it a new map id. Server only (a
        // no-op on a client).
        void OnCraftedPostProcess(ItemStack& stack);

        // The scale of a map as the server knows it (map extending's
        // "not at max scale" test, the cartography table). nullopt when the
        // map has no data.
        std::optional<int> MapScale(const ItemStack& map);
        // Whether the map is locked (the cartography table refuses to lock
        // or extend a locked map).
        bool MapLocked(const ItemStack& map);

        // Off the server thread, MapScale / MapLocked answer from the
        // client's copy of the map (MC's recipes read the menu's own level:
        // the client predicts the result from ClientLevel.getMapData). The
        // client registers its store here.
        using ClientMapInfoFn = bool (*)(int32_t mapId, int& scale, bool& locked);
        void SetClientMapInfoSource(ClientMapInfoFn fn);

    } // namespace MapItemBridge

    namespace MapItemBridge::detail {
        ClientMapInfoFn ClientMapInfoSource();
    } // namespace MapItemBridge::detail

} // namespace Game
