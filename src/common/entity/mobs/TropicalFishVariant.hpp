// File: src/common/entity/mobs/TropicalFishVariant.hpp
//
// MC net.minecraft.world.entity.animal.fish.TropicalFish's variant half —
// TropicalFish.Pattern, TropicalFish.Base, TropicalFish.Variant and the
// packed-variant arithmetic, plus COMMON_VARIANTS and the bucket tooltip
// (Pattern.addToTooltip). Shared by the entity (spawn roll, save, bucket),
// the item NBT codec, the tooltip and the renderer.
//
// The packed variant is MC's int, bit for bit:
//
//   bits  0..15  Pattern.packedId  = base.id | index << 8
//   bits 16..23  base colour       (DyeColor id, white 0 … black 15)
//   bits 24..31  pattern colour    (DyeColor id)
//
// so a "Variant" int from a vanilla save or a /summon compound reads here
// unchanged, and one written here reads in vanilla.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game::TropicalFishVariants {

    // MC TropicalFish.Base: SMALL (model A, "tropical_a") / LARGE (model B).
    enum class Base : uint8_t { Small = 0, Large = 1 };

    // MC TropicalFish.Pattern, in declaration (ordinal) order.
    enum class Pattern : uint8_t {
        Kob = 0, Sunstreak, Snooper, Dasher, Brinely, Spotty,       // SMALL, index 0..5
        Flopper, Stripey, Glitter, Blockfish, Betty, Clayfish,      // LARGE, index 0..5
    };
    inline constexpr int kPatternCount = 12;
    inline constexpr int kDyeCount = 16;

    // MC Pattern.base().
    inline Base BaseOf(Pattern p) {
        return static_cast<uint8_t>(p) < 6 ? Base::Small : Base::Large;
    }
    // The pattern's index within its base (0..5) — the N of the
    // tropical_<a|b>_pattern_<N+1>.png sheet.
    inline int IndexOf(Pattern p) { return static_cast<uint8_t>(p) % 6; }
    // MC Pattern.getPackedId: base.id | index << 8.
    inline int PackedIdOf(Pattern p) {
        return static_cast<int>(BaseOf(p)) | (IndexOf(p) << 8);
    }
    // MC Pattern.byId — ByIdMap.sparse(getPackedId, values(), KOB): an
    // unknown packed id is KOB.
    Pattern PatternByPackedId(int packedId);
    // MC Pattern.getSerializedName ("kob", "sunstreak", …).
    std::string_view PatternName(Pattern p);
    // StringRepresentable.fromEnum's decode; nullopt for an unknown name.
    std::optional<Pattern> PatternFromName(std::string_view name);

    // MC DyeColor.getName ("white" … "black"); an id out of range is white
    // (DyeColor.byId's OutOfBoundsStrategy.ZERO).
    std::string_view DyeName(int dye);
    std::optional<uint8_t> DyeFromName(std::string_view name);
    // DyeColor.byId (ZERO strategy).
    inline uint8_t DyeById(int id) { return (id >= 0 && id < kDyeCount) ? static_cast<uint8_t>(id) : 0; }

    // MC TropicalFish.Variant(pattern, baseColor, patternColor).
    struct Variant {
        Pattern pattern      = Pattern::Kob;
        uint8_t baseColor    = 0;   // DyeColor id
        uint8_t patternColor = 0;   // DyeColor id

        bool operator==(const Variant& o) const {
            return pattern == o.pattern && baseColor == o.baseColor &&
                   patternColor == o.patternColor;
        }
        bool operator!=(const Variant& o) const { return !(*this == o); }
    };

    // MC TropicalFish.packVariant / Variant.getPackedId.
    inline int32_t Pack(const Variant& v) {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(PackedIdOf(v.pattern)) & 0xFFFFu) |
            ((static_cast<uint32_t>(v.baseColor) & 0xFFu) << 16) |
            ((static_cast<uint32_t>(v.patternColor) & 0xFFu) << 24));
    }
    // MC Variant(int packedId): getPattern / getBaseColor / getPatternColor.
    inline Variant Unpack(int32_t packed) {
        const auto u = static_cast<uint32_t>(packed);
        Variant v;
        v.pattern      = PatternByPackedId(static_cast<int>(u & 0xFFFFu));
        v.baseColor    = DyeById(static_cast<int>((u >> 16) & 0xFFu));
        v.patternColor = DyeById(static_cast<int>((u >> 24) & 0xFFu));
        return v;
    }

    // MC TropicalFish.DEFAULT_VARIANT: KOB, WHITE, WHITE.
    inline constexpr Variant kDefaultVariant{ Pattern::Kob, 0, 0 };

    // MC TropicalFish.COMMON_VARIANTS — the 22 named fish, in MC's list
    // order (the index is the "entity.minecraft.tropical_fish.predefined.N"
    // name).
    inline constexpr int kCommonVariantCount = 22;
    const std::array<Variant, kCommonVariantCount>& CommonVariants();
    // COMMON_VARIANTS.indexOf(variant), -1 when it is not a named fish.
    int CommonVariantIndex(const Variant& v);

    // MC Pattern.addToTooltip — the lines a tropical fish bucket shows
    // (ITALIC GRAY in MC): the named fish ("Clownfish"), or the pattern's
    // name and then its colours ("Kob" / "Red, White"; one colour when both
    // match). Resolved through the language table.
    std::vector<std::string> TooltipLines(const Variant& v);

} // namespace Game::TropicalFishVariants
