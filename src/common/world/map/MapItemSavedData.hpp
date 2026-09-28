// File: src/common/world/map/MapItemSavedData.hpp
//
// Port of net/minecraft/world/level/saveddata/maps/MapItemSavedData.java —
// one map: its centre, scale, dimension, the 128×128 packed colour bytes, the
// markers on it, and (server side) who is carrying it and what each carrier
// still has to be sent.
//
// Shared by both sides as in MC. The client builds one with CreateForClient
// and fills it from ClientboundMapItemDataPacket (MapItemDataS2CPacket); the
// server owns the real one in Server::MapDataStore, which also persists it
// as data/map_<id>.dat.
//
// MC's HoldingPlayer holds the Player object and asks it things
// (getInventory().contains, isRemoved, getX, getYRot…). Here a carrier is a
// player id, and the per-tick facts about every online player arrive as a
// MapHolderSnapshot table the server builds once a tick (Server::MapTicker)
// — the same questions, answered from a snapshot instead of a live object.
#pragma once

#include "MapTypes.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Game::Maps {

    class MapItemSavedData;

    // MapBanner record (pos, colour, optional name) — a banner marker the
    // player toggled by using the map on the banner.
    struct MapBanner {
        glm::ivec3 pos{0};
        std::string color = "white";         // DyeColor serialized name
        std::optional<std::string> name;
        std::string Id() const;              // "banner-x,y,z"
        DecorationType Decoration() const;   // getDecoration: the colour's banner marker
        bool operator==(const MapBanner& o) const { return pos == o.pos && color == o.color && name == o.name; }
    };

    // MapFrame record (pos, rotation, entityId).
    struct MapFrame {
        glm::ivec3 pos{0};
        int rotation = 0;
        int entityId = 0;
        std::string Id() const { return FrameId(pos); }
        static std::string FrameId(const glm::ivec3& pos);   // "frame-x,y,z"
        bool operator==(const MapFrame& o) const {
            return pos == o.pos && rotation == o.rotation && entityId == o.entityId;
        }
    };

    // MapItemSavedData.MapPatch — a dirty rectangle of colour bytes.
    struct MapPatch {
        int startX = 0, startY = 0, width = 0, height = 0;
        std::vector<uint8_t> colors;          // width * height, x fastest
        void ApplyToMap(MapItemSavedData& map) const;
    };

    // One online player, as the carrier bookkeeping needs to see them this
    // tick (MC reads the same facts off the Player).
    struct MapHolderSnapshot {
        int32_t key = 0;                      // player id
        std::string name;                     // getPlainTextName — the decoration key
        DimensionId dimension = DimensionId::Overworld;
        double x = 0.0, z = 0.0;
        float yRot = 0.0f;
        // hasMapInvisibilityItemEquipped: an armour slot holds an item in
        // #minecraft:map_invisibility_equipment (the carved pumpkin).
        bool mapInvisible = false;
        // Inventory.contains(mapMatcher) — every (item, map id) the player's
        // inventory holds a stack of.
        std::unordered_set<uint64_t> carriedMaps;
        static uint64_t CarryKey(uint16_t itemId, int32_t mapId) {
            return (static_cast<uint64_t>(itemId) << 32) | static_cast<uint32_t>(mapId);
        }
        bool Carries(uint16_t itemId, int32_t mapId) const { return carriedMaps.count(CarryKey(itemId, mapId)) != 0; }
    };

    using MapHolderTable = std::unordered_map<int32_t, MapHolderSnapshot>;

    // What getUpdatePacket produces — ClientboundMapItemDataPacket's payload
    // minus the id.
    struct MapUpdate {
        int8_t scale = 0;
        bool locked = false;
        std::optional<std::vector<MapDecoration>> decorations;
        std::optional<MapPatch> patch;
    };

    // ItemFrame facts tickCarriedBy reads off the frame.
    struct MapFrameInfo {
        glm::ivec3 pos{0};
        int direction2D = 0;                  // Direction.get2DDataValue of the frame's facing
        int entityId = 0;
    };

    class MapItemSavedData {
    public:
        int centerX = 0;
        int centerZ = 0;
        DimensionId dimension = DimensionId::Overworld;
        bool trackingPosition = true;
        bool unlimitedTracking = false;
        int8_t scale = 0;
        std::array<uint8_t, kMapSize * kMapSize> colors{};
        bool locked = false;

        MapItemSavedData() = default;
        MapItemSavedData(int centerX, int centerZ, int8_t scale, bool trackingPosition,
                         bool unlimitedTracking, bool locked, DimensionId dimension);

        // MapItemSavedData.createFresh: snap the origin onto this scale's grid.
        static std::shared_ptr<MapItemSavedData> CreateFresh(double originX, double originZ, int8_t scale,
                                                             bool trackingPosition, bool unlimitedTracking,
                                                             DimensionId dimension);
        static std::shared_ptr<MapItemSavedData> CreateForClient(int8_t scale, bool locked, DimensionId dimension);

        // The disk-codec constructor's tail: banners and frames re-add their
        // decorations (called by the loader after filling the lists).
        void RestoreMarkersAfterLoad();

        std::shared_ptr<MapItemSavedData> Locked() const;   // MapItemSavedData.locked()
        // MapItemSavedData.scaled(): the next scale up, a new map — with the
        // source's explored pixels downsampled into it and its banner
        // markers kept (a deliberate, Bedrock-like deviation; Java's is blank).
        std::shared_ptr<MapItemSavedData> Scaled() const;

        // ── Carriers (server) ──────────────────────────────────────────
        // MapItemSavedData.tickCarriedBy. `stackItem`/`stackMapId` identify
        // the ticking stack (mapMatcher: same item AND same map id);
        // `staticDecorations` is its MAP_DECORATIONS. `frame` is set when the
        // stack sits in an item frame. `gameTime` is the level's, for the
        // nether's spinning markers.
        void TickCarriedBy(const MapHolderSnapshot& tickingPlayer, const MapHolderTable& players,
                           uint16_t stackItem, int32_t stackMapId,
                           const std::optional<MapDecorations>& staticDecorations,
                           const MapFrameInfo* frame, int64_t gameTime);

        // getUpdatePacket(id, player): nullopt when nothing is owed.
        std::optional<MapUpdate> NextUpdateFor(int32_t playerKey);

        struct HoldingPlayer {
            int32_t key = 0;
            std::string name;                 // the player's decoration key, kept for after they leave
            bool dirtyData = true;
            int minDirtyX = 0, minDirtyY = 0, maxDirtyX = 127, maxDirtyY = 127;
            bool dirtyDecorations = true;
            int tick = 0;
            int step = 0;
        };
        HoldingPlayer& GetHoldingPlayer(const MapHolderSnapshot& player);
        // Drop a carrier who logged out (MC: the Player isRemoved()).
        void ForgetCarrier(int32_t playerKey);

        // ── Markers ───────────────────────────────────────────────────
        // toggleBanner: false when there is no banner there or the marker
        // limit is reached.
        bool ToggleBanner(const std::optional<MapBanner>& bannerAtPos, const glm::ivec3& pos, int64_t gameTime);
        // checkBanners: a banner marker whose block no longer matches goes.
        // `current(pos)` answers MapBanner.fromWorld.
        template <typename CurrentBanner>
        void CheckBanners(int x, int z, CurrentBanner&& current) {
            for (auto it = m_bannerMarkers.begin(); it != m_bannerMarkers.end();) {
                const MapBanner& expected = it->second;
                if (expected.pos.x == x && expected.pos.z == z) {
                    const std::optional<MapBanner> now = current(expected.pos);
                    if (!now || !(*now == expected)) {
                        const std::string id = expected.Id();
                        it = m_bannerMarkers.erase(it);
                        RemoveDecoration(id);
                        SetDirty();
                        continue;
                    }
                }
                ++it;
            }
        }
        bool HasBanners() const { return !m_bannerMarkers.empty(); }
        void RemovedFromFrame(const glm::ivec3& pos, int entityId);

        // ── Colours ───────────────────────────────────────────────────
        bool UpdateColor(int x, int y, uint8_t newColor);
        void SetColor(int x, int y, uint8_t newColor);

        // ── Decorations ───────────────────────────────────────────────
        void AddClientSideDecorations(const std::vector<MapDecoration>& decorations);
        const std::vector<std::pair<std::string, MapDecoration>>& Decorations() const { return m_decorations; }
        bool IsTrackedCountOverLimit(int limit) const { return m_trackedDecorationCount > limit; }

        // Persistence views.
        const std::unordered_map<std::string, MapBanner>& Banners() const { return m_bannerMarkers; }
        const std::unordered_map<std::string, MapFrame>& Frames() const { return m_frameMarkers; }
        std::unordered_map<std::string, MapBanner>& MutableBanners() { return m_bannerMarkers; }
        std::unordered_map<std::string, MapFrame>& MutableFrames() { return m_frameMarkers; }

        // SavedData.setDirty / isDirty — the store saves dirty maps.
        void SetDirty() { m_dirty = true; }
        bool IsDirty() const { return m_dirty; }
        void ClearDirty() { m_dirty = false; }

        // Client: bumped whenever a colour changes, so the texture manager
        // knows to re-upload.
        uint32_t ColorRevision() const { return m_colorRevision; }

        // MapItemSavedData.addTargetDecoration's companion on the item side
        // lives in MapItem (it writes the stack's component).

        void AddDecoration(DecorationType type, bool haveLevel, int64_t gameTime, const std::string& key,
                           double xPos, double zPos, double yRot, const std::optional<std::string>& name);
        void RemoveDecoration(const std::string& key);

    private:
        struct Location { DecorationType type; int8_t x, y, rot; };
        std::optional<Location> CalculateDecorationLocationAndType(DecorationType type, bool haveLevel,
                                                                   int64_t gameTime, double yRot,
                                                                   float xDelta, float yDelta) const;
        int8_t CalculateRotation(bool haveLevel, int64_t gameTime, double yRot) const;
        std::optional<DecorationType> DecorationTypeForPlayerOutsideMap(float xDelta, float yDelta) const;
        void SetColorsDirty(int x, int y);
        void SetDecorationsDirty();
        MapDecoration* FindDecoration(const std::string& key);

        std::vector<HoldingPlayer> m_carriedBy;
        std::unordered_map<std::string, MapBanner> m_bannerMarkers;
        std::vector<std::pair<std::string, MapDecoration>> m_decorations;   // LinkedHashMap order
        std::unordered_map<std::string, MapFrame> m_frameMarkers;
        int m_trackedDecorationCount = 0;
        bool m_dirty = false;
        uint32_t m_colorRevision = 1;
    };

    // "frame-<entityId>" — MapItemSavedData.getFrameKey.
    std::string FrameKey(int entityId);

} // namespace Game::Maps
