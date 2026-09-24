// File: src/common/entity/ai/village/PoiTypes.hpp
//
// MC net.minecraft.world.entity.ai.village.poi.{PoiType, PoiTypes} and
// net.minecraft.tags.PoiTypeTags — the point-of-interest registry the village
// machinery runs on.
//
// A POI type is a set of block STATES plus two numbers:
//   maxTickets  how many mobs may claim one at once (a job site 1, a bed 1,
//               a bell 32);
//   validRange  how close a path has to get before the POI counts as reached
//               (AcquirePoi.findPathToPois hands it to the pathfinder).
//
// Only the types a villager touches are registered — the thirteen job sites,
// HOME (the HEAD half of every dyed bed) and MEETING (the bell). MC's other
// entries (bee nests, beehives, nether portals, lodestones, lightning rods,
// test instances) are consumed by systems this engine runs on its own indexes
// (the nether portal index) or does not run through the POI manager at all
// (the bee). Registering them here with no reader would only make every chunk
// scan pay for blocks nothing asks about.
#pragma once

#include "common/world/block/BlockState.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Game {

    // Declaration order is MC's PoiTypes.bootstrap order — the registry order.
    enum class PoiType : uint8_t {
        Armorer,
        Butcher,
        Cartographer,
        Cleric,
        Farmer,
        Fisherman,
        Fletcher,
        Leatherworker,
        Librarian,
        Mason,
        Shepherd,
        Toolsmith,
        Weaponsmith,
        Home,
        Meeting,
        Count,
    };
    inline constexpr int kPoiTypeCount = static_cast<int>(PoiType::Count);

    struct PoiTypeInfo {
        const char* id;          // registry path, "armorer"
        int         maxTickets;  // MC PoiType.maxTickets
        int         validRange;  // MC PoiType.validRange
    };

    const PoiTypeInfo& GetPoiTypeInfo(PoiType type);
    inline int  PoiMaxTickets(PoiType type) { return GetPoiTypeInfo(type).maxTickets; }
    inline int  PoiValidRange(PoiType type) { return GetPoiTypeInfo(type).validRange; }
    std::string_view PoiTypeId(PoiType type);

    // MC PoiTypes.forState — the type whose state set holds `state`, if any.
    // A bed counts only by its HEAD (MC's BEDS set filters part=head), which
    // is why the foot half of a bed is never a home.
    std::optional<PoiType> PoiTypeForState(BlockState state);
    // MC PoiTypes.hasPoi — the cheap membership test the chunk scan runs
    // against a section palette before walking the section.
    bool BlockMayHavePoi(BlockID block);

    // ── PoiTypeTags ──────────────────────────────────────────────────────
    // data/minecraft/tags/point_of_interest_type/*.json.
    //   acquirable_job_site  the thirteen job sites
    //   village              acquirable_job_site + home + meeting
    bool IsAcquirableJobSite(PoiType type);
    bool IsVillagePoi(PoiType type);

} // namespace Game
