// File: src/common/world/block/RedstoneFamilies.cpp
#include "common/world/block/RedstoneFamilies.hpp"

#include "common/world/block/BlockRegistry.hpp"

#include <array>
#include <string>

namespace Game {

    namespace {
        enum Bit : uint16_t {
            kButton        = 1u << 0,
            kWoodenButton  = 1u << 1,
            kPlate         = 1u << 2,
            kWeightedPlate = 1u << 3,
            kPlateMobsOnly = 1u << 4,
            kRail          = 1u << 5,
            kStraightRail  = 1u << 6,
            kDiode         = 1u << 7,
            kStandingTorch = 1u << 8,
            kWallTorch     = 1u << 9,
            kIron          = 1u << 10,
        };

        std::array<uint16_t, BlockRegistry::Size> s_bits{};
        std::array<BlockID,  BlockRegistry::Size> s_wallTorch{};
        bool s_built = false;

        bool EndsWith(const std::string& s, const char* suffix) {
            const std::string suf(suffix);
            return s.size() >= suf.size() &&
                   s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
        }

        void Build() {
            if (s_built) return;
            s_built = true;
            s_bits.fill(0);
            s_wallTorch.fill(BlockID::Air);

            for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                const BlockID id = static_cast<BlockID>(i);
                const std::string& slug = BlockRegistry::Get(id).registrySlug;
                if (slug.empty()) continue;
                uint16_t bits = 0;

                if (EndsWith(slug, "_button")) {
                    bits |= kButton;
                    if (slug != "stone_button" && slug != "polished_blackstone_button") {
                        bits |= kWoodenButton;
                    }
                }
                if (EndsWith(slug, "_pressure_plate")) {
                    bits |= kPlate;
                    if (slug == "light_weighted_pressure_plate" ||
                        slug == "heavy_weighted_pressure_plate") {
                        bits |= kWeightedPlate;
                    }
                    if (slug == "stone_pressure_plate" ||
                        slug == "polished_blackstone_pressure_plate") {
                        bits |= kPlateMobsOnly;
                    }
                }
                if (slug == "rail" || slug == "powered_rail" ||
                    slug == "detector_rail" || slug == "activator_rail") {
                    bits |= kRail;
                    if (slug != "rail") bits |= kStraightRail;
                }
                if (slug == "repeater" || slug == "comparator") bits |= kDiode;
                // ambrosium_torch: the Aether's TorchBlock copy of Blocks.TORCH.
                if (slug == "torch" || slug == "soul_torch" || slug == "redstone_torch" ||
                    slug == "blue_redstone_torch" || slug == "ambrosium_torch") {
                    bits |= kStandingTorch;
                }
                if (slug == "wall_torch" || slug == "soul_wall_torch" ||
                    slug == "redstone_wall_torch" || slug == "blue_redstone_wall_torch" ||
                    slug == "ambrosium_wall_torch") {
                    bits |= kWallTorch;
                }
                if (slug == "iron_door" || slug == "iron_trapdoor") bits |= kIron;
                s_bits[i] = bits;
            }

            // Pair each standing torch with its wall twin by slug.
            auto find = [](const char* slug) -> BlockID {
                for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                    if (BlockRegistry::Get(static_cast<BlockID>(i)).registrySlug == slug) {
                        return static_cast<BlockID>(i);
                    }
                }
                return BlockID::Air;
            };
            const BlockID torch = find("torch"), soul = find("soul_torch"), red = find("redstone_torch");
            if (torch != BlockID::Air) s_wallTorch[static_cast<size_t>(torch)] = find("wall_torch");
            if (soul  != BlockID::Air) s_wallTorch[static_cast<size_t>(soul)]  = find("soul_wall_torch");
            if (red   != BlockID::Air) s_wallTorch[static_cast<size_t>(red)]   = find("redstone_wall_torch");
            const BlockID blue = find("blue_redstone_torch");
            if (blue  != BlockID::Air) s_wallTorch[static_cast<size_t>(blue)]  = find("blue_redstone_wall_torch");
            const BlockID ambrosium = find("ambrosium_torch");
            if (ambrosium != BlockID::Air) s_wallTorch[static_cast<size_t>(ambrosium)] = find("ambrosium_wall_torch");
        }

        inline bool Has(BlockID id, uint16_t bit) {
            Build();
            const size_t i = static_cast<size_t>(id);
            return i < BlockRegistry::Size && (s_bits[i] & bit) != 0;
        }
    } // namespace

    void InitRedstoneFamilies() { s_built = false; Build(); }

    bool IsButtonBlock(BlockID id)           { return Has(id, kButton); }
    bool IsWoodenButton(BlockID id)          { return Has(id, kWoodenButton); }
    int  ButtonTicksToStayPressed(BlockID id){ return IsWoodenButton(id) ? 30 : 20; }
    bool IsPressurePlateBlock(BlockID id)    { return Has(id, kPlate); }
    bool IsWeightedPressurePlate(BlockID id) { return Has(id, kWeightedPlate); }
    bool PressurePlateMobsOnly(BlockID id)   { return Has(id, kPlateMobsOnly); }
    bool IsRailBlock(BlockID id)             { return Has(id, kRail); }
    bool IsStraightRail(BlockID id)          { return Has(id, kStraightRail); }
    bool IsDiodeBlock(BlockID id)            { return Has(id, kDiode); }
    bool IsRedstoneComponent(BlockID id) {
        switch (id) {
            case BlockID::RedstoneWire:      case BlockID::Repeater:         case BlockID::Comparator:
            case BlockID::RedstoneTorch:     case BlockID::RedstoneWallTorch: case BlockID::RedstoneBlock:
            case BlockID::BlueRedstoneTorch: case BlockID::BlueRedstoneWallTorch: case BlockID::DisplayBlock:
            case BlockID::RedstoneLamp:      case BlockID::Lever:            case BlockID::Observer:
            case BlockID::Piston:            case BlockID::StickyPiston:     case BlockID::PistonHead:
            case BlockID::Hopper:            case BlockID::Dispenser:        case BlockID::Dropper:
            case BlockID::NoteBlock:         case BlockID::PoweredRail:      case BlockID::DetectorRail:
            case BlockID::ActivatorRail:     case BlockID::TripwireHook:     case BlockID::Tripwire:
            case BlockID::DaylightDetector:  case BlockID::Target:           case BlockID::Tnt:
            case BlockID::Lectern:           case BlockID::CopperBulb:       case BlockID::Crafter:
            case BlockID::SculkSensor:       case BlockID::LightningRod:
                return true;
            default:
                break;
        }
        return IsButtonBlock(id) || IsPressurePlateBlock(id);
    }
    bool IsStandingTorch(BlockID id)         { return Has(id, kStandingTorch); }
    bool IsWallTorch(BlockID id)             { return Has(id, kWallTorch); }
    bool IsIronDoorLike(BlockID id)          { return Has(id, kIron); }

    int WeightedPlateMaxWeight(BlockID id) {
        return BlockRegistry::Get(id).registrySlug == "heavy_weighted_pressure_plate" ? 150 : 15;
    }

    BlockID WallTorchOf(BlockID standing) {
        Build();
        const size_t i = static_cast<size_t>(standing);
        return i < BlockRegistry::Size ? s_wallTorch[i] : BlockID::Air;
    }

} // namespace Game
