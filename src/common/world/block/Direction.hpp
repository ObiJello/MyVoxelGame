// File: src/common/world/block/Direction.hpp
//
// Port of MC's net.minecraft.core.Direction, limited to what the block-state
// and placement layers need. Kept in `common` because both the server (which
// decides a block's state on placement) and the client (which predicts that
// decision, and rotates models by it) must agree exactly.
//
// The ordinal values match MC's Direction enum order (DOWN, UP, NORTH, SOUTH,
// WEST, EAST) so anything that round-trips through a numeric id — save files,
// packets, blockstate JSON property indices — lines up with vanilla.
#pragma once

#include <cstdint>
#include <cmath>
#include <string_view>

namespace Game {

    enum class Axis : uint8_t { X = 0, Y = 1, Z = 2 };

    enum class Direction : uint8_t {
        Down  = 0,
        Up    = 1,
        North = 2,
        South = 3,
        West  = 4,
        East  = 5,
    };

    // MC Direction.getOpposite (Direction.java — OPPOSITES table).
    constexpr Direction Opposite(Direction d) {
        switch (d) {
            case Direction::Down:  return Direction::Up;
            case Direction::Up:    return Direction::Down;
            case Direction::North: return Direction::South;
            case Direction::South: return Direction::North;
            case Direction::West:  return Direction::East;
            case Direction::East:  return Direction::West;
        }
        return Direction::North;
    }

    // MC Direction.getClockWise() — 90° about the Y axis. Vertical directions
    // are unchanged, matching MC (rotating UP about Y is still UP).
    constexpr Direction ClockWise(Direction d) {
        switch (d) {
            case Direction::North: return Direction::East;
            case Direction::East:  return Direction::South;
            case Direction::South: return Direction::West;
            case Direction::West:  return Direction::North;
            default:               return d;
        }
    }

    constexpr Direction CounterClockWise(Direction d) {
        return ClockWise(ClockWise(ClockWise(d)));
    }

    constexpr Axis AxisOf(Direction d) {
        switch (d) {
            case Direction::Down:
            case Direction::Up:    return Axis::Y;
            case Direction::North:
            case Direction::South: return Axis::Z;
            default:               return Axis::X;
        }
    }

    constexpr bool IsHorizontal(Direction d) { return AxisOf(d) != Axis::Y; }

    // Unit offset in world space. +X = east, +Y = up, +Z = south — the same
    // handedness MC uses, and the one this engine's world coordinates use.
    constexpr int StepX(Direction d) {
        return d == Direction::East ? 1 : (d == Direction::West ? -1 : 0);
    }
    constexpr int StepY(Direction d) {
        return d == Direction::Up ? 1 : (d == Direction::Down ? -1 : 0);
    }
    constexpr int StepZ(Direction d) {
        return d == Direction::South ? 1 : (d == Direction::North ? -1 : 0);
    }

    // MC Direction.toYRot(): (data2d & 3) * 90, where the 2D order is
    // S=0, W=1, N=2, E=3. Used by block-entity renderers that rotate at draw
    // time (the chest) and by the model rotation baker.
    constexpr float ToYRot(Direction d) {
        switch (d) {
            case Direction::South: return 0.0f;
            case Direction::West:  return 90.0f;
            case Direction::North: return 180.0f;
            case Direction::East:  return 270.0f;
            default:               return 0.0f;
        }
    }

    // MC Direction.fromYRot(yRot): BY_2D_DATA[floor(yRot / 90 + 0.5) & 3]
    // with the 2D ordering S, W, N, E.
    //
    // `yRot` is the engine-wide yaw, which IS Minecraft's (0 = facing south,
    // increasing clockwise) — see Game::Mth::ViewVector. Reproducing MC's
    // rounding exactly matters: a naive (yaw/90) truncation puts the boundaries
    // in the wrong place and every facing comes out one step off near 45°.
    inline Direction FromYRot(float mcYaw) {
        const int idx = static_cast<int>(std::floor(mcYaw / 90.0f + 0.5f)) & 3;
        switch (idx) {
            case 0:  return Direction::South;
            case 1:  return Direction::West;
            case 2:  return Direction::North;
            default: return Direction::East;
        }
    }

    // The camera used to keep its own yaw convention (0 = +X) and this header
    // carried an EngineYawToMcYaw(-90) shim plus a HorizontalFromEngineYaw
    // wrapper for it. Both are gone: the camera now stores MC's yaw directly,
    // so FromYRot IS the conversion and there is nothing left to adapt. If you
    // find yourself reaching for a ±90 offset on a yaw, something upstream is
    // still on the old convention — fix it there.

    // Lowercase names, matching the values used in blockstate JSON property
    // predicates ("facing=east") and in MC's BlockState.toString().
    constexpr std::string_view NameOf(Direction d) {
        switch (d) {
            case Direction::Down:  return "down";
            case Direction::Up:    return "up";
            case Direction::North: return "north";
            case Direction::South: return "south";
            case Direction::West:  return "west";
            case Direction::East:  return "east";
        }
        return "north";
    }

    constexpr std::string_view NameOf(Axis a) {
        switch (a) {
            case Axis::X: return "x";
            case Axis::Y: return "y";
            case Axis::Z: return "z";
        }
        return "y";
    }

} // namespace Game
