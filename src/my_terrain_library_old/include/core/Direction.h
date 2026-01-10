#pragma once

// Re-export Direction from blockpredicates for backwards compatibility
#include "levelgen/blockpredicates/BlockPredicate.h"

namespace minecraft {
namespace core {

// Direction is an alias for blockpredicates::Direction
using Direction = levelgen::blockpredicates::Direction;

// Axis is an alias for blockpredicates::Axis
using Axis = levelgen::blockpredicates::Axis;

// Re-export helper functions
using levelgen::blockpredicates::getStepX;
using levelgen::blockpredicates::getStepY;
using levelgen::blockpredicates::getStepZ;
using levelgen::blockpredicates::getOpposite;
using levelgen::blockpredicates::getAxis;
using levelgen::blockpredicates::fromHorizontalIndex;
using levelgen::blockpredicates::fromIndex;
using levelgen::blockpredicates::rotateYClockwise;
using levelgen::blockpredicates::rotateYCounterClockwise;
using levelgen::blockpredicates::opposite;

/**
 * Get name of direction as string
 * Reference: Direction.java getName()
 */
inline const char* getName(Direction dir) {
    switch (dir) {
        case Direction::DOWN:  return "down";
        case Direction::UP:    return "up";
        case Direction::NORTH: return "north";
        case Direction::SOUTH: return "south";
        case Direction::WEST:  return "west";
        case Direction::EAST:  return "east";
        default: return "unknown";
    }
}

/**
 * Get random horizontal direction
 * Reference: Direction.java Plane.HORIZONTAL.getRandomDirection()
 */
template<typename RandomSource>
inline Direction horizontalRandom(RandomSource& random) {
    return fromHorizontalIndex(random.nextInt(4));
}

} // namespace core
} // namespace minecraft
