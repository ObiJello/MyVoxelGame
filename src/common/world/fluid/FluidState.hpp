// File: src/common/world/fluid/FluidState.hpp
//
// Port of MC's net.minecraft.world.level.material.FluidState, reduced to the
// two fluids that exist (water and lava) and read straight off a BlockState.
//
// MC keeps fluids as a second state registry beside blocks: a LiquidBlock's
// `level` property is mapped through LiquidBlock.stateCache onto a
// FluidState of Fluids.WATER (the source) or Fluids.FLOWING_WATER with LEVEL
// and FALLING, and every other block answers Fluids.EMPTY unless it is
// waterlogged (SimpleWaterloggedBlock) or always holds water (kelp, seagrass,
// bubble column). This engine stores the SAME data — `level` 0..15 on water
// and lava, `waterlogged` on 386 blocks — so a FluidState here is a value
// computed from the block state, never stored, exactly as
// `BlockState.getFluidState()` is a lookup and not a field.
//
// The legacy `level` encoding (LiquidBlock / FlowingFluid.getLegacyLevel):
//   0        = source            (amount 8, isSource)
//   1..7     = flowing, amount 8 - level
//   8..15    = falling           (amount 8, FALLING)
//
// Shared by the server (spreading in FlowingFluid.cpp), the entity code
// (fluid heights, currents, lava contact) and the client (surface heights
// and flow direction for the fluid mesher), so it lives in `common` and pulls
// in nothing but the block state tables.
#pragma once

#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"

#include <glm/vec3.hpp>
#include <cstdint>

namespace Game {

    struct IBlockAccess;

    // MC's Fluids registry, collapsed to a kind. "Same fluid" in MC means
    // `Fluid.isSame` — WATER and FLOWING_WATER are the same fluid — so the
    // source/flowing split is carried on the FluidState, not on the type.
    enum class FluidType : uint8_t {
        Empty = 0,
        Water = 1,
        Lava  = 2,
    };

    struct FluidState {
        FluidType type    = FluidType::Empty;
        // MC FluidState.getAmount(): 8 for a source or a falling column, 1..7
        // for a flowing cell. 0 only for EMPTY.
        uint8_t   amount  = 0;
        // MC FlowingFluid.FALLING — the cell is fed from above.
        bool      falling = false;
        // MC Fluid.isSource — the Fluids.WATER / Fluids.LAVA instance rather
        // than the FLOWING_* one.
        bool      source  = false;

        static constexpr int kAmountMax  = 9;   // FluidState.AMOUNT_MAX
        static constexpr int kAmountFull = 8;   // FluidState.AMOUNT_FULL

        static constexpr FluidState Empty() { return FluidState{}; }
        static constexpr FluidState Source(FluidType t) {
            return FluidState{t, static_cast<uint8_t>(kAmountFull), false, true};
        }
        static constexpr FluidState Flowing(FluidType t, int amount, bool falling) {
            return FluidState{t, static_cast<uint8_t>(amount), falling, false};
        }

        constexpr bool IsEmpty()  const { return type == FluidType::Empty; }
        constexpr bool IsSource() const { return source; }
        constexpr bool IsFull()   const { return amount == kAmountFull; }
        constexpr bool Is(FluidType t) const { return type == t; }
        // MC `state.getType().isSame(other)` — water is water, flowing or not.
        constexpr bool IsSame(FluidType t) const { return type != FluidType::Empty && type == t; }
        // MC `state.isSourceOfType(fluid)`.
        constexpr bool IsSourceOf(FluidType t) const { return source && type == t; }

        // MC FlowingFluid.getOwnHeight: amount / 9. A source stands 8/9 of a
        // block tall; a falling column also reads 8 and so is 8/9 in
        // isolation, full when the cell above is the same fluid (see
        // FluidHeight below).
        constexpr float OwnHeight() const { return static_cast<float>(amount) / 9.0f; }

        constexpr bool operator==(const FluidState& o) const {
            return type == o.type && amount == o.amount && falling == o.falling && source == o.source;
        }
        constexpr bool operator!=(const FluidState& o) const { return !(*this == o); }
    };

    // MC BlockState.getFluidState() — LiquidBlock's stateCache lookup for the
    // two fluid blocks, WATER (source) for waterlogged and always-water
    // blocks, EMPTY for everything else.
    FluidState FluidStateOf(BlockState state);

    // MC `level.getFluidState(pos)`.
    FluidState GetFluidState(const IBlockAccess& level, int x, int y, int z);
    inline FluidState GetFluidState(const IBlockAccess& level, const glm::ivec3& p) {
        return GetFluidState(level, p.x, p.y, p.z);
    }

    // MC FlowingFluid.getHeight: 1.0 when the same fluid sits directly above
    // (the column is continuous through the cell top), else getOwnHeight.
    // `state` is the fluid at `pos`, passed so a caller that already has it
    // pays one neighbour read, not two.
    float FluidHeight(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state);

    // MC FluidState.getHeightForCamera: a SOURCE whose cell above has a
    // sturdy down face fills the cell to the top for the camera test, so the
    // eye cannot sit in the 1/9 air gap under a ceiling and see dry.
    float FluidHeightForCamera(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state);

    // MC FlowingFluid.getFlow — the unit direction this cell's fluid is
    // running in, from the height gradient across its four horizontal
    // neighbours (and one step down where a neighbour is open), with the
    // -6 Y term for a falling column beside a solid face. Zero for a still
    // cell. Read by the mesher (flowing sprite + UV rotation), by the entity
    // current push, and by nothing else.
    glm::dvec3 FluidFlow(const IBlockAccess& level, const glm::ivec3& pos, const FluidState& state);

    // MC Fluid.createLegacyBlock — the block state that stores this fluid.
    // EMPTY gives air.
    BlockState FluidLegacyBlock(const FluidState& state);
    // MC FlowingFluid.getLegacyLevel: the `level` property value for a state.
    int FluidLegacyLevel(const FluidState& state);
    // BlockID::Water / BlockID::Lava / BlockID::Air.
    BlockID FluidBlockId(FluidType type);

    // ── The block tags the fluid code reads ───────────────────────────────
    //
    // Resolved from the data pack through DataTags on first use and cached
    // per BlockID (the tag files never change while the process runs — a
    // resource reload reloads client assets, not the data pack).
    //
    // MC BlockTags.BLOCKS_MOTION = blocks_motion_no_leaves + leaves. The
    // pre-26 data pack shipped here has both halves and not the union file,
    // so it is composed the way VanillaBlockTagsProvider composes it.
    bool BlockBlocksMotion(BlockID id);
    // MC BlockTags.BLOCKS_FLUID_FLOW = BLOCKS_MOTION + ALL_SIGNS.
    bool BlockBlocksFluidFlow(BlockID id);
    // MC BlockTags.WASHED_AWAY_BY_FLUIDS.
    bool BlockWashedAwayByFluids(BlockID id);
    // MC BlockTags.BLOCKS_LAVA_FIRE_SPREAD = BLOCKS_MOTION.
    bool BlockBlocksLavaFireSpread(BlockID id);
    // MC `state.getBlock() instanceof IceBlock` — ice and frosted ice.
    // Packed and blue ice are plain blocks in vanilla and do not count.
    bool IsIceBlock(BlockID id);
    // MC BlockTags.SOUL_FIRE_BASE_BLOCKS — what BaseFireBlock.getState reads
    // to decide between fire and soul fire.
    bool IsSoulFireBaseBlock(BlockID id);

} // namespace Game
