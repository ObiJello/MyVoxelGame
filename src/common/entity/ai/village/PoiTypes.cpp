// File: src/common/entity/ai/village/PoiTypes.cpp
#include "common/entity/ai/village/PoiTypes.hpp"

#include "common/world/block/BedBlock.hpp"
#include "common/world/block/Blocks.hpp"

#include <array>

namespace Game {

    namespace {
        // MC PoiTypes.bootstrap — maxTickets and validRange per type.
        constexpr std::array<PoiTypeInfo, kPoiTypeCount> kInfo = {{
            { "armorer",       1, 1 },
            { "butcher",       1, 1 },
            { "cartographer",  1, 1 },
            { "cleric",        1, 1 },
            { "farmer",        1, 1 },
            { "fisherman",     1, 1 },
            { "fletcher",      1, 1 },
            { "leatherworker", 1, 1 },
            { "librarian",     1, 1 },
            { "mason",         1, 1 },
            { "shepherd",      1, 1 },
            { "toolsmith",     1, 1 },
            { "weaponsmith",   1, 1 },
            { "home",          1, 1 },
            { "meeting",      32, 6 },
        }};

        // MC PoiTypes.BEDS: every state of Blocks.BED.asList() (the sixteen
        // dyed beds — the straw bed is its own block outside that family)
        // whose PART is HEAD.
        bool IsDyedBed(BlockID id) {
            return IsBedBlock(id) && id != BlockID::StrawBed;
        }

        // Whole-block POI types: every state of the block belongs to the type
        // (MC getBlockStates(block)). The cauldrons are MC's CAULDRONS set —
        // the empty, water, lava and powder-snow cauldrons, all four.
        std::optional<PoiType> WholeBlockType(BlockID id) {
            switch (id) {
                case BlockID::BlastFurnace:       return PoiType::Armorer;
                case BlockID::Smoker:             return PoiType::Butcher;
                case BlockID::CartographyTable:   return PoiType::Cartographer;
                case BlockID::BrewingStand:       return PoiType::Cleric;
                case BlockID::Composter:          return PoiType::Farmer;
                case BlockID::Barrel:             return PoiType::Fisherman;
                case BlockID::FletchingTable:     return PoiType::Fletcher;
                case BlockID::Cauldron:
                case BlockID::WaterCauldron:
                case BlockID::LavaCauldron:
                case BlockID::PowderSnowCauldron: return PoiType::Leatherworker;
                case BlockID::Lectern:            return PoiType::Librarian;
                case BlockID::Stonecutter:        return PoiType::Mason;
                case BlockID::Loom:               return PoiType::Shepherd;
                case BlockID::SmithingTable:      return PoiType::Toolsmith;
                case BlockID::Grindstone:         return PoiType::Weaponsmith;
                case BlockID::Bell:               return PoiType::Meeting;
                default:                          return std::nullopt;
            }
        }
    }

    const PoiTypeInfo& GetPoiTypeInfo(PoiType type) {
        const size_t i = static_cast<size_t>(type);
        return kInfo[i < kInfo.size() ? i : 0];
    }

    std::string_view PoiTypeId(PoiType type) { return GetPoiTypeInfo(type).id; }

    std::optional<PoiType> PoiTypeForState(BlockState state) {
        const BlockID id = state.Block();
        if (IsDyedBed(id)) {
            if (IsBedHead(state)) return PoiType::Home;
            return std::nullopt;
        }
        return WholeBlockType(id);
    }

    bool BlockMayHavePoi(BlockID block) {
        return IsDyedBed(block) || WholeBlockType(block).has_value();
    }

    bool IsAcquirableJobSite(PoiType type) {
        return type >= PoiType::Armorer && type <= PoiType::Weaponsmith;
    }

    bool IsVillagePoi(PoiType type) {
        return IsAcquirableJobSite(type) || type == PoiType::Home || type == PoiType::Meeting;
    }

} // namespace Game
