// File: src/common/world/block/RedstoneWire.cpp
#include "RedstoneWire.hpp"

#include "BlockPlacement.hpp"       // IsFaceSturdyAt, IsTrapdoorBlock
#include "BlockRegistry.hpp"
#include "RedstonePlus.hpp"
#include "RedstoneSignal.hpp"
#include "RedstoneStateUtil.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Game {

    namespace {

        // MC PROPERTY_BY_DIRECTION order: NORTH, EAST, SOUTH, WEST.
        constexpr Direction kOrder[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };
        constexpr PropertyId kSideProps[4] = {
            PropertyId::NORTH_REDSTONE, PropertyId::EAST_REDSTONE,
            PropertyId::SOUTH_REDSTONE, PropertyId::WEST_REDSTONE,
        };

        int IndexOfDir(Direction d) {
            for (int i = 0; i < 4; ++i) if (kOrder[i] == d) return i;
            return 0;
        }

        // MC RedstoneWireBlock.shouldSignal — cleared while the wire asks
        // its neighbours for THEIR power so it does not count itself. A
        // single flag serves, as vanilla has one block instance; the server
        // tick is single-threaded through here.
        bool s_shouldSignal = true;

        RedstoneSide SideFromIndex(int i) {
            switch (i) {
                case 0:  return RedstoneSide::Up;
                case 1:  return RedstoneSide::Side;
                default: return RedstoneSide::None;
            }
        }
        int IndexOfSide(RedstoneSide s) {
            return s == RedstoneSide::Up ? 0 : (s == RedstoneSide::Side ? 1 : 2);
        }

        BlockState WithSide(BlockState state, Direction dir, RedstoneSide side) {
            return state.SetIndex(kSideProps[IndexOfDir(dir)], IndexOfSide(side));
        }

        // MC getConnectingSide(level, pos, direction, canConnectUp).
        RedstoneSide ConnectingSide(const IBlockAccess& level, const glm::ivec3& pos,
                                    Direction direction, bool canConnectUp) {
            const glm::ivec3 relativePos = Relative(pos, direction);
            const BlockState relativeState = level.GetBlockState(relativePos.x, relativePos.y, relativePos.z);
            if (canConnectUp) {
                const bool isPlaceableAbove = IsTrapdoorBlock(relativeState.Block()) ||
                                              RedstoneCanSurviveOn(level, relativePos);
                if (isPlaceableAbove &&
                    RedstoneShouldConnectTo(level, Above(relativePos), direction, false)) {
                    if (IsFaceSturdyAt(level, relativePos, Opposite(direction))) {
                        return RedstoneSide::Up;
                    }
                    return RedstoneSide::Side;
                }
            }
            if (RedstoneShouldConnectTo(level, relativePos, direction, true)) return RedstoneSide::Side;
            if (IsRedstoneConductor(level, relativePos, relativeState)) return RedstoneSide::None;
            return RedstoneShouldConnectTo(level, Below(relativePos), direction, false)
                ? RedstoneSide::Side : RedstoneSide::None;
        }

        RedstoneSide ConnectingSide(const IBlockAccess& level, const glm::ivec3& pos, Direction direction) {
            return ConnectingSide(level, pos, direction,
                                  !IsRedstoneConductor(level, Above(pos)));
        }

        // MC getMissingConnections: only sides NOT already connected are
        // resolved from the world.
        BlockState MissingConnections(const IBlockAccess& level, BlockState state, const glm::ivec3& pos) {
            const bool canConnectUp = !IsRedstoneConductor(level, Above(pos));
            for (Direction d : kHorizontalPlane) {
                if (!IsConnected(RedstoneSideOf(state, d))) {
                    state = WithSide(state, d, ConnectingSide(level, pos, d, canConnectUp));
                }
            }
            return state;
        }

        BlockState CrossState(int power) {
            return RedstoneStateFrom(RedstoneSide::Side, RedstoneSide::Side,
                                     RedstoneSide::Side, RedstoneSide::Side, power);
        }

        // ── Power ────────────────────────────────────────────────────────

        // MC RedstoneWireBlock.getBlockSignal.
        int BlockSignal(const IBlockAccess& level, const glm::ivec3& pos) {
            s_shouldSignal = false;
            const int blockSignal = GetBestNeighborSignal(level, pos);
            s_shouldSignal = true;
            return blockSignal;
        }

        int WireSignal(BlockState state) {
            return state.Is(BlockID::RedstoneWire) ? PowerOf(state) : 0;
        }

        // MC RedstoneWireEvaluator.getIncomingWireSignal.
        int IncomingWireSignal(const IBlockAccess& level, const glm::ivec3& pos) {
            int wireSignal = 0;
            for (Direction direction : kHorizontalPlane) {
                const glm::ivec3 neighborPos = Relative(pos, direction);
                const BlockState neighborState = level.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z);
                wireSignal = std::max(wireSignal, WireSignal(neighborState));
                const glm::ivec3 abovePos = Above(pos);
                if (IsRedstoneConductor(level, neighborPos, neighborState) &&
                    !IsRedstoneConductor(level, abovePos)) {
                    const glm::ivec3 p = Above(neighborPos);
                    wireSignal = std::max(wireSignal, WireSignal(level.GetBlockState(p.x, p.y, p.z)));
                } else if (!IsRedstoneConductor(level, neighborPos, neighborState)) {
                    const glm::ivec3 p = Below(neighborPos);
                    wireSignal = std::max(wireSignal, WireSignal(level.GetBlockState(p.x, p.y, p.z)));
                }
            }
            return std::max(0, wireSignal - 1);
        }

        // MC DefaultRedstoneWireEvaluator.calculateTargetStrength.
        int CalculateTargetStrength(const IBlockAccess& level, const glm::ivec3& pos) {
            const int blockSignal = BlockSignal(level, pos);
            return blockSignal == 15 ? blockSignal
                                     : std::max(blockSignal, IncomingWireSignal(level, pos));
        }

        // ── redstone_plus: the network evaluator ─────────────────────────
        //
        // Without decay the recursive evaluator above cannot turn a network
        // off: a wire that lost its source still sees a neighbour at the
        // same strength, which it would keep. So the rule evaluates the way
        // MC's ExperimentalRedstoneWireEvaluator does — the whole connected
        // network in one pass, wires ignoring updates that come from wires
        // (RedstoneWireNeighborChanged) so the pass is not repeated once per
        // cell — with the `- 1` gone: every wire's strength is the least
        // fixpoint of max(block signal, strength of the wires feeding it).
        //
        // The feeding relation is IncomingWireSignal's, kept exactly (it is
        // not quite symmetric: dust on glowstone feeds the dust one step
        // down beside it, which does not feed it back), so the network is
        // gathered over the geometric neighbourhood and the fixpoint walks
        // the real edges. One pass costs the size of the network, and a
        // network is evaluated once per event, not once per cell.

        // The wires that IncomingWireSignal(pos) would read from.
        void IncomingWires(const IBlockAccess& level, const glm::ivec3& pos,
                           std::vector<glm::ivec3>& out) {
            out.clear();
            const bool aboveConductor = IsRedstoneConductor(level, Above(pos));
            for (Direction direction : kHorizontalPlane) {
                const glm::ivec3 neighborPos = Relative(pos, direction);
                const BlockState neighborState = level.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z);
                if (neighborState.Is(BlockID::RedstoneWire)) out.push_back(neighborPos);
                if (IsRedstoneConductor(level, neighborPos, neighborState) && !aboveConductor) {
                    const glm::ivec3 p = Above(neighborPos);
                    if (level.GetBlockState(p.x, p.y, p.z).Is(BlockID::RedstoneWire)) out.push_back(p);
                } else if (!IsRedstoneConductor(level, neighborPos, neighborState)) {
                    const glm::ivec3 p = Below(neighborPos);
                    if (level.GetBlockState(p.x, p.y, p.z).Is(BlockID::RedstoneWire)) out.push_back(p);
                }
            }
        }

        // Every wire that could feed or be fed by `pos` (the geometric
        // neighbourhood: the four horizontal cells and the cells one up and
        // one down beside them), for gathering the network both ways.
        void GeometricWires(const IBlockAccess& level, const glm::ivec3& pos,
                            std::vector<glm::ivec3>& out) {
            out.clear();
            for (Direction direction : kHorizontalPlane) {
                const glm::ivec3 n = Relative(pos, direction);
                for (const glm::ivec3& p : {n, Above(n), Below(n)}) {
                    if (level.GetBlockState(p.x, p.y, p.z).Is(BlockID::RedstoneWire)) out.push_back(p);
                }
            }
        }

        struct IVec3Hash {
            size_t operator()(const glm::ivec3& v) const noexcept {
                const uint64_t x = static_cast<uint64_t>(static_cast<uint32_t>(v.x));
                const uint64_t y = static_cast<uint64_t>(static_cast<uint32_t>(v.y));
                const uint64_t z = static_cast<uint64_t>(static_cast<uint32_t>(v.z));
                return static_cast<size_t>((x * 0x9E3779B97F4A7C15ull) ^ (y * 0xC2B2AE3D27D4EB4Full) ^ (z * 0x165667B19E3779F9ull));
            }
        };

        void UpdatePowerStrengthPlus(ILevelWrite& level, const glm::ivec3& initialPos) {
            // 1. The network: the closure of the geometric neighbourhood from
            //    the initial cell (or, when that cell is no longer a wire —
            //    it was just removed — from the wires around it).
            std::vector<glm::ivec3> network;
            std::unordered_map<glm::ivec3, int, IVec3Hash> index;
            std::vector<glm::ivec3> scratch;
            const auto push = [&](const glm::ivec3& p) {
                if (index.emplace(p, static_cast<int>(network.size())).second) network.push_back(p);
            };
            if (level.GetBlockState(initialPos.x, initialPos.y, initialPos.z).Is(BlockID::RedstoneWire)) {
                push(initialPos);
            } else {
                GeometricWires(level, initialPos, scratch);
                for (const glm::ivec3& p : scratch) push(p);
            }
            for (size_t i = 0; i < network.size(); ++i) {
                const glm::ivec3 p = network[i];          // copy: network grows
                GeometricWires(level, p, scratch);
                for (const glm::ivec3& q : scratch) push(q);
            }
            if (network.empty()) return;

            // 2. The fixpoint. Block signals first, then strengths flow along
            //    the feeding edges (reversed: each wire pushes to the wires
            //    it feeds) until nothing rises. No decay, so a value only
            //    ever moves whole.
            const size_t n = network.size();
            std::vector<int> target(n, 0);
            std::vector<std::vector<int>> feeds(n);       // feeds[i] = wires i feeds
            for (size_t i = 0; i < n; ++i) {
                target[i] = BlockSignal(level, network[i]);
                IncomingWires(level, network[i], scratch);
                for (const glm::ivec3& src : scratch) {
                    const auto it = index.find(src);
                    if (it != index.end()) feeds[static_cast<size_t>(it->second)].push_back(static_cast<int>(i));
                }
            }
            std::vector<int> work;
            work.reserve(n);
            for (size_t i = 0; i < n; ++i) if (target[i] > 0) work.push_back(static_cast<int>(i));
            while (!work.empty()) {
                const int i = work.back();
                work.pop_back();
                for (int j : feeds[static_cast<size_t>(i)]) {
                    if (target[static_cast<size_t>(j)] < target[static_cast<size_t>(i)]) {
                        target[static_cast<size_t>(j)] = target[static_cast<size_t>(i)];
                        work.push_back(j);
                    }
                }
            }

            // 3. Apply, then the neighbour updates vanilla's evaluator makes
            //    for each changed wire (the cell and its six neighbours, so a
            //    component two steps away through a block hears it too).
            std::vector<glm::ivec3> changed;
            for (size_t i = 0; i < n; ++i) {
                const glm::ivec3& p = network[i];
                const BlockState state = level.GetBlockState(p.x, p.y, p.z);
                if (!state.Is(BlockID::RedstoneWire) || PowerOf(state) == target[i]) continue;
                level.SetBlock(p.x, p.y, p.z, WithPower(state, target[i]), World::UpdateFlags::UpdateClients);
                changed.push_back(p);
            }
            for (const glm::ivec3& p : changed) {
                level.UpdateNeighborsAt(p, BlockID::RedstoneWire);
                for (Direction d : kAllDirections) {
                    level.UpdateNeighborsAt(Relative(p, d), BlockID::RedstoneWire);
                }
            }
        }

        // MC DefaultRedstoneWireEvaluator.updatePowerStrength, verbatim in
        // shape. Vanilla's `toUpdate` is a HashSet of seven positions; Java's
        // iteration order over it is hash-defined and this port walks pos
        // then the six offsets in Direction.values() order — the set only
        // dedupes, and all seven are distinct, so the order is the one thing
        // that could differ. The neighbour updater's queue is what makes the
        // resulting cascade deterministic either way.
        void UpdatePowerStrength(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
            if (RedstonePlus::Enabled()) {
                UpdatePowerStrengthPlus(level, pos);
                return;
            }
            const int targetStrength = CalculateTargetStrength(level, pos);
            if (PowerOf(state) == targetStrength) return;

            if (level.GetBlockState(pos.x, pos.y, pos.z) == state) {
                level.SetBlock(pos.x, pos.y, pos.z, WithPower(state, targetStrength),
                               World::UpdateFlags::UpdateClients);
            }
            level.UpdateNeighborsAt(pos, BlockID::RedstoneWire);
            for (Direction d : kAllDirections) {
                level.UpdateNeighborsAt(Relative(pos, d), BlockID::RedstoneWire);
            }
        }

        // MC checkCornerChangeAt.
        void CheckCornerChangeAt(ILevelWrite& level, const glm::ivec3& pos) {
            if (!level.GetBlockState(pos.x, pos.y, pos.z).Is(BlockID::RedstoneWire)) return;
            level.UpdateNeighborsAt(pos, BlockID::RedstoneWire);
            for (Direction d : kAllDirections) {
                level.UpdateNeighborsAt(Relative(pos, d), BlockID::RedstoneWire);
            }
        }

        // MC updateNeighborsOfNeighboringWires.
        void UpdateNeighborsOfNeighboringWires(ILevelWrite& level, const glm::ivec3& pos) {
            for (Direction d : kHorizontalPlane) CheckCornerChangeAt(level, Relative(pos, d));
            for (Direction d : kHorizontalPlane) {
                const glm::ivec3 target = Relative(pos, d);
                if (IsRedstoneConductor(level, target)) CheckCornerChangeAt(level, Above(target));
                else                                    CheckCornerChangeAt(level, Below(target));
            }
        }

        // MC updatesOnShapeChange.
        void UpdatesOnShapeChange(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState oldState, BlockState newState) {
            for (Direction direction : kHorizontalPlane) {
                const glm::ivec3 relativePos = Relative(pos, direction);
                if (IsConnected(RedstoneSideOf(oldState, direction)) !=
                        IsConnected(RedstoneSideOf(newState, direction)) &&
                    IsRedstoneConductor(level, relativePos)) {
                    level.UpdateNeighborsAtExceptFromFacing(relativePos, newState.Block(),
                                                            Opposite(direction));
                }
            }
        }

    } // namespace

    RedstoneSide RedstoneSideOf(BlockState state, Direction dir) {
        return SideFromIndex(state.GetIndex(kSideProps[IndexOfDir(dir)]));
    }

    BlockState RedstoneStateFrom(RedstoneSide north, RedstoneSide east,
                                 RedstoneSide south, RedstoneSide west, int power) {
        const RedstoneSide sides[4] = {north, east, south, west};
        BlockState s = BlockStates::Default(BlockID::RedstoneWire);
        for (int i = 0; i < 4; ++i) s = s.SetIndex(kSideProps[i], IndexOfSide(sides[i]));
        return WithPower(s, power);
    }

    bool RedstoneIsCross(BlockState s) {
        for (Direction d : kOrder) if (!IsConnected(RedstoneSideOf(s, d))) return false;
        return true;
    }

    bool RedstoneIsDot(BlockState s) {
        for (Direction d : kOrder) if (IsConnected(RedstoneSideOf(s, d))) return false;
        return true;
    }

    bool RedstoneCanSurviveOn(const IBlockAccess& level, const glm::ivec3& pos) {
        return IsFaceSturdyAt(level, pos, Direction::Up) ||
               level.GetBlock(pos.x, pos.y, pos.z) == BlockID::Hopper;
    }

    // MC BlockBehaviour.shouldRedstoneWireConnectTo and its three overrides.
    bool RedstoneShouldConnectTo(const IBlockAccess& level, const glm::ivec3& pos,
                                 Direction dir, bool haveDirection) {
        const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
        const BlockID id = state.Block();
        if (id == BlockID::RedstoneWire) return true;
        if (id == BlockID::Repeater) {
            if (!haveDirection) return false;
            const Direction repeaterDirection = HorizontalFacingOf(state);
            return repeaterDirection == dir || Opposite(repeaterDirection) == dir;
        }
        if (id == BlockID::Observer) {
            return haveDirection && FacingOf(state) == dir;
        }
        return IsSignalSource(state) && haveDirection;
    }

    BlockState RedstoneConnectionState(const IBlockAccess& level, const glm::ivec3& pos,
                                       BlockState state) {
        const bool wasDot = RedstoneIsDot(state);
        // defaultBlockState().setValue(POWER, state.POWER) — all four sides
        // NONE, then getMissingConnections fills them from the world.
        state = MissingConnections(level, RedstoneStateFrom(RedstoneSide::None, RedstoneSide::None,
                                                            RedstoneSide::None, RedstoneSide::None,
                                                            PowerOf(state)), pos);
        if (wasDot && RedstoneIsDot(state)) return state;

        const bool north = IsConnected(RedstoneSideOf(state, Direction::North));
        const bool south = IsConnected(RedstoneSideOf(state, Direction::South));
        const bool east  = IsConnected(RedstoneSideOf(state, Direction::East));
        const bool west  = IsConnected(RedstoneSideOf(state, Direction::West));
        const bool northSouthEmpty = !north && !south;
        const bool eastWestEmpty   = !east && !west;
        if (!west  && northSouthEmpty) state = WithSide(state, Direction::West,  RedstoneSide::Side);
        if (!east  && northSouthEmpty) state = WithSide(state, Direction::East,  RedstoneSide::Side);
        if (!north && eastWestEmpty)   state = WithSide(state, Direction::North, RedstoneSide::Side);
        if (!south && eastWestEmpty)   state = WithSide(state, Direction::South, RedstoneSide::Side);
        return state;
    }

    BlockState RedstonePlacementState(const IBlockAccess& level, const glm::ivec3& pos) {
        return RedstoneConnectionState(level, pos, CrossState(0));
    }

    BlockState RedstoneUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                   BlockState state, Direction changed) {
        if (changed == Direction::Down) {
            return RedstoneCanSurviveOn(level, Below(pos)) ? state : BlockState{};
        }
        if (changed == Direction::Up) {
            return RedstoneConnectionState(level, pos, state);
        }
        const RedstoneSide sideConnection = ConnectingSide(level, pos, changed);
        if (IsConnected(sideConnection) == IsConnected(RedstoneSideOf(state, changed)) &&
            !RedstoneIsCross(state)) {
            return WithSide(state, changed, sideConnection);
        }
        return RedstoneConnectionState(level, pos,
                                       WithSide(CrossState(PowerOf(state)), changed, sideConnection));
    }

    bool RedstoneWireUpdateShapeHook(const IBlockAccess& level, const glm::ivec3& pos,
                                     BlockState state, Direction toNeighbour, BlockID /*neighbourId*/,
                                     BlockState& outState, ScheduledTickAccess* /*ticks*/) {
        const BlockState next = RedstoneUpdateShape(level, pos, state, toNeighbour);
        if (next == state) return false;
        outState = next;
        return true;
    }

    // MC updateIndirectNeighbourShapes: dust one step up or down from a
    // connected horizontal neighbour that is NOT itself dust.
    void RedstoneWireUpdateIndirectNeighbourShapes(ILevelWrite& level, const glm::ivec3& pos,
                                                   BlockState state, uint32_t updateFlags,
                                                   int updateLimit) {
        auto* world = dynamic_cast<World*>(&level);
        if (!world) return;
        for (Direction direction : kHorizontalPlane) {
            const RedstoneSide value = RedstoneSideOf(state, direction);
            const glm::ivec3 side = Relative(pos, direction);
            if (value == RedstoneSide::None ||
                level.GetBlockState(side.x, side.y, side.z).Is(BlockID::RedstoneWire)) {
                continue;
            }
            const glm::ivec3 down = Below(side);
            if (level.GetBlockState(down.x, down.y, down.z).Is(BlockID::RedstoneWire)) {
                const glm::ivec3 neighborPos = Relative(down, Opposite(direction));
                world->NeighborShapeChanged(Opposite(direction), down, neighborPos,
                                            level.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z),
                                            updateFlags, updateLimit);
            }
            const glm::ivec3 up = Above(side);
            if (level.GetBlockState(up.x, up.y, up.z).Is(BlockID::RedstoneWire)) {
                const glm::ivec3 neighborPos = Relative(up, Opposite(direction));
                world->NeighborShapeChanged(Opposite(direction), up, neighborPos,
                                            level.GetBlockState(neighborPos.x, neighborPos.y, neighborPos.z),
                                            updateFlags, updateLimit);
            }
        }
    }

    // ── Power hooks ─────────────────────────────────────────────────────────

    int RedstoneWireGetDirectSignal(const IBlockAccess& level, const glm::ivec3& pos,
                                    BlockState state, Direction direction) {
        return !s_shouldSignal ? 0 : RedstoneWireGetSignal(level, pos, state, direction);
    }

    int RedstoneWireGetSignal(const IBlockAccess& level, const glm::ivec3& pos,
                              BlockState state, Direction direction) {
        if (!s_shouldSignal || direction == Direction::Down) return 0;
        const int power = PowerOf(state);
        if (power == 0) return 0;
        if (direction == Direction::Up) return power;
        return IsConnected(RedstoneSideOf(RedstoneConnectionState(level, pos, state),
                                          Opposite(direction)))
            ? power : 0;
    }

    void RedstoneWireOnPlace(ILevelWrite& level, const glm::ivec3& pos,
                             BlockState state, BlockState oldState, bool /*movedByPiston*/) {
        if (oldState.Block() == state.Block() || level.IsClientSide()) return;
        UpdatePowerStrength(level, pos, state);
        for (Direction d : {Direction::Down, Direction::Up}) {
            level.UpdateNeighborsAt(Relative(pos, d), BlockID::RedstoneWire);
        }
        UpdateNeighborsOfNeighboringWires(level, pos);
    }

    void RedstoneWireAfterRemoval(ILevelWrite& level, const glm::ivec3& pos,
                                  BlockState state, bool movedByPiston) {
        if (movedByPiston) return;
        for (Direction d : kAllDirections) {
            level.UpdateNeighborsAt(Relative(pos, d), BlockID::RedstoneWire);
        }
        UpdatePowerStrength(level, pos, state);
        UpdateNeighborsOfNeighboringWires(level, pos);
    }

    void RedstoneWireNeighborChanged(ILevelWrite& level, const glm::ivec3& pos,
                                     BlockState state, BlockID sourceBlock, bool /*movedByPiston*/) {
        if (level.IsClientSide()) return;
        // MC RedstoneWireBlock.neighborChanged with the experimental
        // evaluator: a wire ignores wires. The network evaluator already set
        // every cell of the network this update came from; re-running it
        // here would cost one whole pass per cell.
        if (sourceBlock == BlockID::RedstoneWire && RedstonePlus::Enabled()) return;
        if (RedstoneCanSurviveOn(level, Below(pos))) {
            UpdatePowerStrength(level, pos, state);
        } else {
            DropBlockLoot(level, pos, state);
            if (auto* world = dynamic_cast<World*>(&level)) world->RemoveBlock(pos, false);
            else level.SetBlock(pos.x, pos.y, pos.z, BlockID::Air, World::UpdateFlags::All);
        }
    }

    bool RedstoneWireToggle(ILevelWrite& level, const glm::ivec3& pos, BlockState state) {
        const bool cross = RedstoneIsCross(state);
        if (!cross && !RedstoneIsDot(state)) return false;
        BlockState newState = cross
            ? RedstoneStateFrom(RedstoneSide::None, RedstoneSide::None,
                                RedstoneSide::None, RedstoneSide::None, PowerOf(state))
            : CrossState(PowerOf(state));
        newState = RedstoneConnectionState(level, pos, newState);
        if (newState == state) return false;
        // MC setBlockAndUpdate (flag 3), then the conductor pokes.
        level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::All);
        UpdatesOnShapeChange(level, pos, state, newState);
        return true;
    }

    // MC RedstoneWireBlock.COLORS:
    //   red   = power * 0.6 + (power > 0 ? 0.4 : 0.3)
    //   green = clamp(power * power * 0.7 - 0.5, 0, 1)
    //   blue  = clamp(power * power * 0.6 - 0.7, 0, 1)
    // where `power` is the 0..15 level divided by 15.
    uint32_t RedstoneWireColorForPower(int power) {
        static const std::array<uint32_t, 16> kColors = [] {
            std::array<uint32_t, 16> out{};
            for (int i = 0; i < 16; ++i) {
                const float p = static_cast<float>(i) / 15.0f;
                const float r = p * 0.6f + (p > 0.0f ? 0.4f : 0.3f);
                const float g = std::clamp(p * p * 0.7f - 0.5f, 0.0f, 1.0f);
                const float b = std::clamp(p * p * 0.6f - 0.7f, 0.0f, 1.0f);
                const auto c = [](float v) { return static_cast<uint32_t>(std::lround(v * 255.0f)) & 0xFF; };
                out[static_cast<size_t>(i)] = (c(r) << 16) | (c(g) << 8) | c(b);
            }
            return out;
        }();
        return kColors[static_cast<size_t>(std::clamp(power, 0, 15))];
    }

} // namespace Game
