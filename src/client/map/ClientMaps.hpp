// File: src/client/map/ClientMaps.hpp
//
// The client's maps — MC's ClientLevel map data (getMapData /
// overrideMapData), MapTextureManager and MapRenderer, in one place:
//
//   Store      map id → MapItemSavedData, filled from MapItemDataS2C
//              (ClientPacketListener.handleMapItemData: create on first
//              sight with the packet's scale / locked, then apply the
//              decorations and the colour patch).
//   Textures   one 128×128 RGBA texture per map, rebuilt from the colour
//              bytes (MapColor.getColorFromPackedId) only when a patch has
//              changed them. On Vulkan the backend stages the write into the
//              next frame's per-frame copy of an in-use texture; on GL the
//              map rotates through a small ring so a texture a queued frame
//              still samples is never overwritten (the lightmap's rule).
//   Geometry   MapRenderer.render: the map quad and its decoration sprites
//              (from the map_decorations sprites, packed into one atlas), in
//              map units (0..128) through a caller's pose. Callers turn the
//              quads into their own vertex format and draw with the
//              lightmap light they already use.
//
// Main (render) thread only. The session's maps are dropped with the client
// levels (ClientLevels::DestroySession).
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"
#include "common/world/map/MapItemSavedData.hpp"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Network { struct MapItemDataS2CPacket; }

namespace Client::Maps {

    // One textured quad: corners in draw order (MC's vertex order) and
    // their UVs.
    struct MapQuad {
        std::array<glm::vec3, 4> pos;
        std::array<glm::vec2, 4> uv;
    };

    struct MapGeometry {
        ::Render::TextureHandle mapTexture = ::Render::INVALID_TEXTURE;
        MapQuad mapQuad{};
        ::Render::TextureHandle decorationTexture = ::Render::INVALID_TEXTURE;
        std::vector<MapQuad> decorations;
    };

    // ClientLevel.getMapData: null when the server never sent this map.
    std::shared_ptr<Game::Maps::MapItemSavedData> GetMapData(int32_t mapId);

    // ClientPacketListener.handleMapItemData.
    void HandleMapItemData(const Network::MapItemDataS2CPacket& packet);

    // MapRenderer.render into quads: the map at z -0.01, each decoration a
    // 2×2 sprite scaled by 4 at z -0.02 - n·0.001 (only those shown on a
    // frame when `showOnlyFrame`). `pose` maps map units to the caller's
    // space. False when the map has no data (the caller draws the plain
    // background, as MC's hasMapData = false path does).
    bool BuildMapGeometry(int32_t mapId, const glm::mat4& pose, bool showOnlyFrame, MapGeometry& out);

    // textures/map/map_background.png / map_background_checkerboard.png.
    ::Render::TextureHandle MapBackgroundTexture(bool checkerboard);

    // Forget every map and free its textures (session over).
    void Clear();

} // namespace Client::Maps
