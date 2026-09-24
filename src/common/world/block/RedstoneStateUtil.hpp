// File: src/common/world/block/RedstoneStateUtil.hpp
//
// Typed accessors for the blockstate properties the redstone ports read and
// write, so the ports can say `PoweredOf(state)` instead of remembering that
// BooleanProperty lists "true" FIRST (index 0 == true) or that MC's six-way
// FACING runs north,east,south,west,up,down while HORIZONTAL_FACING runs
// north,south,west,east. Every one of those orderings has bitten once.
#pragma once

#include "BlockState.hpp"
#include "Direction.hpp"
#include "GeneratedBlockStates.hpp"

#include <glm/glm.hpp>

namespace Game {

    // ── Booleans (index 0 == true) ──────────────────────────────────────────
    inline bool BoolOf(BlockState state, PropertyId prop) {
        return state.GetIndex(prop) == 0;
    }
    inline BlockState WithBool(BlockState state, PropertyId prop, bool on) {
        return state.SetIndex(prop, on ? 0 : 1);
    }
    inline bool       PoweredOf(BlockState s)              { return BoolOf(s, PropertyId::POWERED); }
    inline BlockState WithPowered(BlockState s, bool on)   { return WithBool(s, PropertyId::POWERED, on); }
    inline bool       LitOf(BlockState s)                  { return BoolOf(s, PropertyId::LIT); }
    inline BlockState WithLit(BlockState s, bool on)       { return WithBool(s, PropertyId::LIT, on); }
    inline bool       OpenOf(BlockState s)                 { return BoolOf(s, PropertyId::OPEN); }
    inline BlockState WithOpen(BlockState s, bool on)      { return WithBool(s, PropertyId::OPEN, on); }

    // ── Integers (index == value for 0-based ranges) ────────────────────────
    inline int        PowerOf(BlockState s)                { return s.GetIndex(PropertyId::POWER); }
    inline BlockState WithPower(BlockState s, int power)   { return s.SetIndex(PropertyId::POWER, power); }

    // ── MC's six-way FACING: north, east, south, west, up, down ─────────────
    constexpr int FacingIndex(Direction d) {
        switch (d) {
            case Direction::North: return 0;
            case Direction::East:  return 1;
            case Direction::South: return 2;
            case Direction::West:  return 3;
            case Direction::Up:    return 4;
            case Direction::Down:  return 5;
        }
        return 0;
    }
    constexpr Direction FacingFromIndex(int i) {
        switch (i) {
            case 1:  return Direction::East;
            case 2:  return Direction::South;
            case 3:  return Direction::West;
            case 4:  return Direction::Up;
            case 5:  return Direction::Down;
            default: return Direction::North;
        }
    }
    inline Direction  FacingOf(BlockState s)               { return FacingFromIndex(s.GetIndex(PropertyId::FACING)); }
    inline BlockState WithFacing(BlockState s, Direction d){ return s.SetIndex(PropertyId::FACING, FacingIndex(d)); }

    // ── HORIZONTAL_FACING: north, south, west, east ─────────────────────────
    inline Direction  HorizontalFacingOf(BlockState s) {
        return HorizontalFacingFromIndex(s.GetIndex(PropertyId::HORIZONTAL_FACING));
    }
    inline BlockState WithHorizontalFacing(BlockState s, Direction d) {
        return s.SetIndex(PropertyId::HORIZONTAL_FACING, HorizontalFacingIndex(d));
    }

    // ── FACING_HOPPER: down, north, south, west, east ───────────────────────
    inline Direction HopperFacingOf(BlockState s) {
        switch (s.GetIndex(PropertyId::FACING_HOPPER)) {
            case 1:  return Direction::North;
            case 2:  return Direction::South;
            case 3:  return Direction::West;
            case 4:  return Direction::East;
            default: return Direction::Down;
        }
    }

    // ── FACE (AttachFace): floor, wall, ceiling ─────────────────────────────
    enum class AttachFace : uint8_t { Floor = 0, Wall = 1, Ceiling = 2 };
    inline AttachFace AttachFaceOf(BlockState s) {
        return static_cast<AttachFace>(s.GetIndex(PropertyId::FACE));
    }

    // MC FaceAttachedHorizontalDirectionalBlock.getConnectedDirection: the
    // direction from the block toward the surface it is attached to,
    // expressed as vanilla does (UP for a floor button, DOWN for a ceiling
    // one, FACING for a wall one — i.e. the way it points AWAY from the wall).
    inline Direction ConnectedDirectionOf(BlockState s) {
        switch (AttachFaceOf(s)) {
            case AttachFace::Ceiling: return Direction::Down;
            case AttachFace::Floor:   return Direction::Up;
            default:                  return HorizontalFacingOf(s);
        }
    }

    inline glm::ivec3 Relative(const glm::ivec3& pos, Direction d, int n = 1) {
        return glm::ivec3(pos.x + StepX(d) * n, pos.y + StepY(d) * n, pos.z + StepZ(d) * n);
    }
    inline glm::ivec3 Below(const glm::ivec3& p) { return glm::ivec3(p.x, p.y - 1, p.z); }
    inline glm::ivec3 Above(const glm::ivec3& p) { return glm::ivec3(p.x, p.y + 1, p.z); }

    // MC Direction.Plane.HORIZONTAL iteration order: north, south, west, east.
    constexpr Direction kHorizontalPlane[4] = {
        Direction::North, Direction::South, Direction::West, Direction::East,
    };
    // MC Direction.values() order.
    constexpr Direction kAllDirections[6] = {
        Direction::Down, Direction::Up, Direction::North,
        Direction::South, Direction::West, Direction::East,
    };

} // namespace Game
