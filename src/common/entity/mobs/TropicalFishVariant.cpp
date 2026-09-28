// File: src/common/entity/mobs/TropicalFishVariant.cpp
#include "common/entity/mobs/TropicalFishVariant.hpp"

#include "common/text/Language.hpp"

namespace Game::TropicalFishVariants {

    namespace {
        // MC TropicalFish.Pattern names, ordinal order.
        constexpr std::string_view kPatternNames[kPatternCount] = {
            "kob", "sunstreak", "snooper", "dasher", "brinely", "spotty",
            "flopper", "stripey", "glitter", "blockfish", "betty", "clayfish",
        };

        // MC DyeColor.getName, id order.
        constexpr std::string_view kDyeNames[kDyeCount] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black",
        };

        // DyeColor ids, for the COMMON_VARIANTS table below.
        constexpr uint8_t kWhite = 0, kOrange = 1, kLightBlue = 3, kYellow = 4, kLime = 5,
                          kPink = 6, kGray = 7, kCyan = 9, kPurple = 10, kBlue = 11, kRed = 14;
    }

    Pattern PatternByPackedId(int packedId) {
        for (int i = 0; i < kPatternCount; ++i) {
            const auto p = static_cast<Pattern>(i);
            if (PackedIdOf(p) == packedId) return p;
        }
        return Pattern::Kob;
    }

    std::string_view PatternName(Pattern p) {
        const auto i = static_cast<size_t>(p);
        return i < static_cast<size_t>(kPatternCount) ? kPatternNames[i] : kPatternNames[0];
    }

    std::optional<Pattern> PatternFromName(std::string_view name) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        for (int i = 0; i < kPatternCount; ++i) {
            if (kPatternNames[i] == name) return static_cast<Pattern>(i);
        }
        return std::nullopt;
    }

    std::string_view DyeName(int dye) { return kDyeNames[DyeById(dye)]; }

    std::optional<uint8_t> DyeFromName(std::string_view name) {
        if (name.rfind("minecraft:", 0) == 0) name.remove_prefix(10);
        for (int i = 0; i < kDyeCount; ++i) {
            if (kDyeNames[i] == name) return static_cast<uint8_t>(i);
        }
        return std::nullopt;
    }

    const std::array<Variant, kCommonVariantCount>& CommonVariants() {
        // MC TropicalFish.COMMON_VARIANTS, verbatim and in order.
        static const std::array<Variant, kCommonVariantCount> kCommon = {{
            { Pattern::Stripey,   kOrange, kGray      },
            { Pattern::Flopper,   kGray,   kGray      },
            { Pattern::Flopper,   kGray,   kBlue      },
            { Pattern::Clayfish,  kWhite,  kGray      },
            { Pattern::Sunstreak, kBlue,   kGray      },
            { Pattern::Kob,       kOrange, kWhite     },
            { Pattern::Spotty,    kPink,   kLightBlue },
            { Pattern::Blockfish, kPurple, kYellow    },
            { Pattern::Clayfish,  kWhite,  kRed       },
            { Pattern::Spotty,    kWhite,  kYellow    },
            { Pattern::Glitter,   kWhite,  kGray      },
            { Pattern::Clayfish,  kWhite,  kOrange    },
            { Pattern::Dasher,    kCyan,   kPink      },
            { Pattern::Brinely,   kLime,   kLightBlue },
            { Pattern::Betty,     kRed,    kWhite     },
            { Pattern::Snooper,   kGray,   kRed       },
            { Pattern::Blockfish, kRed,    kWhite     },
            { Pattern::Flopper,   kWhite,  kYellow    },
            { Pattern::Kob,       kRed,    kWhite     },
            { Pattern::Sunstreak, kGray,   kWhite     },
            { Pattern::Dasher,    kCyan,   kYellow    },
            { Pattern::Flopper,   kYellow, kYellow    },
        }};
        return kCommon;
    }

    int CommonVariantIndex(const Variant& v) {
        const auto& common = CommonVariants();
        for (int i = 0; i < kCommonVariantCount; ++i) {
            if (common[static_cast<size_t>(i)] == v) return i;
        }
        return -1;
    }

    std::vector<std::string> TooltipLines(const Variant& v) {
        // MC Pattern.addToTooltip: a named fish is its predefined name
        // (TropicalFish.getPredefinedName); anything else is the pattern's
        // display name, then "color.minecraft.<base>" and — only when the
        // two differ — ", " and the pattern colour.
        std::vector<std::string> lines;
        const int common = CommonVariantIndex(v);
        if (common != -1) {
            lines.push_back(Language::Get("entity.minecraft.tropical_fish.predefined." +
                                          std::to_string(common)));
            return lines;
        }
        lines.push_back(Language::Get("entity.minecraft.tropical_fish.type." +
                                      std::string(PatternName(v.pattern))));
        std::string colours = Language::Get("color.minecraft." + std::string(DyeName(v.baseColor)));
        if (v.baseColor != v.patternColor) {
            colours += ", ";
            colours += Language::Get("color.minecraft." + std::string(DyeName(v.patternColor)));
        }
        lines.push_back(std::move(colours));
        return lines;
    }

} // namespace Game::TropicalFishVariants
