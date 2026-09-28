// File: src/common/world/map/MapColor.cpp
#include "MapColor.hpp"

#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/GeneratedBlockHardness.hpp"

#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game::MapColors {

    namespace {

        // MapColor.java, id order.
        constexpr uint32_t kColors[kColorCount] = {
            0,        8368696,  16247203, 13092807, 16711680, 10526975, 10987431, 31744,
            16777215, 10791096, 9923917,  7368816,  4210943,  9402184,  16776437, 14188339,
            11685080, 6724056,  15066419, 8375321,  15892389, 5000268,  10066329, 5013401,
            8339378,  3361970,  6704179,  6717235,  10040115, 1644825,  16445005, 6085589,
            4882687,  55610,    8476209,  7340544,  13742497, 10441252, 9787244,  7367818,
            12223780, 6780213,  10505550, 3746083,  8874850,  5725276,  8014168,  4996700,
            4993571,  5001770,  9321518,  2430480,  12398641, 9715553,  6035741,  1474182,
            3837580,  5647422,  1356933,  6579300,  14200723, 8365974,
        };

        const std::unordered_map<uint32_t, uint8_t>& RgbIndex() {
            static const std::unordered_map<uint32_t, uint8_t> index = [] {
                std::unordered_map<uint32_t, uint8_t> m;
                for (int i = 1; i < kColorCount; ++i) m.emplace(kColors[i], static_cast<uint8_t>(i));
                return m;
            }();
            return index;
        }

        // Every block's default MapColor id, resolved once from the registry's
        // RGB column.
        const std::vector<uint8_t>& BlockDefaults() {
            static const std::vector<uint8_t> table = [] {
                std::vector<uint8_t> t(static_cast<size_t>(BlockID::Count), NONE);
                for (size_t i = 0; i < t.size(); ++i) {
                    t[i] = IdFromRgb(BlockRegistry::Get(static_cast<BlockID>(i)).mapColor);
                }
                return t;
            }();
            return table;
        }

        uint8_t SlugDefault(std::string_view slug, uint8_t fallback) {
            if (const GeneratedBlockHardnessRow* row = FindBlockHardness(slug)) return IdFromRgb(row->mapColor);
            return fallback;
        }

        // Blocks.logProperties(topColor, sideColor): `axis == Y ? top : side`.
        // The table's column is the top colour (the lambda's first MapColor);
        // these are the logs whose side differs, with their side colour —
        // Blocks.java OAK_LOG .. STRIPPED_CHERRY_LOG.
        const std::array<uint8_t, static_cast<size_t>(BlockID::Count)>& LogSides() {
            static const std::array<uint8_t, static_cast<size_t>(BlockID::Count)> sides = [] {
                std::array<uint8_t, static_cast<size_t>(BlockID::Count)> s{};
                s.fill(0xFF);
                const std::pair<std::string_view, uint8_t> rows[] = {
                    {"oak_log",             PODZOL},
                    {"spruce_log",          COLOR_BROWN},
                    {"birch_log",           QUARTZ},
                    {"jungle_log",          PODZOL},
                    {"acacia_log",          STONE},
                    {"cherry_log",          TERRACOTTA_GRAY},
                    {"pale_oak_log",        SlugDefault("pale_oak_wood", STONE)},
                    {"mangrove_log",        PODZOL},
                    {"poplar_log",          PODZOL},
                    {"bamboo_block",        PLANT},
                    {"stripped_cherry_log", TERRACOTTA_PINK},
                };
                for (size_t i = 0; i < s.size(); ++i) {
                    const std::string_view slug = BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug;
                    if (slug.empty()) continue;
                    for (const auto& [name, side] : rows) {
                        if (slug == name) { s[i] = side; break; }
                    }
                }
                return s;
            }();
            return sides;
        }

        bool IsBed(BlockID id) {
            const std::string_view slug = BlockRegistry::Get(id).registrySlug;
            return slug.size() > 4 && slug.substr(slug.size() - 4) == "_bed" && slug != "straw_bed";
        }
    }

    uint32_t ColorRgb(uint8_t colorId) {
        return colorId < kColorCount ? kColors[colorId] : 0u;
    }

    uint8_t IdFromRgb(uint32_t rgb) {
        if (rgb == 0) return NONE;
        const auto& index = RgbIndex();
        const auto it = index.find(rgb & 0xFFFFFFu);
        return it != index.end() ? it->second : NONE;
    }

    uint32_t ArgbFromPackedId(uint8_t packedId) {
        const uint8_t id = static_cast<uint8_t>(packedId >> 2);
        if (id == NONE || id >= kColorCount) return 0u;   // byIdUnsafe → NONE
        const int scale = BrightnessModifier(static_cast<Brightness>(packedId & 3));
        const uint32_t col = kColors[id];
        const uint32_t r = ((col >> 16) & 0xFFu) * static_cast<uint32_t>(scale) / 255u;
        const uint32_t g = ((col >> 8) & 0xFFu) * static_cast<uint32_t>(scale) / 255u;
        const uint32_t b = (col & 0xFFu) * static_cast<uint32_t>(scale) / 255u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    uint8_t GetBlockMapColor(BlockState state) {
        const BlockID id = state.Block();
        const size_t index = static_cast<size_t>(id);
        const auto& defaults = BlockDefaults();
        if (index >= defaults.size()) return NONE;

        // logProperties: the side colour unless the log stands upright.
        const uint8_t side = LogSides()[index];
        if (side != 0xFF) {
            const std::string_view axis = state.GetName(PropertyId::AXIS);
            if (!axis.empty() && axis != "y") return side;
            return defaults[index];
        }
        // BED: `part == FOOT ? color.getMapColor() : MapColor.WOOL`.
        if (IsBed(id) && state.GetName(PropertyId::PART) == "head") return WOOL;
        // WHEAT: `age >= 6 ? COLOR_YELLOW : PLANT`.
        if (id == BlockID::Wheat) {
            const int age = state.GetIndex(PropertyId::AGE_7);
            if (age >= 0 && age < 6) return PLANT;
        }
        return defaults[index];
    }

} // namespace Game::MapColors
