#pragma once

#include "levelgen/structure/StructureStartData.h"
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

// Twilight Forest 4.9 — world/components/structures/TFMaze.java, the maze of
// cells and walls shared by the hedge maze (HedgeMazeComponent, in
// TwilightHedgeMaze.cpp) and, later, the minotaur labyrinth. Cells sit at odd
// raw coordinates, walls at even ones; a raw 0 is a wall.
//
// Every random draw mirrors the mod (generateRecursiveBacktracker, the door
// roll, shouldTree, shouldTorch), so a maze seeded as the mod seeds it lays
// out identically.

namespace minecraft {
namespace world { namespace level { namespace block { namespace state { class BlockState; }}}}
using BlockState = world::level::block::state::BlockState;
namespace levelgen {
class WorldGenLevel;
class ChunkGenerator;
class WorldgenRandom;
namespace structure {
namespace twilight_pieces {

class TFStructureComponentOld;

class TFMaze {
public:
    static constexpr int32_t OUT_OF_BOUNDS = std::numeric_limits<int32_t>::min();
    static constexpr int32_t OOB = OUT_OF_BOUNDS;
    static constexpr int32_t ROOM = 5;
    static constexpr int32_t DOOR = 6;

    /**
     * TFMaze(cellsWidth, cellsDepth, random) — the random is not drawn from;
     * defaults: oddBias 3, evenBias 1, tall 3, head 0, roots 0, cut mazestone
     * walls over mazestone roots, torches at 0.75 rarity, no doors.
     */
    TFMaze(int32_t cellsWidth, int32_t cellsDepth);

    const int32_t width;   // cells wide (x)
    const int32_t depth;   // cells deep (z)

    int32_t oddBias;        // corridor thickness
    const int32_t evenBias; // wall thickness
    int32_t tall;           // wall blocks tall
    int32_t head;           // blocks placed above the maze
    int32_t roots;          // blocks placed under the maze (hedge mazes)
    int32_t type = 0;       // 1-3 hollow hill sizes, 4 = hedge maze (canopy-tree posts)

    /**
     * StructurePiece.BlockSelector for the walls (null = wallBlockState):
     * next(world.getRandom(), x, y, z, true) then getNext().
     */
    std::function<BlockState*(WorldGenLevel*, int32_t, int32_t, int32_t, bool)> wallBlocks;

    BlockState* wallBlockState;
    BlockState* headBlockState;
    BlockState* rootBlockState;
    BlockState* pillarBlockState;
    BlockState* doorBlockState;
    float doorRarity;
    BlockState* torchBlockState;
    float torchRarity;

    int32_t getCell(int32_t x, int32_t z) const;
    void putWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz, int32_t value);
    bool isWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz) const;
    void putRaw(int32_t rawX, int32_t rawZ, int32_t value);
    bool allCellsNonZero() const;
    void resetCells();

    /**
     * copyToStructure(world, manager, generator, dx, dy, dz, component, sbb,
     * rand): walls, posts (canopy trees for type 4), doors, roots, then the
     * torch pass.
     */
    void copyToStructure(WorldGenLevel* level, ChunkGenerator* generator, int32_t dx, int32_t dy, int32_t dz,
                         const TFStructureComponentOld& component, const BoundingBox& chunkBB,
                         WorldgenRandom& random) const;

    bool shouldTorch(int32_t rx, int32_t rz, WorldgenRandom& random) const;
    bool shouldPillar(int32_t rx, int32_t rz) const;
    bool shouldTree(int32_t rx, int32_t rz, WorldgenRandom& random) const;

    /** A 3x3-cell room with exits in every direction (the mod's mixed raw/cell coordinates kept). */
    void carveRoom1(int32_t cx, int32_t cz);
    /** Four exits at the middle of each side. */
    void add4Exits();
    void generateRecursiveBacktracker(int32_t sx, int32_t sz, WorldgenRandom& random);

private:
    const int32_t m_rawWidth;
    const int32_t m_rawDepth;
    std::vector<int32_t> m_storage;

    void putCell(int32_t x, int32_t z, int32_t value);
    bool cellEquals(int32_t x, int32_t z, int32_t value) const;
    int32_t getWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz) const;
    int32_t getRaw(int32_t rawX, int32_t rawZ) const;
    void rbGen(int32_t sx, int32_t sz, WorldgenRandom& random);

    void makeWallThing(WorldGenLevel* level, int32_t dy, const TFStructureComponentOld& component,
                       const BoundingBox& chunkBB, int32_t mdx, int32_t mdz, int32_t even, int32_t odd) const;
    void putPillarBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                        const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;
    void putWallBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                      const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;
    void putDoorBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                      const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;
    void putHeadBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                      const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;
    void putRootBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                      const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;
    void putCanopyTree(WorldGenLevel* level, ChunkGenerator* generator, int32_t x, int32_t y, int32_t z,
                       const TFStructureComponentOld& component, const BoundingBox& chunkBB) const;

    static bool isEven(int32_t n) { return n % 2 == 0; }
};

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
