// File: src/common/entity/npc/VillagerData.hpp
//
// MC net.minecraft.world.entity.npc.villager.{VillagerData, VillagerType,
// VillagerProfession} — who a villager is: the biome it was born to (TYPE), its
// job (PROFESSION) and how far it has come in it (LEVEL, 1..5).
//
// MC keeps type and profession in registries; the vanilla sets are closed, so
// here they are enums in REGISTRY ORDER (the bootstrap order), which is also
// the order the ids take on the wire (entity data packs them into the anim and
// variant bytes — see Villager::GetVariantByte). NBT carries the registry
// NAMES ("minecraft:plains", "minecraft:farmer"), exactly MC's
// VillagerData.CODEC, so a real save round-trips.
#pragma once

#include "common/entity/Item.hpp"
#include "common/entity/ai/village/PoiTypes.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Game {

    // MC VillagerType.bootstrap order.
    enum class VillagerType : uint8_t {
        Desert, Jungle, Plains, Savanna, Snow, Swamp, Taiga,
        Count,
    };
    inline constexpr int kVillagerTypeCount = static_cast<int>(VillagerType::Count);

    // MC VillagerProfession.bootstrap order.
    enum class VillagerProfession : uint8_t {
        None, Armorer, Butcher, Cartographer, Cleric, Farmer, Fisherman, Fletcher,
        Leatherworker, Librarian, Mason, Nitwit, Shepherd, Toolsmith, Weaponsmith,
        Count,
    };
    inline constexpr int kVillagerProfessionCount = static_cast<int>(VillagerProfession::Count);

    // Registry paths ("plains", "farmer").
    std::string_view VillagerTypeId(VillagerType type);
    std::string_view VillagerProfessionId(VillagerProfession profession);
    // Accept "minecraft:plains" and bare "plains". False for an unknown id.
    bool ParseVillagerType(std::string_view name, VillagerType& out);
    bool ParseVillagerProfession(std::string_view name, VillagerProfession& out);

    // MC VillagerType.byBiome: the type a villager born in `biome` (registry
    // path or "minecraft:"-qualified) takes; PLAINS for any biome not in the
    // table.
    VillagerType VillagerTypeByBiome(std::string_view biome);

    // MC VillagerData (a record). `level` is clamped to >= 1 on construction,
    // as MC's canonical constructor does.
    struct VillagerData {
        static constexpr int kMinLevel = 1;   // MIN_VILLAGER_LEVEL
        static constexpr int kMaxLevel = 5;   // MAX_VILLAGER_LEVEL

        VillagerType       type       = VillagerType::Plains;   // DEFAULT_TYPE
        VillagerProfession profession = VillagerProfession::None;
        int                level      = 1;

        VillagerData() = default;
        VillagerData(VillagerType t, VillagerProfession p, int l)
            : type(t), profession(p), level(l < 1 ? 1 : l) {}

        VillagerData WithType(VillagerType t) const { return { t, profession, level }; }
        VillagerData WithProfession(VillagerProfession p) const { return { type, p, level }; }
        VillagerData WithLevel(int l) const { return { type, profession, l }; }

        bool operator==(const VillagerData& o) const {
            return type == o.type && profession == o.profession && level == o.level;
        }
        bool operator!=(const VillagerData& o) const { return !(*this == o); }

        // MC NEXT_LEVEL_XP_THRESHOLDS = {0, 10, 70, 150, 250}.
        static bool CanLevelUp(int currentLevel) { return currentLevel >= 1 && currentLevel < kMaxLevel; }
        static int  GetMinXpPerLevel(int level);
        static int  GetMaxXpPerLevel(int level);
    };

    // ── Profession metadata (the VillagerProfession record's fields) ──────

    // MC profession.name(): "entity.minecraft.villager.<path>" resolved
    // ("Farmer", "Villager" for none).
    std::string VillagerProfessionDisplayName(VillagerProfession profession);
    // MC "merchant.level.<n>" — Novice … Master.
    std::string MerchantLevelName(int level);

    // MC heldJobSite / acquirableJobSite. NONE holds nothing and may acquire
    // any #acquirable_job_site; NITWIT neither holds nor acquires; every other
    // profession holds and acquires exactly its own job site.
    bool ProfessionHoldsJobSite(VillagerProfession profession, PoiType type);
    bool ProfessionCanAcquireJobSite(VillagerProfession profession, PoiType type);
    // The profession whose held job site is `type` — MC
    // AssignProfessionFromJobSite's registry scan. nullopt for HOME/MEETING.
    std::optional<VillagerProfession> ProfessionForJobSite(PoiType type);

    // MC profession.workSound() — "entity.villager.work_farmer". Null for
    // NONE and NITWIT, which have none.
    const char* ProfessionWorkSound(VillagerProfession profession);

    // MC profession.requestedItems() — the farmer's wheat, seeds and bone
    // meal; empty for every other profession.
    bool ProfessionRequestsItem(VillagerProfession profession, ItemID item);
    // MC profession.secondaryPoi() — the farmer's farmland.
    bool ProfessionHasSecondaryPoi(VillagerProfession profession, BlockID block);

    // MC profession.getTrades(level) — the TradeSet key
    // ("minecraft:farmer/level_1") or empty when the profession trades nothing
    // at that level (NONE, NITWIT).
    std::string ProfessionTradeSetKey(VillagerProfession profession, int level);

    // MC DataComponents.VILLAGER_FOOD (Items.java villagerFood(n)): bread 4,
    // carrot / potato / beetroot 1. 0 = not villager food.
    int VillagerFoodNutrition(ItemID item);
    // MC ItemTags.VILLAGER_PLANTABLE_SEEDS.
    bool IsVillagerPlantableSeed(ItemID item);
    // MC ItemTags.VILLAGER_PICKS_UP (#villager_plantable_seeds + bread, wheat,
    // beetroot).
    bool IsVillagerPicksUp(ItemID item);

    // ── Wire packing ─────────────────────────────────────────────────────
    // The villager's synched state rides the tracker's two per-type bytes:
    //   variant byte = type (bits 0-2) | level (bits 3-5)
    //   anim byte    = profession (bits 0-3) | unhappy (bit 4)
    uint8_t PackVillagerVariant(const VillagerData& data);
    void    UnpackVillagerVariant(uint8_t byte, VillagerData& data);

} // namespace Game
