// File: src/common/world/block/BlockPlacement.cpp
#include "BlockPlacement.hpp"
#include "FallingBlock.hpp"
#include <algorithm>
#include "BlockRegistry.hpp"
#include "RedstoneWire.hpp"
#include "RedstoneComponents.hpp"
#include "RedstoneFamilies.hpp"
#include "RedstoneSignal.hpp"
#include "RedstoneStateUtil.hpp"
#include "Rails.hpp"
#include "Stairs.hpp"
#include "PotentSulfurBlock.hpp"
#include "CrossCollision.hpp"
#include "Walls.hpp"
#include "Vine.hpp"
#include "MultifaceBlock.hpp"
#include "FenceGate.hpp"
#include "../../core/Log.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
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

        // MC AmethystClusterBlock: the cluster, its three bud sizes, and The
        // Hush's resonant_cluster (registered on the same class). Keyed on
        // model name so every rule below reads the same family.
        bool IsAmethystClusterLike(const std::string& n) {
            return Is(n, "amethyst_cluster") || Has(n, "amethyst_bud") ||
                   Is(n, "resonant_cluster");
        }

        // MC SaplingBlock (VegetationBlock): every "*_sapling" that is a plant
        // in the ground — not the potted ones (FlowerPotBlock) and not
        // bamboo_sapling (BambooSaplingBlock, its own #bamboo_plantable_on rule).
        bool IsSaplingName(const std::string& n) {
            if (n.rfind("potted_", 0) == 0) return false;
            if (n == "bamboo_sapling") return false;
            constexpr std::string_view kSuffix = "_sapling";
            return n.size() > kSuffix.size() &&
                   n.compare(n.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
        }

        PlacementRule ClassifyPlacement(const std::string& n) {
            // The Aether's berry_bush_stem is an AetherBushBlock with no AXIS;
            // the "_stem" pillar match below is for crimson/warped stems.
            if (Is(n, "berry_bush_stem")) return PlacementRule::None;
            // The Aether's boss/treasure doorway stones are plain cubes whose
            // names contain "_door" (…_doorway_…): not doors.
            if (Has(n, "_doorway_")) return PlacementRule::None;

            // ── Axis pillars — MC RotatedPillarBlock.getStateForPlacement:
            //    setValue(AXIS, context.getClickedFace().getAxis())
            if (Has(n, "_log") || Has(n, "_wood") || Has(n, "_stem") || Has(n, "_hyphae") ||
                Has(n, "_pillar") || Is(n, "bone_block") || Is(n, "hay_block") ||
                Is(n, "basalt") || Is(n, "polished_basalt") || Is(n, "deepslate") ||
                Is(n, "muddy_mangrove_roots") || Has(n, "_froglight") ||
                Is(n, "pillar") ||   // the Aether's dungeon pillar (RotatedPillarBlock)
                Has(n, "_thorns")) { // TF ThornsBlock: AXIS = the clicked face's axis
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
            //    Twilight Forest's CritterBlock (firefly, cicada, moonworm) is
            //    the same rule: setValue(FACING, context.getClickedFace()).
            if (Has(n, "shulker_box") || IsAmethystClusterLike(n) ||
                Is(n, "lightning_rod") || Is(n, "end_rod") ||
                Is(n, "firefly") || Is(n, "cicada") || Is(n, "moonworm") ||
                Is(n, "pillar_top")) {
                // pillar_top: the Aether's FacingPillarBlock, same rule.
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
            //
            //    Stairs are listed here because FACING really is this rule, but
            //    they also carry HALF and SHAPE, so ComputePlacementState
            //    intercepts them before the switch — see the stairs branch
            //    there. The classification stays because it is the truth about
            //    FACING and keeps this table a description of MC's categories.
            //    Beds too (AbstractBedBlock.getStateForPlacement: FACING =
            //    context.getHorizontalDirection(), the head going in that
            //    direction — the pair itself is placed by the caller, see
            //    BedBlock.hpp).
            if (Has(n, "_stairs") || Is(n, "campfire") || Is(n, "soul_campfire") ||
                Has(n, "_fence_gate") || Has(n, "_door") || Has(n, "_bed") ||
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

            // (Ladders, wall torches and signs — MC's "walk the look-ordered
            // directions and take the first that can survive" — are handled
            // by name in ComputePlacementState, with the world when it has one.)
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

    // The body still computes a within-block INDEX, because it is written
    // against BlockStateDefinition's string-keyed IndexOfSingle/IndexOf. That is
    // an internal detail of this one function now — the public entry point below
    // hands back a BlockState, so no caller sees the pair. Converting the
    // string lookups themselves is a separate cleanup.
    static BlockStateIndex ComputePlacementIndex(BlockID id, const UseOnContext& context) {
        const auto& def = BlockRegistry::GetStateDefinition(id);

        // MC's `getStateForPlacement` starts from `defaultBlockState()` and
        // overrides only what the click decides. This used to `return 0` for
        // anything with no rule, which was the same thing while index 0 WAS
        // the default — it no longer is for 627 blocks. State 0 is
        // `StateDefinition.any()`: the first value of every property, and
        // BooleanProperty lists `true` first. Returning it would place every
        // slab as a waterlogged TOP slab, every door open and powered, every
        // furnace lit, every lantern hanging.
        const BlockStateIndex kDefault = def.defaultIndex;
        if (def.properties.empty()) return kDefault;

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
                    const BlockState st = context.world->GetBlockState(at.x, at.y, at.z);
                    if (st.GetValueByName("type") != "single") return {};
                    return st.GetValueByName("facing");
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
        // Signs. The item's standing↔wall choice is already made by
        // SignPlacementBlock (the caller); this is each block's
        // getStateForPlacement.
        if (IsSignBlock(id)) {
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["waterlogged"] = "false";
            if (IsStandingSignBlock(id)) {
                // StandingSignBlock: RotationSegment.convertToSegment(rotation + 180)
                // — round(deg * 16/360) & 15, so the front faces the placer.
                const int segment = static_cast<int>(std::floor(
                    (context.playerYaw + 180.0f) * (16.0f / 360.0f) + 0.5f)) & 15;
                props["rotation"] = std::to_string(segment);
            } else if (IsWallSignBlock(id)) {
                // WallSignBlock: the first looking direction that can hang —
                // with a side click that is the face clicked (the sign faces
                // out of the wall).
                props["facing"] = std::string(NameOf(IsHorizontal(clicked) ? clicked : Opposite(look)));
            } else if (IsCeilingHangingSignBlock(id)) {
                // CeilingHangingSignBlock: hangs from the block's centre
                // (ATTACHED, free 16-way rotation) when the block above has
                // no full bottom face or the placer sneaks; otherwise on
                // the bar, squared to the cardinal the placer faces.
                bool attached = context.player && context.player->IsSneaking();
                if (!attached && context.world) {
                    const glm::ivec3 placePos = context.getPlacementPos();
                    const glm::ivec3 above(placePos.x, placePos.y + 1, placePos.z);
                    const BlockState aboveState = context.world->GetBlockState(above.x, above.y, above.z);
                    const bool fullBottom = BlockRegistry::HasCollision(aboveState.Block()) &&
                                            BlockRegistry::GetBlockCollisionShapeSet(aboveState).IsFullCube();
                    attached = !fullBottom;
                }
                props["attached"] = attached ? "true" : "false";
                if (attached) {
                    const int segment = static_cast<int>(std::floor(
                        (context.playerYaw + 180.0f) * (16.0f / 360.0f) + 0.5f)) & 15;
                    props["rotation"] = std::to_string(segment);
                } else {
                    // convertToSegment(direction.getOpposite()) with direction
                    // = Direction.fromYRot(yaw): S=0, W=4, N=8, E=12 in the
                    // 2D-data order, so the opposite of the facing.
                    const Direction facing = Opposite(look);
                    int segment = 0;
                    switch (facing) {
                        case Direction::South: segment = 0;  break;
                        case Direction::West:  segment = 4;  break;
                        case Direction::North: segment = 8;  break;
                        default:               segment = 12; break;   // East
                    }
                    props["rotation"] = std::to_string(segment);
                }
            } else {
                // WallHangingSignBlock: the first horizontal looking
                // direction not on the clicked face's axis, facing back.
                Direction facing = Opposite(look);
                if (IsHorizontal(clicked) && AxisOf(clicked) == AxisOf(facing)) {
                    facing = ClockWise(facing);
                }
                props["facing"] = std::string(NameOf(facing));
            }
            return def.IndexOf(props);
        }

        // MC TrapDoorBlock.getStateForPlacement:
        //   clicked a SIDE of a block (and not replacing it) → the trapdoor
        //   hangs on that face, top/bottom half by where on the face you hit;
        //   clicked top/bottom (or replacing) → faces away from you, bottom
        //   half when you clicked the top face, top half otherwise.
        if (IsTrapdoorBlock(id)) {
            const glm::ivec3 clickedPos = context.hitResult.blockPos;
            // BlockPlaceContext.replacingClickedOnBlock: the clicked block
            // itself is replaceable, so the trapdoor goes INTO it.
            bool replacingClicked = false;
            if (context.world) {
                const BlockID at = context.world->GetBlock(clickedPos.x, clickedPos.y, clickedPos.z);
                replacingClicked = at == BlockID::Air || BlockRegistry::Get(at).replaceable;
            }
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            if (!replacingClicked && IsHorizontal(clicked)) {
                props["facing"] = std::string(NameOf(clicked));
                const double hitY = context.hitResult.hitPoint.y - static_cast<double>(clickedPos.y);
                props["half"] = hitY > 0.5 ? "top" : "bottom";
            } else {
                props["facing"] = std::string(NameOf(Opposite(look)));
                props["half"]   = (clicked == Direction::Up) ? "bottom" : "top";
            }
            // MC: `if (level.hasNeighborSignal(pos)) state = OPEN true, POWERED true`.
            bool powered = false;
            if (context.world) {
                const glm::ivec3 placePos = context.getPlacementPos();
                powered = HasNeighborSignal(*context.world, placePos);
            }
            props["open"]    = powered ? "true" : "false";
            props["powered"] = powered ? "true" : "false";
            return def.IndexOf(props);
        }

        if (id == BlockID::Ladder) {
            // MC LadderBlock.getStateForPlacement: for each of the player's
            // nearest looking directions that is horizontal, FACING = its
            // opposite, and the first state that can survive (a sturdy face
            // on the block behind it) wins. Clicking a wall's face means
            // looking at it, so that face leads; then the look direction's
            // opposite (clicking a floor or ceiling), then the rest. Without
            // a world (the client's predictor) the first candidate stands and
            // ComputeWorldPlacementState re-resolves it on both sides.
            const Direction first = IsHorizontal(clicked) ? clicked : Opposite(look);
            Direction facing = first;
            if (context.world) {
                const glm::ivec3 placePos = context.getPlacementPos();
                const Direction order[] = { first, Opposite(look), ClockWise(look), CounterClockWise(look), look };
                for (Direction d : order) {
                    if (LadderCanSurvive(*context.world, placePos, d)) { facing = d; break; }
                }
            }
            return def.IndexOfSingle("facing", NameOf(facing));
        }

        if (IsWallTorch(id)) {
            // MC WallTorchBlock.getStateForPlacement: the first horizontal
            // look direction whose wall can hold it; the clicked face comes
            // first in that walk, so it is what lands here.
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["facing"] = std::string(NameOf(IsHorizontal(clicked) ? clicked : Opposite(look)));
            if (id == BlockID::RedstoneWallTorch || id == BlockID::BlueRedstoneWallTorch) props["lit"] = "true";
            return def.IndexOf(props);
        }

        if (id == BlockID::RedstoneLamp && context.world) {
            // MC RedstoneLampBlock.getStateForPlacement: LIT = hasNeighborSignal.
            BlockRegistry::BlockStateDefinition::PropertyMap props;
            props["lit"] = HasNeighborSignal(*context.world, context.getPlacementPos()) ? "true" : "false";
            return def.IndexOf(props);
        }

        if (IsRailBlock(id)) {
            // MC BaseRailBlock.getStateForPlacement: along the player's
            // horizontal facing; the neighbour-aware shaping happens in
            // onPlace once the block is in the world.
            return RailPlacementState(BlockStates::Default(id), Opposite(look)).Index();
        }

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

        // ── Pointed dripstone ─────────────────────────────────────────────
        // MC PointedDripstoneBlock.getStateForPlacement's `defaultTipDirection
        // = getNearestLookingVerticalDirection().getOpposite()` — look DOWN at
        // a floor and you get a stalagmite (tip UP). Only the DEFAULT is
        // recorded here; whether that orientation actually has support, and
        // the THICKNESS it takes in the column, both need the world and are
        // resolved in ComputeWorldPlacementState. Same hint pattern as the
        // multiface/vine pair below.
        if (id == BlockID::PointedDripstone) {
            const Direction tip =
                Opposite(context.getNearestLookingVerticalDirection());
            return def.IndexOfSingle("tip_direction", NameOf(tip));
        }

        // ── Multiface (glow lichen, sculk vein, resin clump) ──────────────
        // Same shape of rule as the vine below, different class: six faces
        // instead of five, and every one it wears must hold.
        if (IsMultifaceBlock(id)) {
            const Direction support = Opposite(clicked);
            return MultifaceStateWithFace(BlockStates::Default(id), support, true).Index();
        }

        // ── Vines ─────────────────────────────────────────────────────────
        // Record the face the click implies; whether it can actually hold the
        // vine needs the world, so ComputeWorldPlacementState re-resolves it.
        // Clicking the north side of a block puts the vine in the cell north of
        // it, and the support is then SOUTH of the vine — hence the opposite.
        if (IsVineBlock(id)) {
            const Direction support = Opposite(clicked);
            if (support != Direction::Down) {
                return VineStateWithFace(BlockStates::Default(id), support, true).Index();
            }
            return kDefault;
        }

        // ── Stairs ────────────────────────────────────────────────────────
        // MC StairBlock.getStateForPlacement's FACING + HALF. SHAPE is the
        // third property and needs the neighbours, so it is applied in
        // ComputeWorldPlacementState — the same split the redstone wire uses,
        // and the reason both functions exist.
        if (IsStairs(id)) {
            return StairsPlacementState(id, look, clicked, context.getCursorPos().y).Index();
        }

        // ── Skulls / mob heads ────────────────────────────────────────────
        // Floor skulls — MC SkullBlock.getStateForPlacement:
        //   setValue(ROTATION, RotationSegment.convertToSegment(context.getRotation()))
        // where convertToSegment is SegmentedAnglePrecision(4).fromDegrees:
        //   normalize(Math.round(yaw * 16 / 360)) — Java Math.round(float) is
        // floor(x + 0.5), reproduced literally so the 16 segment boundaries
        // land where vanilla puts them. The result is that a placed skull
        // looks back at whoever placed it, in 22.5° steps.
        {
            BlockID unusedWall;
            if (SkullWallVariantOf(id, unusedWall)) {
                const int segment =
                    static_cast<int>(std::floor(context.playerYaw * (16.0f / 360.0f) + 0.5f)) & 15;
                return def.IndexOfSingle("rotation", std::to_string(segment));
            }
        }
        // Wall skulls — MC WallSkullBlock.getStateForPlacement walks
        // getNearestLookingDirections() and takes the first horizontal whose
        // support block holds. Reduced to the clicked face (the same
        // approximation the ladder documents above): FACING = the face
        // clicked, which points the skull out of the wall it hangs on.
        if (IsWallSkullBlock(id)) {
            if (IsHorizontal(clicked)) {
                return def.IndexOfSingle("facing", NameOf(clicked));
            }
            return kDefault;
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

        return kDefault;
    }

    BlockState ComputePlacementState(BlockID id, const UseOnContext& context) {
        return BlockStates::FromIndex(id, ComputePlacementIndex(id, context));
    }

    // ── Skulls / mob heads: floor vs wall block choice ──────────────────────

    bool SkullWallVariantOf(BlockID id, BlockID& outWall) {
        switch (id) {
            case BlockID::SkeletonSkull:       outWall = BlockID::SkeletonWallSkull;       return true;
            case BlockID::WitherSkeletonSkull: outWall = BlockID::WitherSkeletonWallSkull; return true;
            case BlockID::ZombieHead:          outWall = BlockID::ZombieWallHead;          return true;
            case BlockID::CreeperHead:         outWall = BlockID::CreeperWallHead;         return true;
            case BlockID::PlayerHead:          outWall = BlockID::PlayerWallHead;          return true;
            case BlockID::PiglinHead:          outWall = BlockID::PiglinWallHead;          return true;
            case BlockID::DragonHead:          outWall = BlockID::DragonWallHead;          return true;
            default:                           return false;
        }
    }

    bool IsWallSkullBlock(BlockID id) {
        switch (id) {
            case BlockID::SkeletonWallSkull:
            case BlockID::WitherSkeletonWallSkull:
            case BlockID::ZombieWallHead:
            case BlockID::CreeperWallHead:
            case BlockID::PlayerWallHead:
            case BlockID::PiglinWallHead:
            case BlockID::DragonWallHead:
                return true;
            default:
                return false;
        }
    }

    BlockID SkullPlacementBlock(BlockID held, Direction clickedFace) {
        // MC StandingAndWallBlockItem.getPlacementState (skull items are one,
        // attached DOWN): it walks context.getNearestLookingDirections() and
        // takes the standing block for DOWN and the wall block for the first
        // horizontal that holds. Clicking the side of a block puts that face
        // first in the ordering, so the observable rule collapses to: a click
        // on a horizontal face hangs the wall variant, a click on a top or
        // bottom face stands the floor variant.
        BlockID wall;
        if (IsHorizontal(clickedFace) && SkullWallVariantOf(held, wall)) {
            return wall;
        }
        return held;
    }

    namespace {
        // MC spells the same idea two ways (BlockStateProperties.java:164-165):
        // leaf_litter and wildflowers carry `segment_amount`, pink_petals
        // carries `flower_amount`. Both are IntegerProperty(1, 4), so only the
        // name differs.
        //
        // This replaced an explicit kSegmentedChains table of 12 BlockIDs —
        // one id per segment count — which is what those ids existed for.
        PropertyId SegmentPropertyOf(BlockID id) {
            const BlockState def = BlockStates::Default(id);
            if (def.HasProperty(PropertyId::SEGMENT_AMOUNT)) return PropertyId::SEGMENT_AMOUNT;
            if (def.HasProperty(PropertyId::FLOWER_AMOUNT))  return PropertyId::FLOWER_AMOUNT;
            return PropertyId::Count;
        }
    } // namespace

    bool IsSegmentedBlock(BlockID id) {
        return SegmentPropertyOf(id) != PropertyId::Count;
    }

    int SegmentAmountOf(BlockState state) {
        const PropertyId prop = SegmentPropertyOf(state.Block());
        if (prop == PropertyId::Count) return 0;
        const int v = state.GetIndex(prop);
        return v < 0 ? 0 : v + 1;              // property values run 1..4
    }

    BlockState SegmentGrownState(BlockState state) {
        const PropertyId prop = SegmentPropertyOf(state.Block());
        if (prop == PropertyId::Count) return state;
        const int amount = SegmentAmountOf(state);
        if (amount <= 0 || amount >= 4) return state;   // MC: min(4, n + 1)
        // Value `amount + 1` sits at index `amount`, and setting one property
        // leaves the rest — the clump keeps the facing it already had, which is
        // MC's `state.setValue(segment, n + 1)`.
        return state.SetIndex(prop, amount);
    }

    bool CanBeReplacedByPlacement(BlockState existing,
                                  BlockID held, bool secondaryUse,
                                  const PlacementClick& click) {
        const BlockID existingId = existing.Block();
        if (existingId == BlockID::Air) return true;

        // ── MC SlabBlock.canBeReplaced ─────────────────────────────────────
        //
        //   ItemStack stack = context.getItemInHand();
        //   if (type != DOUBLE && stack.is(this.asItem())) {
        //       if (context.replacingClickedOnBlock()) {
        //           boolean above = clickLocation.y - clickedPos.getY() > 0.5;
        //           Direction face = context.getClickedFace();
        //           if (type == BOTTOM) return face == UP   || (above  && face.getAxis().isHorizontal());
        //           else                return face == DOWN || (!above && face.getAxis().isHorizontal());
        //       }
        //       return true;
        //   }
        //   return false;
        //
        // This is what merges two slabs into one full block: a bottom slab
        // clicked on its top face agrees to be replaced, and getStateForPlacement
        // then sees a slab already in the cell and answers DOUBLE. Without it
        // the slab is not replaceable at all, the position resolves to the
        // neighbouring cell, and you can never fill a block in.
        //
        // `replacingClickedOnBlock` is vanilla's flag for "this is the block the
        // crosshair was on" — the second call, against whatever sits in the
        // resolved cell, skips the geometry test and just says yes.
        // `existing == held` is now literally MC's `stack.is(this.asItem())` —
        // one BlockID per slab, so the three halves compare equal without a
        // family lookup, and the half comes from the state.
        if (BlockRegistry::IsSlabBlock(existingId) && existingId == held) {
            using SlabType = BlockRegistry::SlabType;
            const SlabType half = BlockRegistry::SlabTypeOf(existing);
            if (half == SlabType::Double) return false;
            if (!click.replacingClickedOnBlock) return true;
            const bool above = click.hitY > 0.5f;
            const Direction face = click.clickedFace;
            if (half == SlabType::Top) {
                return face == Direction::Down || (!above && IsHorizontal(face));
            }
            return face == Direction::Up || (above && IsHorizontal(face));
        }

        // MC MultifaceBlock.canBeReplaced:
        //   !itemInHand.is(asItem()) || hasAnyVacantFace(state)
        // and VineBlock.canBeReplaced: countFaces(state) < 5.
        //
        // Both say the same thing: more of the same block is only allowed onto a
        // clump that still has a bare face to take it. Without this the generic
        // rule below refuses (held == existing), and the placement lands in the
        // neighbouring cell instead of growing the clump you aimed at.
        if (existingId == held) {
            if (IsMultifaceBlock(existingId)) {
                return MultifaceHasAnyVacantFace(existing);
            }
            if (IsVineBlock(existingId)) {
                return VineCountFaces(existing) < 5;   // no DOWN on a vine
            }
        }

        // MC SegmentableBlock.canBeReplaced:
        //   !isSecondaryUseActive() && itemInHand.is(block.asItem()) && n < 4
        // Growth is the ONLY way a segmented clump is replaceable by its own
        // item; at 4 it falls through to the base rule below, which refuses.
        // `existing == held` is MC's `itemInHand.is(block.asItem())`: the
        // segment count is a state now, so all four counts are one BlockID.
        if (IsSegmentedBlock(existingId) && existingId == held) {
            return !secondaryUse && SegmentAmountOf(existing) < 4;
        }

        // MC SnowLayerBlock.canBeReplaced:
        //
        //   int layers = state.getValue(LAYERS);
        //   if (itemInHand.is(this.asItem()) && layers < 8) { … }
        //   else return layers == 1;
        //
        // Snow is the one `.replaceable()` block whose answer depends on its
        // STATE. Only a single layer is replaceable by something else; a
        // 2-to-8-layer pile is not, and a falling block landing on one must
        // pop as an item rather than overwrite it. The blanket replaceable
        // flag said yes at every depth, which quietly deleted snow piles.
        if (existingId == BlockID::Snow) {
            const int layers = std::atoi(
                std::string(existing.GetValueByName("layers")).c_str());
            if (held == existingId && layers < 8) {
                if (!click.replacingClickedOnBlock) return true;
                return click.clickedFace == Direction::Up;
            }
            return layers == 1;
        }

        // MC BlockBehaviour.canBeReplaced:
        //   state.canBeReplaced() && (itemInHand.isEmpty() || !itemInHand.is(asItem()))
        //
        // The second clause is what stops a replaceable block being overwritten
        // by MORE OF ITSELF — placing tall grass into tall grass does nothing
        // rather than silently consuming the item.
        if (!BlockRegistry::Get(existingId).replaceable) return false;
        return held == BlockID::Air || held != existingId;
    }

    namespace {
        // MC Block.isFaceFull(getBlockSupportShape(), UP), approximated from the
        // single collision AABB this engine keeps per state: a block supports
        // what sits on it when its collision box reaches the cell's top and
        // covers the whole square. True for full cubes and top slabs, false for
        // bottom slabs, and false for everything `.noCollision()` — including
        // leaf litter, which is what stops a clump from being stacked on itself.
        bool IsFaceSturdyUp(BlockState state) {
            if (!BlockRegistry::HasCollision(state.Block())) return false;
            // The box UNION, not its bounds: a bottom-half stair's bounds fill
            // the cell, but no single box of it covers the top face, so MC
            // answers false — you cannot put a flower on the low half of a
            // stair. A top-half stair's slab does cover it, and answers true.
            return BlockRegistry::GetBlockShapeSet(state).IsFaceSturdyUp();
        }

        // #minecraft:dirt, verbatim from data/minecraft/tags/block/dirt.json,
        // matched against model names (which are the MC block names).
        constexpr std::string_view kDirtTag[] = {
            "dirt", "grass_block", "podzol", "coarse_dirt", "mycelium",
            "rooted_dirt", "moss_block", "pale_moss_block", "mud",
            "muddy_mangrove_roots",
            // The Hush's two soils — also in data/minecraft/tags/block/dirt.json
            // so the terrain library's trees and flowers treat them as dirt.
            "sculk_loam", "hush_moss",
            // The Aether's ground — its data pack adds #aether:aether_dirt
            // (aether grass + aether dirt) to #minecraft:dirt, also mirrored in
            // data/minecraft/tags/block/dirt.json. Without it skyroot and
            // golden oak saplings, the flowers and the berry bush would refuse
            // the Aether's own soil.
            "aether_grass_block", "aether_dirt",
            // Not a tag entry: vanilla's snowy grass is `grass_block{snowy=true}`,
            // the same block, but this engine promotes it to its own BlockID
            // with its own model name. Omitting it would make snow-covered
            // grass the one dirt that refuses flowers.
            "grass_block_snow",
        };
    } // namespace

    bool CanSurviveOn(BlockID id, BlockState below) {
        const BlockID belowId = below.Block();
        const std::string& name = BlockRegistry::Get(id).modelName;

        // MC LeafLitterBlock.canSurvive — any block with a sturdy top face,
        // not just dirt. This is the override that makes leaf litter placeable
        // on stone or planks while flowers are not.
        if (Has(name, "leaf_litter")) {
            return IsFaceSturdyUp(below);
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
            return IsFaceSturdyUp(below);
        }

        // NetherWartBlock.mayPlaceOn: `state.is(Blocks.SOUL_SAND)`.
        if (name == "nether_wart") {
            return belowId == BlockID::SoulSand;
        }

        // SaplingBlock inherits VegetationBlock.mayPlaceOn as well:
        // `state.is(BlockTags.DIRT) || state.is(Blocks.FARMLAND)`. The Hush's
        // whisperwood sapling is the one that made this matter — sculk loam
        // and hush moss are in kDirtTag, so it plants on its own soils.
        if (IsSaplingName(name)) {
            const std::string& belowName = BlockRegistry::Get(belowId).modelName;
            if (belowName == "farmland") return true;
            for (std::string_view d : kDirtTag) if (belowName == d) return true;
            return false;
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
        // face: some box of the block's shape must cover the whole square of
        // that face and reach it. True for full cubes, for the top of a top
        // slab, and false for anything `.noCollision()` — which is what stops
        // a button being hung on another button.
        //
        // Asked of one box at a time rather than of the union's bounds, which
        // is what a multi-box shape needs: a straight stair's bounds fill the
        // cell and would call all six faces sturdy, where vanilla gives it two
        // — the tall side its step reaches, and the flat side its slab is on.
        // Two boxes jointly covering a face that neither covers alone would be
        // missed, but no stair shape is built that way.
        bool IsBoxFaceSturdy(const BlockRegistry::BlockShape& s, Direction face) {
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

    } // namespace

    bool IsStateFaceSturdy(BlockState state, Direction face) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return false;
        if (!BlockRegistry::HasCollision(id)) return false;
        const auto set = BlockRegistry::GetBlockShapeSet(state);
        for (const auto& s : set) if (IsBoxFaceSturdy(s, face)) return true;
        return false;
    }

    bool IsFaceSturdyAt(const IBlockAccess& level, const glm::ivec3& p, Direction face) {
        return IsStateFaceSturdy(level.GetBlockState(p.x, p.y, p.z), face);
    }

    namespace {
        // Local alias kept so the call sites below read as they did.
        bool IsFaceSturdy(const IBlockAccess& level, const glm::ivec3& p, Direction face) {
            return IsFaceSturdyAt(level, p, face);
        }

        bool IsFaceAttachedBlock(BlockID id) {
            const std::string& n = BlockRegistry::Get(id).modelName;
            return n.find("_button") != std::string::npos || n == "lever";
        }
    } // namespace

    bool IsAmethystClusterBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        return IsAmethystClusterLike(BlockRegistry::Get(id).modelName);
    }

    bool IsSaplingBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        return IsSaplingName(BlockRegistry::Get(id).modelName);
    }

    bool AmethystClusterCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state) {
        // MC AmethystClusterBlock.canSurvive:
        //   Direction d = state.getValue(FACING);
        //   BlockPos adjacent = pos.relative(d.getOpposite());
        //   return level.getBlockState(adjacent).isFaceSturdy(level, adjacent, d);
        // The cluster POINTS along FACING and is held by the block behind it,
        // whose face toward the cluster must be sturdy.
        const Direction facing    = FacingOf(state);
        const Direction toSupport = Opposite(facing);
        const glm::ivec3 support{pos.x + StepX(toSupport),
                                 pos.y + StepY(toSupport),
                                 pos.z + StepZ(toSupport)};
        return IsFaceSturdy(level, support, facing);
    }

    bool LadderCanSurvive(const IBlockAccess& level, const glm::ivec3& pos, Direction facing) {
        // MC LadderBlock.canSurvive: the block behind (opposite FACING) has a
        // sturdy face pointing back at the ladder.
        if (!IsHorizontal(facing)) return false;
        const Direction toWall = Opposite(facing);
        return IsFaceSturdy(level, glm::ivec3(pos.x + StepX(toWall), pos.y, pos.z + StepZ(toWall)), facing);
    }

    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockState state) {
        const BlockID id = state.Block();
        if (HasRedstoneSurvivalRule(id)) return RedstoneComponentCanSurvive(level, pos, state);
        if (id == BlockID::Ladder) {
            return LadderCanSurvive(level, pos, HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING)));
        }
        // MC FaceAttachedHorizontalDirectionalBlock.canSurvive:
        //   canAttach(level, pos, getConnectedDirection(state).getOpposite())
        // where getConnectedDirection is UP for FLOOR, DOWN for CEILING, and
        // FACING for WALL — the direction the block POINTS. Its opposite is the
        // direction of the surface holding it up.
        if (IsFaceAttachedBlock(id)) {
            const std::string_view face   = state.GetValueByName("face");
            const std::string_view facing = state.GetValueByName("facing");

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
        if (IsVineBlock(id)) return VineCanSurvive(level, pos, state);
        if (IsMultifaceBlock(id)) return MultifaceCanSurvive(level, pos, state);
        if (IsAmethystClusterBlock(id)) return AmethystClusterCanSurvive(level, pos, state);
        // Signs: standing needs a solid block below, wall a sturdy face
        // behind, ceiling-hanging a sturdy face above, wall-hanging a
        // sturdy face on either side (MC canSurvive / canPlace).
        if (IsStandingSignBlock(id)) {
            return IsFaceSturdy(level, glm::ivec3(pos.x, pos.y - 1, pos.z), Direction::Up);
        }
        if (IsWallSignBlock(id) || IsWallHangingSignBlock(id)) {
            const Direction facing = HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
            if (IsWallSignBlock(id)) {
                const Direction toWall = Opposite(facing);
                return IsFaceSturdy(level, glm::ivec3(pos.x + StepX(toWall), pos.y, pos.z + StepZ(toWall)), facing);
            }
            const Direction cw = ClockWise(facing), ccw = CounterClockWise(facing);
            return IsFaceSturdy(level, glm::ivec3(pos.x + StepX(cw),  pos.y, pos.z + StepZ(cw)),  ccw) ||
                   IsFaceSturdy(level, glm::ivec3(pos.x + StepX(ccw), pos.y, pos.z + StepZ(ccw)), cw);
        }
        if (IsCeilingHangingSignBlock(id)) {
            return IsFaceSturdy(level, glm::ivec3(pos.x, pos.y + 1, pos.z), Direction::Down);
        }
        // MC ScaffoldingBlock.canSurvive / PointedDripstoneBlock.canSurvive.
        // Both are STATE-shaped rules that the generic "solid below" heuristic
        // cannot express: scaffolding is held by a horizontal distance walk,
        // and a dripstone is held from BEHIND its tip, which for a stalactite
        // means from above.
        if (id == BlockID::Scaffolding) return ScaffoldingCanSurvive(level, pos);
        if (id == BlockID::PointedDripstone) {
            return PointedDripstoneCanSurvive(level, pos, state);
        }
        return CanSurviveAt(level, pos, id);
    }

    BlockState ComputeWorldPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                          BlockState fallback) {
        const BlockID id = fallback.Block();
        if (id == BlockID::Ladder) {
            // The click's facing if its wall holds, else the first wall
            // round the cell that does (MC's loop, minus the look order the
            // predictor does not carry here). A cell with no wall keeps the
            // hint, and CanSurviveAt refuses the placement.
            const Direction hint = HorizontalFacingFromIndex(fallback.GetIndex(PropertyId::HORIZONTAL_FACING));
            if (!LadderCanSurvive(level, pos, hint)) {
                for (Direction d : { ClockWise(hint), Opposite(hint), CounterClockWise(hint) }) {
                    if (LadderCanSurvive(level, pos, d)) {
                        return fallback.SetName(PropertyId::HORIZONTAL_FACING, NameOf(d));
                    }
                }
            }
            return fallback;
        }
        if (id == BlockID::RedstoneWire) {
            // MC RedStoneWireBlock.getStateForPlacement:
            //   getConnectionState(level, this.crossState, pos)
            return RedstonePlacementState(level, pos);
        }
        if (id == BlockID::PotentSulfur) {
            // MC PotentSulfurBlock.getStateForPlacement:
            //   validBlockState(defaultBlockState(), level, clickedPos)
            // No block entity is there yet to reset a countdown on.
            return PotentSulfur::ValidBlockState(fallback, level, pos, nullptr);
        }

        // MC StairBlock.getStateForPlacement's closing line:
        //   state.setValue(SHAPE, getStairsShape(state, level, pos))
        // Applied to the FACING/HALF state ComputePlacementState already
        // chose, and BEFORE the waterlog pass below, which only rewrites the
        // trailing bit and so composes with whatever shape lands here.
        if (IsStairs(id)) {
            fallback = StairsWorldPlacementState(level, pos, fallback);
        }

        // MC FenceBlock / IronBarsBlock.getStateForPlacement — resolve all four
        // connection sides against the neighbours. Same split as the stairs
        // above: nothing about a fence's orientation comes from the player, so
        // all of it lives on this side of the placement pair.
        if (IsCrossCollisionBlock(id)) {
            fallback = CrossPlacementState(level, pos, fallback);
        }

        // MC WallBlock.getStateForPlacement — the four connections, then the
        // block above decides LOW vs TALL and whether the post shows.
        if (IsWallBlock(id)) {
            fallback = WallPlacementState(level, pos, fallback);
        }

        // MC MultifaceBlock / VineBlock.getStateForPlacement.
        //
        // The face bit in `fallback` is a HINT, not a result: ComputePlacementState
        // has no world access, so it records which face the click implied and
        // leaves the actual decision here. The search must therefore start from a
        // BARE state — feeding it the hinted state instead makes the search skip
        // that face as "already taken" and add a SECOND one, which is how a
        // sculk vein came out clinging to both the wall you clicked and the
        // ceiling above it.
        //
        // MC's base state is `oldState.is(this) ? oldState : defaultBlockState()`,
        // so placing onto an existing clump ADDS a face to it rather than
        // starting over — that is the whole point of these blocks stacking up on
        // a corner.
        if (IsMultifaceBlock(id) || IsVineBlock(id)) {
            const bool multiface = IsMultifaceBlock(id);

            Direction hint = Direction::Up;
            for (Direction d : { Direction::Down, Direction::Up, Direction::North,
                                 Direction::East, Direction::South, Direction::West }) {
                const bool has = multiface ? MultifaceFaceOf(fallback, d)
                                           : VineFaceOf(fallback, d);
                if (has) { hint = Opposite(d); break; }
            }

            const BlockState here = level.GetBlockState(pos.x, pos.y, pos.z);
            const BlockState base = here.Is(id) ? here : BlockStates::Default(id);

            fallback = multiface ? MultifacePlacementState(level, pos, base, hint)
                                 : VinePlacementState(level, pos, base, hint);
        }

        // MC ConcretePowderBlock / ScaffoldingBlock / PointedDripstoneBlock
        // getStateForPlacement. All three need the neighbours and none of them
        // needs anything from the click beyond what `fallback` already carries,
        // so they belong on this side of the placement pair.
        //
        // Concrete powder is the odd one: it can return a state of a DIFFERENT
        // BLOCK (the concrete), so callers must take the block from the
        // returned state rather than from the item they were holding.
        if (IsConcretePowder(id)) {
            fallback = ConcretePowderPlacementState(level, pos, fallback);
        }
        if (id == BlockID::Scaffolding) {
            fallback = ScaffoldingPlacementState(level, pos, fallback);
        }
        if (id == BlockID::PointedDripstone) {
            // TIP_DIRECTION is the one part of this that DOES come from the
            // click, so ComputePlacementState leaves it as a hint in
            // `fallback` (the multiface/vine pattern above) and it is read
            // back out here. `secondaryUse` (sneaking) is not plumbed through
            // ComputeWorldPlacementState, so opposing tips always merge —
            // which is the non-sneaking default.
            const bool defaultTipDown =
                fallback.GetValueByName("tip_direction") == "down";
            fallback = PointedDripstonePlacementState(level, pos, fallback,
                                                      defaultTipDown,
                                                      /*secondaryUse=*/false);
        }

        // MC FenceGateBlock.getStateForPlacement's IN_WALL clause. FACING has
        // already been set by the horizontal placement rule below/above, which
        // is the same `context.getHorizontalDirection()` vanilla uses.
        if (IsFenceGateBlock(id)) {
            fallback = FenceGatePlacementState(level, pos, fallback);
        }

        // MC RepeaterBlock.getStateForPlacement: LOCKED from the side inputs.
        if (id == BlockID::Repeater) {
            fallback = RepeaterPlacementState(level, pos, fallback);
        }
        // MC NoteBlock.getStateForPlacement: the instrument of what is below
        // (or the head above).
        if (id == BlockID::NoteBlock) {
            fallback = NoteBlockWithInstrument(level, pos, fallback);
        }
        // MC TripWireBlock.getStateForPlacement: which sides join.
        if (id == BlockID::Tripwire) {
            fallback = TripWirePlacementState(level, pos, fallback);
        }

        // Waterlogging. Every SimpleWaterloggedBlock's getStateForPlacement
        // ends with the same line — StairBlock.java, SlabBlock.java,
        // FenceBlock via CrossCollisionBlock, and the rest:
        //
        //   .setValue(WATERLOGGED, fluidState.getType() == Fluids.WATER)
        //
        // so it belongs here, applied to the whole family at once, rather than
        // as 386 special cases. Note the comparison: `getType() == Fluids
        // .WATER` is the SOURCE fluid — FLOWING_WATER is a different Fluid —
        // so a stair set down in a stream is dry and the stream is simply
        // displaced, while one set down in a pool is waterlogged. Coral and
        // sea pickle's extra `getAmount() == 8` says the same thing.
        //
        // Placed after the fallback is computed so it composes with whatever
        // orientation ComputePlacementState already chose: WithWaterlogged
        // rewrites one bit and leaves every other property alone.
        //
        // Assigned in BOTH directions, not only set-when-wet. Coral, sea
        // pickle and conduit register WATERLOGGED **true** as their default,
        // so state 0 for them already means waterlogged — placing one in air
        // has to actively clear the flag, which is exactly what vanilla's
        // unconditional setValue does.
        if (BlockRegistry::IsWaterloggable(id)) {
            return BlockRegistry::WithWaterlogged(
                fallback, BlockRegistry::IsWaterSource(level.GetBlockState(pos.x, pos.y, pos.z)));
        }
        return fallback;
    }

    bool HasModelledSurvivalRule(BlockID id) {
        if (HasRedstoneSurvivalRule(id)) return true;
        if (id == BlockID::HangingWhisperfruit) return true;   // CanSurviveAt: lantern leaves above
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
            "ladder",
            "sugar_cane", "cactus", "bamboo", "bamboo_sapling",
            "vine", "glow_lichen", "sculk_vein", "resin_clump",
        };
        const std::string& name = BlockRegistry::Get(id).modelName;
        for (std::string_view m : kModelled) if (name == m) return true;
        // Families matched by name shape rather than exact name: saplings
        // (CanSurviveOn: dirt or farmland) and the amethyst clusters
        // (CanSurviveAt: the block behind FACING).
        return IsSaplingName(name) || IsAmethystClusterLike(name);
    }

    bool CanSurviveAt(const IBlockAccess& level, const glm::ivec3& pos, BlockID id) {
        if (HasRedstoneSurvivalRule(id)) {
            // The state-free gate: the default state's rule. Placement asks
            // again with the real state once it is known.
            return RedstoneComponentCanSurvive(level, pos, BlockStates::Default(id));
        }
        if (id == BlockID::Ladder) {
            // Asked by the world's support collapse for a ladder already
            // placed: its facing is in the world.
            const BlockState here = level.GetBlockState(pos.x, pos.y, pos.z);
            if (!here.Is(id)) return true;
            return LadderCanSurvive(level, pos, HorizontalFacingFromIndex(here.GetIndex(PropertyId::HORIZONTAL_FACING)));
        }
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

        // The Hush's whisperfruit hangs from the underside of lantern leaves
        // and nothing else (HushBlocks.cpp; its updateShape drops it when the
        // leaf goes) — the ABOVE-block twin of the support rules here.
        if (id == BlockID::HangingWhisperfruit) {
            return level.GetBlock(pos.x, pos.y + 1, pos.z) == BlockID::LanternLeaves;
        }

        // CocoaBlock.canSurvive is about the block it FACES, not the one below,
        // and the facing is not known until placement resolves. Left to the
        // generic path for now — cocoa places like any other block.

        const BlockState belowState = level.GetBlockState(below.x, below.y, below.z);
        return CanSurviveOn(id, belowState);
    }

    // ── Doors ───────────────────────────────────────────────────────────

    // ── Signs ───────────────────────────────────────────────────────────

    namespace {
        bool SlugEndsWith(BlockID id, std::string_view suffix) {
            if (id == BlockID::Air) return false;
            const std::string& slug = BlockRegistry::Get(id).registrySlug;
            return slug.size() > suffix.size() &&
                   slug.compare(slug.size() - suffix.size(), suffix.size(), suffix) == 0;
        }
    } // namespace

    bool IsSignBlock(BlockID id)               { return SlugEndsWith(id, "_sign"); }
    bool IsWallSignBlock(BlockID id)           { return SlugEndsWith(id, "_wall_sign"); }
    bool IsWallHangingSignBlock(BlockID id)    { return SlugEndsWith(id, "_wall_hanging_sign"); }
    bool IsCeilingHangingSignBlock(BlockID id) { return SlugEndsWith(id, "_hanging_sign") && !IsWallHangingSignBlock(id); }
    bool IsStandingSignBlock(BlockID id) {
        return IsSignBlock(id) && !IsWallSignBlock(id) && !IsCeilingHangingSignBlock(id) &&
               !IsWallHangingSignBlock(id);
    }

    BlockID SignPlacementBlock(BlockID held, Direction clickedFace) {
        if (!IsHorizontal(clickedFace)) return held;
        const std::string& slug = BlockRegistry::Get(held).registrySlug;
        std::string wallSlug;
        if (IsStandingSignBlock(held)) {
            wallSlug = slug.substr(0, slug.size() - 5) + "_wall_sign";                // oak_sign → oak_wall_sign
        } else if (IsCeilingHangingSignBlock(held)) {
            wallSlug = slug.substr(0, slug.size() - 13) + "_wall_hanging_sign";       // oak_hanging_sign → oak_wall_hanging_sign
        } else {
            return held;
        }
        const BlockID wall = BlockStates::FromSlug(wallSlug).Block();
        return wall != BlockID::Air ? wall : held;
    }

    float SignYawDegrees(BlockState state) {
        const BlockID id = state.Block();
        if (IsWallSignBlock(id) || IsWallHangingSignBlock(id)) {
            return ToYRot(HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING)));
        }
        // RotationSegment.convertToDegrees: segment * 22.5, into [-180, 180).
        const std::string_view rot = state.GetValueByName("rotation");
        const float deg = rot.empty() ? 0.0f : 22.5f * static_cast<float>(std::atoi(std::string(rot).c_str()));
        return deg >= 180.0f ? deg - 360.0f : deg;
    }

    bool IsTrapdoorBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        const std::string& slug = BlockRegistry::Get(id).registrySlug;
        constexpr std::string_view kSuffix = "_trapdoor";
        return slug.size() > kSuffix.size() &&
               slug.compare(slug.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
    }

    bool IsHandOpenableTrapdoor(BlockID id) {
        return IsTrapdoorBlock(id) && id != BlockID::IronTrapdoor;
    }

    bool IsDoorBlock(BlockID id) {
        if (id == BlockID::Air) return false;
        const std::string& slug = BlockRegistry::Get(id).registrySlug;
        return slug.size() > 5 && slug.compare(slug.size() - 5, 5, "_door") == 0 &&
               slug.find("trapdoor") == std::string::npos;
    }

    bool IsWoodenDoorBlock(BlockID id) {
        // MC DoorBlock.type().canOpenByHand(): every door but iron.
        return IsDoorBlock(id) && id != BlockID::IronDoor;
    }

    BlockState DoorPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                  BlockState state, const glm::dvec3& clickWorld) {
        // MC DoorBlock.getHinge. A full block beside the door pushes the
        // hinge to the other side; a lower door half beside it makes the
        // pair a double door; otherwise the click's side of the cell decides.
        const BlockID id = state.Block();
        const Direction facing =
            HorizontalFacingFromIndex(state.GetIndex(PropertyId::HORIZONTAL_FACING));
        const Direction ccw = Opposite(ClockWise(facing));
        const Direction cw  = ClockWise(facing);
        auto at = [&](Direction d, int dy) {
            return glm::ivec3(pos.x + StepX(d), pos.y + dy, pos.z + StepZ(d));
        };
        auto full = [&](const glm::ivec3& p) {
            return BlockRegistry::GetBlockShapeSet(level.GetBlockState(p.x, p.y, p.z)).IsFullCube();
        };
        // MC 26.3: `leftState.getBlock() instanceof DoorBlock` — any door,
        // not only this wood.
        auto lowerDoor = [&](const glm::ivec3& p) {
            const BlockState s = level.GetBlockState(p.x, p.y, p.z);
            return IsDoorBlock(s.Block()) && s.GetName(PropertyId::DOUBLE_BLOCK_HALF) == "lower";
        };
        const glm::ivec3 left = at(ccw, 0), right = at(cw, 0);
        const int i = (full(left) ? -1 : 0) + (full(at(ccw, 1)) ? -1 : 0) +
                      (full(right) ? 1 : 0) + (full(at(cw, 1)) ? 1 : 0);
        const bool leftIsDoor  = lowerDoor(left);
        const bool rightIsDoor = lowerDoor(right);
        bool hingeLeft;
        if ((!leftIsDoor || rightIsDoor) && i <= 0) {
            if ((!rightIsDoor || leftIsDoor) && i >= 0) {
                const int j = StepX(facing), k = StepZ(facing);
                const double d0 = clickWorld.x - pos.x;
                const double d1 = clickWorld.z - pos.z;
                hingeLeft = (j >= 0 || !(d1 < 0.5)) && (j <= 0 || !(d1 > 0.5)) &&
                            (k >= 0 || !(d0 > 0.5)) && (k <= 0 || !(d0 < 0.5));
            } else {
                hingeLeft = true;
            }
        } else {
            hingeLeft = false;
        }
        // MC DoorBlock.getStateForPlacement: `hasNeighborSignal(pos) ||
        // hasNeighborSignal(pos.above())` sets both POWERED and OPEN.
        const bool powered = HasNeighborSignal(level, pos) || HasNeighborSignal(level, Above(pos));
        return state.SetName(PropertyId::HINGE, hingeLeft ? "left" : "right")
                    .SetName(PropertyId::DOUBLE_BLOCK_HALF, "lower")
                    .SetName(PropertyId::OPEN, powered ? "true" : "false")
                    .SetName(PropertyId::POWERED, powered ? "true" : "false");
    }

    BlockID TorchPlacementBlock(const IBlockAccess& level, BlockID held, const glm::ivec3& pos,
                                Direction clickedFace) {
        if (!IsStandingTorch(held)) return held;
        const BlockID wall = WallTorchOf(held);
        if (wall == BlockID::Air) return held;
        // MC StandingAndWallBlockItem.getPlacementState: the wall block's
        // own getStateForPlacement is tried first — WallTorchBlock walks the
        // look directions, the clicked face at the front, and takes the
        // first wall that can hold it — and the standing block only when no
        // wall can. A click on a top or bottom face never reaches the wall
        // block in vanilla either, because canAttach fails for a vertical
        // face.
        if (IsHorizontal(clickedFace)) {
            const glm::ivec3 behind = Relative(pos, Opposite(clickedFace));
            if (IsFaceSturdyAt(level, behind, clickedFace)) return wall;
        }
        return held;
    }

    BlockState DoorUpperState(BlockState lower) {
        return lower.SetName(PropertyId::DOUBLE_BLOCK_HALF, "upper");
    }

} // namespace Game
