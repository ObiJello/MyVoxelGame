#pragma once

#include <algorithm>

// The structure-side inputs of the terrain beardifier (TerrainAdjustment,
// BoundingBox, Beardifier.Rigid, JigsawJunction). The density sampler itself
// is density::Beardifier (levelgen/density/terrain/Beardifier.h).

namespace minecraft {
namespace levelgen {

/**
 * TerrainAdjustment - How structures affect surrounding terrain
 * Reference: net/minecraft/world/level/levelgen/structure/TerrainAdjustment.java
 */
enum class TerrainAdjustment {
    NONE,           // No terrain adjustment
    BURY,           // Bury structure underground
    BEARD_THIN,     // Thin beard effect
    BEARD_BOX,      // Box-shaped beard
    ENCAPSULATE     // Encapsulate structure
};

/**
 * BoundingBox - Axis-aligned bounding box for structures
 * Reference: net/minecraft/world/level/levelgen/structure/BoundingBox.java
 */
struct BoundingBox {
    int minX, minY, minZ;
    int maxX, maxY, maxZ;

    BoundingBox() : minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0) {}
    BoundingBox(int x1, int y1, int z1, int x2, int y2, int z2)
        : minX(x1), minY(y1), minZ(z1), maxX(x2), maxY(y2), maxZ(z2) {}

    bool isInside(int x, int y, int z) const {
        return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
    }

    BoundingBox inflatedBy(int amount) const {
        return BoundingBox(minX - amount, minY - amount, minZ - amount,
                          maxX + amount, maxY + amount, maxZ + amount);
    }
};

/**
 * Rigid - A rigid structure piece that affects terrain
 * Reference: Beardifier.java lines 224-226
 */
struct Rigid {
    BoundingBox box;
    TerrainAdjustment terrainAdjustment;
    int groundLevelDelta;

    Rigid() : terrainAdjustment(TerrainAdjustment::NONE), groundLevelDelta(0) {}
    Rigid(const BoundingBox& b, TerrainAdjustment adj, int delta)
        : box(b), terrainAdjustment(adj), groundLevelDelta(delta) {}
};

/**
 * JigsawJunction - Junction point for jigsaw structures
 * Reference: net/minecraft/world/level/levelgen/structure/pools/JigsawJunction.java
 */
struct JigsawJunction {
    int sourceX;
    int sourceGroundY;
    int sourceZ;
    int deltaY;
    // StructureTemplatePool.Projection destProjection; // Not needed for terrain

    JigsawJunction() : sourceX(0), sourceGroundY(0), sourceZ(0), deltaY(0) {}
    JigsawJunction(int x, int groundY, int z, int dy)
        : sourceX(x), sourceGroundY(groundY), sourceZ(z), deltaY(dy) {}

    int getSourceX() const { return sourceX; }
    int getSourceGroundY() const { return sourceGroundY; }
    int getSourceZ() const { return sourceZ; }
};

} // namespace levelgen
} // namespace minecraft
