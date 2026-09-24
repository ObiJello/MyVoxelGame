// File: src/common/entity/npc/VillagerData.cpp
#include "common/entity/npc/VillagerData.hpp"

#include "common/entity/GeneratedItemList.hpp"
#include "common/text/Language.hpp"

#include <string>

#include <array>

namespace Game {

    namespace {
        constexpr std::string_view StripNs(std::string_view s) {
            constexpr std::string_view kNs = "minecraft:";
            return s.substr(0, kNs.size()) == kNs ? s.substr(kNs.size()) : s;
        }

        constexpr std::array<std::string_view, kVillagerTypeCount> kTypeIds = {
            "desert", "jungle", "plains", "savanna", "snow", "swamp", "taiga",
        };
        constexpr std::array<std::string_view, kVillagerProfessionCount> kProfessionIds = {
            "none", "armorer", "butcher", "cartographer", "cleric", "farmer", "fisherman",
            "fletcher", "leatherworker", "librarian", "mason", "nitwit", "shepherd",
            "toolsmith", "weaponsmith",
        };

        // MC VillagerData.NEXT_LEVEL_XP_THRESHOLDS.
        constexpr int kNextLevelXp[5] = { 0, 10, 70, 150, 250 };

        // The profession that holds each job site (MC VillagerProfession
        // .bootstrap's jobSite argument), indexed by PoiType.
        std::optional<VillagerProfession> JobSiteProfession(PoiType type) {
            switch (type) {
                case PoiType::Armorer:       return VillagerProfession::Armorer;
                case PoiType::Butcher:       return VillagerProfession::Butcher;
                case PoiType::Cartographer:  return VillagerProfession::Cartographer;
                case PoiType::Cleric:        return VillagerProfession::Cleric;
                case PoiType::Farmer:        return VillagerProfession::Farmer;
                case PoiType::Fisherman:     return VillagerProfession::Fisherman;
                case PoiType::Fletcher:      return VillagerProfession::Fletcher;
                case PoiType::Leatherworker: return VillagerProfession::Leatherworker;
                case PoiType::Librarian:     return VillagerProfession::Librarian;
                case PoiType::Mason:         return VillagerProfession::Mason;
                case PoiType::Shepherd:      return VillagerProfession::Shepherd;
                case PoiType::Toolsmith:     return VillagerProfession::Toolsmith;
                case PoiType::Weaponsmith:   return VillagerProfession::Weaponsmith;
                default:                     return std::nullopt;
            }
        }
    }

    std::string_view VillagerTypeId(VillagerType type) {
        const size_t i = static_cast<size_t>(type);
        return i < kTypeIds.size() ? kTypeIds[i] : kTypeIds[2];
    }

    std::string_view VillagerProfessionId(VillagerProfession profession) {
        const size_t i = static_cast<size_t>(profession);
        return i < kProfessionIds.size() ? kProfessionIds[i] : kProfessionIds[0];
    }

    bool ParseVillagerType(std::string_view name, VillagerType& out) {
        name = StripNs(name);
        for (size_t i = 0; i < kTypeIds.size(); ++i) {
            if (kTypeIds[i] == name) { out = static_cast<VillagerType>(i); return true; }
        }
        return false;
    }

    bool ParseVillagerProfession(std::string_view name, VillagerProfession& out) {
        name = StripNs(name);
        for (size_t i = 0; i < kProfessionIds.size(); ++i) {
            if (kProfessionIds[i] == name) { out = static_cast<VillagerProfession>(i); return true; }
        }
        return false;
    }

    VillagerType VillagerTypeByBiome(std::string_view biome) {
        // MC VillagerType.BY_BIOME.
        struct Row { std::string_view biome; VillagerType type; };
        static constexpr Row kRows[] = {
            { "badlands",                 VillagerType::Desert },
            { "desert",                   VillagerType::Desert },
            { "eroded_badlands",          VillagerType::Desert },
            { "wooded_badlands",          VillagerType::Desert },
            { "bamboo_jungle",            VillagerType::Jungle },
            { "jungle",                   VillagerType::Jungle },
            { "sparse_jungle",            VillagerType::Jungle },
            { "savanna_plateau",          VillagerType::Savanna },
            { "savanna",                  VillagerType::Savanna },
            { "windswept_savanna",        VillagerType::Savanna },
            { "deep_frozen_ocean",        VillagerType::Snow },
            { "frozen_ocean",             VillagerType::Snow },
            { "frozen_river",             VillagerType::Snow },
            { "ice_spikes",               VillagerType::Snow },
            { "snowy_beach",              VillagerType::Snow },
            { "snowy_taiga",              VillagerType::Snow },
            { "snowy_plains",             VillagerType::Snow },
            { "grove",                    VillagerType::Snow },
            { "snowy_slopes",             VillagerType::Snow },
            { "frozen_peaks",             VillagerType::Snow },
            { "jagged_peaks",             VillagerType::Snow },
            { "swamp",                    VillagerType::Swamp },
            { "mangrove_swamp",           VillagerType::Swamp },
            { "old_growth_spruce_taiga",  VillagerType::Taiga },
            { "old_growth_pine_taiga",    VillagerType::Taiga },
            { "windswept_gravelly_hills", VillagerType::Taiga },
            { "windswept_hills",          VillagerType::Taiga },
            { "taiga",                    VillagerType::Taiga },
            { "windswept_forest",         VillagerType::Taiga },
        };
        biome = StripNs(biome);
        for (const Row& r : kRows) {
            if (r.biome == biome) return r.type;
        }
        return VillagerType::Plains;   // VillagerData.DEFAULT_TYPE
    }

    int VillagerData::GetMinXpPerLevel(int level) {
        return CanLevelUp(level) ? kNextLevelXp[level - 1] : 0;
    }

    int VillagerData::GetMaxXpPerLevel(int level) {
        return CanLevelUp(level) ? kNextLevelXp[level] : 0;
    }

    std::string VillagerProfessionDisplayName(VillagerProfession profession) {
        return Language::GetOrDefault(
            "entity.minecraft.villager." + std::string(VillagerProfessionId(profession)),
            "Villager");
    }

    std::string MerchantLevelName(int level) {
        static const char* kFallback[5] = { "Novice", "Apprentice", "Journeyman", "Expert", "Master" };
        const int i = level < 1 ? 1 : (level > 5 ? 5 : level);
        return Language::GetOrDefault("merchant.level." + std::to_string(i), kFallback[i - 1]);
    }

    bool ProfessionHoldsJobSite(VillagerProfession profession, PoiType type) {
        // NONE and NITWIT hold PoiType.NONE (nothing).
        const auto holder = JobSiteProfession(type);
        return holder && *holder == profession;
    }

    bool ProfessionCanAcquireJobSite(VillagerProfession profession, PoiType type) {
        if (profession == VillagerProfession::None) return IsAcquirableJobSite(type);   // ALL_ACQUIRABLE_JOBS
        if (profession == VillagerProfession::Nitwit) return false;                     // PoiType.NONE
        return ProfessionHoldsJobSite(profession, type);
    }

    std::optional<VillagerProfession> ProfessionForJobSite(PoiType type) {
        return JobSiteProfession(type);
    }

    const char* ProfessionWorkSound(VillagerProfession profession) {
        switch (profession) {
            case VillagerProfession::Armorer:       return "entity.villager.work_armorer";
            case VillagerProfession::Butcher:       return "entity.villager.work_butcher";
            case VillagerProfession::Cartographer:  return "entity.villager.work_cartographer";
            case VillagerProfession::Cleric:        return "entity.villager.work_cleric";
            case VillagerProfession::Farmer:        return "entity.villager.work_farmer";
            case VillagerProfession::Fisherman:     return "entity.villager.work_fisherman";
            case VillagerProfession::Fletcher:      return "entity.villager.work_fletcher";
            case VillagerProfession::Leatherworker: return "entity.villager.work_leatherworker";
            case VillagerProfession::Librarian:     return "entity.villager.work_librarian";
            case VillagerProfession::Mason:         return "entity.villager.work_mason";
            case VillagerProfession::Shepherd:      return "entity.villager.work_shepherd";
            case VillagerProfession::Toolsmith:     return "entity.villager.work_toolsmith";
            case VillagerProfession::Weaponsmith:   return "entity.villager.work_weaponsmith";
            default:                                return nullptr;
        }
    }

    bool ProfessionRequestsItem(VillagerProfession profession, ItemID item) {
        // MC: FARMER's ImmutableSet.of(WHEAT, WHEAT_SEEDS, BEETROOT_SEEDS,
        // BONE_MEAL); every other profession's set is empty.
        if (profession != VillagerProfession::Farmer) return false;
        return item == Items::Wheat || item == Items::WheatSeeds ||
               item == Items::BeetrootSeeds || item == Items::BoneMeal;
    }

    bool ProfessionHasSecondaryPoi(VillagerProfession profession, BlockID block) {
        return profession == VillagerProfession::Farmer && block == BlockID::Farmland;
    }

    std::string ProfessionTradeSetKey(VillagerProfession profession, int level) {
        if (profession == VillagerProfession::None || profession == VillagerProfession::Nitwit) return {};
        if (level < 1 || level > 5) return {};
        return "minecraft:" + std::string(VillagerProfessionId(profession)) +
               "/level_" + std::to_string(level);
    }

    int VillagerFoodNutrition(ItemID item) {
        if (item == Items::Bread) return 4;
        if (item == Items::Carrot || item == Items::Potato || item == Items::Beetroot) return 1;
        return 0;
    }

    bool IsVillagerPlantableSeed(ItemID item) {
        // data/minecraft/tags/item/villager_plantable_seeds.json.
        return item == Items::WheatSeeds || item == Items::Potato || item == Items::Carrot ||
               item == Items::BeetrootSeeds || item == Items::TorchflowerSeeds ||
               item == Items::PitcherPod;
    }

    bool IsVillagerPicksUp(ItemID item) {
        // data/minecraft/tags/item/villager_picks_up.json.
        return IsVillagerPlantableSeed(item) || item == Items::Bread || item == Items::Wheat ||
               item == Items::Beetroot;
    }

    uint8_t PackVillagerVariant(const VillagerData& data) {
        const int level = data.level < 1 ? 1 : (data.level > 7 ? 7 : data.level);
        return static_cast<uint8_t>((static_cast<uint8_t>(data.type) & 0x7) |
                                    ((static_cast<uint8_t>(level) & 0x7) << 3));
    }

    void UnpackVillagerVariant(uint8_t byte, VillagerData& data) {
        const int type = byte & 0x7;
        data.type  = type < kVillagerTypeCount ? static_cast<VillagerType>(type) : VillagerType::Plains;
        const int level = (byte >> 3) & 0x7;
        data.level = level < 1 ? 1 : level;
    }

} // namespace Game
