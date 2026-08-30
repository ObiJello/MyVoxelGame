#pragma once

#include "levelgen/structure/StructureStartData.h"
#include <cstdint>
#include <string>
#include <vector>

// Reference: StructureTemplate.getJigsaws + JigsawBlockInfo + Palette.jigsaws.
// Full-template jigsaw extraction from data/<ns>/structure/<path>.nbt:
// palette entries with Name == "minecraft:jigsaw" carry Properties.orientation;
// the block entry's nbt carries name/pool/target/joint/placement_priority/
// selection_priority.

namespace minecraft {
namespace levelgen {
namespace structure {

// Directions in Java data3d order.
enum : int { D_DOWN = 0, D_UP = 1, D_NORTH = 2, D_SOUTH = 3, D_WEST = 4, D_EAST = 5 };

int oppositeDir(int d);
int rotateDirY(int d, int rotation);  // rotation 0..3 = NONE/CW90/CW180/CCW90
int dirStepX(int d);
int dirStepY(int d);
int dirStepZ(int d);

struct JigsawBlockData {
    int32_t x, y, z;      // local template position
    int front, top;       // orientation
    std::string name;     // "minecraft:empty" default
    std::string pool;     // target pool id ("minecraft:empty" default)
    std::string target;   // "minecraft:empty" default
    bool rollable;        // joint: rollable vs aligned
    int placementPriority = 0;
    int selectionPriority = 0;
};

struct JigsawTemplateData {
    int32_t sizeX, sizeY, sizeZ;
    // Per-palette jigsaw lists (most templates have exactly one palette).
    std::vector<std::vector<JigsawBlockData>> jigsawsPerPalette;
};

/** Placed (world-space, rotated) jigsaw block. */
struct PlacedJigsaw {
    int32_t x, y, z;
    int front, top;
    std::string name, pool, target;
    bool rollable;
    int placementPriority = 0;
    int selectionPriority = 0;
};

namespace JigsawTemplates {

/** Cached full-template load (size + per-palette jigsaws). Throws on error. */
const JigsawTemplateData& get(const std::string& templateId);

/**
 * Reference: StructureTemplate.getJigsaws(position, rotation). Palette picked
 * with LegacyRandomSource(Mth.getSeed(position)).nextInt(paletteCount);
 * positions transformed with pivot ZERO + offset; orientations rotated.
 * NOT shuffled/sorted - callers layer Util.shuffle + selection-priority sort.
 */
std::vector<PlacedJigsaw> getJigsaws(const std::string& templateId,
                                     int32_t posX, int32_t posY, int32_t posZ, int rotation);

/** Template bbox with pivot ZERO (pool-element semantics). */
BoundingBox elementBoundingBox(const std::string& templateId,
                               int32_t posX, int32_t posY, int32_t posZ, int rotation);

} // namespace JigsawTemplates

} // namespace structure
} // namespace levelgen
} // namespace minecraft
