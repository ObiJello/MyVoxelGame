// File: src/common/world/block/BlockPlacement.cpp
#include "BlockPlacement.hpp"
#include <algorithm>
#include "BlockRegistry.hpp"
#include "RedstoneWire.hpp"
#include "../../core/Log.hpp"

#include <array>
#include <iterator>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        std::array<PlacementRule, BlockRegistry::Size> s_rules{};
        bool s_rulesBuilt = false;

        bool Has(const std::string& n, std::string_view sub) {
            return n.find(sub) != std::string::npos;
        }
        bool Is(const std::string& n, std::string_view exact) { return n == exact; }

        PlacementRule ClassifyPlacement(const std::string& n) {
            // ── Axis pillars — MC RotatedPillarBlock.getStateForPlacement:
            //    setValue(AXIS, context.getClickedFace().getAxis())
            if (Has(n, "_log") || Has(n, "_wood") || Has(n, "_stem") || Has(n, "_hyphae") ||
                Has(n, "_pillar") || Is(n, "bone_block") || Is(n, "hay_block") ||
                Is(n, "basalt") || Is(n, "polished_basalt") || Is(n, "deepslate") ||
                Is(n, "muddy_mangrove_roots") || Has(n, "_froglight")) {
                return PlacementRule::ClickedFaceAxis;
            }

            // ── Six-way, front toward the player — MC DispenserBlock:
            //    setValue(FACING, context.getNearestLookingDirection().getOpposite())
            if (Is(n, "dispenser") || Is(n, "dropper") || Is(n, "barrel") ||
                Is(n, "piston") || Is(n, "sticky_piston") || Is(n, "command_block") ||
                Is(n, "repeating_command_block") || Is(n, "chain_command_block") ||
                Is(n, "crafter")) {
                return PlacementRule::NearestOpposite;
            }

            // ── Observer is the odd one out: ObserverBlock.java:111 literally
            //    reads getNearestLookingDirection().getOpposite().getOpposite(),
            //    a vanilla double negative, so its face points at the block you
            //    clicked rather than back at you. Keep the quirk.
            if (Is(n, "observer")) {
                return PlacementRule::None; // handled explicitly below
            }

            // ── Grows out of the clicked surface — MC ShulkerBoxBlock /
            //    AmethystClusterBlock / LightningRodBlock / EndRodBlock:
            //    setValue(FACING, context.getClickedFace())
            if (Has(n, "shulker_box") || Is(n, "amethyst_cluster") || Has(n, "amethyst_bud") ||
                Is(n, "lightning_rod") || Is(n, "end_rod")) {
                return PlacementRule::ClickedFace;
            }

            // ── Into the clicked surface — MC HopperBlock (which then forces
            //    DOWN for a vertical click; see ComputePlacementState).
            if (Is(n, "hopper")) {
                return PlacementRule::ClickedFaceOpposite;
            }

            // ── Anvil is the only 90° case — MC AnvilBlock.java:50:
            //    setValue(FACING, context.getHorizontalDirection().getClockWise())
            if (Is(n, "anvil") || Is(n, "chipped_anvil") || Is(n, "damaged_anvil")) {
                return PlacementRule::HorizontalClockwise;
            }

            // ── Facing points AWAY from the player (raw horizontal direction) —
            //    MC StairBlock / DoorBlock / FenceGateBlock / BedBlock /
            //    CampfireBlock / DecoratedPotBlock.
            if (Has(n, "_stairs") || Is(n, "campfire") || Is(n, "soul_campfire") ||
                Is(n, "decorated_pot") || Is(n, "calibrated_sculk_sensor") ||
                Is(n, "grindstone")) {
                return PlacementRule::Horizontal;
            }

            // ── Front looks BACK at the player — MC AbstractFurnaceBlock:47,
            //    ChestBlock:147, and the rest of the container/machine family.
            if (Has(n, "_glazed_terracotta") ||
                Is(n, "furnace") || Is(n, "blast_furnace") || Is(n, "smoker") ||
                Is(n, "chest") || Is(n, "trapped_chest") || Is(n, "ender_chest") ||
                Is(n, "carved_pumpkin") || Is(n, "jack_o_lantern") ||
                Is(n, "loom") || Is(n, "stonecutter") || Is(n, "lectern") ||
                Is(n, "chiseled_bookshelf") || Is(n, "beehive") || Is(n, "bee_nest") ||
                Is(n, "end_portal_frame") || Is(n, "vault") ||
                Is(n, "big_dripleaf") || Is(n, "small_dripleaf") ||
                Is(n, "repeater") || Is(n, "comparator") ||
                // Segmented ground cover — MC SegmentableBlock.getStateForPlacement
                // ends in setValue(facing, context.getHorizontalDirection()
                // .getOpposite()), the same rule as the container family.
                // Substring match because segment count is part of the model
                // name here (leaf_litter_1 … leaf_litter_4).
                Has(n, "leaf_litter") || Has(n, "wildflowers") || Has(n, "pink_petals")) {
                return PlacementRule::HorizontalOpposite;
            }

            // ── Wall-attached blocks (ladder, wall torches, signs) use MC's
            //    "walk the look-ordered directions and take the first that can
            //    survive" loop, which needs neighbour queries we don't do here.
            //    Ladder gets the clicked-face rule as a close approximation —
            //    clicking a wall gives the wall you clicked, which is what the
            //    loop lands on in the overwhelmingly common case.
            if (Is(n, "ladder")) {
                return PlacementRule::ClickedFaceOpposite;
            }

            return PlacementRule::None;
        }

        void BuildRules() {
            if (s_rulesBuilt) return;
            for (size_t i = 0; i < BlockRegistry::Size; ++i) {
                const Block& b = BlockRegistry::blockDefinitions[i];
                if (b.modelName.empty() && b.name.empty()) continue;
                const std::string& name = !b.modelName.empty() ? b.modelName : b.name;
                s_rules[i] = ClassifyPlacement(name);
            }
            s_rulesBuilt = true;
        }

    } // namespace

    PlacementRule GetPlacementRule(BlockID id) {
        BuildRules();
        const size_t idx = static_cast<size_t>(id);
        return idx < BlockRegistry::Size ? s_rules[idx] : PlacementRule::None;
    }

    uint8_t ComputePlacementState(BlockID id, const UseOnContext& context) {
        const auto& def = BlockRegistry::GetStateDefinition(id);
        if (def.properties.empty()) return 0;

        const PlacementRule rule = GetPlacementRule(id);
        const Direction look    = context.getHorizontalDirection();
        const Direction clicked = context.getClickedFace();

        // ── Chests: MC ChestBlock.getStateForPlacement, verbatim ──────────
        // Sets FACING and TYPE together. TYPE is the whole point: it records
        // WHICH neighbour this chest paired with at the moment it was placed,
        // which geometry alone can never recover — with a lone chest on either
        // side, both are equally valid partners and only the click says which.
        if ((id == BlockID::Chest || id == BlockID::TrappedChest) && context.world) {
            const glm::ivec3 clicked = context.hitResult.blockPos;
            // face: 0 bottom, 1 top, 2 north, 3 south, 4 west, 5 east.
            static const glm::ivec3 kFaceOffset[6] = {
                {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0},
            };
            const int face = std::clamp(context.hitResult.face, 0, 5);
            const glm::ivec3 placePos = clicked + kFaceOffset[face];

            // MC candidatePartnerFacing: the neighbour must be this same chest
            // AND still SINGLE — a chest already in a pair cannot take a third.
            auto candidatePartnerFacing =
                [&](const glm::ivec3& at) -> std::string_view {
                    if (context.world->GetBlock(at.x, at.y, at.z) != id) return {};
                    const uint8_t st = context.world->GetBlockState(at.x, at.y, at.z);
                    if (def.ValueOf(st, "type") != "single") return {};
                    return def.ValueOf(st, "facing");
                };
            auto axisOf = [](std::string_view f) {
                return (f == "north" || f == "south") ? 2 : 0;   // 2 = Z, 0 = X
            };
            auto clockWise = [](std::string_view f) -> std::string_view {
                if (f == "north") return "east";  if (f == "east")  return "south";
                if (f == "south") return "west";  return "north";
            };
            auto counterClockWise = [](std::string_view f) -> std::string_view {
                if (f == "north") return "west";  if (f == "west")  return "south";
                if (f == "south") return "east";  return "north";
            };
            auto offsetOf = [](std::string_view f) -> glm::ivec3 {
                if (f == "north") return {0,0,-1};  if (f == "south") return {0,0,1};
                if (f == "west")  return {-1,0,0};  return {1,0,0};
            };

            // Default: face the player (HorizontalOpposite, MC's
            // getHorizontalDirection().getOpposite()).
            std::string_view facing = NameOf(Opposite(look));
            std::string_view type   = "single";

            // 1. Clicked the SIDE of a chest -> adopt its facing and pair with
            //    THAT chest (ChestBlock.java:150-156). This is the branch that
            //    makes "place against the one I clicked" work.
            const int clickedAxis = (face <= 1) ? 1 : (face <= 3 ? 2 : 0);
            if (clickedAxis != 1) {
                const std::string_view nf = candidatePartnerFacing(clicked);
                if (!nf.empty() && axisOf(nf) != clickedAxis) {
                    facing = nf;
                    // MC: type = neighbourFacing.getCounterClockWise() ==
                    //            clickedFace.getOpposite() ? RIGHT : LEFT
                    // clickedFace.getOpposite() is the step from the placement
                    // cell back to the clicked chest.
                    const glm::ivec3 backToClicked = clicked - placePos;
                    type = (offsetOf(counterClockWise(nf)) == backToClicked) ? "right" : "left";
                }
            }

            // 2. Otherwise fall back to MC's getChestType scan, which joins a
            //    lone chest sitting to either side of the placement cell.
            if (type == "single") {
                if (facing == candidatePartnerFacing(placePos + offsetOf(clockWise(facing)))) {
                    type = "left";
                } else if (facing == candidatePartnerFacing(
                               placePos + offsetOf(counterClockWise(facing)))) {
                    type = "right";
                }
            }

            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["facing"] = std::string(facing);
            props["type"]   = std::string(type);
            return def.IndexOf(props);
        }

        // ── Buttons and levers ────────────────────────────────────────────
        // MC FaceAttachedHorizontalDirectionalBlock.getStateForPlacement walks
        // context.getNearestLookingDirections() and takes the first direction
        // whose resulting state can survive. That list is ordered by how
        // closely each direction matches the look vector, EXCEPT that
        // BlockPlaceContext moves `clickedFace.getOpposite()` to the front — so
        // the first candidate, and in practice always the winning one, is the
        // face you actually clicked.
        //
        // Reduced to that first candidate:
        //   clicked UP    -> opposite DOWN, vertical -> FACE = floor
        //   clicked DOWN  -> opposite UP,   vertical -> FACE = ceiling
        //   clicked side  -> FACE = wall, FACING = direction.getOpposite(),
        //                    and direction is already the clicked face's
        //                    opposite, so FACING is the clicked face itself —
        //                    the button points out of the wall it is on.
        // Vertical placements take FACING from the player, which is what makes
        // a floor button line up with the way you were standing.
        {
            const std::string& name = BlockRegistry::Get(id).modelName;
            if (name.find("_button") != std::string::npos || name == "lever") {
                BlockRegistry::BlockStateDefinition::PropertyMap props;
                if (clicked == Direction::Up) {
                    props["face"]   = "floor";
                    props["facing"] = std::string(NameOf(look));
                } else if (clicked == Direction::Down) {
                    props["face"]   = "ceiling";
                    props["facing"] = std::string(NameOf(look));
                } else {
                    props["face"]   = "wall";
                    props["facing"] = std::string(NameOf(clicked));
                }
                props["powered"] = "false";
                return def.IndexOf(props);
            }
        }

        switch (rule) {
            case PlacementRule::HorizontalOpposite:
                return def.IndexOfSingle("facing", NameOf(Opposite(look)));

            case PlacementRule::Horizontal:
                return def.IndexOfSingle("facing", NameOf(look));

            case PlacementRule::HorizontalClockwise:
                return def.IndexOfSingle("facing", NameOf(ClockWise(look)));

            case PlacementRule::NearestOpposite:
                return def.IndexOfSingle(
                    "facing", NameOf(Opposite(context.getNearestLookingDirection())));

            case PlacementRule::ClickedFace:
                return def.IndexOfSingle("facing", NameOf(clicked));

            case PlacementRule::ClickedFaceOpposite: {
                Direction d = Opposite(clicked);
                // MC HopperBlock.java:76-77: a hopper placed against a floor or
                // ceiling points DOWN rather than up/down-along-the-click.
                const Block& b = BlockRegistry::Get(id);
                if (b.modelName == "hopper" && !IsHorizontal(d)) d = Direction::Down;
                return def.IndexOfSingle("facing", NameOf(d));
            }

            case PlacementRule::ClickedFaceAxis:
                return def.IndexOfSingle("axis", NameOf(AxisOf(clicked)));

            case PlacementRule::None:
            default:
                break;
        }

        // Observer's vanilla double-getOpposite (ObserverBlock.java:111) leaves
        // its face pointing at whatever you clicked, unlike every other 6-way
        // block. Handled here rather than as its own rule so the rule set stays
        // a description of MC's categories instead of a list of exceptions.
        const Block& b = BlockRegistry::Get(id);
        if (b.modelName == "observer") {
            return def.IndexOfSingle("facing", NameOf(context.getNearestLookingDirection()));
        }

        return 0;
    }

    namespace {
        // One row per SegmentableBlock family, ordered 1 → 4 segments. Same
        // shape as BlockRegistry's kSlabPairs: an explicit table beats name
        // matching here because the ORDER is the data.
        constexpr BlockID kSegmentedChains[][4] = {
            { BlockID::LeafLitter,  BlockID::LeafLitter2,  BlockID::LeafLitter3,  BlockID::LeafLitter4  },
            { BlockID::Wildflowers, BlockID::Wildflowers2, BlockID::Wildflowers3, BlockID::Wildflowers4 },
            { BlockID::PinkPetals,  BlockID::PinkPetals2,  BlockID::PinkPetals3,  BlockID::PinkPetals4  },
        };

        // Index of `id` within its chain, or -1. Linear over 12 entries, and
        // only on a right-click, so no table needed.
        bool FindSegment(BlockID id, size_t& outChain, size_t& outStep) {
            for (size_t c = 0; c < std::size(kSegmentedChains); ++c) {
                for (size_t s = 0; s < 4; ++s) {
                    if (kSegmentedChains[c][s] == id) { outChain = c; outStep = s; return true; }
                }
            }
            return false;
        }
    } // namespace

    BlockID SegmentedFamilyBase(BlockID id) {
        size_t chain = 0, step = 0;
        return FindSegment(id, chain, step) ? kSegmentedChains[chain][0] : BlockID::Air;
    }

    BlockID SegmentedGrowth(BlockID id) {
        size_t chain = 0, step = 0;
        if (!FindSegment(id, chain, step)) return BlockID::Air;
        if (step + 1 >= 4) return BlockID::Air;      // MC: min(4, n + 1) — already full
        return kSegmentedChains[chain][step + 1];
    }

    bool CanBeReplacedByPlacement(BlockID existing, uint8_t /*existingState*/,
                                  BlockID held, bool secondaryUse) {
        if (existing == BlockID::Air) return true;

        // MC SegmentableBlock.canBeReplaced:
        //   !isSecondaryUseActive() && itemInHand.is(block.asItem()) && n < 4
        // Growth is the ONLY way a segmented clump is replaceable by its own
        // item; at 4 it falls through to the base rule below, which refuses.
        const BlockID base = SegmentedFamilyBase(existing);
        if (base != BlockID::Air && base == SegmentedFamilyBase(held)) {
            return !secondaryUse && SegmentedGrowth(existing) != BlockID::Air;
        }

        // MC BlockBehaviour.canBeReplaced:
        //   state.canBeReplaced() && (itemInHand.isEmpty() || !itemInHand.is(asItem()))
        //
        // The second clause is what stops a replaceable block being overwritten
        // by MORE OF ITSELF — placing tall grass into tall grass does nothing
        // rather than silently consuming the item.
        if (!BlockRegistry::Get(existing).replaceable) return false;
        return held == BlockID::Air || held != existing;
    }

    namespace {
        // MC Block.isFaceFull(getBlockSupportShape(), UP), approximated from the
        // single collision AABB this engine keeps per state: a block supports
        // what sits on it when its collision box reaches the cell's top and
        // covers the whole square. True for full cubes and top slabs, false for
        // bottom slabs, and false for everything `.noCollision()` — including
        // leaf litter, which is what stops a clump from being stacked on itself.
        bool IsFaceSturdyUp(BlockID id, uint8_t state) {
            if (!BlockRegistry::HasCollision(id)) return false;
            const auto& s = BlockRegistry::GetBlockShape(id, state);
            return s.max.y >= 0.9999f &&
                   s.min.x <= 0.0001f && s.max.x >= 0.9999f &&
                   s.min.z <= 0.0001f && s.max.z >= 0.9999f;
        }

        // #minecraft:dirt, verbatim from data/minecraft/tags/block/dirt.json,
        // matched against model names (which are the MC block names).
        constexpr std::string_view kDirtTag[] = {
            "dirt", "grass_block", "podzol", "coarse_dirt", "mycelium",
            "rooted_dirt", "moss_block", "pale_moss_block", "mud",
            "muddy_mangrove_roots",
            // Not a tag entry: vanilla's snowy grass is `grass_block{snowy=true}`,
            // the same block, but this engine promotes it to its own BlockID
            // with its own model name. Omitting it would make snow-covered
            // grass the one dirt that refuses flowers.
            "grass_block_snow",
        };
    } // namespace

    bool CanSurviveOn(BlockID id, BlockID belowId, uint8_t belowState) {
        const std::string& name = BlockRegistry::Get(id).modelName;

        // MC LeafLitterBlock.canSurvive — any block with a sturdy top face,
        // not just dirt. This is the override that makes leaf litter placeable
        // on stone or planks while flowers are not.
        if (Has(name, "leaf_litter")) {
            return IsFaceSturdyUp(belowId, belowState);
        }

        // MC FlowerBedBlock (wildflowers, pink_petals) inherits
        // VegetationBlock.canSurvive → mayPlaceOn(below), which is
        // `state.is(BlockTags.DIRT) || state.is(Blocks.FARMLAND)`.
        if (Has(name, "wildflowers") || Has(name, "pink_petals")) {
            const std::string& belowName = BlockRegistry::Get(belowId).modelName;
            if (belowName == "farmland") return true;
            for (std::string_view d : kDirtTag) if (belowName == d) return true;
            return false;
        }

        // ── Farming (MC VegetationBlock.canSurvive → mayPlaceOn) ────────────
        //
        // Deliberately NOT gated on light, even though CropBlock.canSurvive is
        // `hasSufficientLight(level, pos) && super.canSurvive(...)`. This
        // overload has no world to ask, and the light stand-in is a column walk
        // that would need one; the growth rule already refuses to advance a
        // crop in the dark, so what a player sees — plantable but frozen — is
        // the same outcome by a slightly different route.

        // CropBlock.mayPlaceOn / StemBlock.mayPlaceOn / PitcherCropBlock:
        // `state.is(Blocks.FARMLAND)`, and nothing else. This is the rule that
        // stops seeds being planted on plain grass.
        if (name == "wheat" || name == "carrots" || name == "potatoes" ||
            name == "beetroots" || name == "torchflower_crop" ||
            name == "pitcher_crop" ||
            name == "melon_stem" || name == "pumpkin_stem" ||
            name == "attached_melon_stem" || name == "attached_pumpkin_stem") {
            return belowId == BlockID::Farmland;
        }

        // MC RedStoneWireBlock.canSurvive / TripWireBlock.canSurvive: both want
        // a sturdy top face underneath (vanilla also allows a hopper for
        // redstone, which this engine's shape test already accepts). Without
        // this, dust and string could be placed in mid-air.
        if (name == "redstone_wire" || name == "tripwire_ns") {
            return IsFaceSturdyUp(belowId, belowState);
        }

        // NetherWartBlock.mayPlaceOn: `state.is(Blocks.SOUL_SAND)`.
        if (name == "nether_wart") {
            return belowId == BlockID::SoulSand;
        }

        // SweetBerryBushBlock inherits VegetationBlock.mayPlaceOn:
        // `state.is(BlockTags.DIRT) || state.is(Blocks.FARMLAND)`.
        if (name == "sweet_berry_bush") {
            const std::string& belowName = BlockRegistry::Get(belowId).modelName;
            if (belowName == "farmland") return true;
            for (std::string_view d : kDirtTag) if (belowName == d) return true;
            return false;
        }

        // No modelled rule — placement is unconstrained, as it was before.
        return true;
    }

    namespace {
        // data/minecraft/tags/block/sand.json
        constexpr std::string_view kSandTag[] = { "sand", "red_sand", "suspicious_sand" };

        bool IsInDirtTag(BlockID id) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            for (std::string_view d : kDirtTag) if (n == d) return true;
            return false;
        }
        bool IsInSandTag(BlockID id) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            for (std::string_view s : kSandTag) if (n == s) return true;
            return false;
        }
    } // namespace

    namespace {
        // MC Block.isFaceSturdy(state, level, pos, direction) for an arbitrary
        // face, approximated from the single collision AABB this engine keeps
        // per state: the box must cover the whole square of that face and reach
        // it. True for full cubes, for the top of a top slab, and false for
        // anything `.noCollision()` — which is what stops a button being hung
        // on another button.
        bool IsFaceSturdy(const IBlockAccess& level, const glm::ivec3& p, Direction face) {
            const BlockID id = level.GetBlock(p.x, p.y, p.z);
            if (id == BlockID::Air) return false;
            if (!BlockRegistry::HasCollision(id)) return false;
            const auto& s = BlockRegistry::GetBlockShape(id, level.GetBlockState(p.x, p.y, p.z));
            constexpr float lo = 0.0001f, hi = 0.9999f;
            switch (face) {
                case Direction::Up:    return s.max.y >= hi && s.min.x <= lo && s.max.x >= hi &&
                                              s.min.z <= lo && s.max.z >= hi;
                case Direction::Down:  return s.min.y <= lo && s.min.x <= lo && s.max.x >= hi &&
                                              s.min.z <= lo && s.max.z >= hi;
                case Direction::North: return s.min.z <= lo && s.min.x <= lo && s.max.x >= hi &&
                                              s.min.y <= lo && s.max.y >= hi;
                case Direction::South: return s.max.z >= hi && s.min.x <= lo && s.max.x >= hi &&
                                              s.min.y <= lo && s.max.y >= hi;
                case Direction::West:  return s.min.x <= lo && s.min.z <= lo && s.max.z >= hi &&
                                              s.min.y <= lo && s.max.y >= hi;
                case Direction::East:  return s.max.x >= hi && s.min.z <= lo && s.max.z >= hi &&
                                              s.min.y <= lo && s.max.y >= hi;
            }
            return false;
        }

        bool IsFaceAttachedBlock(BlockID id) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            return n.find("_button") != std::string::npos || n == "lever";
        }
    } // namespace

    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockID id,
                      uint8_t stateIndex) {
        // MC FaceAttachedHorizontalDirectionalBlock.canSurvive:
        //   canAttach(level, pos, getConnectedDirection(state).getOpposite())
        // where getConnectedDirection is UP for FLOOR, DOWN for CEILING, and
        // FACING for WALL — the direction the block POINTS. Its opposite is the
        // direction of the surface holding it up.
        if (IsFaceAttachedBlock(id)) {
            const auto& def = BlockRegistry::GetStateDefinition(id);
            const std::string_view face   = def.ValueOf(stateIndex, "face");
            const std::string_view facing = def.ValueOf(stateIndex, "facing");

            Direction connected = Direction::North;
            if (face == "floor")        connected = Direction::Up;
            else if (face == "ceiling") connected = Direction::Down;
            else {
                if      (facing == "east")  connected = Direction::East;
                else if (facing == "south") connected = Direction::South;
                else if (facing == "west")  connected = Direction::West;
                else                        connected = Direction::North;
            }

            const Direction toSupport = Opposite(connected);
            const glm::ivec3 support{pos.x + StepX(toSupport),
                                     pos.y + StepY(toSupport),
                                     pos.z + StepZ(toSupport)};
            // The support's face that we are stuck to points back at us.
            return IsFaceSturdy(level, support, Opposite(toSupport));
        }
        return CanSurviveAt(level, pos, id);
    }

    uint8_t ComputeWorldPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockID id, uint8_t fallback) {
        if (id == BlockID::RedstoneWire) {
            // MC RedStoneWireBlock.getStateForPlacement:
            //   getConnectionState(level, this.crossState, pos)
            return RedstonePlacementState(level, pos);
        }
        return fallback;
    }

    bool HasModelledSurvivalRule(BlockID id) {
        // Every family the two functions above actually branch on. Kept as one
        // list so adding a rule and advertising it is a single edit.
        static constexpr std::string_view kModelled[] = {
            // CanSurviveOn
            "leaf_litter", "wildflowers", "pink_petals",
            "wheat", "carrots", "potatoes", "beetroots", "torchflower_crop",
            "pitcher_crop", "melon_stem", "pumpkin_stem",
            "attached_melon_stem", "attached_pumpkin_stem",
            "nether_wart", "sweet_berry_bush",
            // tripwire is a model name, not a slug — it is re-registered onto
            // its no-connection sub-model (see BlockRegistry's multipart note).
            "redstone_wire", "tripwire_ns",
            // CanSurviveAt (world-aware)
            "sugar_cane", "cactus", "bamboo", "bamboo_sapling",
        };
        const std::string& name = BlockRegistry::Get(id).modelName;
        for (std::string_view m : kModelled) if (name == m) return true;
        return false;
    }

    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockID id) {
        const std::string& name = BlockRegistry::Get(id).modelName;
        const glm::ivec3 below{pos.x, pos.y - 1, pos.z};
        const BlockID belowId = level.GetBlock(below.x, below.y, below.z);

        // SugarCaneBlock.canSurvive (SugarCaneBlock.java:44-62): stacking on
        // itself always works; otherwise dirt or sand with water (or frosted
        // ice, which we don't have) orthogonally adjacent to the block BELOW —
        // not to the cane itself. Getting that wrong by one block is the
        // classic reimplementation bug, so note it: the water sits beside the
        // ground, level with it.
        if (name == "sugar_cane") {
            if (belowId == BlockID::SugarCane) return true;
            if (!IsInDirtTag(belowId) && !IsInSandTag(belowId)) return false;
            for (Direction d : {Direction::North, Direction::East,
                                Direction::South, Direction::West}) {
                if (level.GetBlock(below.x + StepX(d), below.y, below.z + StepZ(d))
                    == BlockID::Water) {
                    return true;
                }
            }
            return false;
        }

        // CactusBlock.canSurvive (CactusBlock.java:60-70): no solid block on any
        // horizontal side, sand or cactus below, and nothing liquid above.
        if (name == "cactus") {
            for (Direction d : {Direction::North, Direction::East,
                                Direction::South, Direction::West}) {
                const BlockID n = level.GetBlock(pos.x + StepX(d), pos.y, pos.z + StepZ(d));
                if (n != BlockID::Air && BlockRegistry::Get(n).opaque) return false;
                if (n == BlockID::Lava) return false;
            }
            if (belowId != BlockID::Cactus && !IsInSandTag(belowId)) return false;
            const BlockID above = level.GetBlock(pos.x, pos.y + 1, pos.z);
            return above != BlockID::Water && above != BlockID::Lava;
        }

        // BambooStalkBlock / BambooSaplingBlock.canSurvive: the
        // #bamboo_plantable_on tag (dirt + sand + bamboo's own two blocks).
        if (name == "bamboo" || name == "bamboo_sapling") {
            return IsInDirtTag(belowId) || IsInSandTag(belowId) ||
                   belowId == BlockID::Bamboo || belowId == BlockID::BambooSapling;
        }

        // CocoaBlock.canSurvive is about the block it FACES, not the one below,
        // and the facing is not known until placement resolves. Left to the
        // generic path for now — cocoa places like any other block.

        const uint8_t belowState = level.GetBlockState(below.x, below.y, below.z);
        return CanSurviveOn(id, belowId, belowState);
    }

} // namespace Game
