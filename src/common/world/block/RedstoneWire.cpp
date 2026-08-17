// File: src/common/world/block/RedstoneWire.cpp
#include "RedstoneWire.hpp"
#include "BlockRegistry.hpp"

#include <array>
#include <string>
#include <string_view>

namespace Game {

    namespace {

        constexpr std::string_view kSideNames[3] = {"none", "side", "up"};

        // MC PROPERTY_BY_DIRECTION order: NORTH, EAST, SOUTH, WEST.
        constexpr Direction kOrder[4] = {
            Direction::North, Direction::East, Direction::South, Direction::West,
        };
        constexpr std::string_view kPropNames[4] = {"north", "east", "south", "west"};

        int IndexOfDir(Direction d) {
            for (int i = 0; i < 4; ++i) if (kOrder[i] == d) return i;
            return 0;
        }

        // MC BlockState.isRedstoneConductor — "a solid full cube". The engine's
        // sturdy-face test is the closest thing it has, and it agrees for every
        // block that matters here (full cubes yes, slabs/stairs/glass no).
        bool IsRedstoneConductor(const IBlockAccess& level, const glm::ivec3& p) {
            const BlockID id = level.GetBlock(p.x, p.y, p.z);
            if (id == BlockID::Air) return false;
            if (!BlockRegistry::HasCollision(id)) return false;
            const auto& s = BlockRegistry::GetBlockShape(id, level.GetBlockState(p.x, p.y, p.z));
            return s.min.x <= 0.0001f && s.max.x >= 0.9999f &&
                   s.min.y <= 0.0001f && s.max.y >= 0.9999f &&
                   s.min.z <= 0.0001f && s.max.z >= 0.9999f;
        }

        // MC Block.isFaceSturdy(..., UP) — can dust sit on top of this?
        bool IsFaceSturdyUp(const IBlockAccess& level, const glm::ivec3& p) {
            const BlockID id = level.GetBlock(p.x, p.y, p.z);
            if (id == BlockID::Air) return false;
            if (!BlockRegistry::HasCollision(id)) return false;
            const auto& s = BlockRegistry::GetBlockShape(id, level.GetBlockState(p.x, p.y, p.z));
            return s.max.y >= 0.9999f &&
                   s.min.x <= 0.0001f && s.max.x >= 0.9999f &&
                   s.min.z <= 0.0001f && s.max.z >= 0.9999f;
        }

        // MC RedStoneWireBlock.canSurviveOn: a sturdy top face, or a hopper
        // (whose top is open but still holds dust).
        bool CanSurviveOn(const IBlockAccess& level, const glm::ivec3& p) {
            return IsFaceSturdyUp(level, p) ||
                   level.GetBlock(p.x, p.y, p.z) == BlockID::Hopper;
        }

        // MC RedStoneWireBlock.shouldConnectTo(state, direction).
        //
        //   redstone_wire        -> always
        //   repeater             -> only along its own axis
        //   observer             -> only out of its output face
        //   anything else        -> isSignalSource() && direction != null
        //
        // That last clause is the interesting one for this engine: MC's
        // isSignalSource is a per-block flag on every redstone COMPONENT
        // (torch, lever, button, pressure plate, daylight detector, target,
        // trapped chest, tripwire hook, comparator, block of redstone...).
        // None of those are functional here, so the set is hardcoded from the
        // vanilla blocks that answer true — the dust still points at them,
        // which is what the player sees, even though no signal flows.
        bool IsSignalSource(BlockID id) {
            static constexpr std::string_view kSignalSources[] = {
                "redstone_torch", "redstone_wall_torch", "redstone_block",
                "lever", "tripwire_hook", "trapped_chest",
                "daylight_detector", "target", "comparator", "observer",
                "detector_rail", "lectern", "sculk_sensor",
                "calibrated_sculk_sensor", "jukebox",
            };
            const std::string& n = BlockRegistry::Get(id).modelName;
            for (std::string_view s : kSignalSources) if (n == s) return true;
            // Every button and pressure plate, of which there are many.
            return n.find("_button") != std::string::npos ||
                   n.find("_pressure_plate") != std::string::npos;
        }

        bool ShouldConnectTo(const IBlockAccess& level, const glm::ivec3& p,
                             Direction dir, bool haveDirection) {
            const BlockID id = level.GetBlock(p.x, p.y, p.z);
            if (id == BlockID::RedstoneWire) return true;

            const std::string& n = BlockRegistry::Get(id).modelName;
            const auto& def = BlockRegistry::GetStateDefinition(id);
            const uint8_t st = level.GetBlockState(p.x, p.y, p.z);

            if (n == "repeater") {
                // MC: facing == direction || facing.getOpposite() == direction.
                const std::string_view f = def.ValueOf(st, "facing");
                if (f.empty() || !haveDirection) return false;
                return f == NameOf(dir) || f == NameOf(Opposite(dir));
            }
            if (n == "observer") {
                const std::string_view f = def.ValueOf(st, "facing");
                if (f.empty() || !haveDirection) return false;
                return f == NameOf(dir);
            }
            return IsSignalSource(id) && haveDirection;
        }

        // MC RedStoneWireBlock.getConnectingSide.
        RedstoneSide ConnectingSide(const IBlockAccess& level, const glm::ivec3& pos,
                                    Direction dir, bool canConnectUp) {
            const glm::ivec3 rel{pos.x + StepX(dir), pos.y, pos.z + StepZ(dir)};

            if (canConnectUp) {
                // Dust climbs the side of a block when there is more dust on
                // top of it. MC also lets it climb a trapdoor, which this
                // engine has no state for, so only the canSurviveOn half runs.
                if (CanSurviveOn(level, rel)) {
                    const glm::ivec3 above{rel.x, rel.y + 1, rel.z};
                    if (ShouldConnectTo(level, above, dir, /*haveDirection=*/false)) {
                        // UP only when the block is a full cube — otherwise the
                        // dust would be drawn climbing thin air.
                        return IsRedstoneConductor(level, rel) ? RedstoneSide::Up
                                                              : RedstoneSide::Side;
                    }
                }
            }

            if (ShouldConnectTo(level, rel, dir, /*haveDirection=*/true)) {
                return RedstoneSide::Side;
            }
            // Nothing beside it — but dust one step DOWN still counts, which is
            // how a wire follows a staircase. Blocked when the neighbour is a
            // full cube, since the dust below is then buried.
            const glm::ivec3 below{rel.x, rel.y - 1, rel.z};
            if (!IsRedstoneConductor(level, rel) &&
                ShouldConnectTo(level, below, dir, /*haveDirection=*/false)) {
                return RedstoneSide::Side;
            }
            return RedstoneSide::None;
        }

    } // namespace

    RedstoneSide RedstoneSideOf(uint8_t stateIndex, Direction dir) {
        const auto& def = BlockRegistry::GetStateDefinition(BlockID::RedstoneWire);
        const std::string_view v = def.ValueOf(stateIndex, kPropNames[IndexOfDir(dir)]);
        if (v == "side") return RedstoneSide::Side;
        if (v == "up")   return RedstoneSide::Up;
        return RedstoneSide::None;
    }

    uint8_t RedstoneStateFrom(RedstoneSide north, RedstoneSide east,
                              RedstoneSide south, RedstoneSide west) {
        const RedstoneSide sides[4] = {north, east, south, west};
        BlockRegistry::BlockStateDefinition::PropertyMap props;
        for (int i = 0; i < 4; ++i) {
            props[std::string(kPropNames[i])] =
                std::string(kSideNames[static_cast<int>(sides[i])]);
        }
        return BlockRegistry::GetStateDefinition(BlockID::RedstoneWire).IndexOf(props);
    }

    bool RedstoneIsCross(uint8_t s) {
        for (Direction d : kOrder) if (!IsConnected(RedstoneSideOf(s, d))) return false;
        return true;
    }

    bool RedstoneIsDot(uint8_t s) {
        for (Direction d : kOrder) if (IsConnected(RedstoneSideOf(s, d))) return false;
        return true;
    }

    // MC getConnectionState + getMissingConnections, merged. Vanilla threads a
    // BlockState through both; here the four sides are just locals.
    uint8_t RedstoneConnectionState(const IBlockAccess& level, const glm::ivec3& pos,
                                    bool startFromCross) {
        // getMissingConnections: resolve every side from the world. Vanilla
        // only fills sides that are not already connected, which matters
        // because it starts from crossState — but its crossState sides are
        // then immediately overwritten by this same resolution for any side
        // that comes back connected, so resolving all four is equivalent.
        const bool canConnectUp =
            !IsRedstoneConductor(level, {pos.x, pos.y + 1, pos.z});

        RedstoneSide sides[4];
        for (int i = 0; i < 4; ++i) {
            sides[i] = ConnectingSide(level, pos, kOrder[i], canConnectUp);
        }

        // A wire that found nothing at all stays a dot only if it STARTED as
        // one. Placement and the cross half of the right-click toggle come in
        // with startFromCross, which is what makes a lone dust render as a
        // cross rather than a dot.
        const bool nowDot = !IsConnected(sides[0]) && !IsConnected(sides[1]) &&
                            !IsConnected(sides[2]) && !IsConnected(sides[3]);
        if (!startFromCross && nowDot) {
            return RedstoneStateFrom(sides[0], sides[1], sides[2], sides[3]);
        }

        // MC's "keep it straight" rule: a wire connected on only one axis
        // still points BOTH ways along the other, so a line of dust reads as a
        // line rather than a row of stubs.
        const bool north = IsConnected(sides[0]);
        const bool east  = IsConnected(sides[1]);
        const bool south = IsConnected(sides[2]);
        const bool west  = IsConnected(sides[3]);
        const bool northSouthEmpty = !north && !south;
        const bool eastWestEmpty   = !east && !west;

        if (!west  && northSouthEmpty) sides[3] = RedstoneSide::Side;
        if (!east  && northSouthEmpty) sides[1] = RedstoneSide::Side;
        if (!north && eastWestEmpty)   sides[0] = RedstoneSide::Side;
        if (!south && eastWestEmpty)   sides[2] = RedstoneSide::Side;

        return RedstoneStateFrom(sides[0], sides[1], sides[2], sides[3]);
    }

    uint8_t RedstoneUpdateShape(const IBlockAccess& level, const glm::ivec3& pos,
                                uint8_t stateIndex, Direction changed) {
        // MC updateShape. DOWN is handled by the caller (the wire breaks); UP
        // re-resolves everything, because what sits above decides whether any
        // side may climb.
        if (changed == Direction::Up || changed == Direction::Down) {
            // MC passes the CURRENT state, so its `wasDot` guard is
            // isDot(state) — not "is cross". Those differ for a wire with one
            // or two connections, where treating it as a dot would let it
            // collapse instead of keeping the straight-line rule.
            return RedstoneConnectionState(level, pos, !RedstoneIsDot(stateIndex));
        }

        const bool canConnectUp = !IsRedstoneConductor(level, {pos.x, pos.y + 1, pos.z});
        const RedstoneSide fresh = ConnectingSide(level, pos, changed, canConnectUp);
        const RedstoneSide current = RedstoneSideOf(stateIndex, changed);

        // Vanilla's fast path: if the side's CONNECTEDNESS did not change and
        // the wire is not a cross, just write the new side value. Otherwise the
        // whole shape has to be recomputed, because gaining or losing one
        // connection can flip the straight-line rule on the other axis.
        if (IsConnected(fresh) == IsConnected(current) && !RedstoneIsCross(stateIndex)) {
            RedstoneSide sides[4];
            for (int i = 0; i < 4; ++i) sides[i] = RedstoneSideOf(stateIndex, kOrder[i]);
            sides[IndexOfDir(changed)] = fresh;
            return RedstoneStateFrom(sides[0], sides[1], sides[2], sides[3]);
        }
        return RedstoneConnectionState(level, pos, /*startFromCross=*/true);
    }

    bool RedstoneToggleShape(const IBlockAccess& level, const glm::ivec3& pos,
                             uint8_t stateIndex, uint8_t& outState) {
        // MC useWithoutItem: only a full cross or a bare dot toggles. Anything
        // with a real connection is left alone — there is nothing sensible to
        // toggle it to, and vanilla returns PASS.
        const bool cross = RedstoneIsCross(stateIndex);
        const bool dot   = RedstoneIsDot(stateIndex);
        if (!cross && !dot) return false;

        // cross -> defaultBlockState (all NONE, i.e. a dot); dot -> crossState.
        const uint8_t next = RedstoneConnectionState(level, pos,
                                                     /*startFromCross=*/!cross);
        if (next == stateIndex) return false;
        outState = next;
        return true;
    }

} // namespace Game
