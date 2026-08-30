// File: src/common/world/block/FallingBlock.cpp
#include "common/world/block/FallingBlock.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/SoundEvents.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/FallingBlockEntity.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/level/WorldFallingBlock.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace Game {

    namespace {

        constexpr glm::ivec3 kDown{0, -1, 0};

        // MC BlockTags.FIRE.
        bool IsFireBlock(BlockID id) {
            return id == BlockID::Fire || id == BlockID::SoulFire;
        }

        // MC BlockState.liquid() — set by Properties.liquid(), which only
        // water and lava carry.
        bool IsLiquidBlock(BlockID id) {
            return id == BlockID::Water || id == BlockID::Lava;
        }

        // ── Concrete powder <-> concrete ────────────────────────────────────
        //
        // MC gives every ConcretePowderBlock its solid twin as a constructor
        // argument. Sixteen rows, in BlockDefs.inc's own order.
        struct ConcretePair { BlockID powder; BlockID solid; };
        constexpr std::array<ConcretePair, 16> kConcrete{{
            { BlockID::WhiteConcretePowder,     BlockID::WhiteConcrete     },
            { BlockID::OrangeConcretePowder,    BlockID::OrangeConcrete    },
            { BlockID::MagentaConcretePowder,   BlockID::MagentaConcrete   },
            { BlockID::LightBlueConcretePowder, BlockID::LightBlueConcrete },
            { BlockID::YellowConcretePowder,    BlockID::YellowConcrete    },
            { BlockID::LimeConcretePowder,      BlockID::LimeConcrete      },
            { BlockID::PinkConcretePowder,      BlockID::PinkConcrete      },
            { BlockID::GrayConcretePowder,      BlockID::GrayConcrete      },
            { BlockID::LightGrayConcretePowder, BlockID::LightGrayConcrete },
            { BlockID::CyanConcretePowder,      BlockID::CyanConcrete      },
            { BlockID::PurpleConcretePowder,    BlockID::PurpleConcrete    },
            { BlockID::BlueConcretePowder,      BlockID::BlueConcrete      },
            { BlockID::BrownConcretePowder,     BlockID::BrownConcrete     },
            { BlockID::GreenConcretePowder,     BlockID::GreenConcrete     },
            { BlockID::RedConcretePowder,       BlockID::RedConcrete       },
            { BlockID::BlackConcretePowder,     BlockID::BlackConcrete     },
        }};

        // MC ColoredFallingBlock's ColorRGBA, from Blocks.java:1351-1354.
        // Gravel's is written as the NEGATIVE int -8356741; two's complement
        // gives 0xFF807C7B, so the RGB is 807C7B. (It was transcribed as
        // 807F7F, which is a slip in the conversion, not in MC — the three
        // channels are genuinely unequal.)
        constexpr uint32_t kSandDust    = 0xDBD3A0;   // ColorRGBA(14406560)
        constexpr uint32_t kRedSandDust = 0xA95821;   // ColorRGBA(11098145)
        constexpr uint32_t kGravelDust  = 0x807C7B;   // ColorRGBA(-8356741)

        ScheduledTickAccess* TicksOf(ILevelWrite& level) { return level.Ticks(); }

        // Book the fall appointment. Shared by onPlace and the two
        // neighbour hooks, and a no-op wherever there is no scheduler — which
        // is the client, whose predicted world has no authority to run ticks.
        void ScheduleFall(ScheduledTickAccess* ticks, const glm::ivec3& pos,
                          BlockState state) {
            if (!ticks) return;
            ticks->ScheduleTick(pos, state.Block(), FallingBlockDelayAfterPlace(state));
        }

    } // namespace

    // ── Family membership ──────────────────────────────────────────────────

    bool IsConcretePowder(BlockID id) {
        for (const ConcretePair& p : kConcrete) if (p.powder == id) return true;
        return false;
    }

    bool IsAnvil(BlockID id) {
        return id == BlockID::Anvil || id == BlockID::ChippedAnvil ||
               id == BlockID::DamagedAnvil;
    }

    BlockID ConcreteFor(BlockID powder) {
        for (const ConcretePair& p : kConcrete) if (p.powder == powder) return p.solid;
        return BlockID::Air;
    }

    bool IsFallingBlock(BlockID id) {
        switch (id) {
            case BlockID::Sand:
            case BlockID::RedSand:
            case BlockID::Gravel:
            case BlockID::SuspiciousSand:
            case BlockID::SuspiciousGravel:
            case BlockID::DragonEgg:
            case BlockID::Scaffolding:
            case BlockID::PointedDripstone:
                return true;
            default:
                return IsConcretePowder(id) || IsAnvil(id);
        }
    }

    bool FallingBlockIsFree(BlockState state) {
        const BlockID id = state.Block();
        if (id == BlockID::Air) return true;
        if (IsFireBlock(id)) return true;
        if (IsLiquidBlock(id)) return true;
        // MC BlockState.canBeReplaced() — the `replaceable()` property, which
        // is what lets sand fall through tall grass and snow layers.
        return BlockRegistry::Get(id).replaceable;
    }

    int FallingBlockDelayAfterPlace(BlockState state) {
        switch (state.Block()) {
            // MC DragonEggBlock.getDelayAfterPlace.
            case BlockID::DragonEgg:   return 5;
            // MC ScaffoldingBlock.TICK_DELAY.
            case BlockID::Scaffolding: return 1;
            case BlockID::PointedDripstone:
                // MC PointedDripstoneBlock.updateShape: one tick when the tip
                // points UP (a stalagmite reacts to losing its floor
                // immediately), two when it points down.
                return state.GetValueByName("tip_direction") == "up" ? 1 : 2;
            default:                   return 2;
        }
    }

    // ── The three hooks ────────────────────────────────────────────────────

    void FallingBlockOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState newState, BlockState /*oldState*/) {
        ScheduleFall(TicksOf(level), pos, newState);
    }

    bool FallingBlockNeighborChanged(const IBlockAccess& /*level*/, const glm::ivec3& pos,
                                     BlockState state,
                                     Direction /*toNeighbour*/, BlockID /*neighbourId*/,
                                     BlockState& /*outState*/,
                                     ScheduledTickAccess* ticks) {
        // MC FallingBlock.updateShape schedules unconditionally and returns the
        // state unchanged. It does NOT check the block below here — that is the
        // tick's job, two ticks later, and the delay is the whole reason a
        // column collapses one block per tick instead of instantaneously.
        ScheduleFall(ticks, pos, state);
        return false;   // no transform
    }

    void FallingBlockTick(ILevelWrite& level, const glm::ivec3& pos,
                          BlockState state, JavaRandom& /*random*/) {
        // MC FallingBlock.tick.
        if (pos.y < World::MIN_Y) return;
        const glm::ivec3 below = pos + kDown;
        if (!FallingBlockIsFree(level.GetBlockState(below.x, below.y, below.z))) return;

        // MC FallingBlockEntity.fall: the carried state loses `waterlogged`,
        // and the vacated cell becomes the FLUID'S legacy block — water if the
        // block was logged, air otherwise. Writing air unconditionally is the
        // classic way to make a waterlogged slab eat the water it was in.
        BlockState carried = state;
        BlockID    replacement = BlockID::Air;
        if (BlockRegistry::ContainsWater(state) &&
            BlockRegistry::IsWaterloggable(state.Block())) {
            carried     = BlockRegistry::WithWaterlogged(state, false);
            replacement = BlockID::Water;
        }

        // MC setBlock(pos, ..., 3) = UPDATE_NEIGHBORS | UPDATE_CLIENTS.
        level.SetBlock(pos.x, pos.y, pos.z, replacement, World::UpdateFlags::All);

        FallingBlockEntity* entity = SpawnFallingBlock(level, pos, carried);
        if (!entity) return;

        // MC FallingBlock.falling(entity) — the per-block hook, overridden only
        // by the anvil (and by the dripstone through its own tick path).
        if (IsAnvil(carried.Block())) {
            // MC AnvilBlock: FALL_DAMAGE_PER_DISTANCE 2.0, FALL_DAMAGE_MAX 40.
            entity->SetHurtsEntities(2.0f, FallingBlockEntity::kDefaultFallHurtMax);
        }
        // MC BrushableBlock.tick: a suspicious block that falls yields nothing,
        // because its buried loot only exists while it is un-brushed.
        if (carried.Block() == BlockID::SuspiciousSand ||
            carried.Block() == BlockID::SuspiciousGravel) {
            entity->SetCancelDrop(true);
        }
    }

    // ── Concrete powder ────────────────────────────────────────────────────

    namespace {

        bool CanSolidify(BlockState state) {
            // MC ConcretePowderBlock.canSolidify: getFluidState().is(WATER),
            // which is true for water itself AND for any waterlogged block.
            return BlockRegistry::ContainsWater(state);
        }

        // MC ConcretePowderBlock.touchesLiquid, INCLUDING its off-by-one read.
        //
        // Vanilla's loop reads `getBlockState(testPos)` at the TOP, before
        // moving testPos — so on the first iteration it reads `pos` itself and
        // on later ones the PREVIOUS direction's neighbour. That stale read is
        // only consulted for the DOWN branch, where it has the effect of
        // letting powder solidify against water directly beneath it. It looks
        // like a bug and it is load-bearing; reproducing it is why concrete
        // powder dropped into a one-deep pool behaves the way players expect.
        bool TouchesLiquid(const IBlockAccess& level, const glm::ivec3& pos) {
            glm::ivec3 testPos = pos;
            for (int i = 0; i < 6; ++i) {
                const Direction dir = static_cast<Direction>(i);
                BlockState stale = level.GetBlockState(testPos.x, testPos.y, testPos.z);
                if (dir != Direction::Down || CanSolidify(stale)) {
                    testPos = pos + glm::ivec3(StepX(dir), StepY(dir), StepZ(dir));
                    const BlockState neighbour =
                        level.GetBlockState(testPos.x, testPos.y, testPos.z);
                    // The sturdy test is asked of the NEIGHBOUR's state, not of
                    // the powder's cell. MC reuses its `blockState` local — by
                    // this line it already holds the neighbour — and passes
                    // `pos` only as context that isFaceSturdy does not read.
                    //
                    // Asking about `pos` instead answered about the powder,
                    // which is a full cube and therefore sturdy on all six
                    // faces, so the `!sturdy` term was constant-false and this
                    // whole function could never return true: powder next to
                    // water simply never solidified. It only matters for
                    // WATERLOGGED neighbours — plain water is never sturdy —
                    // which is exactly the case MC is excluding here.
                    if (CanSolidify(neighbour) &&
                        !IsStateFaceSturdy(neighbour, Opposite(dir))) {
                        return true;
                    }
                }
            }
            return false;
        }

    } // namespace

    bool ConcretePowderShouldSolidify(const IBlockAccess& level, const glm::ivec3& pos,
                                      BlockState replaced) {
        return CanSolidify(replaced) || TouchesLiquid(level, pos);
    }

    BlockState ConcretePowderPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                            BlockState fallback) {
        // MC ConcretePowderBlock.getStateForPlacement: powder placed INTO or
        // BESIDE water is concrete the instant it lands, not powder that waits
        // for a tick that never comes. `replacedBlock` is whatever occupies the
        // cell right now — the water itself, in the common case.
        const BlockState replaced = level.GetBlockState(pos.x, pos.y, pos.z);
        if (ConcretePowderShouldSolidify(level, pos, replaced)) {
            return BlockStates::Default(ConcreteFor(fallback.Block()));
        }
        return fallback;
    }

    bool ConcretePowderNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state,
                                       Direction toNeighbour, BlockID neighbourId,
                                       BlockState& outState,
                                       ScheduledTickAccess* ticks) {
        // MC ConcretePowderBlock.updateShape: solidifying WINS over falling.
        // The transform path in World::NotifyNeighborBlocks writes the concrete
        // and moves on, so the fall is never scheduled — which is right, since
        // concrete does not fall.
        if (TouchesLiquid(level, pos)) {
            outState = BlockStates::Default(ConcreteFor(state.Block()));
            return true;
        }
        return FallingBlockNeighborChanged(level, pos, state, toNeighbour, neighbourId,
                                           outState, ticks);
    }

    // ── Landing ────────────────────────────────────────────────────────────

    void FallingBlockOnLand(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState placed, BlockState replaced) {
        const BlockID id = placed.Block();

        // MC ConcretePowderBlock.onLand — powder that lands in or beside water
        // becomes concrete on the spot.
        if (IsConcretePowder(id) &&
            ConcretePowderShouldSolidify(level, pos, replaced)) {
            const BlockID solid = ConcreteFor(id);
            level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(solid),
                           World::UpdateFlags::All);
            return;
        }

        // MC AnvilBlock.onLand -> levelEvent 1031 (ANVIL_LAND, volume 0.3).
        if (IsAnvil(id)) {
            PlaySound("block.anvil.land", pos);
        }
    }

    void FallingBlockOnBrokenAfterFall(ILevelWrite& /*level*/, const glm::ivec3& pos,
                                       BlockState state) {
        const BlockID id = state.Block();
        // MC AnvilBlock.onBrokenAfterFall -> levelEvent 1029 (ANVIL_DESTROY).
        if (IsAnvil(id)) {
            PlaySound("block.anvil.destroy", pos);
            return;
        }
        // MC PointedDripstoneBlock.onBrokenAfterFall -> levelEvent 1045.
        if (id == BlockID::PointedDripstone) {
            PlaySound("block.pointed_dripstone.land", pos);
        }
    }

    BlockID AnvilDamaged(BlockID anvil) {
        // MC AnvilBlock.damage — one step down the chain; null (here Air) means
        // the anvil is destroyed outright rather than replaced.
        if (anvil == BlockID::Anvil)        return BlockID::ChippedAnvil;
        if (anvil == BlockID::ChippedAnvil) return BlockID::DamagedAnvil;
        return BlockID::Air;
    }

    // ── Dust ───────────────────────────────────────────────────────────────

    uint32_t FallingBlockDustColor(BlockState state) {
        switch (state.Block()) {
            case BlockID::Sand:             return kSandDust;
            case BlockID::SuspiciousSand:   return kSandDust;
            case BlockID::RedSand:          return kRedSandDust;
            case BlockID::Gravel:           return kGravelDust;
            case BlockID::SuspiciousGravel: return kGravelDust;
            // MC DragonEggBlock.getDustColor returns -16777216 — pure black,
            // NOT the block's map colour, which is COLOR_BLACK (0x191919).
            case BlockID::DragonEgg:        return 0x000000;
            default: break;
        }
        // Everything else is MC's `state.getMapColor(level, pos).col`.
        return BlockRegistry::Get(state.Block()).mapColor;
    }

    // ── Scaffolding (MC ScaffoldingBlock) ──────────────────────────────────
    //
    // Scaffolding does not fall because the block below went away — it falls
    // because its `distance` from a solid anchor reached 7. That distance is a
    // BFS the block re-runs for itself every tick, propagating one step per
    // tick from each neighbour, which is why a long horizontal run collapses
    // progressively from the far end rather than all at once.

    namespace {
        constexpr int kScaffoldMaxDistance = 7;   // MC STABILITY_MAX_DISTANCE

        BlockState ScaffoldStateWith(BlockState state, int distance, bool bottom) {
            const BlockID id = state.Block();
            const auto& def = BlockRegistry::GetStateDefinition(id);
            BlockRegistry::BlockStateDefinition::PropertyMap props =
                def.PropertiesOf(state.Index());
            props["distance"] = std::to_string(distance);
            props["bottom"]   = bottom ? "true" : "false";
            return BlockStates::FromIndex(id, def.IndexOf(props));
        }

        int ScaffoldDistanceOf(BlockState state) {
            const std::string_view v = state.GetValueByName("distance");
            if (v.empty()) return kScaffoldMaxDistance;
            return std::atoi(std::string(v).c_str());
        }
    } // namespace

    int ScaffoldingDistance(const IBlockAccess& level, const glm::ivec3& pos) {
        // MC ScaffoldingBlock.getDistance, verbatim including the early
        // return: a sturdy face directly below is distance 0 and short-circuits
        // the horizontal scan entirely.
        const glm::ivec3 below = pos + kDown;
        const BlockState belowState = level.GetBlockState(below.x, below.y, below.z);

        int distance = kScaffoldMaxDistance;
        if (belowState.Is(BlockID::Scaffolding)) {
            distance = ScaffoldDistanceOf(belowState);
        } else if (IsFaceSturdyAt(level, below, Direction::Up)) {
            return 0;
        }

        for (int i = 0; i < 4; ++i) {
            const Direction dir = HorizontalFacingFromIndex(i);
            const glm::ivec3 n = pos + glm::ivec3(StepX(dir), StepY(dir), StepZ(dir));
            const BlockState ns = level.GetBlockState(n.x, n.y, n.z);
            if (ns.Is(BlockID::Scaffolding)) {
                distance = std::min(distance, ScaffoldDistanceOf(ns) + 1);
                if (distance == 1) break;
            }
        }
        return distance;
    }

    bool ScaffoldingCanSurvive(const IBlockAccess& level, const glm::ivec3& pos) {
        // MC ScaffoldingBlock.canSurvive: getDistance(level, pos) < 7.
        //
        // Without this, scaffolding placed anywhere at all — mid-air, or seven
        // blocks past the last anchor — stuck, because the generic
        // "something solid below" heuristic never applies to a block whose
        // whole support model is a horizontal distance walk.
        return ScaffoldingDistance(level, pos) < kScaffoldMaxDistance;
    }

    BlockState ScaffoldingPlacementState(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState fallback) {
        // MC ScaffoldingBlock.getStateForPlacement — DISTANCE and BOTTOM are
        // both resolved at placement time. Leaving them at the state default
        // put every freshly placed piece at DISTANCE=7, i.e. one tick from
        // collapsing, and made BOTTOM wrong for the whole tower.
        const int distance = ScaffoldingDistance(level, pos);
        const bool bottom  = distance > 0 &&
                             !level.GetBlockState(pos.x, pos.y - 1, pos.z)
                                   .Is(BlockID::Scaffolding);
        return ScaffoldStateWith(fallback, distance, bottom);
    }

    void ScaffoldingOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                            BlockState newState, BlockState /*oldState*/) {
        if (level.IsClientSide()) return;      // MC guards this one explicitly
        if (auto* ticks = level.Ticks()) {
            ticks->ScheduleTick(pos, newState.Block(), 1);
        }
    }

    bool ScaffoldingNeighborChanged(const IBlockAccess& /*level*/, const glm::ivec3& pos,
                                    BlockState state,
                                    Direction /*toNeighbour*/, BlockID /*neighbourId*/,
                                    BlockState& /*outState*/,
                                    ScheduledTickAccess* ticks) {
        if (ticks) ticks->ScheduleTick(pos, state.Block(), 1);
        return false;
    }

    void ScaffoldingTick(ILevelWrite& level, const glm::ivec3& pos,
                         BlockState state, JavaRandom& /*random*/) {
        const int distance = ScaffoldingDistance(level, pos);
        const bool bottom  = distance > 0 &&
                             !level.GetBlockState(pos.x, pos.y - 1, pos.z)
                                   .Is(BlockID::Scaffolding);
        const BlockState updated = ScaffoldStateWith(state, distance, bottom);

        if (distance == kScaffoldMaxDistance) {
            // TWO ticks at distance 7 before it falls, not one. The first tick
            // writes the 7 and the second sees it was already 7 — that is what
            // gives a collapsing tower its visible one-block-per-tick ripple
            // instead of vanishing whole.
            if (ScaffoldDistanceOf(state) == kScaffoldMaxDistance) {
                BlockState carried = updated;
                BlockID replacement = BlockID::Air;
                if (BlockRegistry::ContainsWater(updated)) {
                    carried     = BlockRegistry::WithWaterlogged(updated, false);
                    replacement = BlockID::Water;
                }
                level.SetBlock(pos.x, pos.y, pos.z, replacement, World::UpdateFlags::All);
                SpawnFallingBlock(level, pos, carried);
            } else {
                // MC level.destroyBlock(pos, true) — breaks WITH drops.
                DestroyBlockWithDrops(level, pos);
            }
        } else if (updated.RawId() != state.RawId()) {
            level.SetBlock(pos.x, pos.y, pos.z, updated, World::UpdateFlags::All);
        }
    }

    // ── Pointed dripstone (MC PointedDripstoneBlock) ───────────────────────
    //
    // Only the FALLING half is modelled here: a stalagmite whose floor went
    // breaks, and a stalactite whose ceiling went collapses as a column of
    // falling entities. Growth, water/lava dripping and cauldron filling are
    // separate mechanisms on randomTick and are not part of this work — they
    // are named at the tick site so their absence is visible rather than
    // mysterious.

    namespace {
        bool DripstoneTipDown(BlockState s) {
            return s.Is(BlockID::PointedDripstone) &&
                   s.GetValueByName("tip_direction") == "down";
        }
        bool DripstoneTipUp(BlockState s) {
            return s.Is(BlockID::PointedDripstone) &&
                   s.GetValueByName("tip_direction") == "up";
        }
        // MC isTip(state, includeMergedTip=true).
        bool DripstoneIsTip(BlockState s) {
            if (!s.Is(BlockID::PointedDripstone)) return false;
            const std::string_view t = s.GetValueByName("thickness");
            return t == "tip" || t == "tip_merge";
        }
        // MC isValidPointedDripstonePlacement: the cell BEHIND the tip (i.e.
        // above a stalactite, below a stalagmite) is either sturdy on the
        // facing side or another dripstone pointing the same way.
        bool DripstoneSupported(const IBlockAccess& level, const glm::ivec3& pos,
                                bool tipDown) {
            const Direction behind = tipDown ? Direction::Up : Direction::Down;
            const glm::ivec3 b = pos + glm::ivec3(StepX(behind), StepY(behind), StepZ(behind));
            const BlockState bs = level.GetBlockState(b.x, b.y, b.z);
            if (bs.Is(BlockID::PointedDripstone)) {
                return (tipDown ? DripstoneTipDown(bs) : DripstoneTipUp(bs));
            }
            return IsFaceSturdyAt(level, b, tipDown ? Direction::Down : Direction::Up);
        }

        // MC isPointedDripstoneWithDirection.
        bool DripstoneWithDirection(BlockState s, bool tipDown) {
            return tipDown ? DripstoneTipDown(s) : DripstoneTipUp(s);
        }
        // MC isTip(state, includeMergedTip=false).
        bool DripstoneIsUnmergedTip(BlockState s) {
            return s.Is(BlockID::PointedDripstone) &&
                   s.GetValueByName("thickness") == "tip";
        }

        BlockState DripstoneStateWith(BlockState state, bool tipDown,
                                      std::string_view thickness) {
            const BlockID id = state.Block();
            const auto& def = BlockRegistry::GetStateDefinition(id);
            BlockRegistry::BlockStateDefinition::PropertyMap props =
                def.PropertiesOf(state.Index());
            props["tip_direction"] = tipDown ? "down" : "up";
            props["thickness"]     = std::string(thickness);
            return BlockStates::FromIndex(id, def.IndexOf(props));
        }

        // MC PointedDripstoneBlock.calculateDripstoneThickness, verbatim.
        //
        // A dripstone's THICKNESS is not a property of the block, it is a
        // property of its position in the column: the cell in front (along the
        // tip) and the cell behind decide between tip / tip_merge / frustum /
        // middle / base. That is why breaking one segment has to re-ask every
        // survivor — a MIDDLE that loses the segment below it is a TIP now, and
        // leaving the old value behind gives it both the wrong model and the
        // wrong collision box.
        std::string_view DripstoneThicknessAt(const IBlockAccess& level,
                                              const glm::ivec3& pos,
                                              bool tipDown, bool mergeOpposingTips) {
            const Direction tip  = tipDown ? Direction::Down : Direction::Up;
            const Direction base = Opposite(tip);

            const glm::ivec3 front =
                pos + glm::ivec3(StepX(tip), StepY(tip), StepZ(tip));
            const BlockState inFront = level.GetBlockState(front.x, front.y, front.z);

            // Facing us: two tips meeting nose to nose.
            if (DripstoneWithDirection(inFront, !tipDown)) {
                return (!mergeOpposingTips &&
                        inFront.GetValueByName("thickness") != "tip_merge")
                    ? "tip" : "tip_merge";
            }
            // Nothing continuing the column past us: we are the tip.
            if (!DripstoneWithDirection(inFront, tipDown)) return "tip";

            const std::string_view frontThickness = inFront.GetValueByName("thickness");
            if (frontThickness != "tip" && frontThickness != "tip_merge") {
                const glm::ivec3 behind =
                    pos + glm::ivec3(StepX(base), StepY(base), StepZ(base));
                const BlockState behindState =
                    level.GetBlockState(behind.x, behind.y, behind.z);
                return !DripstoneWithDirection(behindState, tipDown) ? "base" : "middle";
            }
            return "frustum";
        }
    } // namespace

    bool PointedDripstoneCanSurvive(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state) {
        // MC canSurvive -> isValidPointedDripstonePlacement(level, pos,
        // state.getValue(TIP_DIRECTION)).
        return DripstoneSupported(level, pos, DripstoneTipDown(state));
    }

    BlockState PointedDripstonePlacementState(const IBlockAccess& level,
                                              const glm::ivec3& pos,
                                              BlockState fallback,
                                              bool defaultTipDown, bool secondaryUse) {
        // MC getStateForPlacement:
        //   defaultTipDirection = context.getNearestLookingVerticalDirection()
        //                                .getOpposite()
        // Note the OPPOSITE: looking DOWN gives a tip pointing UP, i.e. a
        // stalagmite standing on the floor you are looking at. Looking up gives
        // a stalactite hanging from the ceiling. calculateTipDirection then
        // falls back to the other orientation when the preferred side has
        // nothing to hold it, so a click that would otherwise fail still works.
        bool tipDown = defaultTipDown;
        if (!DripstoneSupported(level, pos, tipDown)) {
            if (!DripstoneSupported(level, pos, !tipDown)) {
                // MC returns null — no valid placement. Nothing here can say
                // "refuse", so hand back the fallback and let the caller's
                // CanSurviveAt gate reject it, which reaches the same outcome.
                return fallback;
            }
            tipDown = !tipDown;
        }

        // MC: mergeOpposingTips = !context.isSecondaryUseActive() — sneaking
        // while placing keeps two meeting tips separate instead of merging.
        const std::string_view thickness =
            DripstoneThicknessAt(level, pos, tipDown, !secondaryUse);
        return DripstoneStateWith(fallback, tipDown, thickness);
    }

    bool PointedDripstoneNeighborChanged(const IBlockAccess& level, const glm::ivec3& pos,
                                         BlockState state,
                                         Direction toNeighbour, BlockID /*neighbourId*/,
                                         BlockState& outState,
                                         ScheduledTickAccess* ticks) {
        // MC updateShape: only vertical neighbours matter. A dripstone ignores
        // everything happening beside it.
        if (toNeighbour != Direction::Up && toNeighbour != Direction::Down) return false;
        const bool tipDown = DripstoneTipDown(state);

        // MC bails when a stalactite already has an appointment, so a column
        // losing its ceiling schedules once rather than once per segment.
        if (tipDown && ticks && ticks->HasScheduledTick(pos, state.Block())) return false;

        const Direction behind = tipDown ? Direction::Up : Direction::Down;
        if (toNeighbour == behind && !DripstoneSupported(level, pos, tipDown)) {
            if (ticks) ticks->ScheduleTick(pos, state.Block(), tipDown ? 2 : 1);
            return false;
        }

        // The support is fine, so this is a SHAPE change: MC recomputes
        // THICKNESS and returns the new state. Missing this left every
        // survivor of a broken column carrying the thickness it had when the
        // column was whole — a MIDDLE segment whose neighbour went still
        // rendering (and colliding) as a mid-column piece rather than a tip.
        //
        // mergeOpposingTips = state.getValue(THICKNESS) == TIP_MERGE, so a
        // pair that was already merged stays merged.
        const bool mergeOpposingTips = state.GetValueByName("thickness") == "tip_merge";
        const std::string_view thickness =
            DripstoneThicknessAt(level, pos, tipDown, mergeOpposingTips);
        const BlockState updated = DripstoneStateWith(state, tipDown, thickness);
        if (updated.RawId() == state.RawId()) return false;
        outState = updated;
        return true;
    }

    void PointedDripstoneTick(ILevelWrite& level, const glm::ivec3& pos,
                              BlockState state, JavaRandom& /*random*/) {
        // NOT MODELLED (MC PointedDripstoneBlock.randomTick): growth at
        // p=0.011377778, water transfer at 0.17578125 and lava at 0.05859375,
        // and cauldron filling. Those live on the random tick, not this one,
        // and are a separate feature from falling.
        if (DripstoneTipUp(state)) {
            if (!DripstoneSupported(level, pos, /*tipDown=*/false)) {
                DestroyBlockWithDrops(level, pos);
            }
            return;
        }

        // MC spawnFallingStalactite: walk DOWN from here, turning every
        // segment into its own falling entity, and stop at the tip — which is
        // the one that hurts.
        glm::ivec3 fallPos = pos;
        for (int guard = 0; guard < 64; ++guard) {
            const BlockState fallState =
                level.GetBlockState(fallPos.x, fallPos.y, fallPos.z);
            if (!DripstoneTipDown(fallState)) break;

            BlockState carried = fallState;
            BlockID replacement = BlockID::Air;
            if (BlockRegistry::ContainsWater(fallState)) {
                carried     = BlockRegistry::WithWaterlogged(fallState, false);
                replacement = BlockID::Water;
            }
            level.SetBlock(fallPos.x, fallPos.y, fallPos.z, replacement,
                           World::UpdateFlags::All);

            FallingBlockEntity* entity = SpawnFallingBlock(level, fallPos, carried);
            if (!entity) return;

            if (DripstoneIsTip(fallState)) {
                // MC: `size = Math.max(1 + pos.getY() - fallPos.getY(), 6)`.
                // That is a MAX, not a min — a one-block stalactite still does
                // 6 damage per fall distance. Transcribed as written.
                const int size = std::max(1 + pos.y - fallPos.y, 6);
                entity->SetHurtsEntities(1.0f * static_cast<float>(size),
                                         FallingBlockEntity::kDefaultFallHurtMax);
                break;
            }
            fallPos.y -= 1;
        }
    }

    void FallingBlockAnimateTick(EntityLevel& level, const glm::ivec3& pos,
                                 BlockState state, JavaRandom& random) {
        // MC FallingBlock.animateTick: one in sixteen, and only while the cell
        // below is actually free — an unsupported block trickles dust, a
        // supported one does not.
        if (random.NextInt(16) != 0) return;

        const IBlockAccess* blocks = level.Blocks();
        if (!blocks) return;
        const glm::ivec3 below = pos + kDown;
        if (!FallingBlockIsFree(blocks->GetBlockState(below.x, below.y, below.z))) return;

        // MC ParticleUtils.spawnParticleBelow: a random point on the block's
        // underside, 0.05 below it, with no velocity of its own.
        const uint32_t rgb = FallingBlockDustColor(state);
        level.AddColorParticle(
            ParticleKind::FallingDust,
            static_cast<double>(pos.x) + random.NextDouble(),
            static_cast<double>(pos.y) - 0.05,
            static_cast<double>(pos.z) + random.NextDouble(),
            0.0, 0.0, 0.0,
            static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8)  & 0xFF) / 255.0f,
            static_cast<float>( rgb        & 0xFF) / 255.0f,
            1.0f);
    }

} // namespace Game
