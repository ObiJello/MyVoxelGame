#include "levelgen/structure/twilight/TwilightNagaCourtyard.h"

#include "levelgen/structure/twilight/TwilightTemplatePieces.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/Structures.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — type/NagaCourtyardStructure.java, courtyard/
// CourtyardMain.java, courtyard/StructureMazeGenerator.java and the courtyard
// pieces (NagaCourtyardHedge*Component, CourtyardTerrace(Hedge),
// CourtyardPathPiece, CourtyardWall*, CourtyardTerraceTemplateProcessor).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

namespace {

namespace tt = twilight_template;
using core::BlockPos;
using core::Direction;

// CourtyardMain constants.
constexpr int kRowOfCells = 8;
// (int) ((((ROW_OF_CELLS - 2) / 2.0F) * 12.0F) + 8) = 44.
constexpr int kRadius = static_cast<int>((((kRowOfCells - 2) / 2.0f) * 12.0f) + 8);
constexpr float kHedgeFloof = 0.5f;
constexpr float kWallDecay = 0.1f;
constexpr float kWallIntegrity = 0.95f;
constexpr int kMazeSize = kRowOfCells - 1;  // "Size in cell count" - 1

const char* const kCenterPool = "twilightforest:courtyard/center";

// ---------------------------------------------------------------------------
// StructureMazeGenerator.WallFacing — EAST, SOUTH, WEST, NORTH.
// ---------------------------------------------------------------------------
struct WallFacing {
    int bite;
    int opposite;
    int inverted;
    int invertedOpposite;
    int xOffset;
    int zOffset;
    bool test(int directions) const { return (bite & directions) == bite; }
};

constexpr int kEast = 0;
constexpr int kSouth = 1;
constexpr int kWest = 2;
constexpr int kNorth = 3;
constexpr WallFacing kWallFacings[4] = {
    {0b0001, 0b0100, 0b1110, 0b1011, 1, 0},   // EAST
    {0b0010, 0b1000, 0b1101, 0b0111, 0, 1},   // SOUTH
    {0b0100, 0b0001, 0b1011, 0b1110, -1, 0},  // WEST
    {0b1000, 0b0010, 0b0111, 0b1101, 0, -1},  // NORTH
};

// ---------------------------------------------------------------------------
// twilightforest.enums.Diagonals — TOP_RIGHT, BOTTOM_RIGHT, BOTTOM_LEFT,
// TOP_LEFT (operationX, operationY, isTop, isLeft).
// ---------------------------------------------------------------------------
struct Diagonal {
    bool invertX;   // operationX: rX - x (else x)
    bool invertY;   // operationY: rY - y (else y)
    bool top;
    bool left;
    int convertX(int x, int range) const { return invertX ? range - x : x; }
    int convertY(int y, int range) const { return invertY ? range - y : y; }
};

constexpr Diagonal kDiagonals[4] = {
    {true, false, true, false},   // TOP_RIGHT
    {true, true, false, false},   // BOTTOM_RIGHT
    {false, true, false, true},   // BOTTOM_LEFT
    {false, false, true, true},   // TOP_LEFT
};

// ---------------------------------------------------------------------------
// Behaviors
// ---------------------------------------------------------------------------

// CourtyardMain.postProcess: "Boss spawner placed via template" — nothing.
class CourtyardMainBehavior final : public StructurePieceBehavior {
public:
    void postProcess(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const BoundingBox&,
                     const ::world::ChunkPos&, const core::BlockPos&, StructurePieceData&) override {}
};

// NagaCourtyardHedgeAbstractComponent (TFStructureComponentTemplate): the
// hedge template, then its "big" leaf overlay (air ignored, 50 % rot), both
// at the rotated template position with pivot ZERO and no known shape.
class HedgeBehavior final : public StructurePieceBehavior {
public:
    HedgeBehavior(std::string hedge, std::string hedgeBig, int rotation, BlockPos rotatedPosition)
        : m_hedge(std::move(hedge)), m_hedgeBig(std::move(hedgeBig)), m_rotation(rotation),
          m_rotatedPosition(rotatedPosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos&,
                     const core::BlockPos&, StructurePieceData&) override {
        TemplatePlaceSettings hedgeSettings;
        hedgeSettings.rotation = m_rotation;
        hedgeSettings.knownShape = false;
        hedgeSettings.keepLiquids = true;
        hedgeSettings.processors.push_back(tt::nagastoneVariants());

        TemplatePlaceSettings hedgeBigSettings = hedgeSettings;
        hedgeBigSettings.processors.push_back(tt::ignoreAir());
        hedgeBigSettings.processors.push_back(tt::blockRot(kHedgeFloof));

        TemplateEngine::placeInWorld(level, m_hedge, m_rotatedPosition, m_rotatedPosition,
                                     hedgeSettings, random, chunkBB);
        TemplateEngine::placeInWorld(level, m_hedgeBig, m_rotatedPosition, m_rotatedPosition,
                                     hedgeBigSettings, random, chunkBB);
    }

private:
    std::string m_hedge;
    std::string m_hedgeBig;
    int m_rotation;
    BlockPos m_rotatedPosition;
};

// CourtyardTerraceHedge: the terrace, then terrace_hedge_big at the template
// position with the piece's settings (known shape) but only air-ignore + 50 %
// rot. Java places the overlay unclipped; the overlay lies inside the piece
// box and its processors are positional, so clipping each chunk's pass to
// that chunk produces the same blocks.
class TerraceHedgeBehavior final : public tt::TemplatePieceBehavior {
public:
    explicit TerraceHedgeBehavior(Config config) : TemplatePieceBehavior(std::move(config)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        TemplatePieceBehavior::postProcess(level, generator, random, chunkBB, chunkPos,
                                           referencePos, self);
        TemplatePlaceSettings big;
        big.rotation = m_config.rotation;
        big.mirror = m_config.mirror;
        big.rotationPivot = m_config.pivot;
        big.knownShape = m_config.knownShape;
        big.keepLiquids = m_config.keepLiquids;
        big.processors.push_back(tt::ignoreAir());
        big.processors.push_back(tt::blockRot(kHedgeFloof));
        TemplateEngine::placeInWorld(level, "twilightforest:courtyard/terrace_hedge_big",
                                     m_config.templatePosition, referencePos, big, random, chunkBB);
    }
};

// ---------------------------------------------------------------------------
// The builder (StructurePiecesBuilder + the piece constructors).
// ---------------------------------------------------------------------------
class CourtyardBuilder {
public:
    CourtyardBuilder(StructureStartData& out, LegacyRandomSource& random)
        : m_out(out), m_random(random) {}

    void add(StructurePieceData data, std::shared_ptr<StructurePieceBehavior> behavior) {
        m_out.pieces.push_back(std::move(data));
        m_out.behaviors.push_back(std::move(behavior));
    }

    // TFStructureComponentTemplate.setup: rotatedPosition + the TF bounding
    // box (x/z spans NOT reduced by one, as in the mod).
    void addHedge(const char* pieceType, const char* hedge, const char* hedgeBig, int genDepth,
                  int x, int y, int z, int rotation) {
        const std::string hedgeId = std::string("twilightforest:courtyard/") + hedge;
        const std::string hedgeBigId = std::string("twilightforest:courtyard/") + hedgeBig;
        const std::array<int, 3> size = tt::templateSize(hedgeId, rotation);
        BlockPos rotatedPosition(x, y, z);
        if (rotation == tt::ROT_CW90 || rotation == tt::ROT_CW180) {
            rotatedPosition = rotatedPosition.east(size[2] - 1);
        }
        if (rotation == tt::ROT_CW180 || rotation == tt::ROT_CCW90) {
            rotatedPosition = rotatedPosition.south(size[0] - 1);
        }
        BoundingBox box(0, 0, 0, size[0], size[1] - 1, size[2]);
        switch (rotation) {
            case tt::ROT_CW90: box.move(-size[0], 0, 0); break;
            case tt::ROT_CCW90: box.move(0, 0, -size[2]); break;
            case tt::ROT_CW180: box.move(-size[0], 0, -size[2]); break;
            default: break;
        }
        box.move(rotatedPosition.getX(), rotatedPosition.getY(), rotatedPosition.getZ());
        add(tt::makePiece(pieceType, box, rotation, genDepth, hedgeId),
            std::make_shared<HedgeBehavior>(hedgeId, hedgeBigId, rotation, rotatedPosition));
    }

    void addCap(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfnccp", "hedge_end", "hedge_end_big", genDepth, x, y, z, rotation);
    }
    void addCapPillar(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfnccpp", "hedge_end_pillar", "hedge_end_pillar_big", genDepth, x, y, z, rotation);
    }
    void addCorner(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfnccr", "hedge_corner", "hedge_corner_big", genDepth, x, y, z, rotation);
    }
    void addTJunction(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfnct", "hedge_t", "hedge_t_big", genDepth, x, y, z, rotation);
    }
    void addLine(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfncln", "hedge_line", "hedge_line_big", genDepth, x, y, z, rotation);
    }
    void addIntersection(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfncis", "hedge_intersection", "hedge_intersection_big", genDepth, x, y, z, rotation);
    }
    void addPadder(int genDepth, int x, int y, int z, int rotation) {
        addHedge("twilightforest:tfncpd", "hedge_between", "hedge_between_big", genDepth, x, y, z, rotation);
    }

    // CourtyardTerrace / CourtyardTerraceHedge (TwilightTemplateStructurePiece
    // with CourtyardTerraceTemplateProcessor, NagastoneVariants,
    // StoneBricksVariants; PieceBeardifierModifier BEARD_BOX, delta 3).
    void addTerrace(int genDepth, int x, int y, int z, const char* templateName, bool hedge) {
        tt::TemplatePieceBehavior::Config config;
        config.templateId = std::string("twilightforest:courtyard/") + templateName;
        config.rotation = tt::ROT_NONE;
        config.templatePosition = BlockPos(x, y, z);
        config.processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) {
            chain.push_back(tt::courtyardTerrace(level));
            chain.push_back(tt::nagastoneVariants());
            chain.push_back(tt::stoneBricksVariants());
        };
        const BoundingBox box = tt::templateBoundingBox(config.templateId, TemplatePlaceSettings{},
                                                        config.templatePosition);
        StructurePieceData data = tt::makePiece(hedge ? "twilightforest:tfnche" : "twilightforest:tfncte",
                                                box, tt::ROT_NONE, genDepth, config.templateId);
        tt::applyBeardifierModifier(data, true, 3);
        std::shared_ptr<StructurePieceBehavior> behavior;
        if (hedge) {
            behavior = std::make_shared<TerraceHedgeBehavior>(std::move(config));
        } else {
            behavior = std::make_shared<tt::TemplatePieceBehavior>(std::move(config));
        }
        add(std::move(data), std::move(behavior));
    }

    // CourtyardPathPiece: template at (x, y + 1, z), placed one lower
    // (placePieceAdjusted(-1)).
    void addPath(int genDepth, int x, int y, int z) {
        tt::TemplatePieceBehavior::Config config;
        config.templateId = "twilightforest:courtyard/pathway";
        config.rotation = tt::ROT_NONE;
        config.templatePosition = BlockPos(x, y + 1, z);
        config.adjustY = -1;
        config.processors = [](std::vector<tt::Processor>& chain, WorldGenLevel*) {
            chain.push_back(tt::nagastoneVariants());
        };
        const BoundingBox box = tt::templateBoundingBox(config.templateId, TemplatePlaceSettings{},
                                                        config.templatePosition);
        add(tt::makePiece("twilightforest:tfncpa", box, tt::ROT_NONE, genDepth, config.templateId),
            std::make_shared<tt::TemplatePieceBehavior>(std::move(config)));
    }

    // CourtyardWall / WallPadder / WallCornerOuter / WallCornerInner
    // (TwilightDoubleTemplateStructurePiece): the wall with 95 % integrity and
    // the stone variants, then its decayed overlay with 10 % integrity.
    void addWallPiece(const char* pieceType, const char* templateName, int genDepth,
                      int x, int y, int z, int rotation) {
        tt::TemplatePieceBehavior::Config config;
        config.templateId = std::string("twilightforest:courtyard/") + templateName;
        config.rotation = rotation;
        config.templatePosition = BlockPos(x, y, z);
        config.processors = [](std::vector<tt::Processor>& chain, WorldGenLevel*) {
            chain.push_back(tt::blockRot(kWallIntegrity));
            chain.push_back(tt::smoothStoneVariants());
            chain.push_back(tt::nagastoneVariants());
            chain.push_back(tt::stoneBricksVariants());
            chain.push_back(tt::cobbleVariants());
        };
        TemplatePlaceSettings settings;
        settings.rotation = rotation;
        const BoundingBox box = tt::templateBoundingBox(config.templateId, settings, config.templatePosition);
        const std::string overlay = config.templateId + "_decayed";
        add(tt::makePiece(pieceType, box, rotation, genDepth, config.templateId),
            std::make_shared<tt::DoubleTemplatePieceBehavior>(
                std::move(config), overlay,
                [](std::vector<tt::Processor>& chain, WorldGenLevel*) {
                    chain.push_back(tt::blockRot(kWallDecay));
                    chain.push_back(tt::cobbleVariants());
                }));
    }

    void addWall(int genDepth, int x, int y, int z, int rotation) {
        addWallPiece("twilightforest:tfncwl", "courtyard_wall", genDepth, x, y, z, rotation);
    }
    void addWallPadder(int genDepth, int x, int y, int z, int rotation) {
        addWallPiece("twilightforest:tfncwp", "courtyard_wall_padding", genDepth, x, y, z, rotation);
    }
    void addWallCornerOuter(int genDepth, int x, int y, int z, int rotation) {
        addWallPiece("twilightforest:tfncwc", "courtyard_wall_corner", genDepth, x, y, z, rotation);
    }
    void addWallCornerInner(int genDepth, int x, int y, int z, int rotation) {
        addWallPiece("twilightforest:tfncwa", "courtyard_wall_corner_inner", genDepth, x, y, z, rotation);
    }

    LegacyRandomSource& random() { return m_random; }

private:
    StructureStartData& m_out;
    LegacyRandomSource& m_random;
};

// ---------------------------------------------------------------------------
// StructureMazeGenerator.generateMaze (maximumClipping 2).
// ---------------------------------------------------------------------------
struct Maze {
    int cells[kMazeSize][kMazeSize] = {};
    int cornerClipping[4][2] = {};
};

void generateMaze(Maze& maze, LegacyRandomSource& random, int width, int height, int maximumClipping) {
    int rotations[kMazeSize][kMazeSize] = {};
    for (int x = 0; x < width - 1; ++x) {
        for (int y = 0; y < height - 1; ++y) {
            rotations[x][y] = random.nextInt(4);
            maze.cells[x][y] |= kWallFacings[rotations[x][y]].bite;
        }
    }

    // Java: final int[][] mazeLocal = maze.clone() - a SHALLOW clone, so the
    // row arrays are shared and every mazeLocal read below sees the live maze.
    const int halfWayPointX = (width / 2) - 1;
    const int halfWayPointY = (height / 2) - 1;

    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            if (x == halfWayPointX && y == halfWayPointY) continue;
            const int rot = rotations[x][y];
            const WallFacing& facing = kWallFacings[rot];
            if (rot == kWest && x > 0) {
                if (!facing.test(maze.cells[x - 1][y])) {
                    maze.cells[x - 1][y] |= facing.opposite;
                } else {
                    maze.cells[x][y] &= facing.inverted;
                    maze.cells[x - 1][y] &= kWallFacings[rotations[x - 1][y]].invertedOpposite;
                }
            }
            if (rot == kNorth && y > 0) {
                if (!facing.test(maze.cells[x][y - 1])) {
                    maze.cells[x][y - 1] |= facing.opposite;
                } else {
                    maze.cells[x][y] &= facing.inverted;
                    maze.cells[x][y - 1] &= kWallFacings[rotations[x][y - 1]].invertedOpposite;
                }
            }
            if (rot == kEast && x < width - 2) {
                if (!facing.test(maze.cells[x + 1][y])) {
                    maze.cells[x + 1][y] |= facing.opposite;
                } else {
                    maze.cells[x][y] &= facing.inverted;
                    maze.cells[x + 1][y] &= kWallFacings[rotations[x + 1][y]].invertedOpposite;
                }
            }
            if (rot == kSouth && y < height - 2) {
                if (!facing.test(maze.cells[x][y + 1])) {
                    maze.cells[x][y + 1] |= facing.opposite;
                } else {
                    maze.cells[x][y] &= facing.inverted;
                    maze.cells[x][y + 1] &= kWallFacings[rotations[x][y + 1]].invertedOpposite;
                }
            }
        }
    }

    for (const WallFacing& facing : kWallFacings) {
        maze.cells[halfWayPointX + facing.xOffset][halfWayPointY + facing.zOffset] &= facing.invertedOpposite;
    }
    maze.cells[halfWayPointX][halfWayPointY] = 0b10000;

    for (int x = 1; x < kMazeSize; ++x) {
        for (int y = 1; y < kMazeSize; ++y) {
            if (maze.cells[x][y] == 0) {
                if (maze.cells[x - 1][y] == 0) {
                    maze.cells[x][y] |= kWallFacings[kWest].bite;
                    maze.cells[x - 1][y] |= kWallFacings[kWest].opposite;
                }
                if (maze.cells[x][y - 1] == 0) {
                    maze.cells[x][y] |= kWallFacings[kNorth].bite;
                    maze.cells[x][y - 1] |= kWallFacings[kNorth].opposite;
                }
            }
        }
    }

    for (int d = 0; d < 4; ++d) {
        maze.cornerClipping[d][0] = random.nextInt(maximumClipping) + 1;
        maze.cornerClipping[d][1] = random.nextInt(maximumClipping) + 1;
        for (int y = 0; y < maze.cornerClipping[d][0]; ++y) {
            for (int x = 0; x < maze.cornerClipping[d][1]; ++x) {
                maze.cells[kDiagonals[d].convertX(x, width - 2)][kDiagonals[d].convertY(y, height - 2)] |= 0b10000;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// StructureMazeGenerator.processInnerWallsAndFloor.
// ---------------------------------------------------------------------------
void processInnerWallsAndFloor(CourtyardBuilder& b, const Maze& m, const BoundingBox& sc,
                               int width, int height, int offset) {
    LegacyRandomSource& random = b.random();
    const auto& maze = m.cells;
    for (int x = 0; x < width - 1; ++x) {
        for (int y = 0; y < height - 1; ++y) {
            const bool xCenter = x == (width / 2) - 1;
            const bool yCenter = y == (height / 2) - 1;
            // -------- HEDGE
            if (!(xCenter || yCenter) && (maze[x][y] & 0b10000) == 0b10000) continue;

            int rotation = 0;
            int xBB = sc.minX + (x * 12) + offset;
            const int yBB = sc.minY + 1;
            int zBB = sc.minZ + (y * 12) + offset;

            if (!(xCenter && yCenter)) {
                // The mod's fall-through switch: each case adds one quarter turn.
                switch (maze[x][y] & 0b1111) {
                    case 0b0010:  // FACE SOUTH
                    case 0b0001:  // FACE EAST
                    case 0b1000:  // FACE NORTH
                    case 0b0100: {  // FACE WEST
                        const int bits = maze[x][y] & 0b1111;
                        rotation = bits == 0b0010 ? 3 : bits == 0b0001 ? 2 : bits == 0b1000 ? 1 : 0;
                        if (random.nextBoolean()) {
                            b.addCap((x * width) + y, xBB, yBB, zBB, rotation);
                        } else {
                            b.addCapPillar((x * width) + y, xBB, yBB, zBB, rotation);
                        }
                        break;
                    }
                    case 0b1001:  // NORTH EAST
                    case 0b1100:  // NORTH WEST
                    case 0b0110:  // SOUTH WEST
                    case 0b0011: {  // SOUTH EAST
                        const int bits = maze[x][y] & 0b1111;
                        rotation = bits == 0b1001 ? 3 : bits == 0b1100 ? 2 : bits == 0b0110 ? 1 : 0;
                        b.addCorner(maze[x][y], xBB, yBB, zBB, rotation);
                        break;
                    }
                    case 0b1101:  // NOT SOUTH
                    case 0b1110:  // NOT EAST
                    case 0b0111:  // NOT NORTH
                    case 0b1011: {  // NOT WEST
                        const int bits = maze[x][y] & 0b1111;
                        rotation = bits == 0b1101 ? 3 : bits == 0b1110 ? 2 : bits == 0b0111 ? 1 : 0;
                        b.addTJunction(maze[x][y], xBB, yBB, zBB, rotation);
                        break;
                    }
                    case 0b1010:  // NORTH AND SOUTH
                    case 0b0101: {  // EAST AND WEST
                        rotation = (maze[x][y] & 0b1111) == 0b1010 ? 1 : 0;
                        b.addLine(maze[x][y], xBB, yBB, zBB, rotation);
                        break;
                    }
                    case 0b1111:
                        b.addIntersection(maze[x][y], xBB, yBB, zBB, tt::ROT_NONE);
                        break;
                    default:
                        if (random.nextInt(150) == 0) {
                            b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_statue", false);
                        } else {
                            switch (random.nextInt(5)) {
                                case 1: b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_duct", false); break;
                                case 2: b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_channel", false); break;
                                case 3: b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_reservoir", false); break;
                                case 4: b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_hedge", true); break;
                                default: b.addTerrace(maze[x][y], xBB - 6, yBB - 3, zBB - 6, "terrace_fire", false); break;
                            }
                        }
                        break;
                }
            }

            // -------- Hedge Connectors
            xBB = sc.minX + (x * 12) + offset;
            zBB = sc.minZ + (y * 12) + offset;

            const bool connectWest = kWallFacings[kWest].test(maze[x][y]);
            const bool connectNorth = kWallFacings[kNorth].test(maze[x][y]);
            const bool connectEast = kWallFacings[kEast].test(maze[x][y]);
            const bool connectSouth = kWallFacings[kSouth].test(maze[x][y]);

            if (connectWest) {
                b.addPadder(maze[x][y], xBB - 1, yBB, zBB, tt::ROT_NONE);
                if (x > 0 && (maze[x - 1][y] & 0b10000) != 0b10000) {
                    b.addPadder(maze[x][y], xBB - 7, yBB, zBB, tt::ROT_NONE);
                }
                b.addLine(maze[x][y], xBB - 6, yBB, zBB, tt::ROT_NONE);
            }
            if (connectNorth) {
                b.addPadder(maze[x][y], xBB + 4, yBB, zBB - 1, tt::ROT_CW90);
                if (y > 0 && (maze[x][y - 1] & 0b10000) != 0b10000) {
                    b.addPadder(maze[x][y], xBB + 4, yBB, zBB - 7, tt::ROT_CW90);
                }
                b.addLine(maze[x][y], xBB, yBB, zBB - 6, tt::ROT_CW90);
            }
            if ((x >= width - 2 || (maze[x + 1][y] & 0b10000) == 0b10000) && connectEast) {
                b.addPadder(maze[x][y], xBB + 5, yBB, zBB, tt::ROT_NONE);
                b.addLine(maze[x][y], xBB + 6, yBB, zBB, tt::ROT_NONE);
            }
            if ((y >= height - 2 || (maze[x][y + 1] & 0b10000) == 0b10000) && connectSouth) {
                b.addPadder(maze[x][y], xBB + 4, yBB, zBB + 5, tt::ROT_CW90);
                b.addLine(maze[x][y], xBB, yBB, zBB + 6, tt::ROT_CW90);
            }

            const bool hasNoTerrace = (maze[x][y] & 0b1111) != 0;
            const bool westSafe = x == 0 || (maze[x - 1][y] & 0b10000) == 0b10000 || (maze[x - 1][y] & 0b1111) != 0;
            const bool northSafe = y == 0 || (maze[x][y - 1] & 0b10000) == 0b10000 || (maze[x][y - 1] & 0b1111) != 0;
            const bool eastSafe = x == width - 2 || (maze[x + 1][y] & 0b10000) == 0b10000;
            const bool southSafe = y == height - 2 || (maze[x][y + 1] & 0b10000) == 0b10000;
            const bool westNorthSafe = x == 0 || y == 0 || maze[x - 1][y - 1] != 0;
            const bool westSouthSafe = x == 0 || y >= height - 2 || maze[x - 1][y + 1] != 0;
            const bool eastNorthSafe = x >= width - 2 || y == 0 || maze[x + 1][y - 1] != 0;
            const bool eastSouthSafe = x >= width - 2 || y >= height - 2 || maze[x + 1][y + 1] != 0;

            // -------- PATHS - cardinal
            if (xCenter && yCenter) b.addPath(maze[x][y], xBB - 1, yBB - 1, zBB - 1);
            if (hasNoTerrace && westSafe && !connectWest) b.addPath(maze[x][y], xBB - 7, yBB - 1, zBB - 1);
            if (hasNoTerrace && northSafe && !connectNorth) b.addPath(maze[x][y], xBB - 1, yBB - 1, zBB - 7);
            if (hasNoTerrace && eastSafe) b.addPath(maze[x][y], xBB + 5, yBB - 1, zBB - 1);
            if (hasNoTerrace && southSafe) b.addPath(maze[x][y], xBB - 1, yBB - 1, zBB + 5);

            // -------- PATHS - Diagonal
            if (hasNoTerrace && westSafe && northSafe && westNorthSafe) b.addPath(maze[x][y], xBB - 7, yBB - 1, zBB - 7);
            if (hasNoTerrace && westSafe && southSafe && westSouthSafe) b.addPath(maze[x][y], xBB - 7, yBB - 1, zBB + 5);
            if (hasNoTerrace && eastSafe && northSafe && eastNorthSafe) b.addPath(maze[x][y], xBB + 5, yBB - 1, zBB - 7);
            if (hasNoTerrace && eastSafe && southSafe && eastSouthSafe) b.addPath(maze[x][y], xBB + 5, yBB - 1, zBB + 5);
        }
    }
}

// ---------------------------------------------------------------------------
// StructureMazeGenerator.processOuterWalls.
// ---------------------------------------------------------------------------
void processOuterWalls(CourtyardBuilder& b, const Maze& m, const BoundingBox& sc,
                       int width, int height, int offset) {
    const auto& clip = m.cornerClipping;
    const int y = sc.minY + 1;
    for (int d = 0; d < 4; ++d) {
        const Diagonal& diagonal = kDiagonals[d];
        // Walls at corner notches going with X Axis, crossing Z Axis.
        const int zBoundX = diagonal.top ? sc.minZ + (clip[d][0] * 12) - 3
                                         : sc.maxZ - (clip[d][0] * 12) + 1;
        b.addWallPadder((clip[d][1] * 2) + 1, diagonal.left ? sc.minX + 2 : sc.maxX - 2, y, zBoundX, tt::ROT_NONE);
        const int xPadOffset = diagonal.left ? 11 : -1;
        for (int i = 0; i < clip[d][1] - 1; ++i) {
            const int xBound = diagonal.left ? sc.minX + (i * 12) + 3 : sc.maxX - (i * 12) - 13;
            b.addWall(i * 2, xBound, y, zBoundX, tt::ROT_NONE);
            b.addWallPadder((i * 2) + 1, xBound + xPadOffset, y, zBoundX, tt::ROT_NONE);
        }

        // Walls at corner notches going with Z Axis, crossing X Axis.
        const int xBoundZ = diagonal.left ? sc.minX + (clip[d][1] * 12) - 1
                                          : sc.maxX - (clip[d][1] * 12) + 3;
        b.addWallPadder((clip[d][1] * 2) + 1, xBoundZ, y, diagonal.top ? sc.minZ + 2 : sc.maxZ - 2, tt::ROT_CW90);
        const int zPadOffset = diagonal.top ? 11 : -1;
        for (int i = 0; i < clip[d][0] - 1; ++i) {
            const int zBound = diagonal.top ? sc.minZ + (i * 12) + 3 : sc.maxZ - (i * 12) - 13;
            b.addWall(i * 2, xBoundZ, y, zBound, tt::ROT_CW90);
            b.addWallPadder((i * 2) + 1, xBoundZ, y, zBound + zPadOffset, tt::ROT_CW90);
        }

        // WALL CORNERS
        const int wallCornerInnerX = sc.minX + (diagonal.convertX(clip[d][1], width - 1) * 12);
        const int wallCornerInnerZ = sc.minZ + (diagonal.convertY(clip[d][0], height - 1) * 12);
        const int cornerRotation = d % 4;  // rotations[diagonal.ordinal() % rotations.length]
        const bool shiftX = cornerRotation == tt::ROT_CW180 || cornerRotation == tt::ROT_CCW90;
        const bool shiftZ = cornerRotation == tt::ROT_CW90 || cornerRotation == tt::ROT_CW180;

        // These touch upper/lower borders.
        b.addWallCornerOuter(d * 3,
            wallCornerInnerX + (shiftZ ? (shiftX ? 1 : 7) : (shiftX ? -3 : 3)),
            y,
            (diagonal.top ? sc.minZ : sc.maxZ - 1) + (shiftZ ? (shiftX ? 4 : 0) : (shiftX ? 1 : -3)),
            cornerRotation);
        // These touch side borders.
        b.addWallCornerOuter((d * 3) + 1,
            (diagonal.left ? sc.minX : sc.maxX - 1) + (shiftZ ? (shiftX ? 1 : 4) : (shiftX ? -3 : 0)),
            y,
            wallCornerInnerZ + (shiftZ ? (shiftX ? 7 : 3) : (shiftX ? 1 : -3)),
            cornerRotation);
        // These are inner corners.
        b.addWallCornerInner((d * 3) + 3,
            wallCornerInnerX + (shiftZ ? (shiftX ? -1 : 13) : (shiftX ? -9 : 5)),
            y,
            wallCornerInnerZ + (shiftZ ? (shiftX ? 13 : 5) : (shiftX ? -1 : -9)),
            cornerRotation);
    }

    // Top / North
    for (int i = clip[3][1]; i < (width - 1) - clip[0][1]; ++i) {
        b.addWall(i, sc.minX + (i * 12) + offset - 3, y, sc.minZ - 3, tt::ROT_NONE);
        b.addWallPadder(i, sc.minX + (i * 12) + offset - 4, y, sc.minZ - 3, tt::ROT_NONE);
    }
    b.addWallPadder((width - 1) - clip[0][1], sc.minX + (((width - 1) - clip[0][1]) * 12) + offset - 4,
                    y, sc.minZ - 3, tt::ROT_NONE);

    // Bottom / South
    for (int i = clip[2][1]; i < (width - 1) - clip[1][1]; ++i) {
        b.addWall(i, sc.minX + (i * 12) + offset - 3, y, sc.maxZ + 1, tt::ROT_NONE);
        b.addWallPadder(i, sc.minX + (i * 12) + offset - 4, y, sc.maxZ + 1, tt::ROT_NONE);
    }
    b.addWallPadder((width - 1) - clip[1][1], sc.minX + (((width - 1) - clip[1][1]) * 12) + offset - 4,
                    y, sc.maxZ + 1, tt::ROT_NONE);

    // Left / West
    for (int i = clip[3][0]; i < (height - 1) - clip[2][0]; ++i) {
        b.addWall(i, sc.minX - 1, y, sc.minZ + (i * 12) + offset - 3, tt::ROT_CW90);
        b.addWallPadder(i, sc.minX - 1, y, sc.minZ + (i * 12) + offset - 4, tt::ROT_CW90);
    }
    b.addWallPadder((height - 1) - clip[2][0], sc.minX - 1, y,
                    sc.minZ + (((height - 1) - clip[2][0]) * 12) + offset - 4, tt::ROT_CW90);

    // Right / East
    for (int i = clip[0][0]; i < (height - 1) - clip[1][0]; ++i) {
        b.addWall(i, sc.maxX + 3, y, sc.minZ + (i * 12) + offset - 3, tt::ROT_CW90);
        b.addWallPadder(i, sc.maxX + 3, y, sc.minZ + (i * 12) + offset - 4, tt::ROT_CW90);
    }
    b.addWallPadder((height - 1) - clip[1][0], sc.maxX + 3, y,
                    sc.minZ + (((height - 1) - clip[1][0]) * 12) + offset - 4, tt::ROT_CW90);
}

} // namespace

// ============================================================================
// NagaCourtyardStructure
// ============================================================================

bool buildNagaCourtyard(const StructureInfo& info, GenerationContext& ctx,
                        LegacyRandomSource& firstPieceRandom,
                        int32_t x, int32_t y, int32_t z, StructureStartData& out) {
    (void)info;
    (void)y;
    // NagaCourtyardStructure.adjustForTerrain: WorldUtil.adjustForTerrain(
    // context, x, z, 40, 4) + 2 (replaces the dispatcher's generic height).
    const int32_t courtyardY = tt::adjustForTerrain(ctx, x, z, 40, 4) + 2;

    // getFirstPiece: new CourtyardMain(random, 0, x + 1, y, z + 1).
    const int mainX = x + 1;
    const int mainY = courtyardY;
    const int mainZ = z + 1;

    // StructureMazeGenerator constructor: the maze is drawn from the
    // first-piece random.
    Maze maze;
    generateMaze(maze, firstPieceRandom, kRowOfCells, kRowOfCells, 2);

    // CourtyardMain: orientation NORTH;
    // getComponentToAddBoundingBox(x, y, z, -R/2, -1, -R/2, R, 10, R, NORTH).
    const BoundingBox mainBox(mainX - kRadius + kRadius / 2, mainY - 1, mainZ - kRadius + kRadius / 2,
                              mainX + kRadius / 2, mainY + 9, mainZ + kRadius / 2);
    const BoundingBox sizeConstraints(mainX - 2 * kRadius + kRadius, mainY - 1,
                                      mainZ - 2 * kRadius + kRadius, mainX + kRadius,
                                      mainY + 9, mainZ + kRadius);

    CourtyardBuilder builder(out, ctx.random);
    builder.add(tt::makePiece("twilightforest:tfncmn", mainBox, tt::ROT_NONE, 0, "-"),
                std::make_shared<CourtyardMainBehavior>());

    // generateFromStartingPiece: addChildren with the context random.
    const int offset = 6;
    processInnerWallsAndFloor(builder, maze, sizeConstraints, kRowOfCells, kRowOfCells, offset);
    processOuterWalls(builder, maze, sizeConstraints, kRowOfCells, kRowOfCells, offset);

    // CourtyardMain.addChildren: the boss-spawner center from its pool.
    // getWorldPos(R/2, 1, R/2) with orientation NORTH.
    const BlockPos centerPos(mainBox.minX + kRadius / 2, mainBox.minY + 1, mainBox.maxZ - kRadius / 2);
    const Direction direction = tt::rotate(tt::randomRotation(ctx.random), Direction::SOUTH);
    const tt::FrontAndTop oriented{Direction::UP, direction};

    // StructureTemplateDefinitions.initializeTemplateFromPool (the
    // random-source overload: no terrain adjustment).
    if (const tt::PoolEntry* entry = tt::randomPoolEntry(ctx.random, kCenterPool)) {
        std::optional<tt::JigsawPlaceContext> placeContext = tt::pickPlaceableJunction(
            centerPos, BlockPos(0, 0, 0), oriented, entry->templateId, "twilightforest:center",
            ctx.random);
        if (placeContext) {
            // chooseRandomProcessors: the pool instance has none (no draws).
            tt::JigsawPiece center = tt::makeJigsawPiece("twilightforest:tfjigsawtemplate", 1,
                                                         entry->templateId, *placeContext);
            StructurePieceData data = tt::makePiece(center.pieceType, center.boundingBox,
                                                    center.rotation(), center.genDepth,
                                                    center.templateId);
            tt::applyBeardifierModifier(data, entry->instance.terrainAdaptation != "none",
                                        entry->instance.yOffset);
            builder.add(std::move(data),
                        std::make_shared<tt::JigsawPieceBehavior>(
                            center, tt::TemplatePieceBehavior::ProcessorFactory{},
                            !entry->instance.ignoreWorldWaterlog));
        }
    }

    // LandmarkStructure stub: stable sort by SortablePiece key (no courtyard
    // piece is sortable - order kept); the lazy template loaders' bounding
    // boxes were computed at construction.
    return true;
}

TwilightTerraformer nagaCourtyardTerraformer(const StructureInfo& info, const StructureStartData& start,
                                             const ::world::ChunkPos& chunkPos) {
    (void)info;
    (void)chunkPos;
    // CustomDensitySource.getInvertedPyramidTerraformer(start, 3, 4).
    constexpr int kYOffset = 3;
    constexpr int kHorizontalPadding = 4;
    constexpr int kVanillaBoxInflationFactor = 12;
    const BoundingBox& structureBox = start.boundingBox;
    const int squareHorizontalSpan = std::max(structureBox.getXSpan(), structureBox.getZSpan())
                                   - 2 * kVanillaBoxInflationFactor;
    const double centerX = static_cast<double>(static_cast<float>(structureBox.centerX()) + 0.5f);
    const double centerZ = static_cast<double>(static_cast<float>(structureBox.centerZ()) + 0.5f);
    const int yOffset = kYOffset + structureBox.minY + kVanillaBoxInflationFactor;
    const double spanD = static_cast<double>(squareHorizontalSpan);
    const double padding = static_cast<double>(-(squareHorizontalSpan >> 1) - kHorizontalPadding);

    return [=](int32_t blockX, int32_t blockY, int32_t blockZ) -> double {
        const double y = static_cast<double>(blockY);
        // AbsoluteDifferenceFunction.Max(span, centerX, centerZ).
        const double absDiff = std::min(std::max(std::abs(blockX - centerX), std::abs(blockZ - centerZ)), spanD);
        const double invertedPyramid =
            Mth::clampedMap(y, static_cast<double>(yOffset - 1), static_cast<double>(yOffset + squareHorizontalSpan),
                            1.0, -1.0 - spanD)
            + absDiff;
        const double lid = Mth::clampedMap(y, static_cast<double>(yOffset - 1), static_cast<double>(yOffset), 1.0, -1.0);
        return std::min(0.0, std::max(lid, padding + invertedPyramid));
    };
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
