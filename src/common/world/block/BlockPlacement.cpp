// File: src/common/world/block/BlockPlacement.cpp
#include "BlockPlacement.hpp"
#include "BlockRegistry.hpp"
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
        // The `replaceable` property itself is still unmodelled here — only air
        // qualifies, so grass, snow layers and fluids are not yet placement-
        // replaceable (same gap the caller has always had). Once that flag
        // exists, this is the one place it needs to be read.
        return false;
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

        // No modelled rule — placement is unconstrained, as it was before.
        return true;
    }

} // namespace Game
