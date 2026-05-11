// File: src/common/world/enchantment/Enchantment.cpp
#include "Enchantment.hpp"

#include <unordered_map>

namespace Game {

    // MC's ChatFormatting palette — ARGB. ChatFormatting.RED = 0xFF5555 + alpha,
    // ChatFormatting.GRAY = 0xAAAAAA + alpha. Used by Enchantment.getFullname
    // (Enchantment.java:147 RED for curses, :149 GRAY for non-curses).
    namespace {
        constexpr uint32_t COLOR_GRAY = 0xFFAAAAAAu;
        constexpr uint32_t COLOR_RED  = 0xFFFF5555u;

        // Roman numerals 1..10. MC vanilla never goes above V; 6+ are defensive.
        std::string ToRoman(int n) {
            switch (n) {
                case 1: return "I";
                case 2: return "II";
                case 3: return "III";
                case 4: return "IV";
                case 5: return "V";
                case 6: return "VI";
                case 7: return "VII";
                case 8: return "VIII";
                case 9: return "IX";
                case 10: return "X";
                default: return std::to_string(n);
            }
        }

        // Hand-extracted from Enchantments.java:87–129 (declaration order, IDs
        // stable) and the maxLevel from each `Enchantment.definition(...)` call's
        // 3rd arg (or 4th when 2 itemtags are passed). Display names from
        // assets/lang/en_us.json key `enchantment.minecraft.<slug>` (with `lunge`
        // hardcoded — newer enchantment, lang file lags asset dump).
        const std::vector<Enchantment>& BuildTable() {
            static const std::vector<Enchantment> kTable = {
                {"protection",            "Protection",            1, 4, false},
                {"fire_protection",       "Fire Protection",       1, 4, false},
                {"feather_falling",       "Feather Falling",       1, 4, false},
                {"blast_protection",      "Blast Protection",      1, 4, false},
                {"projectile_protection", "Projectile Protection", 1, 4, false},
                {"respiration",           "Respiration",           1, 3, false},
                {"aqua_affinity",         "Aqua Affinity",         1, 1, false},
                {"thorns",                "Thorns",                1, 3, false},
                {"depth_strider",         "Depth Strider",         1, 3, false},
                {"frost_walker",          "Frost Walker",          1, 2, false},
                {"binding_curse",         "Curse of Binding",      1, 1, true },
                {"soul_speed",            "Soul Speed",            1, 3, false},
                {"swift_sneak",           "Swift Sneak",           1, 3, false},
                {"sharpness",             "Sharpness",             1, 5, false},
                {"smite",                 "Smite",                 1, 5, false},
                {"bane_of_arthropods",    "Bane of Arthropods",    1, 5, false},
                {"knockback",             "Knockback",             1, 2, false},
                {"fire_aspect",           "Fire Aspect",           1, 2, false},
                {"looting",               "Looting",               1, 3, false},
                {"sweeping_edge",         "Sweeping Edge",         1, 3, false},
                {"efficiency",            "Efficiency",            1, 5, false},
                {"silk_touch",            "Silk Touch",            1, 1, false},
                {"unbreaking",            "Unbreaking",            1, 3, false},
                {"fortune",               "Fortune",               1, 3, false},
                {"power",                 "Power",                 1, 5, false},
                {"punch",                 "Punch",                 1, 2, false},
                {"flame",                 "Flame",                 1, 1, false},
                {"infinity",              "Infinity",              1, 1, false},
                {"luck_of_the_sea",       "Luck of the Sea",       1, 3, false},
                {"lure",                  "Lure",                  1, 3, false},
                {"loyalty",               "Loyalty",               1, 3, false},
                {"impaling",              "Impaling",              1, 5, false},
                {"riptide",               "Riptide",               1, 3, false},
                {"channeling",            "Channeling",            1, 1, false},
                {"multishot",             "Multishot",             1, 1, false},
                {"quick_charge",          "Quick Charge",          1, 3, false},
                {"piercing",              "Piercing",              1, 4, false},
                {"density",               "Density",               1, 5, false},
                {"breach",                "Breach",                1, 4, false},
                {"wind_burst",            "Wind Burst",            1, 3, false},
                {"lunge",                 "Lunge",                 1, 3, false},
                {"mending",               "Mending",               1, 1, false},
                {"vanishing_curse",       "Curse of Vanishing",    1, 1, true },
            };
            return kTable;
        }

        const std::unordered_map<std::string, EnchantmentId>& BuildIndex() {
            static const std::unordered_map<std::string, EnchantmentId> kIndex = []{
                std::unordered_map<std::string, EnchantmentId> m;
                const auto& t = BuildTable();
                for (size_t i = 0; i < t.size(); ++i) m[t[i].slug] = static_cast<EnchantmentId>(i);
                return m;
            }();
            return kIndex;
        }
    } // namespace

    Enchantment::FormattedLine Enchantment::GetFullname(const Enchantment& e, int level) {
        // Mirrors Enchantment.java:144–157.
        const uint32_t color = e.isCurse ? COLOR_RED : COLOR_GRAY;
        std::string text = e.displayName;
        // MC line 152: append level only if level != 1 || maxLevel != 1.
        if (level != 1 || e.maxLevel != 1) {
            text += " ";
            text += ToRoman(level);
        }
        return {std::move(text), color};
    }

    const std::vector<Enchantment>& EnchantmentRegistry::All() { return BuildTable(); }

    const Enchantment& EnchantmentRegistry::Get(EnchantmentId id) {
        const auto& t = BuildTable();
        return id < t.size() ? t[id] : t[0]; // sentinel: defensive return
    }

    std::optional<EnchantmentId> EnchantmentRegistry::ByName(std::string_view slug) {
        const auto& idx = BuildIndex();
        auto it = idx.find(std::string(slug));
        if (it == idx.end()) return std::nullopt;
        return it->second;
    }

} // namespace Game
