#pragma once

namespace minecraft {
namespace levelgen {

/**
 * CaveSurface - the floor or the ceiling of a cave
 * Reference: net/minecraft/world/level/levelgen/placement/CaveSurface.java
 * (the feature configurations' surface field; the placement modifiers keep
 * their own placement::CaveSurface).
 */
enum class CaveSurface {
    CEILING,  // Direction.UP, y = 1
    FLOOR     // Direction.DOWN, y = -1
};

} // namespace levelgen
} // namespace minecraft
