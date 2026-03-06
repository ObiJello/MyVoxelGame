#pragma once

// Reference: net/minecraft/core/Direction.java

namespace minecraft {
namespace core {

/**
 * Direction enum for block face checks
 * Reference: net/minecraft/core/Direction.java
 */
enum class Direction {
    DOWN = 0,
    UP = 1,
    NORTH = 2,
    SOUTH = 3,
    WEST = 4,
    EAST = 5
};

/**
 * Axis enum for block/direction alignment
 * Reference: Direction.Axis
 */
enum class Axis {
    X,
    Y,
    Z
};

/**
 * Get the axis of a direction
 * Reference: Direction.java getAxis()
 */
inline Axis getAxis(Direction dir) {
    switch (dir) {
        case Direction::DOWN:
        case Direction::UP:
            return Axis::Y;
        case Direction::NORTH:
        case Direction::SOUTH:
            return Axis::Z;
        case Direction::WEST:
        case Direction::EAST:
            return Axis::X;
        default:
            return Axis::Y;
    }
}

/**
 * Get X step for direction (-1, 0, or 1)
 * Reference: Direction.java getStepX()
 */
inline int getStepX(Direction dir) {
    switch (dir) {
        case Direction::WEST: return -1;
        case Direction::EAST: return 1;
        default: return 0;
    }
}

/**
 * Get Y step for direction (-1, 0, or 1)
 * Reference: Direction.java getStepY()
 */
inline int getStepY(Direction dir) {
    switch (dir) {
        case Direction::DOWN: return -1;
        case Direction::UP: return 1;
        default: return 0;
    }
}

/**
 * Get Z step for direction (-1, 0, or 1)
 * Reference: Direction.java getStepZ()
 */
inline int getStepZ(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return -1;
        case Direction::SOUTH: return 1;
        default: return 0;
    }
}

/**
 * Get the opposite direction
 * Reference: Direction.java getOpposite()
 */
inline Direction getOpposite(Direction dir) {
    switch (dir) {
        case Direction::DOWN: return Direction::UP;
        case Direction::UP: return Direction::DOWN;
        case Direction::NORTH: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::NORTH;
        case Direction::WEST: return Direction::EAST;
        case Direction::EAST: return Direction::WEST;
        default: return dir;
    }
}

// Alias for backwards compatibility
inline Direction opposite(Direction dir) {
    return getOpposite(dir);
}

/**
 * Get direction from horizontal index (0-3)
 * Reference: Direction.java fromHorizontalIndex()
 */
inline Direction fromHorizontalIndex(int index) {
    // Order: SOUTH, WEST, NORTH, EAST
    switch (index & 3) {
        case 0: return Direction::SOUTH;
        case 1: return Direction::WEST;
        case 2: return Direction::NORTH;
        case 3: return Direction::EAST;
        default: return Direction::SOUTH;
    }
}

/**
 * Get direction from index (0-5)
 * Reference: Direction.java from3DDataValue()
 */
inline Direction fromIndex(int index) {
    switch (index) {
        case 0: return Direction::DOWN;
        case 1: return Direction::UP;
        case 2: return Direction::NORTH;
        case 3: return Direction::SOUTH;
        case 4: return Direction::WEST;
        case 5: return Direction::EAST;
        default: return Direction::DOWN;
    }
}

/**
 * Rotate direction clockwise around Y axis
 * Reference: Direction.java getClockWise()
 */
inline Direction rotateYClockwise(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::EAST;
        case Direction::EAST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::WEST;
        case Direction::WEST: return Direction::NORTH;
        default: return dir;
    }
}

/**
 * Rotate direction counter-clockwise around Y axis
 * Reference: Direction.java getCounterClockWise()
 */
inline Direction rotateYCounterClockwise(Direction dir) {
    switch (dir) {
        case Direction::NORTH: return Direction::WEST;
        case Direction::WEST: return Direction::SOUTH;
        case Direction::SOUTH: return Direction::EAST;
        case Direction::EAST: return Direction::NORTH;
        default: return dir;
    }
}

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
