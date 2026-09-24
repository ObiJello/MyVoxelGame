#include "levelgen/structure/twilight/TwilightHedgeMaze.h"

#include "levelgen/structure/twilight/TwilightPieceBase.h"
#include "levelgen/structure/TwilightStructures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/feature/Feature.h"
#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/state/properties/BlockStateProperties.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 hedge maze:
//   world/components/structures/type/HedgeMazeStructure.java (getFirstPiece,
//       adjustForTerrain override, getStructureTerraformer)
//   world/components/structures/HedgeMazeComponent.java (the one piece)
//   world/components/structures/TFMaze.java (the maze generator)
//   world/components/structures/CustomDensitySource.java
//       (getInvertedPyramidTerraformer), chunkgenerators/
//       AbsoluteDifferenceFunction.java
//   util/WorldUtil.java (adjustForTerrain grid sampling)

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

using world::level::block::Blocks;
using world::level::block::state::properties::BlockStateProperties;
using core::Direction;

namespace {

void logOnce(const char* key, const std::string& message) {
    static std::mutex s_mutex;
    static std::vector<std::string> s_logged;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (std::find(s_logged.begin(), s_logged.end(), key) != s_logged.end()) return;
    s_logged.emplace_back(key);
    fprintf(stderr, "[TwilightHedgeMaze] %s\n", message.c_str());
}

// TF blocks the maze defaults and the hedge maze use, resolved once.
struct MazeBlocks {
    BlockState* cutMazestone;
    BlockState* mazestone;
    BlockState* torch;
    BlockState* air;
    BlockState* hedge;
    BlockState* firefly;
    BlockState* grass;
    BlockState* jackOLantern[6];  // by core::Direction (horizontal entries used)
};

const MazeBlocks& mazeBlocks() {
    static const MazeBlocks s_blocks = [] {
        MazeBlocks b{};
        b.air = Blocks::AIR->defaultBlockState();
        b.cutMazestone = twilight_blocks::defaultState("twilightforest:cut_mazestone");
        b.mazestone = twilight_blocks::defaultState("twilightforest:mazestone");
        b.torch = Blocks::getDefaultState("minecraft:torch");
        b.hedge = twilight_blocks::defaultState("twilightforest:hedge");
        b.firefly = twilight_blocks::defaultState("twilightforest:firefly");
        b.grass = Blocks::GRASS_BLOCK->defaultBlockState();
        BlockState* jack = Blocks::getDefaultState("minecraft:jack_o_lantern");
        for (int d = 0; d < 6; ++d) {
            const Direction dir = static_cast<Direction>(d);
            if (jack == nullptr) {
                b.jackOLantern[d] = nullptr;
            } else if (dir == Direction::UP || dir == Direction::DOWN) {
                b.jackOLantern[d] = jack;
            } else {
                b.jackOLantern[d] = jack->trySetValue(*BlockStateProperties::HORIZONTAL_FACING, dir);
            }
        }
        return b;
    }();
    return s_blocks;
}

// Mth.lerpDiscrete(alpha, p0, p1).
int32_t lerpDiscrete(float alpha, int32_t p0, int32_t p1) {
    const int32_t delta = p1 - p0;
    return p0 + Mth::floor(static_cast<double>(alpha * static_cast<float>(delta - 1))) + (alpha > 0.0f ? 1 : 0);
}

// WorldUtil.adjustForTerrain(context, xMin, zMin, xMax, zMax, gridLength,
// WORLD_SURFACE_WG): heights on a gridLength^2 grid, sorted high to low,
// weighted by rank (1, 2, ...), rounded.
int32_t adjustForTerrain(GenerationContext& ctx, int32_t xMin, int32_t zMin, int32_t xMax, int32_t zMax,
                         int32_t gridLength) {
    const int32_t subDivisions = gridLength - 1;
    std::vector<int32_t> heights;
    heights.reserve(static_cast<std::size_t>(gridLength * gridLength));
    for (int32_t zStep = 0; zStep <= subDivisions; ++zStep) {
        const int32_t zPos = lerpDiscrete(static_cast<float>(zStep) / static_cast<float>(subDivisions), zMin, zMax);
        for (int32_t xStep = 0; xStep <= subDivisions; ++xStep) {
            const int32_t xPos = lerpDiscrete(static_cast<float>(xStep) / static_cast<float>(subDivisions), xMin, xMax);
            // ChunkGenerator.getFirstOccupiedHeight = getBaseHeight - 1.
            heights.push_back(ctx.generator->getBaseHeight(xPos, zPos, Heightmap::Types::WORLD_SURFACE_WG,
                                                           ctx.randomState) - 1);
        }
    }
    std::sort(heights.begin(), heights.end(), std::greater<int32_t>());
    double weightedSum = 0.0;
    double totalWeight = 0.0;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        const double weight = static_cast<double>(i + 1);
        weightedSum += weight * static_cast<double>(heights[i]);
        totalWeight += weight;
    }
    return tf_common::javaRound(weightedSum / totalWeight);
}

// WorldUtil.adjustForTerrain(context, xInCenterChunk, zInCenterChunk,
// radiusFromCenterChunk, gridLength).
int32_t adjustForTerrain(GenerationContext& ctx, int32_t xInCenterChunk, int32_t zInCenterChunk,
                         int32_t radiusFromCenterChunk, int32_t gridLength) {
    const int32_t chunkOriginX = xInCenterChunk & ~0xF;
    const int32_t chunkOriginZ = zInCenterChunk & ~0xF;
    return adjustForTerrain(ctx, chunkOriginX - radiusFromCenterChunk, chunkOriginZ - radiusFromCenterChunk,
                            chunkOriginX + 15 + radiusFromCenterChunk, chunkOriginZ + 15 + radiusFromCenterChunk,
                            gridLength);
}

} // namespace

// ============================================================================
// TFMaze
// ============================================================================

TFMaze::TFMaze(int32_t cellsWidth, int32_t cellsDepth)
    : width(cellsWidth)
    , depth(cellsDepth)
    , oddBias(3)
    , evenBias(1)
    , tall(3)
    , head(0)
    , roots(0)
    , m_rawWidth(cellsWidth * 2 + 1)
    , m_rawDepth(cellsDepth * 2 + 1)
    , m_storage(static_cast<std::size_t>((cellsWidth * 2 + 1) * (cellsDepth * 2 + 1)), 0) {
    const MazeBlocks& blocks = mazeBlocks();
    wallBlockState = blocks.cutMazestone;
    rootBlockState = blocks.mazestone;
    torchBlockState = blocks.torch;
    pillarBlockState = blocks.air;
    headBlockState = blocks.air;
    doorBlockState = blocks.air;
    torchRarity = 0.75f;
    doorRarity = 0.0f;
}

int32_t TFMaze::getCell(int32_t x, int32_t z) const {
    return getRaw(x * 2 + 1, z * 2 + 1);
}

void TFMaze::putCell(int32_t x, int32_t z, int32_t value) {
    putRaw(x * 2 + 1, z * 2 + 1, value);
}

bool TFMaze::cellEquals(int32_t x, int32_t z, int32_t value) const {
    return getCell(x, z) == value;
}

int32_t TFMaze::getWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz) const {
    if (dx == sx + 1 && dz == sz) return getRaw(sx * 2 + 2, sz * 2 + 1);
    if (dx == sx - 1 && dz == sz) return getRaw(sx * 2, sz * 2 + 1);
    if (dx == sx && dz == sz + 1) return getRaw(sx * 2 + 1, sz * 2 + 2);
    if (dx == sx && dz == sz - 1) return getRaw(sx * 2 + 1, sz * 2);
    return OUT_OF_BOUNDS;
}

void TFMaze::putWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz, int32_t value) {
    if (dx == sx + 1 && dz == sz) putRaw(sx * 2 + 2, sz * 2 + 1, value);
    if (dx == sx - 1 && dz == sz) putRaw(sx * 2, sz * 2 + 1, value);
    if (dx == sx && dz == sz + 1) putRaw(sx * 2 + 1, sz * 2 + 2, value);
    if (dx == sx && dz == sz - 1) putRaw(sx * 2 + 1, sz * 2, value);
}

bool TFMaze::isWall(int32_t sx, int32_t sz, int32_t dx, int32_t dz) const {
    return getWall(sx, sz, dx, dz) == 0;
}

void TFMaze::putRaw(int32_t rawX, int32_t rawZ, int32_t value) {
    if (rawX >= 0 && rawX < m_rawWidth && rawZ >= 0 && rawZ < m_rawDepth) {
        m_storage[static_cast<std::size_t>(rawZ * m_rawWidth + rawX)] = value;
    }
}

int32_t TFMaze::getRaw(int32_t rawX, int32_t rawZ) const {
    if (rawX < 0 || rawX >= m_rawWidth || rawZ < 0 || rawZ >= m_rawDepth) return OUT_OF_BOUNDS;
    return m_storage[static_cast<std::size_t>(rawZ * m_rawWidth + rawX)];
}

bool TFMaze::allCellsNonZero() const {
    for (int32_t x = 0; x < width; ++x) {
        for (int32_t z = 0; z < depth; ++z) {
            if (getCell(x, z) == 0) return false;
        }
    }
    return true;
}

void TFMaze::resetCells() {
    std::fill(m_storage.begin(), m_storage.end(), 0);
}

void TFMaze::copyToStructure(WorldGenLevel* level, ChunkGenerator* generator, int32_t dx, int32_t dy, int32_t dz,
                             const TFStructureComponentOld& component, const BoundingBox& chunkBB,
                             WorldgenRandom& random) const {
    for (int32_t x = 0; x < m_rawWidth; ++x) {
        for (int32_t z = 0; z < m_rawDepth; ++z) {
            const int32_t raw = getRaw(x, z);
            // Only draw walls: raw 0 is a wall.
            if (raw == 0) {
                int32_t mdx = dx + (x / 2 * (evenBias + oddBias));
                int32_t mdz = dz + (z / 2 * (evenBias + oddBias));
                if (evenBias > 1) {
                    --mdx;
                    --mdz;
                }

                if (isEven(x) && isEven(z)) {
                    if (type == 4 && shouldTree(x, z, random)) {
                        // Occasionally make a tree.
                        putCanopyTree(level, generator, mdx, dy, mdz, component, chunkBB);
                    } else {
                        // Make a block!
                        for (int32_t even = 0; even < evenBias; ++even) {
                            for (int32_t even2 = 0; even2 < evenBias; ++even2) {
                                for (int32_t y = 0; y < head; ++y) {
                                    putHeadBlock(level, mdx + even, dy + tall + y, mdz + even2, component, chunkBB);
                                }
                                for (int32_t y = 0; y < tall; ++y) {
                                    if (shouldPillar(x, z)) {
                                        putPillarBlock(level, mdx + even, dy + y, mdz + even2, component, chunkBB);
                                    } else {
                                        putWallBlock(level, mdx + even, dy + y, mdz + even2, component, chunkBB);
                                    }
                                }
                                for (int32_t y = 1; y <= roots; ++y) {
                                    putRootBlock(level, mdx + even, dy - y, mdz + even2, component, chunkBB);
                                }
                            }
                        }
                    }
                }
                if (isEven(x) && !isEven(z)) {
                    // Make a | vertical | wall!
                    for (int32_t even = 0; even < evenBias; ++even) {
                        for (int32_t odd = 1; odd <= oddBias; ++odd) {
                            makeWallThing(level, dy, component, chunkBB, mdx, mdz, even, odd);
                        }
                    }
                }
                if (!isEven(x) && isEven(z)) {
                    // Make a - horizontal - wall!
                    for (int32_t even = 0; even < evenBias; ++even) {
                        for (int32_t odd = 1; odd <= oddBias; ++odd) {
                            makeWallThing(level, dy, component, chunkBB, mdx, mdz, odd, even);
                        }
                    }
                }
            } else if (raw == DOOR) {
                int32_t mdx = dx + (x / 2 * (evenBias + oddBias));
                int32_t mdz = dz + (z / 2 * (evenBias + oddBias));
                if (evenBias > 1) {
                    --mdx;
                    --mdz;
                }

                if (isEven(x) && !isEven(z)) {
                    // Make a | vertical | door!
                    for (int32_t even = 0; even < evenBias; ++even) {
                        for (int32_t odd = 1; odd <= oddBias; ++odd) {
                            for (int32_t y = 0; y < head; ++y) {
                                putHeadBlock(level, mdx + even, dy + tall + y, mdz + odd, component, chunkBB);
                            }
                            for (int32_t y = 0; y < tall; ++y) {
                                putDoorBlock(level, mdx + even, dy + y, mdz + odd, component, chunkBB);
                            }
                            for (int32_t y = 1; y <= roots; ++y) {
                                putRootBlock(level, mdx + even, dy - y, mdz + odd, component, chunkBB);
                            }
                        }
                    }
                }
                if (!isEven(x) && isEven(z)) {
                    // Make a - horizontal - door!
                    for (int32_t even = 0; even < evenBias; ++even) {
                        for (int32_t odd = 1; odd <= oddBias; ++odd) {
                            for (int32_t y = 0; y < head; ++y) {
                                putHeadBlock(level, mdx + odd, dy + tall + y, mdz + even, component, chunkBB);
                            }
                            for (int32_t y = 0; y < tall; ++y) {
                                putDoorBlock(level, mdx + odd, dy + y, mdz + even, component, chunkBB);
                            }
                            for (int32_t y = 1; y <= roots; ++y) {
                                putRootBlock(level, mdx + odd, dy - y, mdz + even, component, chunkBB);
                            }
                        }
                    }
                }
            }
        }
    }

    // Torches (placeTorches folded in here, as in the mod).
    for (int32_t x = 0; x < m_rawWidth; ++x) {
        for (int32_t z = 0; z < m_rawDepth; ++z) {
            if (getRaw(x, z) != 0) continue;
            const int32_t mdx = dx + (x / 2 * (evenBias + oddBias));
            const int32_t mdy = dy + 1;
            const int32_t mdz = dz + (z / 2 * (evenBias + oddBias));
            if (isEven(x) && isEven(z)) {
                if (shouldTorch(x, z, random)) {
                    BlockState* here = component.getBlock(level, mdx, mdy, mdz, chunkBB);
                    if (wallBlockState != nullptr && here->is(wallBlockState)) {
                        component.placeBlock(level, torchBlockState, mdx, mdy, mdz, chunkBB);
                    }
                }
            }
        }
    }
}

void TFMaze::makeWallThing(WorldGenLevel* level, int32_t dy, const TFStructureComponentOld& component,
                           const BoundingBox& chunkBB, int32_t mdx, int32_t mdz, int32_t even, int32_t odd) const {
    for (int32_t y = 0; y < head; ++y) {
        putHeadBlock(level, mdx + even, dy + tall + y, mdz + odd, component, chunkBB);
    }
    for (int32_t y = 0; y < tall; ++y) {
        putWallBlock(level, mdx + even, dy + y, mdz + odd, component, chunkBB);
    }
    for (int32_t y = 1; y <= roots; ++y) {
        putRootBlock(level, mdx + even, dy - y, mdz + odd, component, chunkBB);
    }
}

void TFMaze::putPillarBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                            const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    component.placeBlock(level, pillarBlockState, x, y, z, chunkBB);
}

void TFMaze::putWallBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                          const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    if (wallBlocks) {
        component.placeBlock(level, wallBlocks(level, x, y, z, true), x, y, z, chunkBB);
    } else {
        component.placeBlock(level, wallBlockState, x, y, z, chunkBB);
    }
}

void TFMaze::putDoorBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                          const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    component.placeBlock(level, doorBlockState, x, y, z, chunkBB);
}

void TFMaze::putHeadBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                          const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    component.placeBlock(level, headBlockState, x, y, z, chunkBB);
}

void TFMaze::putRootBlock(WorldGenLevel* level, int32_t x, int32_t y, int32_t z,
                          const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    component.placeBlock(level, rootBlockState, x, y, z, chunkBB);
}

void TFMaze::putCanopyTree(WorldGenLevel* level, ChunkGenerator* generator, int32_t x, int32_t y, int32_t z,
                           const TFStructureComponentOld& component, const BoundingBox& chunkBB) const {
    const core::BlockPos pos = component.getBlockPosWithOffset(x, y, z);
    // Only place it if we're generating the chunk the tree stands in.
    if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;

    // TFConfiguredFeatures.CANOPY_TREE placed with world.getRandom(): the
    // region's own random, never the structure random. The engine's level
    // random seeds a WorldgenRandom for the tree, leaving the maze's draws
    // untouched as in the mod.
    levelgen::ConfiguredFeature* canopyTree =
        data::worldgen::features::twilight::findConfigured("twilightforest:tree/canopy_tree");
    bool placed = false;
    if (canopyTree != nullptr) {
        WorldgenRandom treeRandom(XoroshiroRandomSource(level->getRandom().nextLong()));
        placed = canopyTree->place(level, generator, treeRandom, pos);
    } else {
        logOnce("canopy_tree", "twilightforest:tree/canopy_tree not registered - maze posts stay hedge");
    }
    if (!placed) {
        makeWallThing(level, y, component, chunkBB, x, z, 0, 0);
    }
}

bool TFMaze::shouldTorch(int32_t rx, int32_t rz, WorldgenRandom& random) const {
    // Out of bounds in any direction: no.
    if (getRaw(rx + 1, rz) == OOB || getRaw(rx - 1, rz) == OOB
        || getRaw(rx, rz + 1) == OOB || getRaw(rx, rz - 1) == OOB) {
        return false;
    }
    // Walls in two opposite directions: no.
    if ((getRaw(rx + 1, rz) == 0 && getRaw(rx - 1, rz) == 0)
        || (getRaw(rx, rz + 1) == 0 && getRaw(rx, rz - 1) == 0)) {
        return false;
    }
    return random.nextFloat() <= torchRarity;
}

bool TFMaze::shouldPillar(int32_t rx, int32_t rz) const {
    if (pillarBlockState == nullptr || pillarBlockState->isAir()) return false;
    if (getRaw(rx + 1, rz) == OOB || getRaw(rx - 1, rz) == OOB
        || getRaw(rx, rz + 1) == OOB || getRaw(rx, rz - 1) == OOB) {
        return false;
    }
    return (getRaw(rx + 1, rz) != 0 || getRaw(rx - 1, rz) != 0)
        && (getRaw(rx, rz + 1) != 0 || getRaw(rx, rz - 1) != 0);
}

bool TFMaze::shouldTree(int32_t rx, int32_t rz, WorldgenRandom& random) const {
    // Yes for the corners and the exits.
    if ((rx == 0 || rx == m_rawWidth - 1) && (getRaw(rx, rz + 1) != 0 || getRaw(rx, rz - 1) != 0)) {
        return true;
    }
    if ((rz == 0 || rz == m_rawDepth - 1) && (getRaw(rx + 1, rz) != 0 || getRaw(rx - 1, rz) != 0)) {
        return true;
    }
    return random.nextInt(50) == 0;
}

void TFMaze::carveRoom1(int32_t cx, int32_t cz) {
    const int32_t rx = cx * 2 + 1;
    const int32_t rz = cz * 2 + 1;

    // Remove walls and cells.
    for (int32_t i = -2; i <= 2; ++i) {
        for (int32_t j = -2; j <= 2; ++j) {
            putRaw(rx + i, rz + j, ROOM);
        }
    }

    // Mark the exit areas as unmazed (cell coordinates fed raw values, as
    // in the mod).
    putCell(rx, rz + 1, 0);
    putCell(rx, rz - 1, 0);
    putCell(rx + 1, rz, 0);
    putCell(rx - 1, rz, 0);

    // Four exits, unless at the edge of the maze.
    if (getRaw(rx, rz + 4) != OUT_OF_BOUNDS) putRaw(rx, rz + 3, ROOM);
    if (getRaw(rx, rz - 4) != OUT_OF_BOUNDS) putRaw(rx, rz - 3, ROOM);
    if (getRaw(rx + 4, rz) != OUT_OF_BOUNDS) putRaw(rx + 3, rz, ROOM);
    if (getRaw(rx - 4, rz) != OUT_OF_BOUNDS) putRaw(rx - 3, rz, ROOM);
}

void TFMaze::add4Exits() {
    const int32_t hx = m_rawWidth / 2 + 1;
    const int32_t hz = m_rawDepth / 2 + 1;
    putRaw(hx, 0, ROOM);
    putRaw(hx, m_rawDepth - 1, ROOM);
    putRaw(0, hz, ROOM);
    putRaw(m_rawWidth - 1, hz, ROOM);
}

void TFMaze::generateRecursiveBacktracker(int32_t sx, int32_t sz, WorldgenRandom& random) {
    rbGen(sx, sz, random);
}

void TFMaze::rbGen(int32_t sx, int32_t sz, WorldgenRandom& random) {
    // Mark the cell as visited.
    putCell(sx, sz, 1);

    // Count the unvisited neighbours.
    int32_t unvisited = 0;
    if (cellEquals(sx + 1, sz, 0)) ++unvisited;
    if (cellEquals(sx - 1, sz, 0)) ++unvisited;
    if (cellEquals(sx, sz + 1, 0)) ++unvisited;
    if (cellEquals(sx, sz - 1, 0)) ++unvisited;
    if (unvisited == 0) return;

    // Pick one at random.
    int32_t rn = random.nextInt(unvisited);
    int32_t dx = 0;
    int32_t dz = 0;
    if (cellEquals(sx + 1, sz, 0)) {
        if (rn == 0) {
            dx = sx + 1;
            dz = sz;
        }
        --rn;
    }
    if (cellEquals(sx - 1, sz, 0)) {
        if (rn == 0) {
            dx = sx - 1;
            dz = sz;
        }
        --rn;
    }
    if (cellEquals(sx, sz + 1, 0)) {
        if (rn == 0) {
            dx = sx;
            dz = sz + 1;
        }
        --rn;
    }
    if (cellEquals(sx, sz - 1, 0)) {
        if (rn == 0) {
            dx = sx;
            dz = sz - 1;
        }
    }

    // Carve a wall or a door.
    if (random.nextFloat() <= doorRarity) {
        putWall(sx, sz, dx, dz, DOOR);
    } else {
        putWall(sx, sz, dx, dz, 2);
    }

    // Recurse at the destination, then retry this cell up to two more times.
    rbGen(dx, dz, random);
    rbGen(sx, sz, random);
    rbGen(sx, sz, random);
}

// ============================================================================
// HedgeMazeComponent
// ============================================================================
namespace {

constexpr int32_t kMazeSize = 16;                       // MSIZE
constexpr int32_t kRadius = (kMazeSize / 2 * 3) + 1;    // RADIUS
constexpr int32_t kDiameter = 2 * kRadius;              // DIAMETER
constexpr int32_t kFloorLevel = 0;                      // FLOOR_LEVEL

class HedgeMazeComponent final : public TFStructureComponentOld {
public:
    HedgeMazeComponent() : TFStructureComponentOld(static_cast<int>(Direction::SOUTH)) {}

    // HedgeMazeComponent.postProcess.
    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)chunkPos;
        (void)referencePos;
        bind(self);
        const MazeBlocks& blocks = mazeBlocks();

        // The maze is re-generated in every chunk from a seed fixed by the
        // maze's own position.
        random.setSeed(getMazeSeed(level));
        TFMaze maze(kMazeSize, kMazeSize);
        maze.oddBias = 2;
        maze.torchBlockState = blocks.firefly;
        maze.wallBlockState = blocks.hedge;
        maze.type = 4;
        maze.tall = 3;
        maze.roots = 3;

        // Grass underneath.
        for (int32_t fx = 0; fx <= kDiameter; ++fx) {
            for (int32_t fz = 0; fz <= kDiameter; ++fz) {
                placeBlock(level, blocks.grass, fx, kFloorLevel - 1, fz, chunkBB);
            }
        }

        BlockState* northJacko = blocks.jackOLantern[static_cast<int>(Direction::SOUTH)];
        BlockState* southJacko = blocks.jackOLantern[static_cast<int>(Direction::NORTH)];
        BlockState* westJacko = blocks.jackOLantern[static_cast<int>(Direction::EAST)];
        BlockState* eastJacko = blocks.jackOLantern[static_cast<int>(Direction::WEST)];

        // Jack-o'-lanterns outside for decoration.
        placeBlock(level, westJacko, 0, kFloorLevel, 24, chunkBB);
        placeBlock(level, westJacko, 0, kFloorLevel, 29, chunkBB);
        placeBlock(level, eastJacko, 50, kFloorLevel, 24, chunkBB);
        placeBlock(level, eastJacko, 50, kFloorLevel, 29, chunkBB);

        placeBlock(level, northJacko, 24, kFloorLevel, 0, chunkBB);
        placeBlock(level, northJacko, 29, kFloorLevel, 0, chunkBB);
        placeBlock(level, southJacko, 24, kFloorLevel, 50, chunkBB);
        placeBlock(level, southJacko, 29, kFloorLevel, 50, chunkBB);

        const int32_t nrooms = kMazeSize / 3;
        std::vector<int32_t> rcoords(static_cast<std::size_t>(nrooms * 2), 0);

        while (!maze.allCellsNonZero()) {
            maze.resetCells();
            rcoords.assign(static_cast<std::size_t>(nrooms * 2), 0);
            for (int32_t i = 0; i < nrooms; ++i) {
                int32_t rx;
                int32_t rz;
                do {
                    rx = random.nextInt(kMazeSize - 2) + 1;
                    rz = random.nextInt(kMazeSize - 2) + 1;
                } while (isNearRoom(rx, rz, rcoords));

                maze.carveRoom1(rx, rz);

                rcoords[static_cast<std::size_t>(i * 2)] = rx;
                rcoords[static_cast<std::size_t>(i * 2 + 1)] = rz;
            }
            maze.generateRecursiveBacktracker(0, 0, random);
        }

        maze.add4Exits();
        maze.copyToStructure(level, generator, 1, kFloorLevel, 1, *this, chunkBB, random);
        decorate3x3Rooms(level, rcoords, chunkBB);
    }

private:
    int64_t getMazeSeed(WorldGenLevel* level) const {
        const BoundingBox& box = m_self->boundingBox;
        return static_cast<int64_t>(static_cast<uint64_t>(level->getSeed())
            + static_cast<uint64_t>(static_cast<int64_t>(box.minX) * static_cast<int64_t>(box.minZ)));
    }

    // True if (dx, dz) is within 3 of a room already in rcoords.
    static bool isNearRoom(int32_t dx, int32_t dz, const std::vector<int32_t>& rcoords) {
        // Covering the origin would make the maze fail.
        if (dx == 1 && dz == 1) return true;
        for (std::size_t i = 0; i < rcoords.size() / 2; ++i) {
            const int32_t rx = rcoords[i * 2];
            const int32_t rz = rcoords[i * 2 + 1];
            if (rx == 0 && rz == 0) continue;
            if (std::abs(dx - rx) < 3 && std::abs(dz - rz) < 3) return true;
        }
        return false;
    }

    void decorate3x3Rooms(WorldGenLevel* level, const std::vector<int32_t>& rcoords,
                          const BoundingBox& chunkBB) const {
        for (std::size_t i = 0; i < rcoords.size() / 2; ++i) {
            // Maze coordinates to structure coordinates.
            const int32_t dx = rcoords[i * 2] * 3 + 3;
            const int32_t dz = rcoords[i * 2 + 1] * 3 + 3;
            decorate3x3Room(level, dx, dz, chunkBB);
        }
    }

    // A 3x3-cell (11x11 block) room.
    void decorate3x3Room(WorldGenLevel* level, int32_t x, int32_t z, const BoundingBox& chunkBB) const {
        // RandomSource.create(world.getSeed() ^ x + z): '+' binds tighter.
        LegacyRandomSource roomRNG(level->getSeed() ^ static_cast<int64_t>(x + z));

        // A few jack-o'-lanterns.
        roomJackO(level, roomRNG, x, z, 8, chunkBB);
        if (roomRNG.nextInt(4) == 0) {
            roomJackO(level, roomRNG, x, z, 8, chunkBB);
        }

        // Every room has one spawner.
        roomSpawner(level, roomRNG, x, z, 8, chunkBB);

        // And 1-2 chests.
        roomTreasure(level, roomRNG, x, z, 8, chunkBB, "twilightforest:hedge_maze");
        if (roomRNG.nextInt(4) == 0) {
            roomTreasure(level, roomRNG, x, z, 8, chunkBB, "twilightforest:hedge_cloth");
        }
    }

    void roomSpawner(WorldGenLevel* level, LegacyRandomSource& random, int32_t x, int32_t z, int32_t diameter,
                     const BoundingBox& chunkBB) const {
        const int32_t rx = x + random.nextInt(diameter) - (diameter / 2);
        const int32_t rz = z + random.nextInt(diameter) - (diameter / 2);
        const char* mobID;
        switch (random.nextInt(3)) {
            case 1: mobID = "twilightforest:swarm_spider"; break;
            case 2: mobID = "twilightforest:hostile_wolf"; break;
            default: mobID = "twilightforest:hedge_spider"; break;
        }
        setSpawner(level, rx, kFloorLevel, rz, chunkBB, mobID);
    }

    void roomTreasure(WorldGenLevel* level, LegacyRandomSource& random, int32_t x, int32_t z, int32_t diameter,
                      const BoundingBox& chunkBB, const std::string& table) const {
        const int32_t rx = x + random.nextInt(diameter) - (diameter / 2);
        const int32_t rz = z + random.nextInt(diameter) - (diameter / 2);
        const int32_t xDiff = x - rx;
        const int32_t zDiff = z - rz;

        const core::BlockPos pos(worldX(rx, rz), worldY(kFloorLevel), worldZ(rx, rz));
        if (!chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())) return;
        if (level->getBlockState(pos)->is(Blocks::CHEST)) return;
        Direction facing;
        if (std::abs(xDiff) > std::abs(zDiff)) {
            facing = xDiff < 0 ? Direction::WEST : Direction::EAST;
        } else {
            facing = zDiff < 0 ? Direction::NORTH : Direction::SOUTH;
        }
        tf_common::generateChest(level, pos, facing, false, table);
    }

    void roomJackO(WorldGenLevel* level, LegacyRandomSource& random, int32_t x, int32_t z, int32_t diameter,
                   const BoundingBox& chunkBB) const {
        const int32_t rx = x + random.nextInt(diameter) - (diameter / 2);
        const int32_t rz = z + random.nextInt(diameter) - (diameter / 2);
        // Direction.from2DDataValue(rand.nextInt(4)): SOUTH, WEST, NORTH, EAST.
        const Direction facing = core::fromHorizontalIndex(random.nextInt(4));
        placeBlock(level, mazeBlocks().jackOLantern[static_cast<int>(facing)], rx, kFloorLevel, rz, chunkBB);
    }
};

} // namespace

// ============================================================================
// HedgeMazeStructure.adjustForTerrain + getFirstPiece
// ============================================================================
bool buildHedgeMaze(const StructureInfo& info, GenerationContext& ctx, LegacyRandomSource& firstPieceRandom,
                    int32_t x, int32_t y, int32_t z, StructureStartData& out) {
    (void)info;
    (void)firstPieceRandom;  // getFirstPiece draws nothing; no children
    (void)y;                 // HedgeMazeStructure overrides adjustForTerrain:
    const int32_t groundY = adjustForTerrain(ctx, x, z, 24, 4);  // WorldUtil.adjustForTerrain(ctx, x, z, 24, 4)

    // new HedgeMazeComponent(0, x + 1, y + 4, z + 1): the maze is 50 x 50.
    const int32_t px = x + 1;
    const int32_t py = groundY + 4;
    const int32_t pz = z + 1;
    StructurePieceData piece;
    piece.pieceType = "twilightforest:tfhedge";
    piece.boundingBox = tf_common::getComponentToAddBoundingBox(
        px, py, pz, -kRadius, -3, -kRadius, kRadius * 2, 10, kRadius * 2, static_cast<int>(Direction::SOUTH), false);
    piece.rotation = tf_common::rotationName(OrientedPieceBehavior::ROT_CW180);
    piece.genDepth = 0;
    out.pieces.push_back(std::move(piece));
    out.behaviors.push_back(std::make_shared<HedgeMazeComponent>());
    return true;
}

// ============================================================================
// HedgeMazeStructure.getStructureTerraformer ->
// CustomDensitySource.getInvertedPyramidTerraformer(start, 0, 4)
// ============================================================================
TwilightTerraformer hedgeMazeTerraformer(const StructureInfo& info, const StructureStartData& start,
                                         const ::world::ChunkPos& chunkPos) {
    (void)info;
    (void)chunkPos;
    constexpr int32_t kYOffset = 0;
    constexpr int32_t kHorizontalPadding = 4;
    // Structure.adjustBoundingBox inflates a start's box by 12 for any
    // terrain adaptation; the start box here is that inflated box.
    constexpr int32_t kVanillaBoxInflationFactor = 12;

    const BoundingBox& structureBox = start.boundingBox;
    const int32_t squareHorizontalSpan =
        std::max(structureBox.getXSpan(), structureBox.getZSpan()) - 2 * kVanillaBoxInflationFactor;
    const double centerX = static_cast<double>(static_cast<float>(structureBox.centerX()) + 0.5f);
    const double centerZ = static_cast<double>(static_cast<float>(structureBox.centerZ()) + 0.5f);
    const int32_t yOffset = kYOffset + structureBox.minY + kVanillaBoxInflationFactor;

    // DensityFunctions.yClampedGradient(fromY, toY, fromValue, toValue).
    auto yClampedGradient = [](int32_t blockY, int32_t fromY, int32_t toY, double fromValue, double toValue) {
        return Mth::clampedMap(static_cast<double>(blockY), static_cast<double>(fromY), static_cast<double>(toY),
                               fromValue, toValue);
    };
    const double pyramidConstant = static_cast<double>(-(squareHorizontalSpan >> 1) - kHorizontalPadding);

    // min(0, max(yClampedGradient(yOffset - 1, yOffset, 1, -1),
    //            -(span >> 1) - padding + yClampedGradient(yOffset - 1,
    //            yOffset + span, 1, -1 - span) + AbsoluteDifference.Max(span, cx, cz)))
    return [=](int32_t blockX, int32_t blockY, int32_t blockZ) -> double {
        const double absoluteDifference = std::min(
            std::max(std::abs(static_cast<double>(blockX) - centerX), std::abs(static_cast<double>(blockZ) - centerZ)),
            static_cast<double>(squareHorizontalSpan));
        const double invertedPyramid =
            yClampedGradient(blockY, yOffset - 1, yOffset + squareHorizontalSpan, 1.0,
                             static_cast<double>(-1 - squareHorizontalSpan))
            + absoluteDifference;
        const double cut = std::max(yClampedGradient(blockY, yOffset - 1, yOffset, 1.0, -1.0),
                                    pyramidConstant + invertedPyramid);
        return std::min(0.0, cut);
    };
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
