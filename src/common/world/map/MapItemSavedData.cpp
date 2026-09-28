// File: src/common/world/map/MapItemSavedData.cpp
#include "MapItemSavedData.hpp"

#include <algorithm>
#include <cmath>

namespace Game::Maps {

    namespace {

        // MapItemSavedData.isInsideMap.
        bool IsInsideMap(float xd, float yd) {
            return xd >= -63.0f && yd >= -63.0f && xd <= 63.0f && yd <= 63.0f;
        }

        // MapItemSavedData.clampMapCoordinate.
        int8_t ClampMapCoordinate(float delta) {
            if (delta <= -63.0f) return -128;
            if (delta >= 63.0f) return 127;
            return static_cast<int8_t>(static_cast<int>(static_cast<double>(delta * 2.0f) + 0.5));
        }

        int FloorDiv(double v) { return static_cast<int>(std::floor(v)); }

        constexpr const char* kDyeNames[16] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
        };
    }

    // ── Records ─────────────────────────────────────────────────────────

    std::string MapBanner::Id() const {
        return "banner-" + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z);
    }

    DecorationType MapBanner::Decoration() const {
        for (int i = 0; i < 16; ++i) {
            if (color == kDyeNames[i]) {
                return static_cast<DecorationType>(static_cast<int>(DecorationType::BannerWhite) + i);
            }
        }
        return DecorationType::BannerWhite;
    }

    std::string MapFrame::FrameId(const glm::ivec3& pos) {
        return "frame-" + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z);
    }

    std::string FrameKey(int entityId) { return "frame-" + std::to_string(entityId); }

    void MapPatch::ApplyToMap(MapItemSavedData& map) const {
        for (int x = 0; x < width; ++x) {
            for (int y = 0; y < height; ++y) {
                const size_t i = static_cast<size_t>(x + y * width);
                if (i < colors.size()) map.SetColor(startX + x, startY + y, colors[i]);
            }
        }
    }

    // ── Construction ────────────────────────────────────────────────────

    MapItemSavedData::MapItemSavedData(int cx, int cz, int8_t s, bool tracking, bool unlimited, bool isLocked,
                                       DimensionId dim)
        : centerX(cx), centerZ(cz), dimension(dim), trackingPosition(tracking), unlimitedTracking(unlimited),
          scale(s), locked(isLocked) {
        colors.fill(0);
    }

    std::shared_ptr<MapItemSavedData> MapItemSavedData::CreateFresh(double originX, double originZ, int8_t scale,
                                                                    bool trackingPosition, bool unlimitedTracking,
                                                                    DimensionId dimension) {
        const int size = kMapSize * (1 << scale);
        const int areaX = FloorDiv((originX + 64.0) / static_cast<double>(size));
        const int areaZ = FloorDiv((originZ + 64.0) / static_cast<double>(size));
        const int x = areaX * size + size / 2 - 64;
        const int z = areaZ * size + size / 2 - 64;
        return std::make_shared<MapItemSavedData>(x, z, scale, trackingPosition, unlimitedTracking, false, dimension);
    }

    std::shared_ptr<MapItemSavedData> MapItemSavedData::CreateForClient(int8_t scale, bool locked, DimensionId dimension) {
        return std::make_shared<MapItemSavedData>(0, 0, scale, false, false, locked, dimension);
    }

    void MapItemSavedData::RestoreMarkersAfterLoad() {
        // The codec constructor: every banner and frame re-adds its marker
        // with no level (so no nether spin), banners facing 180.
        for (const auto& [id, banner] : m_bannerMarkers) {
            AddDecoration(banner.Decoration(), false, 0, id, banner.pos.x, banner.pos.z, 180.0, banner.name);
        }
        for (const auto& [id, frame] : m_frameMarkers) {
            AddDecoration(DecorationType::Frame, false, 0, FrameKey(frame.entityId), frame.pos.x, frame.pos.z,
                          static_cast<double>(frame.rotation), std::nullopt);
        }
    }

    std::shared_ptr<MapItemSavedData> MapItemSavedData::Locked() const {
        auto result = std::make_shared<MapItemSavedData>(centerX, centerZ, scale, trackingPosition,
                                                         unlimitedTracking, true, dimension);
        result->m_bannerMarkers = m_bannerMarkers;
        result->m_decorations = m_decorations;
        result->m_trackedDecorationCount = m_trackedDecorationCount;
        result->colors = colors;
        return result;
    }

    std::shared_ptr<MapItemSavedData> MapItemSavedData::Scaled() const {
        auto result = CreateFresh(centerX, centerZ, static_cast<int8_t>(std::clamp(scale + 1, 0, kMaxScale)),
                                  trackingPosition, unlimitedTracking, dimension);
        if (result->scale == scale) return result;   // already at the maximum scale

        // DELIBERATE DEVIATION FROM JAVA (Bedrock-like): MC Java's scaled()
        // is a blank map — the zoomed-out copy has to be explored again. Here
        // what the source map shows is carried over: each new pixel covers
        // 2×2 source pixels, and takes the most common colour among those
        // that were explored (packed byte of the first one with that colour,
        // so its shading comes along; ties go to the first in scan order),
        // unexplored (0) ones skipped. The source covers one quarter of the
        // new map's area; the rest starts blank, as it would in Java.
        const int oldScale = 1 << scale;
        const int newScale = 1 << result->scale;
        const int oldOriginX = centerX / oldScale - kHalfMapSize;   // MapItem.update's (center / scale + img - 64)
        const int oldOriginZ = centerZ / oldScale - kHalfMapSize;
        for (int y = 0; y < kMapSize; ++y) {
            for (int x = 0; x < kMapSize; ++x) {
                const int minX = (result->centerX / newScale + x - kHalfMapSize) * newScale;
                const int minZ = (result->centerZ / newScale + y - kHalfMapSize) * newScale;
                const int ox = minX / oldScale - oldOriginX;
                const int oz = minZ / oldScale - oldOriginZ;
                uint8_t best = 0;
                int bestCount = 0;
                uint8_t seen[4];
                int counts[4] = {0, 0, 0, 0};
                int distinct = 0;
                for (int dz = 0; dz < 2; ++dz) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int sx = ox + dx, sz = oz + dz;
                        if (sx < 0 || sz < 0 || sx >= kMapSize || sz >= kMapSize) continue;
                        const uint8_t packed = colors[static_cast<size_t>(sx + sz * kMapSize)];
                        if ((packed >> 2) == 0) continue;   // unexplored
                        int slot = 0;
                        while (slot < distinct && (seen[slot] >> 2) != (packed >> 2)) ++slot;
                        if (slot == distinct) seen[distinct++] = packed;
                        if (++counts[slot] > bestCount) {
                            bestCount = counts[slot];
                            best = seen[slot];
                        }
                    }
                }
                if (bestCount > 0) result->colors[static_cast<size_t>(x + y * kMapSize)] = best;
            }
        }

        // The banner markers come along too (MapItemSavedData.locked()
        // copies them; Java's scaled() does not). Each is re-placed on the
        // new scale; one that no longer fits the map is dropped by
        // addDecoration exactly as a toggle would be.
        result->m_bannerMarkers = m_bannerMarkers;
        for (const auto& [id, banner] : result->m_bannerMarkers) {
            result->AddDecoration(banner.Decoration(), false, 0, id, banner.pos.x + 0.5, banner.pos.z + 0.5, 180.0,
                                  banner.name);
        }
        ++result->m_colorRevision;
        return result;
    }

    // ── Carriers ────────────────────────────────────────────────────────

    MapItemSavedData::HoldingPlayer& MapItemSavedData::GetHoldingPlayer(const MapHolderSnapshot& player) {
        for (HoldingPlayer& h : m_carriedBy) {
            if (h.key == player.key) return h;
        }
        HoldingPlayer h;
        h.key = player.key;
        h.name = player.name;
        m_carriedBy.push_back(std::move(h));
        return m_carriedBy.back();
    }

    void MapItemSavedData::ForgetCarrier(int32_t playerKey) {
        for (auto it = m_carriedBy.begin(); it != m_carriedBy.end(); ++it) {
            if (it->key == playerKey) {
                const std::string name = it->name;
                m_carriedBy.erase(it);
                RemoveDecoration(name);
                return;
            }
        }
    }

    void MapItemSavedData::TickCarriedBy(const MapHolderSnapshot& tickingPlayer, const MapHolderTable& players,
                                         uint16_t stackItem, int32_t stackMapId,
                                         const std::optional<MapDecorations>& staticDecorations,
                                         const MapFrameInfo* frame, int64_t gameTime) {
        GetHoldingPlayer(tickingPlayer);

        if (!tickingPlayer.Carries(stackItem, stackMapId)) RemoveDecoration(tickingPlayer.name);

        // MC walks carriedBy by index and removes in place, so the carrier
        // after a removed one is skipped until the next tick — kept.
        for (size_t i = 0; i < m_carriedBy.size(); ++i) {
            const int32_t otherKey = m_carriedBy[i].key;
            const std::string otherName = m_carriedBy[i].name;
            const auto found = players.find(otherKey);
            const MapHolderSnapshot* other = found != players.end() ? &found->second : nullptr;
            if (!other || (frame == nullptr && !other->Carries(stackItem, stackMapId))) {
                m_carriedBy.erase(m_carriedBy.begin() + static_cast<std::ptrdiff_t>(i));
                RemoveDecoration(otherName);
                if (!other) continue;
            } else if (frame == nullptr && other->dimension == dimension && trackingPosition) {
                AddDecoration(DecorationType::Player, true, gameTime, otherName, other->x, other->z,
                              static_cast<double>(other->yRot), std::nullopt);
            }
            if (other->key != tickingPlayer.key && other->mapInvisible) RemoveDecoration(otherName);
        }

        if (frame != nullptr && trackingPosition) {
            const auto existing = m_frameMarkers.find(MapFrame::FrameId(frame->pos));
            if (existing != m_frameMarkers.end() && frame->entityId != existing->second.entityId) {
                RemoveDecoration(FrameKey(existing->second.entityId));
            }
            MapFrame mapFrame{frame->pos, frame->direction2D * 90, frame->entityId};
            AddDecoration(DecorationType::Frame, true, gameTime, FrameKey(frame->entityId), frame->pos.x,
                          frame->pos.z, static_cast<double>(frame->direction2D * 90), std::nullopt);
            auto [slot, inserted] = m_frameMarkers.try_emplace(mapFrame.Id(), mapFrame);
            if (inserted || !(slot->second == mapFrame)) {
                slot->second = mapFrame;
                SetDirty();
            }
        }

        if (staticDecorations) {
            for (const auto& [id, entry] : staticDecorations->decorations) {
                if (FindDecoration(id) == nullptr) {
                    AddDecoration(entry.type, true, gameTime, id, entry.x, entry.z,
                                  static_cast<double>(entry.rotation), std::nullopt);
                }
            }
        }
    }

    std::optional<MapUpdate> MapItemSavedData::NextUpdateFor(int32_t playerKey) {
        HoldingPlayer* holding = nullptr;
        for (HoldingPlayer& h : m_carriedBy) {
            if (h.key == playerKey) { holding = &h; break; }
        }
        if (!holding) return std::nullopt;

        std::optional<MapPatch> patch;
        if (holding->dirtyData) {
            holding->dirtyData = false;
            MapPatch p;
            p.startX = holding->minDirtyX;
            p.startY = holding->minDirtyY;
            p.width  = holding->maxDirtyX + 1 - holding->minDirtyX;
            p.height = holding->maxDirtyY + 1 - holding->minDirtyY;
            p.colors.resize(static_cast<size_t>(p.width * p.height));
            for (int x = 0; x < p.width; ++x) {
                for (int y = 0; y < p.height; ++y) {
                    p.colors[static_cast<size_t>(x + y * p.width)] =
                        colors[static_cast<size_t>(p.startX + x + (p.startY + y) * kMapSize)];
                }
            }
            patch = std::move(p);
        }

        std::optional<std::vector<MapDecoration>> decorations;
        if (holding->dirtyDecorations && holding->tick++ % 5 == 0) {
            holding->dirtyDecorations = false;
            std::vector<MapDecoration> list;
            list.reserve(m_decorations.size());
            for (const auto& [key, decoration] : m_decorations) list.push_back(decoration);
            decorations = std::move(list);
        }

        if (!decorations && !patch) return std::nullopt;
        MapUpdate update;
        update.scale = scale;
        update.locked = locked;
        update.decorations = std::move(decorations);
        update.patch = std::move(patch);
        return update;
    }

    // ── Markers ─────────────────────────────────────────────────────────

    bool MapItemSavedData::ToggleBanner(const std::optional<MapBanner>& bannerAtPos, const glm::ivec3& pos,
                                        int64_t gameTime) {
        const double xPos = pos.x + 0.5;
        const double zPos = pos.z + 0.5;
        const int s = 1 << scale;
        const double xd = (xPos - centerX) / s;
        const double yd = (zPos - centerZ) / s;
        if (xd >= -63.0 && yd >= -63.0 && xd <= 63.0 && yd <= 63.0) {
            if (!bannerAtPos) return false;
            const MapBanner& banner = *bannerAtPos;
            const auto it = m_bannerMarkers.find(banner.Id());
            if (it != m_bannerMarkers.end() && it->second == banner) {   // bannerMarkers.remove(id, banner)
                m_bannerMarkers.erase(it);
                RemoveDecoration(banner.Id());
                SetDirty();
                return true;
            }
            if (!IsTrackedCountOverLimit(kTrackedDecorationLimit)) {
                m_bannerMarkers[banner.Id()] = banner;
                AddDecoration(banner.Decoration(), true, gameTime, banner.Id(), xPos, zPos, 180.0, banner.name);
                SetDirty();
                return true;
            }
        }
        return false;
    }

    void MapItemSavedData::RemovedFromFrame(const glm::ivec3& pos, int entityId) {
        RemoveDecoration(FrameKey(entityId));
        m_frameMarkers.erase(MapFrame::FrameId(pos));
        SetDirty();
    }

    // ── Colours ─────────────────────────────────────────────────────────

    bool MapItemSavedData::UpdateColor(int x, int y, uint8_t newColor) {
        const uint8_t oldColor = colors[static_cast<size_t>(x + y * kMapSize)];
        if (oldColor != newColor) {
            SetColor(x, y, newColor);
            return true;
        }
        return false;
    }

    void MapItemSavedData::SetColor(int x, int y, uint8_t newColor) {
        if (x < 0 || y < 0 || x >= kMapSize || y >= kMapSize) return;
        colors[static_cast<size_t>(x + y * kMapSize)] = newColor;
        SetColorsDirty(x, y);
    }

    void MapItemSavedData::SetColorsDirty(int x, int y) {
        SetDirty();
        ++m_colorRevision;
        for (HoldingPlayer& h : m_carriedBy) {
            if (h.dirtyData) {
                h.minDirtyX = std::min(h.minDirtyX, x);
                h.minDirtyY = std::min(h.minDirtyY, y);
                h.maxDirtyX = std::max(h.maxDirtyX, x);
                h.maxDirtyY = std::max(h.maxDirtyY, y);
            } else {
                h.dirtyData = true;
                h.minDirtyX = x;
                h.minDirtyY = y;
                h.maxDirtyX = x;
                h.maxDirtyY = y;
            }
        }
    }

    void MapItemSavedData::SetDecorationsDirty() {
        for (HoldingPlayer& h : m_carriedBy) h.dirtyDecorations = true;
    }

    // ── Decorations ─────────────────────────────────────────────────────

    MapDecoration* MapItemSavedData::FindDecoration(const std::string& key) {
        for (auto& [k, d] : m_decorations) {
            if (k == key) return &d;
        }
        return nullptr;
    }

    void MapItemSavedData::RemoveDecoration(const std::string& key) {
        for (auto it = m_decorations.begin(); it != m_decorations.end(); ++it) {
            if (it->first == key) {
                if (Info(it->second.type).trackCount) --m_trackedDecorationCount;
                m_decorations.erase(it);
                break;
            }
        }
        SetDecorationsDirty();
    }

    void MapItemSavedData::AddDecoration(DecorationType type, bool haveLevel, int64_t gameTime,
                                         const std::string& key, double xPos, double zPos, double yRot,
                                         const std::optional<std::string>& name) {
        const int scaling = 1 << scale;
        const float xDelta = static_cast<float>(xPos - centerX) / static_cast<float>(scaling);
        const float yDelta = static_cast<float>(zPos - centerZ) / static_cast<float>(scaling);
        const std::optional<Location> location =
            CalculateDecorationLocationAndType(type, haveLevel, gameTime, yRot, xDelta, yDelta);
        if (!location) {
            RemoveDecoration(key);
            return;
        }
        MapDecoration decoration;
        decoration.type = location->type;
        decoration.x = location->x;
        decoration.y = location->y;
        decoration.rot = static_cast<int8_t>(location->rot & 15);   // MapDecoration's constructor
        decoration.name = name;

        MapDecoration* previous = FindDecoration(key);
        if (previous != nullptr && *previous == decoration) return;
        if (previous != nullptr) {
            if (Info(previous->type).trackCount) --m_trackedDecorationCount;
            *previous = decoration;
        } else {
            m_decorations.emplace_back(key, decoration);
        }
        if (Info(decoration.type).trackCount) ++m_trackedDecorationCount;
        SetDecorationsDirty();
    }

    std::optional<MapItemSavedData::Location> MapItemSavedData::CalculateDecorationLocationAndType(
        DecorationType type, bool haveLevel, int64_t gameTime, double yRot, float xDelta, float yDelta) const {
        const int8_t cx = ClampMapCoordinate(xDelta);
        const int8_t cy = ClampMapCoordinate(yDelta);
        if (type == DecorationType::Player) {
            if (IsInsideMap(xDelta, yDelta)) {
                return Location{type, cx, cy, CalculateRotation(haveLevel, gameTime, yRot)};
            }
            const std::optional<DecorationType> outside = DecorationTypeForPlayerOutsideMap(xDelta, yDelta);
            if (!outside) return std::nullopt;
            return Location{*outside, cx, cy, CalculateRotation(haveLevel, gameTime, yRot)};
        }
        if (!IsInsideMap(xDelta, yDelta) && !unlimitedTracking) return std::nullopt;
        return Location{type, cx, cy, CalculateRotation(haveLevel, gameTime, yRot)};
    }

    int8_t MapItemSavedData::CalculateRotation(bool haveLevel, int64_t gameTime, double yRot) const {
        if (dimension == DimensionId::Nether && haveLevel) {
            // The nether's spinning needle: Java int arithmetic, wrapping.
            const uint32_t s = static_cast<uint32_t>(static_cast<int32_t>(gameTime / 10));
            const int32_t v = static_cast<int32_t>(s * s * 34187121u + s * 121u);
            return static_cast<int8_t>((v >> 15) & 15);
        }
        const double adjusted = yRot < 0.0 ? yRot - 8.0 : yRot + 8.0;
        return static_cast<int8_t>(static_cast<int>(adjusted * 16.0 / 360.0));
    }

    std::optional<DecorationType> MapItemSavedData::DecorationTypeForPlayerOutsideMap(float xDelta,
                                                                                      float yDelta) const {
        const bool withinLimits = std::abs(xDelta) < 320.0f && std::abs(yDelta) < 320.0f;
        if (withinLimits) return DecorationType::PlayerOffMap;
        return unlimitedTracking ? std::optional<DecorationType>(DecorationType::PlayerOffLimits) : std::nullopt;
    }

    void MapItemSavedData::AddClientSideDecorations(const std::vector<MapDecoration>& decorations) {
        m_decorations.clear();
        m_trackedDecorationCount = 0;
        for (size_t i = 0; i < decorations.size(); ++i) {
            m_decorations.emplace_back("icon-" + std::to_string(i), decorations[i]);
            if (Info(decorations[i].type).trackCount) ++m_trackedDecorationCount;
        }
    }

    // ── Decoration type registry (MapDecorationTypes.java, registration order) ──

    namespace {
        constexpr DecorationTypeInfo kTypes[static_cast<size_t>(DecorationType::Count)] = {
            {"player",            "player",            false, true},
            {"frame",             "frame",             true,  true},
            {"red_marker",        "red_marker",        false, true},
            {"blue_marker",       "blue_marker",       false, true},
            {"target_x",          "target_x",          true,  false},
            {"target_point",      "target_point",      true,  false},
            {"player_off_map",    "player_off_map",    false, true},
            {"player_off_limits", "player_off_limits", false, true},
            {"mansion",           "woodland_mansion",  true,  false},
            {"monument",          "ocean_monument",    true,  false},
            {"banner_white",      "white_banner",      true,  true},
            {"banner_orange",     "orange_banner",     true,  true},
            {"banner_magenta",    "magenta_banner",    true,  true},
            {"banner_light_blue", "light_blue_banner", true,  true},
            {"banner_yellow",     "yellow_banner",     true,  true},
            {"banner_lime",       "lime_banner",       true,  true},
            {"banner_pink",       "pink_banner",       true,  true},
            {"banner_gray",       "gray_banner",       true,  true},
            {"banner_light_gray", "light_gray_banner", true,  true},
            {"banner_cyan",       "cyan_banner",       true,  true},
            {"banner_purple",     "purple_banner",     true,  true},
            {"banner_blue",       "blue_banner",       true,  true},
            {"banner_brown",      "brown_banner",      true,  true},
            {"banner_green",      "green_banner",      true,  true},
            {"banner_red",        "red_banner",        true,  true},
            {"banner_black",      "black_banner",      true,  true},
            {"red_x",             "red_x",             true,  false},
            {"village_desert",    "desert_village",    true,  false},
            {"village_plains",    "plains_village",    true,  false},
            {"village_savanna",   "savanna_village",   true,  false},
            {"village_snowy",     "snowy_village",     true,  false},
            {"village_taiga",     "taiga_village",     true,  false},
            {"jungle_temple",     "jungle_temple",     true,  false},
            {"swamp_hut",         "swamp_hut",         true,  false},
            {"trial_chambers",    "trial_chambers",    true,  false},
            {"abandoned_camp",    "abandoned_camp",    true,  false},
            {"ancient_city",      "ancient_city",      true,  false},
            {"desert_pyramid",    "desert_pyramid",    true,  false},
            {"mineshaft",         "mineshaft",         true,  false},
            {"ocean_ruin_warm",   "warm_ocean_ruins",  true,  false},
        };
    }

    const DecorationTypeInfo& Info(DecorationType type) {
        const size_t i = static_cast<size_t>(type);
        return kTypes[i < static_cast<size_t>(DecorationType::Count) ? i : 0];
    }

    std::optional<DecorationType> DecorationTypeFromKey(std::string_view key) {
        if (const size_t colon = key.find(':'); colon != std::string_view::npos) {
            if (key.substr(0, colon) != "minecraft") return std::nullopt;
            key = key.substr(colon + 1);
        }
        for (size_t i = 0; i < static_cast<size_t>(DecorationType::Count); ++i) {
            if (kTypes[i].name == key) return static_cast<DecorationType>(i);
        }
        return std::nullopt;
    }

} // namespace Game::Maps
