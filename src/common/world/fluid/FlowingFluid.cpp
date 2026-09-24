// File: src/common/world/fluid/FlowingFluid.cpp
#include "common/world/fluid/FlowingFluid.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/LevelEventSounds.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/ShapeOcclusion.hpp"
#include "common/world/chunk/IBlockAccess.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace Game {

    namespace {

        using BlockShape    = BlockRegistry::BlockShape;
        using BlockShapeSet = BlockRegistry::BlockShapeSet;

        constexpr Direction kHorizontal[4] = {
            Direction::North, Direction::South, Direction::West, Direction::East
        };
        // MC LiquidBlock.POSSIBLE_FLOW_DIRECTIONS.
        constexpr Direction kPossibleFlowDirections[5] = {
            Direction::Down, Direction::South, Direction::North, Direction::East, Direction::West
        };

        constexpr glm::ivec3 kUp{0, 1, 0};
        constexpr glm::ivec3 kDown{0, -1, 0};

        glm::ivec3 Relative(const glm::ivec3& p, Direction d) {
            return glm::ivec3(p.x + StepX(d), p.y + StepY(d), p.z + StepZ(d));
        }

        // MC EnvironmentAttributes.FAST_LAVA — set by the nether dimension.
        bool IsFastLava(const ILevelWrite& level) {
            return level.GetDimension() == DimensionId::Nether;
        }

        // ── Per-fluid parameters (WaterFluid / LavaFluid) ──────────────────

        // MC FlowingFluid.getSlopeFindDistance: water 4; lava 2, 4 in the nether.
        int SlopeFindDistance(const ILevelWrite& level, FluidType type) {
            if (type == FluidType::Lava) return IsFastLava(level) ? 4 : 2;
            return 4;
        }

        // MC FlowingFluid.getDropOff: water 1; lava 2, 1 in the nether.
        int DropOff(const ILevelWrite& level, FluidType type) {
            if (type == FluidType::Lava) return IsFastLava(level) ? 1 : 2;
            return 1;
        }

        // MC FlowingFluid.canConvertToSource — the two *_source_conversion
        // game rules (water true, lava false by default).
        bool CanConvertToSource(FluidType type) {
            return type == FluidType::Lava
                ? Rules::GetBool(Rules::Id::LavaSourceConversion)
                : Rules::GetBool(Rules::Id::WaterSourceConversion);
        }

        // MC Fluid.canBeReplacedWith(state, level, pos, other, direction) —
        // may the fluid `state` sitting at `pos` be overwritten by `other`
        // arriving from `direction`?
        //   EmptyFluid: always.
        //   WaterFluid: only from above, and only by something that is not
        //               water (lava falling onto water).
        //   LavaFluid:  only by water, and only when the lava stands at
        //               least 4/9 tall (MIN_LEVEL_CUTOFF) — a thin lava film
        //               is not washed away.
        bool CanBeReplacedWith(const ILevelWrite& level, const glm::ivec3& pos,
                               const FluidState& state, FluidType other, Direction direction) {
            switch (state.type) {
                case FluidType::Empty:
                    return true;
                case FluidType::Water:
                    return direction == Direction::Down && other != FluidType::Water;
                case FluidType::Lava:
                    return FluidHeight(level, pos, state) >= 0.44444445f && other == FluidType::Water;
            }
            return false;
        }

        // ── Shapes (MC Shapes.mergedFaceOccludes, reduced to box lists) ────

        // The collision shape as MC's canPassThroughWall classifies it:
        // `== Shapes.block()`, `== Shapes.empty()`, or something in between.
        struct CollisionShape {
            BlockShapeSet set;
            bool empty = true;
            bool full  = false;
        };

        CollisionShape CollisionShapeOf(BlockState state) {
            CollisionShape s;
            // LiquidBlock and AirBlock (and every noCollision block) return
            // Shapes.empty() from getCollisionShape; this engine spells that
            // hasCollision = false.
            if (!BlockRegistry::HasCollision(state.Block())) return s;
            s.set   = BlockRegistry::GetBlockCollisionShapeSet(state);
            s.empty = s.set.count == 0;
            s.full  = s.set.IsFullCube();
            return s;
        }

        // MC FlowingFluid.canPassThroughWall: may fluid cross from
        // `sourceState`'s cell into `targetState`'s cell in `direction`?
        // (The 200-entry occlusion cache vanilla keeps is a Java-side
        // allocation dodge; the box test here is cheap enough to run.)
        bool CanPassThroughWall(Direction direction, BlockState sourceState, BlockState targetState) {
            const CollisionShape target = CollisionShapeOf(targetState);
            if (target.full) return false;
            const CollisionShape source = CollisionShapeOf(sourceState);
            if (source.full) return false;
            if (source.empty && target.empty) return true;
            return !Shapes::MergedFaceOccludes(source.set, source.full, target.set, target.full, direction);
        }

        // ── Holding rules ──────────────────────────────────────────────────

        // MC FlowingFluid.canHoldAnyFluid: a LiquidBlockContainer, or
        // anything in #washed_away_by_fluids (air, the fluids themselves,
        // plants, torches, rails…).
        bool CanHoldAnyFluid(BlockState state) {
            const BlockID id = state.Block();
            return Fluids::IsLiquidBlockContainer(id) || BlockWashedAwayByFluids(id);
        }

        // MC canHoldSpecificFluid: a container is asked; everything else
        // that can hold ANY fluid holds this one.
        bool CanHoldSpecificFluid(BlockState state, const FluidState& newFluid) {
            if (Fluids::IsLiquidBlockContainer(state.Block())) {
                return Fluids::CanPlaceLiquid(state, newFluid);
            }
            return true;
        }

        bool CanHoldFluid(BlockState state, const FluidState& newFluid) {
            return CanHoldAnyFluid(state) && CanHoldSpecificFluid(state, newFluid);
        }

        // MC isSourceBlockOfThisType.
        bool IsSourceBlockOfThisType(const FluidState& state, FluidType type) {
            return state.IsSourceOf(type);
        }

        // MC canMaybePassThrough.
        bool CanMaybePassThrough(FluidType type, BlockState sourceState, Direction direction,
                                 BlockState testState, const FluidState& testFluid) {
            return !IsSourceBlockOfThisType(testFluid, type) &&
                   CanHoldAnyFluid(testState) &&
                   CanPassThroughWall(direction, sourceState, testState);
        }

        // MC canPassThrough(level, this.getFlowing(), …): may FLOWING fluid
        // of this type enter the test cell?
        bool CanPassThrough(FluidType type, BlockState sourceState, Direction direction,
                            BlockState testState, const FluidState& testFluid) {
            return CanMaybePassThrough(type, sourceState, direction, testState, testFluid) &&
                   CanHoldSpecificFluid(testState, FluidState::Flowing(type, 1, false));
        }

        // MC isWaterHole: can fluid drop from `topPos` into `bottomPos`?
        bool IsWaterHole(FluidType type, BlockState topState,
                         BlockState bottomState) {
            if (!CanPassThroughWall(Direction::Down, topState, bottomState)) return false;
            if (FluidStateOf(bottomState).IsSame(type)) return true;
            return CanHoldFluid(bottomState, FluidState::Flowing(type, 1, false));
        }

        // ── getNewLiquid ───────────────────────────────────────────────────

        // MC FlowingFluid.getNewLiquid: what fluid `pos` SHOULD hold given
        // its neighbours. `state` is the block at pos.
        FluidState GetNewLiquid(const ILevelWrite& level, FluidType type,
                                const glm::ivec3& pos, BlockState state) {
            int highestNeighbour = 0;
            int neighbourSources = 0;

            for (Direction direction : kHorizontal) {
                const glm::ivec3 rp = Relative(pos, direction);
                const BlockState  bs = level.GetBlockState(rp.x, rp.y, rp.z);
                const FluidState  fs = FluidStateOf(bs);
                if (fs.IsSame(type) && CanPassThroughWall(direction, state, bs)) {
                    if (fs.IsSource()) ++neighbourSources;
                    highestNeighbour = std::max<int>(highestNeighbour, fs.amount);
                }
            }

            // Two source neighbours over a solid floor (or over a source of
            // the same fluid) make a new source — the infinite water spring.
            if (neighbourSources >= 2 && CanConvertToSource(type)) {
                const glm::ivec3 belowPos = pos + kDown;
                const BlockState belowState = level.GetBlockState(belowPos.x, belowPos.y, belowPos.z);
                const FluidState belowFluid = FluidStateOf(belowState);
                // MC BlockState.isSolid — the legacy "has a collision shape".
                const bool belowSolid = BlockRegistry::HasCollision(belowState.Block());
                if (belowSolid || IsSourceBlockOfThisType(belowFluid, type)) {
                    return FluidState::Source(type);
                }
            }

            const glm::ivec3 abovePos = pos + kUp;
            const BlockState aboveState = level.GetBlockState(abovePos.x, abovePos.y, abovePos.z);
            const FluidState aboveFluid = FluidStateOf(aboveState);
            if (!aboveFluid.IsEmpty() && aboveFluid.IsSame(type) &&
                CanPassThroughWall(Direction::Up, state, aboveState)) {
                return FluidState::Flowing(type, FluidState::kAmountFull, true);
            }

            const int amount = highestNeighbour - DropOff(level, type);
            if (amount <= 0) return FluidState::Empty();
            return FluidState::Flowing(type, amount, false);
        }

        // ── beforeDestroyingBlock / spreadTo ───────────────────────────────

        // MC WaterFluid.beforeDestroyingBlock drops the washed-away block's
        // loot; LavaFluid's fizzes instead (lava burns what it covers).
        void BeforeDestroyingBlock(ILevelWrite& level, FluidType type,
                                   const glm::ivec3& pos, BlockState state) {
            if (type == FluidType::Lava) {
                Fluids::Fizz(level, pos);
            } else {
                DropBlockLoot(level, pos, state);
            }
        }

        // MC FlowingFluid.spreadTo, with LavaFluid's override folded in.
        void SpreadTo(ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                      BlockState state, Direction direction, const FluidState& target) {
            if (type == FluidType::Lava && direction == Direction::Down) {
                // LavaFluid.spreadTo: lava dropping onto water makes stone
                // (a plain water block only — a waterlogged block keeps its
                // block and its water) and never places the lava.
                const FluidState fluidHere = FluidStateOf(state);
                if (fluidHere.Is(FluidType::Water)) {
                    if (state.Block() == BlockID::Water) {
                        level.SetBlock(pos.x, pos.y, pos.z,
                                       BlockStates::Default(BlockID::Stone),
                                       World::UpdateFlags::All);
                    }
                    Fluids::Fizz(level, pos);
                    return;
                }
            }

            if (Fluids::IsLiquidBlockContainer(state.Block())) {
                Fluids::PlaceLiquid(level, pos, state, target);
                return;
            }
            if (state.Block() != BlockID::Air) {
                BeforeDestroyingBlock(level, type, pos, state);
            }
            level.SetBlock(pos.x, pos.y, pos.z, FluidLegacyBlock(target), World::UpdateFlags::All);
        }

        // ── Spread search (getSpread / getSlopeDistance / SpreadContext) ───

        // MC FlowingFluid.SpreadContext: the block states and hole answers
        // the slope search touches, memoised for the duration of one
        // spread. Vanilla keys a hash map on a packed (dx, dz); the search
        // never leaves a small window around the origin (slope distance 4
        // from a neighbour), so a flat 17x17 table does the same job with
        // no allocation.
        class SpreadContext {
        public:
            SpreadContext(const ILevelWrite& level, FluidType type, const glm::ivec3& origin)
                : m_level(level), m_type(type), m_origin(origin) {}

            BlockState GetBlockState(const glm::ivec3& pos) {
                Cell* c = CellFor(pos);
                if (!c) return m_level.GetBlockState(pos.x, pos.y, pos.z);
                if (!c->hasState) {
                    c->state    = m_level.GetBlockState(pos.x, pos.y, pos.z);
                    c->hasState = true;
                }
                return c->state;
            }

            bool IsHole(const glm::ivec3& pos) {
                Cell* c = CellFor(pos);
                if (c && c->hasHole) return c->hole;
                const BlockState state = GetBlockState(pos);
                const glm::ivec3 below = pos + kDown;
                const BlockState belowState = m_level.GetBlockState(below.x, below.y, below.z);
                const bool hole = IsWaterHole(m_type, state, belowState);
                if (c) { c->hole = hole; c->hasHole = true; }
                return hole;
            }

        private:
            struct Cell {
                BlockState state;
                bool hasState = false;
                bool hasHole  = false;
                bool hole     = false;
            };
            static constexpr int kRadius = 8;
            static constexpr int kSize   = kRadius * 2 + 1;

            Cell* CellFor(const glm::ivec3& pos) {
                const int dx = pos.x - m_origin.x + kRadius;
                const int dz = pos.z - m_origin.z + kRadius;
                if (pos.y != m_origin.y || dx < 0 || dx >= kSize || dz < 0 || dz >= kSize) {
                    return nullptr;
                }
                return &m_cells[static_cast<size_t>(dz) * kSize + static_cast<size_t>(dx)];
            }

            const ILevelWrite& m_level;
            FluidType          m_type;
            glm::ivec3         m_origin;
            std::array<Cell, static_cast<size_t>(kSize) * kSize> m_cells{};
        };

        // MC FlowingFluid.getSlopeDistance: how many cells from `pos`, in
        // any direction but `from`, is the nearest drop? 1000 when none
        // within getSlopeFindDistance.
        int GetSlopeDistance(const ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                             int pass, Direction from, BlockState state, SpreadContext& context) {
            int lowest = 1000;
            for (Direction direction : kHorizontal) {
                if (direction == from) continue;
                const glm::ivec3 testPos = Relative(pos, direction);
                const BlockState testState = context.GetBlockState(testPos);
                const FluidState testFluid = FluidStateOf(testState);
                if (!CanPassThrough(type, state, direction, testState, testFluid)) {
                    continue;
                }
                if (context.IsHole(testPos)) return pass;
                if (pass < SlopeFindDistance(level, type)) {
                    const int v = GetSlopeDistance(level, type, testPos, pass + 1,
                                                   Opposite(direction), testState, context);
                    if (v < lowest) lowest = v;
                }
            }
            return lowest;
        }

        struct SpreadTarget {
            Direction  direction;
            FluidState fluid;
        };

        // MC FlowingFluid.getSpread: the sides this cell spreads to this
        // tick — every passable neighbour tied for the shortest distance to
        // a drop — with the fluid each receives.
        void GetSpread(const ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                       BlockState state, std::vector<SpreadTarget>& result) {
            int lowest = 1000;
            result.clear();
            SpreadContext context(level, type, pos);

            for (Direction direction : kHorizontal) {
                const glm::ivec3 testPos = Relative(pos, direction);
                const BlockState testState = level.GetBlockState(testPos.x, testPos.y, testPos.z);
                const FluidState testFluid = FluidStateOf(testState);
                if (!CanMaybePassThrough(type, state, direction, testState, testFluid)) continue;

                const FluidState newFluid = GetNewLiquid(level, type, testPos, testState);
                if (!CanHoldSpecificFluid(testState, newFluid)) continue;

                const int distance = context.IsHole(testPos)
                    ? 0
                    : GetSlopeDistance(level, type, testPos, 1, Opposite(direction), testState, context);

                if (distance < lowest) result.clear();
                if (distance <= lowest) {
                    if (CanBeReplacedWith(level, testPos, testFluid, newFluid.type, direction)) {
                        result.push_back({direction, newFluid});
                    }
                    lowest = distance;
                }
            }
        }

        // MC sourceNeighborCount.
        int SourceNeighbourCount(const ILevelWrite& level, FluidType type, const glm::ivec3& pos) {
            int count = 0;
            for (Direction direction : kHorizontal) {
                const glm::ivec3 p = Relative(pos, direction);
                if (IsSourceBlockOfThisType(GetFluidState(level, p), type)) ++count;
            }
            return count;
        }

        // MC spreadToSides.
        void SpreadToSides(ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                           const FluidState& fluidState, BlockState state) {
            int neighbour = static_cast<int>(fluidState.amount) - DropOff(level, type);
            if (fluidState.falling) neighbour = 7;
            if (neighbour <= 0) return;

            std::vector<SpreadTarget> spreads;
            spreads.reserve(4);
            GetSpread(level, type, pos, state, spreads);
            for (const SpreadTarget& target : spreads) {
                const glm::ivec3 neighbourPos = Relative(pos, target.direction);
                SpreadTo(level, type, neighbourPos,
                         level.GetBlockState(neighbourPos.x, neighbourPos.y, neighbourPos.z),
                         target.direction, target.fluid);
            }
        }

        // MC FlowingFluid.spread: down if possible, else (or additionally,
        // with three source neighbours) sideways.
        void Spread(ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                    BlockState state, const FluidState& fluidState) {
            if (fluidState.IsEmpty()) return;

            const glm::ivec3 belowPos = pos + kDown;
            const BlockState belowState = level.GetBlockState(belowPos.x, belowPos.y, belowPos.z);
            const FluidState belowFluid = FluidStateOf(belowState);
            if (CanMaybePassThrough(type, state, Direction::Down, belowState, belowFluid)) {
                const FluidState newBelowFluid = GetNewLiquid(level, type, belowPos, belowState);
                if (CanBeReplacedWith(level, belowPos, belowFluid, newBelowFluid.type, Direction::Down) &&
                    CanHoldSpecificFluid(belowState, newBelowFluid)) {
                    SpreadTo(level, type, belowPos, belowState, Direction::Down, newBelowFluid);
                    if (SourceNeighbourCount(level, type, pos) >= 3) {
                        SpreadToSides(level, type, pos, fluidState, state);
                    }
                    return;
                }
            }

            if (fluidState.IsSource() || !IsWaterHole(type, state, belowState)) {
                SpreadToSides(level, type, pos, fluidState, state);
            }
        }

        // MC FlowingFluid.getSpreadDelay, with LavaFluid's override: lava
        // that is RISING (a cell being fed more, neither side falling) is
        // three times in four given four times the delay, which is the
        // characteristic slow swell of a lava pool filling.
        int SpreadDelay(ILevelWrite& level, FluidType type, const glm::ivec3& pos,
                        const FluidState& oldFluid, const FluidState& newFluid) {
            int result = Fluids::TickDelay(level, type);
            if (type == FluidType::Lava &&
                !oldFluid.IsEmpty() && !newFluid.IsEmpty() &&
                !oldFluid.falling && !newFluid.falling &&
                FluidHeight(level, pos, newFluid) > FluidHeight(level, pos, oldFluid)) {
                JavaRandom* random = level.Random();
                if (random && random->NextInt(4) != 0) result *= 4;
            }
            return result;
        }

        // ── LiquidBlock ────────────────────────────────────────────────────

        // MC LiquidBlock.shouldSpreadLiquid: the lava/water and lava/blue-ice
        // reactions. Returns false when the cell stopped being lava.
        bool ShouldSpreadLiquid(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            const FluidState fluid = FluidStateOf(state);
            if (!fluid.Is(FluidType::Lava)) return true;

            const glm::ivec3 below = pos + kDown;
            const bool isOverSoulSoil = level.GetBlock(below.x, below.y, below.z) == BlockID::SoulSoil;
            for (Direction direction : kPossibleFlowDirections) {
                const glm::ivec3 neighbourPos = Relative(pos, Opposite(direction));
                if (GetFluidState(level, neighbourPos).Is(FluidType::Water)) {
                    const BlockID convertTo = fluid.IsSource() ? BlockID::Obsidian : BlockID::Cobblestone;
                    level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(convertTo),
                                   World::UpdateFlags::All);
                    Fluids::Fizz(level, pos);
                    return false;
                }
                if (isOverSoulSoil &&
                    level.GetBlock(neighbourPos.x, neighbourPos.y, neighbourPos.z) == BlockID::BlueIce) {
                    level.SetBlock(pos.x, pos.y, pos.z, BlockStates::Default(BlockID::Basalt),
                                   World::UpdateFlags::All);
                    Fluids::Fizz(level, pos);
                    return false;
                }
            }
            return true;
        }

        FluidType TypeOfBlock(BlockID id) {
            return id == BlockID::Lava ? FluidType::Lava : FluidType::Water;
        }

        // MC LiquidBlock.onPlace: book the first tick.
        void LiquidOnPlace(ILevelWrite& level, const glm::ivec3& pos, BlockState newState,
                           BlockState /*oldState*/, bool /*movedByPiston*/) {
            if (ShouldSpreadLiquid(level, pos, newState)) {
                Fluids::ScheduleTick(level, pos, TypeOfBlock(newState.Block()));
            }
        }

        // MC LiquidBlock.neighborChanged: same as onPlace — a neighbour
        // opening up (a block broken beside a pool) is what restarts a
        // still cell.
        void LiquidNeighborChanged(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                                   BlockID /*sourceBlock*/, bool /*movedByPiston*/) {
            if (ShouldSpreadLiquid(level, pos, state)) {
                Fluids::ScheduleTick(level, pos, TypeOfBlock(state.Block()));
            }
        }

        // MC LiquidBlock.updateShape: a source, or a source next door, books
        // a tick. Read-only apart from the appointment — the block itself
        // never changes shape.
        bool LiquidUpdateShape(const IBlockAccess& level, const glm::ivec3& pos, BlockState state,
                               Direction toNeighbour, BlockID /*neighbourId*/,
                               BlockState& /*outState*/, ScheduledTickAccess* ticks) {
            if (!ticks) return false;
            const glm::ivec3 np = Relative(pos, toNeighbour);
            const FluidState self      = FluidStateOf(state);
            const FluidState neighbour = GetFluidState(level, np);
            if (self.IsSource() || neighbour.IsSource()) {
                // The tick delay needs the dimension (fast lava); a shape
                // update only sees the read side of the level.
                int delay = self.Is(FluidType::Lava) ? 30 : 5;
                if (const auto* writable = dynamic_cast<const ILevelWrite*>(&level)) {
                    delay = Fluids::TickDelay(*writable, self.type);
                }
                ticks->ScheduleTick(pos, state.Block(), delay);
            }
            return false;
        }

        // The scheduled tick on the fluid block itself. World routes the
        // water/lava appointments to Fluids::Tick on the cell's FLUID (so a
        // waterlogged fence's water ticks too); this hook is the path for a
        // plain water or lava cell and exists so the block table is honest
        // about the block ticking.
        void LiquidTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom& /*random*/) {
            Fluids::Tick(level, pos, state, FluidStateOf(state));
        }

        void LavaRandomTickHook(ILevelWrite& level, const glm::ivec3& pos, BlockState /*state*/,
                                JavaRandom& random) {
            Fluids::LavaRandomTick(level, pos, random);
        }

        bool LavaIsRandomlyTicking(BlockState /*state*/) {
            // LavaFluid.isRandomlyTicking is unconditionally true.
            return true;
        }

        // MC LavaFluid.hasFlammableNeighbours / isFlammable.
        bool IsFlammable(const ILevelWrite& level, const glm::ivec3& pos) {
            if (!level.IsPositionLoaded(pos.x, pos.y, pos.z)) return false;
            return BlockRegistry::Get(level.GetBlock(pos.x, pos.y, pos.z)).ignitedByLava;
        }

        bool HasFlammableNeighbours(const ILevelWrite& level, const glm::ivec3& pos) {
            for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                                Direction::South, Direction::West, Direction::East}) {
                if (IsFlammable(level, Relative(pos, d))) return true;
            }
            return false;
        }

    } // namespace

    // ── Public surface ─────────────────────────────────────────────────────

    namespace Fluids {

        int TickDelay(const ILevelWrite& level, FluidType type) {
            if (type == FluidType::Lava) return IsFastLava(level) ? 10 : 30;
            return 5;
        }

        void ScheduleTick(ILevelWrite& level, const glm::ivec3& pos, FluidType type) {
            ScheduledTickAccess* ticks = level.Ticks();
            if (!ticks || type == FluidType::Empty) return;
            ticks->ScheduleTick(pos, FluidBlockId(type), TickDelay(level, type));
        }

        void Tick(ILevelWrite& level, const glm::ivec3& pos, BlockState blockState,
                  const FluidState& fluidStateIn) {
            FluidState fluidState = fluidStateIn;
            if (fluidState.IsEmpty()) return;
            const FluidType type = fluidState.type;

            if (!fluidState.IsSource()) {
                const FluidState newFluidState =
                    GetNewLiquid(level, type, pos, level.GetBlockState(pos.x, pos.y, pos.z));
                const int tickDelay = SpreadDelay(level, type, pos, fluidState, newFluidState);
                if (newFluidState.IsEmpty()) {
                    fluidState = newFluidState;
                    blockState = BlockState{};
                    level.SetBlock(pos.x, pos.y, pos.z, blockState, World::UpdateFlags::All);
                } else if (newFluidState != fluidState) {
                    fluidState = newFluidState;
                    blockState = FluidLegacyBlock(fluidState);
                    level.SetBlock(pos.x, pos.y, pos.z, blockState, World::UpdateFlags::All);
                    if (ScheduledTickAccess* ticks = level.Ticks()) {
                        ticks->ScheduleTick(pos, FluidBlockId(type), tickDelay);
                    }
                }
            }

            Spread(level, type, pos, blockState, fluidState);
        }

        void LavaRandomTick(ILevelWrite& level, const glm::ivec3& pos, JavaRandom& random) {
            const int passes = random.NextInt(3);
            if (passes > 0) {
                glm::ivec3 testPos = pos;
                for (int pass = 0; pass < passes; ++pass) {
                    testPos += glm::ivec3(random.NextInt(3) - 1, 1, random.NextInt(3) - 1);
                    if (!level.IsPositionLoaded(testPos.x, testPos.y, testPos.z)) return;
                    const BlockState blockState = level.GetBlockState(testPos.x, testPos.y, testPos.z);
                    if (blockState.Block() == BlockID::Air) {
                        if (HasFlammableNeighbours(level, testPos)) {
                            level.SetBlock(testPos.x, testPos.y, testPos.z,
                                           FireStateFor(level, testPos), World::UpdateFlags::All);
                            return;
                        }
                    } else if (BlockBlocksLavaFireSpread(blockState.Block())) {
                        return;
                    }
                }
            } else {
                for (int i = 0; i < 3; ++i) {
                    const glm::ivec3 testPos = pos + glm::ivec3(random.NextInt(3) - 1, 0, random.NextInt(3) - 1);
                    if (!level.IsPositionLoaded(testPos.x, testPos.y, testPos.z)) return;
                    const glm::ivec3 above = testPos + kUp;
                    if (level.GetBlock(above.x, above.y, above.z) == BlockID::Air &&
                        IsFlammable(level, testPos)) {
                        // MC passes testPos (the flammable block), not the
                        // cell the fire goes in, to getState — kept.
                        level.SetBlock(above.x, above.y, above.z,
                                       FireStateFor(level, testPos), World::UpdateFlags::All);
                    }
                }
            }
        }

        bool IsLiquidBlockContainer(BlockID id) {
            return BlockRegistry::IsWaterloggable(id) || BlockRegistry::IsAlwaysWaterlogged(id);
        }

        bool CanPlaceLiquid(BlockState state, const FluidState& fluid) {
            const BlockID id = state.Block();
            if (BlockRegistry::IsWaterloggable(id)) {
                // SimpleWaterloggedBlock.canPlaceLiquid: `type == Fluids.WATER`.
                return fluid.type == FluidType::Water && fluid.IsSource();
            }
            // KelpBlock / SeagrassBlock / TallSeagrassBlock / BubbleColumnBlock
            // all answer false.
            return false;
        }

        bool PlaceLiquid(ILevelWrite& level, const glm::ivec3& pos, BlockState state,
                         const FluidState& fluid) {
            if (!BlockRegistry::IsWaterloggable(state.Block())) return false;
            if (BlockRegistry::ContainsWater(state)) return false;
            if (!(fluid.type == FluidType::Water && fluid.IsSource())) return false;
            if (!level.IsClientSide()) {
                level.SetBlock(pos.x, pos.y, pos.z, BlockRegistry::WithWaterlogged(state, true),
                               World::UpdateFlags::All);
                ScheduleTick(level, pos, FluidType::Water);
            }
            return true;
        }

        void Fizz(ILevelWrite& level, const glm::ivec3& pos) {
            // MC LiquidBlock.fizz → levelEvent 1501: LAVA_EXTINGUISH at 0.5
            // volume, pitch 2.6 + (r - r) * 0.8, and eight LARGE_SMOKE
            // particles. The particles have no channel to the client from here.
            PlayLevelEventSound(level, nullptr, LevelEvent::LAVA_FIZZ, pos, 0, level.Random());
        }

        BlockState FireStateFor(const IBlockAccess& level, const glm::ivec3& pos) {
            const BlockID below = level.GetBlock(pos.x, pos.y - 1, pos.z);
            return BlockStates::Default(IsSoulFireBaseBlock(below) ? BlockID::SoulFire : BlockID::Fire);
        }

    } // namespace Fluids

    void BlockRegistry_RegisterFluids(std::array<Block, BlockRegistry::Size>& blocks) {
        for (BlockID id : {BlockID::Water, BlockID::Lava}) {
            Block& b = blocks[static_cast<size_t>(id)];
            b.onPlace         = &LiquidOnPlace;
            b.neighborChanged = &LiquidNeighborChanged;
            b.updateShape     = &LiquidUpdateShape;
            b.tick            = &LiquidTick;
        }
        Block& lava = blocks[static_cast<size_t>(BlockID::Lava)];
        lava.isRandomlyTicking = &LavaIsRandomlyTicking;
        lava.randomTick        = &LavaRandomTickHook;

        // Resolve the fluid block tags for every block now, on this thread,
        // so the mesher workers (FluidFlow → BlockBlocksFluidFlow) and the
        // server never touch DataTags concurrently with a resource reload.
        for (size_t i = 0; i < blocks.size(); ++i) {
            const BlockID id = static_cast<BlockID>(i);
            (void)BlockBlocksFluidFlow(id);
            (void)BlockWashedAwayByFluids(id);
            (void)BlockBlocksLavaFireSpread(id);
            (void)IsSoulFireBaseBlock(id);
        }
        Log::Info("[Fluids] water and lava wired (spread, lava fire, lava/water reactions)");
    }

} // namespace Game
