// File: src/common/data/components/ItemFeatureComponents.hpp
//
// MC 26.3 item components (item features): trim, provides_trim_material,
// jukebox_playable, provides_banner_patterns, provides_pottery_pattern,
// recipes, lodestone_tracker, dye.
// Wire ids 370-399 (DataComponents.hpp's id table). The NBT codecs live in
// server/world/storage/anvil/components/ItemFeatureNbt.cpp, the tooltip
// providers register with ComponentTooltips.
#pragma once

#include "../DataComponents.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/level/DimensionId.hpp"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Game {

    class World;
    class IUsePlayer;

    // MC ArmorTrim: the trim material and pattern holders ("minecraft:iron",
    // "minecraft:coast").
    struct ArmorTrim {
        std::string material;
        std::string pattern;
        bool operator==(const ArmorTrim& o) const { return material == o.material && pattern == o.pattern; }
    };

    // MC GlobalPos: a dimension and a block position.
    struct GlobalPos {
        std::string dimension;   // "minecraft:overworld"
        glm::ivec3  pos{0};
        bool operator==(const GlobalPos& o) const { return dimension == o.dimension && pos == o.pos; }
    };

    // MC LodestoneTracker: the lodestone a compass points at (none once the
    // lodestone is gone), and whether the server keeps checking it.
    struct LodestoneTracker {
        std::optional<GlobalPos> target;
        bool                     tracked = true;
        bool operator==(const LodestoneTracker& o) const { return target == o.target && tracked == o.tracked; }
    };

    // ── Trim registries (data/<ns>/trim_material, trim_pattern) ─────────

    // TrimMaterial.description / TrimPattern.description from the data
    // pack (a translatable with the material's colour); nullopt when the
    // registry has no such entry.
    std::optional<Text::Component> TrimMaterialDescription(std::string_view id);
    std::optional<Text::Component> TrimPatternDescription(std::string_view id);
    bool TrimMaterialExists(std::string_view id);
    bool TrimPatternExists(std::string_view id);
    // TrimMaterial.assets' base asset suffix ("iron") and its per-armour
    // override ("iron_darker" on iron armour) — the armour layer's palette.
    std::string TrimMaterialAssetFor(std::string_view material, std::string_view equipmentAsset);
    // TrimPattern.asset_id's path ("coast").
    std::string TrimPatternAsset(std::string_view pattern);

    // ── Helpers ─────────────────────────────────────────────────────────

    // MC `stack.get(DataComponents.DYE)`: the dye colour ordinal (white 0 …
    // black 15), -1 without it.
    int DyeColorOf(const ItemStack& stack);

    // PROVIDES_POTTERY_PATTERN of an item (its default component): the
    // pattern's texture stem ("angler_pottery_pattern"), or "" when the
    // item provides none (a brick, an empty side).
    std::string PotteryPatternStem(ItemID item);

    // PROVIDES_BANNER_PATTERNS of a stack, as holder-set entries
    // ("#minecraft:pattern_item/flower" or pattern ids); empty without it.
    std::vector<std::string> ProvidedBannerPatterns(const ItemStack& stack);

    // CompassItem: useOn (lock onto a lodestone), getName, isFoil and the
    // server's inventoryTick (LodestoneTracker.tick: a lost lodestone in the
    // holder's dimension clears the target). Returns whether the stack
    // changed.
    UseResult CompassUseOn(const UseOnContext& ctx, ItemStack& stack);
    bool TickLodestoneTracker(ItemStack& stack, DimensionId dimension, World& world);

    // KnowledgeBookItem.use.
    UseResult KnowledgeBookUse(ILevelWrite* world, IUsePlayer* player, uint32_t hand, ItemStack& stack);

    // Items.java's defaults: the discs' JUKEBOX_PLAYABLE, the banner
    // pattern items' PROVIDES_BANNER_PATTERNS, the sherds'
    // PROVIDES_POTTERY_PATTERN, the trim materials' PROVIDES_TRIM_MATERIAL,
    // the dyes' DYE, the knowledge book's empty RECIPES — and the compass's
    // and knowledge book's behaviours.
    void ItemRegistry_RegisterItemFeatureDefaults(std::unordered_map<ItemID, Item>& pureItems);

} // namespace Game

namespace Game::DataComponents {

    extern const DataComponentType<ArmorTrim>                TRIM;
    extern const DataComponentType<std::string>              PROVIDES_TRIM_MATERIAL;
    extern const DataComponentType<std::string>              JUKEBOX_PLAYABLE;
    extern const DataComponentType<std::vector<std::string>> PROVIDES_BANNER_PATTERNS;
    extern const DataComponentType<std::string>              PROVIDES_POTTERY_PATTERN;
    extern const DataComponentType<std::vector<std::string>> RECIPES;
    extern const DataComponentType<LodestoneTracker>         LODESTONE_TRACKER;
    extern const DataComponentType<int32_t>                  DYE;

} // namespace Game::DataComponents
