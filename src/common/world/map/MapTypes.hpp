// File: src/common/world/map/MapTypes.hpp
//
// The small value types of MC's map system, shared by the item components,
// the saved data, the wire and the renderer:
//
//   MapDecorationType   world/level/saveddata/maps/MapDecorationTypes.java —
//                       the registry of marker kinds (name, sprite asset,
//                       showOnItemFrame, trackCount). The enum order IS the
//                       registry order, so the value doubles as the network
//                       id (MC's ByteBufCodecs.holderRegistry writes the
//                       registry index).
//   MapDecoration       MapDecoration.java — one marker as the map shows it
//                       (byte x/y in half-pixels from the centre, rot 0..15).
//   MapDecorations      world/item/component/MapDecorations.java — the
//                       MAP_DECORATIONS item component: markers pinned to a
//                       stack in WORLD coordinates (the exploration map's
//                       target "+").
//   MapPostProcessing   world/item/component/MapPostProcessing.java — what a
//                       crafted map still has to become (LOCK / SCALE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Game::Maps {

    constexpr int kMapSize     = 128;   // MapItem.IMAGE_WIDTH / IMAGE_HEIGHT
    constexpr int kHalfMapSize = 64;
    constexpr int kMaxScale    = 4;     // MapItemSavedData.MAX_SCALE
    constexpr int kTrackedDecorationLimit = 256;

    enum class DecorationType : uint8_t {
        Player = 0, Frame, RedMarker, BlueMarker, TargetX, TargetPoint,
        PlayerOffMap, PlayerOffLimits, Mansion, Monument,
        BannerWhite, BannerOrange, BannerMagenta, BannerLightBlue, BannerYellow,
        BannerLime, BannerPink, BannerGray, BannerLightGray, BannerCyan,
        BannerPurple, BannerBlue, BannerBrown, BannerGreen, BannerRed, BannerBlack,
        RedX, VillageDesert, VillagePlains, VillageSavanna, VillageSnowy, VillageTaiga,
        JungleTemple, SwampHut, TrialChambers, AbandonedCamp, AncientCity,
        DesertPyramid, Mineshaft, OceanRuinWarm,
        Count
    };

    struct DecorationTypeInfo {
        std::string_view name;       // registry key path ("mansion")
        std::string_view asset;      // sprite in the map_decorations atlas ("woodland_mansion")
        bool showOnItemFrame;
        bool trackCount;
    };

    const DecorationTypeInfo& Info(DecorationType type);

    // "minecraft:red_x" / "red_x" → the type; nullopt for an unknown key.
    std::optional<DecorationType> DecorationTypeFromKey(std::string_view key);

    // MapDecoration record. The constructor's `rot & 15` is applied by the
    // producers.
    struct MapDecoration {
        DecorationType type = DecorationType::Player;
        int8_t x = 0;
        int8_t y = 0;
        int8_t rot = 0;
        std::optional<std::string> name;   // Optional<Component>, plain text here

        bool RenderOnFrame() const { return Info(type).showOnItemFrame; }
        bool operator==(const MapDecoration& o) const {
            return type == o.type && x == o.x && y == o.y && rot == o.rot && name == o.name;
        }
        bool operator!=(const MapDecoration& o) const { return !(*this == o); }
    };

    // MAP_DECORATIONS: key → Entry(type, x, z, rotation), world coordinates.
    struct MapDecorations {
        struct Entry {
            DecorationType type = DecorationType::Player;
            double x = 0.0;
            double z = 0.0;
            float  rotation = 0.0f;
            bool operator==(const Entry& o) const {
                return type == o.type && x == o.x && z == o.z && rotation == o.rotation;
            }
        };
        std::vector<std::pair<std::string, Entry>> decorations;

        // MapDecorations.withDecoration: a copy with `key` set.
        MapDecorations WithDecoration(const std::string& key, const Entry& entry) const {
            MapDecorations out = *this;
            for (auto& [k, e] : out.decorations) {
                if (k == key) { e = entry; return out; }
            }
            out.decorations.emplace_back(key, entry);
            return out;
        }
    };

    enum class MapPostProcessing : uint8_t { Lock = 0, Scale = 1 };

} // namespace Game::Maps
