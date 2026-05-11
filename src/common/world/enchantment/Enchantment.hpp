// File: src/common/world/enchantment/Enchantment.hpp
//
// Mirrors net/minecraft/world/item/enchantment/Enchantment.java — minimal
// public surface used by the inventory rendering & tooltip path. The full MC
// class also tracks weight, anvil cost, cost curves, equipment slots, and
// effect components — not needed for this PR (deferred until in-game enchanting
// mechanics land).
//
// MC stores enchantments in a runtime-built registry (loaded from JSON
// datapacks). For our purposes we hand-extract the static list from MC's
// `Enchantments.java:87–129` (the 43 vanilla enchantments) since we don't
// have a datapack pipeline.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    // Stable index into the enchantment table — matches declaration order in
    // Enchantments.java so save / network IDs are consistent across builds.
    using EnchantmentId = uint16_t;

    struct Enchantment {
        std::string slug;        // canonical key, e.g. "sharpness"
        std::string displayName; // pulled from assets/lang/en_us.json
        int         minLevel = 1; // mirrors Enchantment.java:120 getMinLevel
        int         maxLevel;     // mirrors Enchantment.java:124 getMaxLevel
        bool        isCurse = false; // colour switch (RED vs GRAY) for tooltip — see Enchantment.java:146

        // Mirrors Enchantment.java:144 getFullname — produces the hover-text
        // line for an enchantment at a given level. Curses → red, others → gray.
        // Level suffix appended only if `level != 1 || maxLevel != 1`.
        // Returned colour is the ARGB constant for use with our text renderer.
        struct FormattedLine { std::string text; uint32_t colorARGB; };
        static FormattedLine GetFullname(const Enchantment& e, int level);
    };

    class EnchantmentRegistry {
    public:
        static const std::vector<Enchantment>& All();
        static const Enchantment&              Get(EnchantmentId id);
        static std::optional<EnchantmentId>    ByName(std::string_view slug);
    };

    // Strongly-named index constants for call sites that prefer the symbolic
    // form (e.g. EnchantmentHelper::CreateBook(Enchantments::Sharpness, 5)).
    // IDs are stable: declaration order MUST match Enchantments.java:87–129.
    namespace Enchantments {
        constexpr EnchantmentId Protection           = 0;
        constexpr EnchantmentId FireProtection       = 1;
        constexpr EnchantmentId FeatherFalling       = 2;
        constexpr EnchantmentId BlastProtection      = 3;
        constexpr EnchantmentId ProjectileProtection = 4;
        constexpr EnchantmentId Respiration          = 5;
        constexpr EnchantmentId AquaAffinity         = 6;
        constexpr EnchantmentId Thorns               = 7;
        constexpr EnchantmentId DepthStrider         = 8;
        constexpr EnchantmentId FrostWalker          = 9;
        constexpr EnchantmentId BindingCurse         = 10;
        constexpr EnchantmentId SoulSpeed            = 11;
        constexpr EnchantmentId SwiftSneak           = 12;
        constexpr EnchantmentId Sharpness            = 13;
        constexpr EnchantmentId Smite                = 14;
        constexpr EnchantmentId BaneOfArthropods     = 15;
        constexpr EnchantmentId Knockback            = 16;
        constexpr EnchantmentId FireAspect           = 17;
        constexpr EnchantmentId Looting              = 18;
        constexpr EnchantmentId SweepingEdge         = 19;
        constexpr EnchantmentId Efficiency           = 20;
        constexpr EnchantmentId SilkTouch            = 21;
        constexpr EnchantmentId Unbreaking           = 22;
        constexpr EnchantmentId Fortune              = 23;
        constexpr EnchantmentId Power                = 24;
        constexpr EnchantmentId Punch                = 25;
        constexpr EnchantmentId Flame                = 26;
        constexpr EnchantmentId Infinity             = 27;
        constexpr EnchantmentId LuckOfTheSea         = 28;
        constexpr EnchantmentId Lure                 = 29;
        constexpr EnchantmentId Loyalty              = 30;
        constexpr EnchantmentId Impaling             = 31;
        constexpr EnchantmentId Riptide              = 32;
        constexpr EnchantmentId Channeling           = 33;
        constexpr EnchantmentId Multishot            = 34;
        constexpr EnchantmentId QuickCharge          = 35;
        constexpr EnchantmentId Piercing             = 36;
        constexpr EnchantmentId Density              = 37;
        constexpr EnchantmentId Breach               = 38;
        constexpr EnchantmentId WindBurst            = 39;
        constexpr EnchantmentId Lunge                = 40;
        constexpr EnchantmentId Mending              = 41;
        constexpr EnchantmentId VanishingCurse       = 42;
        constexpr EnchantmentId Count                = 43;
    }

} // namespace Game
