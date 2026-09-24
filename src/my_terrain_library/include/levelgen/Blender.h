#pragma once

namespace minecraft {

/**
 * Blender - blending between pre-1.18 chunks and new terrain
 * Reference: net/minecraft/world/level/levelgen/blending/Blender.java
 *
 * The engine never has old-format chunks next to new ones, so every Blender
 * is Blender.empty(): no density or biome blending. The chunk-status tasks
 * still pass one where Java passes Blender.of(region).
 */
class Blender {
public:
    static Blender* empty();
    bool isEmpty() const { return true; }
};

} // namespace minecraft
