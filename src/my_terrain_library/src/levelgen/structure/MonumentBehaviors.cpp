#include "levelgen/structure/PieceBehaviors.h"

#include "levelgen/structure/OrientedPieceBehavior.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "random/LegacyRandomSource.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <algorithm>
#include <memory>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// OceanMonumentPieces.java. The MonumentBuilding constructor builds the room
// graph, fitter assignments, and child pieces AT LAYOUT TIME with the layout
// random (those draws were invisible to the layout gate because nothing reads
// the stream afterwards); postProcess replays the fixed graph per chunk. The
// child pieces are internal to the building piece (Java childPieces list, not
// StructureStart pieces - P lines show only minecraft:omb).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;
using core::Direction;

// Direction.get3DDataValue(): DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5.
constexpr int D_DOWN = 0;
constexpr int D_UP = 1;
constexpr int D_NORTH = 2;
constexpr int D_SOUTH = 3;
constexpr int D_WEST = 4;
constexpr int D_EAST = 5;
// Opposite pairs differ in the lowest bit.
inline int oppositeDir(int dir) { return dir ^ 1; }

constexpr int kStepX[6] = {0, 0, 0, 0, -1, 1};
constexpr int kStepY[6] = {-1, 1, 0, 0, 0, 0};
constexpr int kStepZ[6] = {0, 0, -1, 1, 0, 0};

// Reference: OceanMonumentPiece.getRoomIndex.
inline int roomIndexOf(int x, int y, int z) { return y * 25 + z * 5 + x; }

// Reference: RoomDefinition (graph stored by index; -1 == null connection).
struct RoomDef {
    int index = 0;
    int connections[6] = {-1, -1, -1, -1, -1, -1};
    bool hasOpening[6] = {false, false, false, false, false, false};
    bool claimed = false;
    bool isSource = false;
    int scanIndex = 0;

    bool isSpecial() const { return index >= 75; }
    int countOpenings() const {
        int c = 0;
        for (int i = 0; i < 6; ++i) {
            if (hasOpening[i]) ++c;
        }
        return c;
    }
};

struct MonumentGraph {
    std::vector<RoomDef> rooms;

    void setConnection(int from, int dir, int to) {
        rooms[from].connections[dir] = to;
        rooms[to].connections[oppositeDir(dir)] = from;
    }
    void updateOpenings(int r) {
        for (int i = 0; i < 6; ++i) {
            rooms[r].hasOpening[i] = rooms[r].connections[i] != -1;
        }
    }
    // Reference: RoomDefinition.findSource.
    bool findSource(int r, int scan) {
        RoomDef& room = rooms[r];
        if (room.isSource) return true;
        room.scanIndex = scan;
        for (int i = 0; i < 6; ++i) {
            int neighbor = room.connections[i];
            if (neighbor != -1 && room.hasOpening[i]
                && rooms[neighbor].scanIndex != scan && findSource(neighbor, scan)) {
                return true;
            }
        }
        return false;
    }
};

enum ChildKind {
    C_ENTRY, C_CORE, C_SIMPLE, C_SIMPLE_TOP, C_DOUBLE_Y, C_DOUBLE_X,
    C_DOUBLE_Z, C_DOUBLE_XY, C_DOUBLE_YZ, C_WING, C_PENTHOUSE
};

struct MonumentChild {
    int kind;
    int roomIdx = -1;      // into MonumentGraph.rooms
    int mainDesign = 0;    // SimpleRoom nextInt(3) / WingRoom randomValue & 1
    BoundingBox box;
};

struct MonumentData {
    MonumentGraph graph;
    std::vector<MonumentChild> children;
};

// Parent-piece world transform (StructurePiece.getWorldX/Y/Z for the given
// orientation and box) - needed at build time before any behavior binding.
struct Transform {
    int orientation;  // core::Direction value
    BoundingBox box;
    int worldX(int x, int z) const {
        switch (static_cast<Direction>(orientation)) {
            case Direction::NORTH:
            case Direction::SOUTH: return box.minX + x;
            case Direction::WEST: return box.maxX - z;
            case Direction::EAST: return box.minX + z;
            default: return x;
        }
    }
    int worldY(int y) const { return box.minY + y; }
    int worldZ(int x, int z) const {
        switch (static_cast<Direction>(orientation)) {
            case Direction::NORTH: return box.maxZ - z;
            case Direction::SOUTH: return box.minZ + z;
            case Direction::WEST:
            case Direction::EAST: return box.minZ + x;
            default: return z;
        }
    }
};

// Reference: OceanMonumentPiece.makeBoundingBox(orientation, roomDefinition,
// roomWidth, roomHeight, roomDepth) - local box, moved into the grid.
BoundingBox gridRoomBox(int orientation, int roomIndex, int w, int h, int d) {
    int roomX = roomIndex % 5;
    int roomZ = roomIndex / 5 % 5;
    int roomY = roomIndex / 25;
    bool axisZ = orientation == static_cast<int>(Direction::NORTH)
              || orientation == static_cast<int>(Direction::SOUTH);
    BoundingBox box = axisZ
        ? BoundingBox(0, 0, 0, w * 8 - 1, h * 4 - 1, d * 8 - 1)
        : BoundingBox(0, 0, 0, d * 8 - 1, h * 4 - 1, w * 8 - 1);
    switch (static_cast<Direction>(orientation)) {
        case Direction::NORTH:
            box.move(roomX * 8, roomY * 4, -(roomZ + d) * 8 + 1);
            break;
        case Direction::SOUTH:
            box.move(roomX * 8, roomY * 4, roomZ * 8);
            break;
        case Direction::WEST:
            box.move(-(roomZ + d) * 8 + 1, roomY * 4, roomX * 8);
            break;
        default:  // EAST
            box.move(roomZ * 8, roomY * 4, roomX * 8);
            break;
    }
    return box;
}

// Reference: MonumentBuilding ctor + generateRoomGraph - ALL layout-time RNG.
std::shared_ptr<MonumentData> buildMonument(const Transform& parent,
                                            LegacyRandomSource& random) {
    auto data = std::make_shared<MonumentData>();
    MonumentGraph& graph = data->graph;

    int roomGrid[75];
    std::fill(roomGrid, roomGrid + 75, -1);
    auto makeRoom = [&](int index) {
        RoomDef room;
        room.index = index;
        graph.rooms.push_back(room);
        return static_cast<int>(graph.rooms.size()) - 1;
    };

    for (int x = 0; x < 5; ++x) {
        for (int z = 0; z < 4; ++z) {
            int pos = roomIndexOf(x, 0, z);
            roomGrid[pos] = makeRoom(pos);
        }
    }
    for (int x = 0; x < 5; ++x) {
        for (int z = 0; z < 4; ++z) {
            int pos = roomIndexOf(x, 1, z);
            roomGrid[pos] = makeRoom(pos);
        }
    }
    for (int x = 1; x < 4; ++x) {
        for (int z = 0; z < 2; ++z) {
            int pos = roomIndexOf(x, 2, z);
            roomGrid[pos] = makeRoom(pos);
        }
    }

    int sourceIdx = roomGrid[roomIndexOf(2, 0, 0)];

    // Connections: same-z moves keep the direction; z moves store the
    // OPPOSITE (vanilla grid-z flip; NORTH connection == grid z+1).
    for (int x = 0; x < 5; ++x) {
        for (int z = 0; z < 5; ++z) {
            for (int y = 0; y < 3; ++y) {
                int pos = roomIndexOf(x, y, z);
                if (roomGrid[pos] == -1) continue;
                for (int dir = 0; dir < 6; ++dir) {
                    int nx = x + kStepX[dir];
                    int ny = y + kStepY[dir];
                    int nz = z + kStepZ[dir];
                    if (nx < 0 || nx >= 5 || nz < 0 || nz >= 5 || ny < 0 || ny >= 3) continue;
                    int neighPos = roomIndexOf(nx, ny, nz);
                    if (roomGrid[neighPos] == -1) continue;
                    if (nz == z) {
                        graph.setConnection(roomGrid[pos], dir, roomGrid[neighPos]);
                    } else {
                        graph.setConnection(roomGrid[pos], oppositeDir(dir), roomGrid[neighPos]);
                    }
                }
            }
        }
    }

    int roofIdx = makeRoom(1003);
    int leftWingIdx = makeRoom(1001);
    int rightWingIdx = makeRoom(1002);
    graph.setConnection(roomGrid[roomIndexOf(2, 2, 0)], D_UP, roofIdx);
    graph.setConnection(roomGrid[roomIndexOf(0, 1, 0)], D_SOUTH, leftWingIdx);
    graph.setConnection(roomGrid[roomIndexOf(4, 1, 0)], D_SOUTH, rightWingIdx);
    graph.rooms[roofIdx].claimed = true;
    graph.rooms[leftWingIdx].claimed = true;
    graph.rooms[rightWingIdx].claimed = true;
    graph.rooms[sourceIdx].isSource = true;

    int coreIdx = roomGrid[roomIndexOf(random.nextInt(4), 0, 2)];
    {
        RoomDef& core = graph.rooms[coreIdx];
        core.claimed = true;
        int east = core.connections[D_EAST];
        int north = core.connections[D_NORTH];
        graph.rooms[east].claimed = true;
        graph.rooms[north].claimed = true;
        int eastNorth = graph.rooms[east].connections[D_NORTH];
        graph.rooms[eastNorth].claimed = true;
        graph.rooms[graph.rooms[coreIdx].connections[D_UP]].claimed = true;
        graph.rooms[graph.rooms[east].connections[D_UP]].claimed = true;
        graph.rooms[graph.rooms[north].connections[D_UP]].claimed = true;
        graph.rooms[graph.rooms[eastNorth].connections[D_UP]].claimed = true;
    }

    // roomDefs list in roomGrid ARRAY order (index order), openings updated.
    std::vector<int> roomDefs;
    for (int pos = 0; pos < 75; ++pos) {
        if (roomGrid[pos] != -1) {
            graph.updateOpenings(roomGrid[pos]);
            roomDefs.push_back(roomGrid[pos]);
        }
    }
    graph.updateOpenings(roofIdx);

    // Reference: Util.shuffle - reverse Fisher-Yates.
    for (int i = static_cast<int>(roomDefs.size()); i > 1; --i) {
        std::swap(roomDefs[i - 1], roomDefs[random.nextInt(i)]);
    }

    // Opening-closing pass.
    int scanIndex = 1;
    for (int defIdx : roomDefs) {
        int closeCount = 0;
        int attemptCount = 0;
        while (closeCount < 2 && attemptCount < 5) {
            ++attemptCount;
            int f = random.nextInt(6);
            if (graph.rooms[defIdx].hasOpening[f]) {
                int neighbor = graph.rooms[defIdx].connections[f];
                int of = oppositeDir(f);
                graph.rooms[defIdx].hasOpening[f] = false;
                graph.rooms[neighbor].hasOpening[of] = false;
                if (graph.findSource(defIdx, scanIndex++) && graph.findSource(neighbor, scanIndex++)) {
                    ++closeCount;
                } else {
                    graph.rooms[defIdx].hasOpening[f] = true;
                    graph.rooms[neighbor].hasOpening[of] = true;
                }
            }
        }
    }

    roomDefs.push_back(roofIdx);
    roomDefs.push_back(leftWingIdx);
    roomDefs.push_back(rightWingIdx);

    // ---- MonumentBuilding ctor body ----
    graph.rooms[sourceIdx].claimed = true;
    auto& children = data->children;
    children.push_back({C_ENTRY, sourceIdx, 0,
                        gridRoomBox(parent.orientation, graph.rooms[sourceIdx].index, 1, 1, 1)});
    children.push_back({C_CORE, coreIdx, 0,
                        gridRoomBox(parent.orientation, graph.rooms[coreIdx].index, 2, 2, 2)});

    // Fitters in declaration order: XY, YZ, Z, X, Y, SimpleTop, Simple.
    for (int defIdx : roomDefs) {
        RoomDef& def = graph.rooms[defIdx];
        if (def.claimed || def.isSpecial()) continue;

        int east = def.connections[D_EAST];
        int north = def.connections[D_NORTH];
        int up = def.connections[D_UP];

        // FitDoubleXYRoom.
        if (def.hasOpening[D_EAST] && !graph.rooms[east].claimed
            && def.hasOpening[D_UP] && !graph.rooms[up].claimed
            && graph.rooms[east].hasOpening[D_UP]
            && !graph.rooms[graph.rooms[east].connections[D_UP]].claimed) {
            def.claimed = true;
            graph.rooms[east].claimed = true;
            graph.rooms[up].claimed = true;
            graph.rooms[graph.rooms[east].connections[D_UP]].claimed = true;
            children.push_back({C_DOUBLE_XY, defIdx, 0,
                                gridRoomBox(parent.orientation, def.index, 2, 2, 1)});
            continue;
        }
        // FitDoubleYZRoom.
        if (def.hasOpening[D_NORTH] && !graph.rooms[north].claimed
            && def.hasOpening[D_UP] && !graph.rooms[up].claimed
            && graph.rooms[north].hasOpening[D_UP]
            && !graph.rooms[graph.rooms[north].connections[D_UP]].claimed) {
            def.claimed = true;
            graph.rooms[north].claimed = true;
            graph.rooms[up].claimed = true;
            graph.rooms[graph.rooms[north].connections[D_UP]].claimed = true;
            children.push_back({C_DOUBLE_YZ, defIdx, 0,
                                gridRoomBox(parent.orientation, def.index, 1, 2, 2)});
            continue;
        }
        // FitDoubleZRoom (create() has a dead fallback to the SOUTH neighbor;
        // fits() guarantees the primary path - transcribed anyway).
        if (def.hasOpening[D_NORTH] && !graph.rooms[north].claimed) {
            int source = defIdx;
            if (!def.hasOpening[D_NORTH] || graph.rooms[def.connections[D_NORTH]].claimed) {
                source = def.connections[D_SOUTH];
            }
            graph.rooms[source].claimed = true;
            graph.rooms[graph.rooms[source].connections[D_NORTH]].claimed = true;
            children.push_back({C_DOUBLE_Z, source, 0,
                                gridRoomBox(parent.orientation, graph.rooms[source].index, 1, 1, 2)});
            continue;
        }
        // FitDoubleXRoom.
        if (def.hasOpening[D_EAST] && !graph.rooms[east].claimed) {
            def.claimed = true;
            graph.rooms[east].claimed = true;
            children.push_back({C_DOUBLE_X, defIdx, 0,
                                gridRoomBox(parent.orientation, def.index, 2, 1, 1)});
            continue;
        }
        // FitDoubleYRoom.
        if (def.hasOpening[D_UP] && !graph.rooms[up].claimed) {
            def.claimed = true;
            graph.rooms[up].claimed = true;
            children.push_back({C_DOUBLE_Y, defIdx, 0,
                                gridRoomBox(parent.orientation, def.index, 1, 2, 1)});
            continue;
        }
        // FitSimpleTopRoom.
        if (!def.hasOpening[D_WEST] && !def.hasOpening[D_EAST]
            && !def.hasOpening[D_NORTH] && !def.hasOpening[D_SOUTH]
            && !def.hasOpening[D_UP]) {
            def.claimed = true;
            children.push_back({C_SIMPLE_TOP, defIdx, 0,
                                gridRoomBox(parent.orientation, def.index, 1, 1, 1)});
            continue;
        }
        // FitSimpleRoom (always fits) - ctor draws nextInt(3).
        def.claimed = true;
        children.push_back({C_SIMPLE, defIdx, random.nextInt(3),
                            gridRoomBox(parent.orientation, def.index, 1, 1, 1)});
    }

    // Move all grid-room children by the parent's world offset.
    int offX = parent.worldX(9, 22);
    int offY = parent.worldY(0);
    int offZ = parent.worldZ(9, 22);
    for (auto& child : children) {
        child.box.move(offX, offY, offZ);
    }

    auto corners = [&](int x0, int y0, int z0, int x1, int y1, int z1) {
        int wx0 = parent.worldX(x0, z0);
        int wz0 = parent.worldZ(x0, z0);
        int wx1 = parent.worldX(x1, z1);
        int wz1 = parent.worldZ(x1, z1);
        return BoundingBox(std::min(wx0, wx1), parent.worldY(y0), std::min(wz0, wz1),
                           std::max(wx0, wx1), parent.worldY(y1), std::max(wz0, wz1));
    };
    BoundingBox leftWingBox = corners(1, 1, 1, 23, 8, 21);
    BoundingBox rightWingBox = corners(34, 1, 1, 56, 8, 21);
    BoundingBox penthouseBox = corners(22, 13, 22, 35, 17, 35);
    int32_t wingRandom = random.nextInt();
    children.push_back({C_WING, -1, wingRandom & 1, leftWingBox});
    children.push_back({C_WING, -1, (wingRandom + 1) & 1, rightWingBox});
    children.push_back({C_PENTHOUSE, -1, 0, penthouseBox});
    return data;
}

// Shared placement base: monument palette + the OceanMonumentPiece helpers.
class MonumentPieceBase : public OrientedPieceBehavior {
public:
    MonumentPieceBase(int orientation, std::shared_ptr<MonumentData> data)
        : OrientedPieceBehavior(orientation), m_data(std::move(data)) {}

protected:
    std::shared_ptr<MonumentData> m_data;

    static BlockState* baseGray() { return Blocks::getDefaultState("minecraft:prismarine"); }
    static BlockState* baseLight() { return Blocks::getDefaultState("minecraft:prismarine_bricks"); }
    static BlockState* baseBlack() { return Blocks::getDefaultState("minecraft:dark_prismarine"); }
    static BlockState* dotDeco() { return baseLight(); }
    static BlockState* lamp() { return Blocks::getDefaultState("minecraft:sea_lantern"); }
    static BlockState* fillBlock() { return Blocks::getDefaultState("minecraft:water"); }

    const RoomDef& room(int idx) const { return m_data->graph.rooms[idx]; }
    bool roomHasUp(int idx) const { return room(idx).connections[D_UP] != -1; }

    // Reference: OceanMonumentPiece.FILL_KEEP (Block-level membership - any
    // water state is kept).
    static bool fillKeep(const BlockState* state) {
        return state->is(Blocks::getBlock("minecraft:ice"))
            || state->is(Blocks::getBlock("minecraft:packed_ice"))
            || state->is(Blocks::getBlock("minecraft:blue_ice"))
            || state->is(Blocks::getBlock("minecraft:water"));
    }

    // Reference: OceanMonumentPiece.generateWaterBox.
    void generateWaterBox(WorldGenLevel* level, ChunkGenerator* generator,
                          const BoundingBox& chunkBB,
                          int x0, int y0, int z0, int x1, int y1, int z1) const {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                for (int z = z0; z <= z1; ++z) {
                    BlockState* block = getBlock(level, x, y, z, chunkBB);
                    if (fillKeep(block)) continue;
                    if (worldY(y) >= generator->getSeaLevel() && block != fillBlock()) {
                        placeBlock(level, Blocks::AIR->defaultBlockState(), x, y, z, chunkBB);
                    } else {
                        placeBlock(level, fillBlock(), x, y, z, chunkBB);
                    }
                }
            }
        }
    }

    // Reference: OceanMonumentPiece.generateDefaultFloor.
    void generateDefaultFloor(WorldGenLevel* level, const BoundingBox& chunkBB,
                              int xOff, int zOff, bool downOpening) const {
        if (downOpening) {
            generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 2, 0, zOff + 8 - 1,
                        baseGray(), baseGray(), false);
            generateBox(level, chunkBB, xOff + 5, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1,
                        baseGray(), baseGray(), false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 0, xOff + 4, 0, zOff + 2,
                        baseGray(), baseGray(), false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 8 - 1,
                        baseGray(), baseGray(), false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 2, xOff + 4, 0, zOff + 2,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xOff + 3, 0, zOff + 5, xOff + 4, 0, zOff + 5,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xOff + 2, 0, zOff + 3, xOff + 2, 0, zOff + 4,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xOff + 5, 0, zOff + 3, xOff + 5, 0, zOff + 4,
                        baseLight(), baseLight(), false);
        } else {
            generateBox(level, chunkBB, xOff + 0, 0, zOff + 0, xOff + 8 - 1, 0, zOff + 8 - 1,
                        baseGray(), baseGray(), false);
        }
    }

    // Reference: OceanMonumentPiece.generateBoxOnFillOnly.
    void generateBoxOnFillOnly(WorldGenLevel* level, const BoundingBox& chunkBB,
                               int x0, int y0, int z0, int x1, int y1, int z1,
                               BlockState* targetBlock) const {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                for (int z = z0; z <= z1; ++z) {
                    if (getBlock(level, x, y, z, chunkBB) == fillBlock()) {
                        placeBlock(level, targetBlock, x, y, z, chunkBB);
                    }
                }
            }
        }
    }

    // Reference: OceanMonumentPiece.chunkIntersects (2D x/z).
    bool chunkIntersects(const BoundingBox& chunkBB, int x0, int z0, int x1, int z1) const {
        int wx0 = worldX(x0, z0);
        int wz0 = worldZ(x0, z0);
        int wx1 = worldX(x1, z1);
        int wz1 = worldZ(x1, z1);
        int minX = std::min(wx0, wx1);
        int minZ = std::min(wz0, wz1);
        int maxX = std::max(wx0, wx1);
        int maxZ = std::max(wz0, wz1);
        return chunkBB.maxX >= minX && chunkBB.minX <= maxX
            && chunkBB.maxZ >= minZ && chunkBB.minZ <= maxZ;
    }

    // Reference: OceanMonumentPiece.spawnElder - entity only, no draws from
    // the passed random (level-random draws are dump-invisible here).
    void spawnElder(WorldGenLevel*, const BoundingBox&, int, int, int) const {}
};

// Reference: OceanMonumentEntryRoom.postProcess.
class MonumentEntryRoomBehavior final : public MonumentPieceBase {
public:
    MonumentEntryRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& def = room(m_roomIdx);
        generateBox(level, chunkBB, 0, 3, 0, 2, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 3, 0, 7, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 2, 0, 1, 2, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 2, 0, 7, 2, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 1, 7, 7, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 0, 2, 3, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 0, 6, 3, 0, baseLight(), baseLight(), false);
        if (def.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 7, 4, 2, 7);
        }
        if (def.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 3, 1, 2, 4);
        }
        if (def.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 6, 1, 3, 7, 2, 4);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentSimpleRoom.postProcess.
class MonumentSimpleRoomBehavior final : public MonumentPieceBase {
public:
    MonumentSimpleRoomBehavior(int orientation, std::shared_ptr<MonumentData> data,
                               int roomIdx, int mainDesign)
        : MonumentPieceBase(orientation, std::move(data)),
          m_roomIdx(roomIdx), m_mainDesign(mainDesign) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& def = room(m_roomIdx);
        if (def.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 0, 0, def.hasOpening[D_DOWN]);
        }
        if (def.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, baseGray());
        }
        bool centerPillar = m_mainDesign != 0 && random.nextBoolean()
            && !def.hasOpening[D_DOWN] && !def.hasOpening[D_UP]
            && def.countOpenings() > 1;
        if (m_mainDesign == 0) {
            generateBox(level, chunkBB, 0, 1, 0, 2, 1, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 3, 0, 2, 3, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 2, 2, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 1, 2, 0, 2, 2, 0, baseGray(), baseGray(), false);
            placeBlock(level, lamp(), 1, 2, 1, chunkBB);
            generateBox(level, chunkBB, 5, 1, 0, 7, 1, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 5, 3, 0, 7, 3, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 2, 0, 7, 2, 2, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 5, 2, 0, 6, 2, 0, baseGray(), baseGray(), false);
            placeBlock(level, lamp(), 6, 2, 1, chunkBB);
            generateBox(level, chunkBB, 0, 1, 5, 2, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 3, 5, 2, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 2, 5, 0, 2, 7, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 1, 2, 7, 2, 2, 7, baseGray(), baseGray(), false);
            placeBlock(level, lamp(), 1, 2, 6, chunkBB);
            generateBox(level, chunkBB, 5, 1, 5, 7, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 5, 3, 5, 7, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 2, 5, 7, 2, 7, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 5, 2, 7, 6, 2, 7, baseGray(), baseGray(), false);
            placeBlock(level, lamp(), 6, 2, 6, chunkBB);
            if (def.hasOpening[D_SOUTH]) {
                generateBox(level, chunkBB, 3, 3, 0, 4, 3, 0, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 3, 3, 0, 4, 3, 1, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 3, 2, 0, 4, 2, 0, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 3, 1, 0, 4, 1, 1, baseLight(), baseLight(), false);
            }
            if (def.hasOpening[D_NORTH]) {
                generateBox(level, chunkBB, 3, 3, 7, 4, 3, 7, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 3, 3, 6, 4, 3, 7, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 3, 2, 7, 4, 2, 7, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 3, 1, 6, 4, 1, 7, baseLight(), baseLight(), false);
            }
            if (def.hasOpening[D_WEST]) {
                generateBox(level, chunkBB, 0, 3, 3, 0, 3, 4, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 0, 3, 3, 1, 3, 4, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 0, 2, 3, 0, 2, 4, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 0, 1, 3, 1, 1, 4, baseLight(), baseLight(), false);
            }
            if (def.hasOpening[D_EAST]) {
                generateBox(level, chunkBB, 7, 3, 3, 7, 3, 4, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 6, 3, 3, 7, 3, 4, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 7, 2, 3, 7, 2, 4, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 6, 1, 3, 7, 1, 4, baseLight(), baseLight(), false);
            }
        } else if (m_mainDesign == 1) {
            generateBox(level, chunkBB, 2, 1, 2, 2, 3, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 2, 1, 5, 2, 3, 5, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 5, 1, 5, 5, 3, 5, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 5, 1, 2, 5, 3, 2, baseLight(), baseLight(), false);
            placeBlock(level, lamp(), 2, 2, 2, chunkBB);
            placeBlock(level, lamp(), 2, 2, 5, chunkBB);
            placeBlock(level, lamp(), 5, 2, 5, chunkBB);
            placeBlock(level, lamp(), 5, 2, 2, chunkBB);
            generateBox(level, chunkBB, 0, 1, 0, 1, 3, 0, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 1, 1, 0, 3, 1, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 1, 7, 1, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 1, 6, 0, 3, 6, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 7, 7, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 1, 6, 7, 3, 6, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 0, 7, 3, 0, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 1, 1, 7, 3, 1, baseLight(), baseLight(), false);
            placeBlock(level, baseGray(), 1, 2, 0, chunkBB);
            placeBlock(level, baseGray(), 0, 2, 1, chunkBB);
            placeBlock(level, baseGray(), 1, 2, 7, chunkBB);
            placeBlock(level, baseGray(), 0, 2, 6, chunkBB);
            placeBlock(level, baseGray(), 6, 2, 7, chunkBB);
            placeBlock(level, baseGray(), 7, 2, 6, chunkBB);
            placeBlock(level, baseGray(), 6, 2, 0, chunkBB);
            placeBlock(level, baseGray(), 7, 2, 1, chunkBB);
            if (!def.hasOpening[D_SOUTH]) {
                generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, baseLight(), baseLight(), false);
            }
            if (!def.hasOpening[D_NORTH]) {
                generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, baseLight(), baseLight(), false);
            }
            if (!def.hasOpening[D_WEST]) {
                generateBox(level, chunkBB, 0, 3, 1, 0, 3, 6, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 0, 2, 1, 0, 2, 6, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 0, 1, 1, 0, 1, 6, baseLight(), baseLight(), false);
            }
            if (!def.hasOpening[D_EAST]) {
                generateBox(level, chunkBB, 7, 3, 1, 7, 3, 6, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 7, 2, 1, 7, 2, 6, baseGray(), baseGray(), false);
                generateBox(level, chunkBB, 7, 1, 1, 7, 1, 6, baseLight(), baseLight(), false);
            }
        } else if (m_mainDesign == 2) {
            generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, baseBlack(), baseBlack(), false);
            if (def.hasOpening[D_SOUTH]) {
                generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
            }
            if (def.hasOpening[D_NORTH]) {
                generateWaterBox(level, generator, chunkBB, 3, 1, 7, 4, 2, 7);
            }
            if (def.hasOpening[D_WEST]) {
                generateWaterBox(level, generator, chunkBB, 0, 1, 3, 0, 2, 4);
            }
            if (def.hasOpening[D_EAST]) {
                generateWaterBox(level, generator, chunkBB, 7, 1, 3, 7, 2, 4);
            }
        }
        if (centerPillar) {
            generateBox(level, chunkBB, 3, 1, 3, 4, 1, 4, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 3, 2, 3, 4, 2, 4, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 3, 3, 3, 4, 3, 4, baseLight(), baseLight(), false);
        }
    }

private:
    int m_roomIdx;
    int m_mainDesign;
};

// Reference: OceanMonumentSimpleTopRoom.postProcess.
class MonumentSimpleTopRoomBehavior final : public MonumentPieceBase {
public:
    MonumentSimpleTopRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& def = room(m_roomIdx);
        if (def.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 0, 0, def.hasOpening[D_DOWN]);
        }
        if (def.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 6, baseGray());
        }
        BlockState* wetSponge = Blocks::getDefaultState("minecraft:wet_sponge");
        for (int x = 1; x <= 6; ++x) {
            for (int z = 1; z <= 6; ++z) {
                if (random.nextInt(3) != 0) {
                    int y0 = 2 + (random.nextInt(4) == 0 ? 0 : 1);
                    generateBox(level, chunkBB, x, y0, z, x, 3, z, wetSponge, wetSponge, false);
                }
            }
        }
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 0, 6, 1, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 7, 6, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 7, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 1, 2, 0, 6, 2, 0, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 1, 2, 7, 6, 2, 7, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 3, 0, 7, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 0, 6, 3, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 7, 6, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 1, 3, 0, 2, 4, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 7, 1, 3, 7, 2, 4, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 3, 1, 0, 4, 2, 0, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 3, 1, 7, 4, 2, 7, baseBlack(), baseBlack(), false);
        if (def.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentDoubleYRoom.postProcess.
class MonumentDoubleYRoomBehavior final : public MonumentPieceBase {
public:
    MonumentDoubleYRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& def = room(m_roomIdx);
        int aboveIdx = def.connections[D_UP];
        if (def.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 0, 0, def.hasOpening[D_DOWN]);
        }
        if (room(aboveIdx).connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 6, baseGray());
        }
        generateBox(level, chunkBB, 0, 4, 0, 0, 4, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 4, 0, 7, 4, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 4, 0, 6, 4, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 4, 7, 6, 4, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 4, 1, 2, 4, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 4, 2, 1, 4, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 4, 1, 5, 4, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 4, 2, 6, 4, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 4, 5, 2, 4, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 4, 5, 1, 4, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 4, 5, 5, 4, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 4, 5, 6, 4, 5, baseLight(), baseLight(), false);
        int defIdx = m_roomIdx;
        for (int y = 1; y <= 5; y += 4) {
            const RoomDef& current = room(defIdx);
            int z = 0;
            if (current.hasOpening[D_SOUTH]) {
                generateBox(level, chunkBB, 2, y, z, 2, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 5, y, z, 5, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 3, y + 2, z, 4, y + 2, z, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 0, y, z, 7, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 0, y + 1, z, 7, y + 1, z, baseGray(), baseGray(), false);
            }
            z = 7;
            if (current.hasOpening[D_NORTH]) {
                generateBox(level, chunkBB, 2, y, z, 2, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 5, y, z, 5, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 3, y + 2, z, 4, y + 2, z, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, 0, y, z, 7, y + 2, z, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, 0, y + 1, z, 7, y + 1, z, baseGray(), baseGray(), false);
            }
            int x = 0;
            if (current.hasOpening[D_WEST]) {
                generateBox(level, chunkBB, x, y, 2, x, y + 2, 2, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y, 5, x, y + 2, 5, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y + 2, 3, x, y + 2, 4, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, x, y, 0, x, y + 2, 7, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y + 1, 0, x, y + 1, 7, baseGray(), baseGray(), false);
            }
            x = 7;
            if (current.hasOpening[D_EAST]) {
                generateBox(level, chunkBB, x, y, 2, x, y + 2, 2, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y, 5, x, y + 2, 5, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y + 2, 3, x, y + 2, 4, baseLight(), baseLight(), false);
            } else {
                generateBox(level, chunkBB, x, y, 0, x, y + 2, 7, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, y + 1, 0, x, y + 1, 7, baseGray(), baseGray(), false);
            }
            defIdx = aboveIdx;
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentDoubleXRoom.postProcess.
class MonumentDoubleXRoomBehavior final : public MonumentPieceBase {
public:
    MonumentDoubleXRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& west = room(m_roomIdx);
        const RoomDef& east = room(west.connections[D_EAST]);
        if (west.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 8, 0, east.hasOpening[D_DOWN]);
            generateDefaultFloor(level, chunkBB, 0, 0, west.hasOpening[D_DOWN]);
        }
        if (west.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 7, 4, 6, baseGray());
        }
        if (east.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 8, 4, 1, 14, 4, 6, baseGray());
        }
        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 15, 3, 0, 15, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 0, 15, 3, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 7, 14, 3, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 7, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 15, 2, 0, 15, 2, 7, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 1, 2, 0, 15, 2, 0, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 1, 2, 7, 14, 2, 7, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 15, 1, 0, 15, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 0, 15, 1, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 7, 14, 1, 7, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 0, 10, 1, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 2, 0, 9, 2, 3, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 5, 3, 0, 10, 3, 4, baseLight(), baseLight(), false);
        placeBlock(level, lamp(), 6, 2, 3, chunkBB);
        placeBlock(level, lamp(), 9, 2, 3, chunkBB);
        if (west.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
        }
        if (west.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 7, 4, 2, 7);
        }
        if (west.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 3, 0, 2, 4);
        }
        if (east.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 1, 0, 12, 2, 0);
        }
        if (east.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 1, 7, 12, 2, 7);
        }
        if (east.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 15, 1, 3, 15, 2, 4);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentDoubleZRoom.postProcess.
class MonumentDoubleZRoomBehavior final : public MonumentPieceBase {
public:
    MonumentDoubleZRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& south = room(m_roomIdx);
        const RoomDef& north = room(south.connections[D_NORTH]);
        if (south.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 0, 8, north.hasOpening[D_DOWN]);
            generateDefaultFloor(level, chunkBB, 0, 0, south.hasOpening[D_DOWN]);
        }
        if (south.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 4, 1, 6, 4, 7, baseGray());
        }
        if (north.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 4, 8, 6, 4, 14, baseGray());
        }
        generateBox(level, chunkBB, 0, 3, 0, 0, 3, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 3, 0, 7, 3, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 0, 7, 3, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 15, 6, 3, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, 2, 0, 0, 2, 15, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 7, 2, 0, 7, 2, 15, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 1, 2, 0, 7, 2, 0, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 1, 2, 15, 6, 2, 15, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 0, 1, 0, 0, 1, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 7, 1, 0, 7, 1, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 0, 7, 1, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 15, 6, 1, 15, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 1, 1, 1, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 1, 1, 6, 1, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 1, 1, 3, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 3, 1, 6, 3, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 1, 13, 1, 1, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 1, 13, 6, 1, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 3, 13, 1, 3, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 3, 13, 6, 3, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 1, 6, 2, 3, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 6, 5, 3, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 1, 9, 2, 3, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 9, 5, 3, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 2, 6, 4, 2, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 2, 9, 4, 2, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 2, 7, 2, 2, 8, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 2, 7, 5, 2, 8, baseLight(), baseLight(), false);
        placeBlock(level, lamp(), 2, 2, 5, chunkBB);
        placeBlock(level, lamp(), 5, 2, 5, chunkBB);
        placeBlock(level, lamp(), 2, 2, 10, chunkBB);
        placeBlock(level, lamp(), 5, 2, 10, chunkBB);
        placeBlock(level, baseLight(), 2, 3, 5, chunkBB);
        placeBlock(level, baseLight(), 5, 3, 5, chunkBB);
        placeBlock(level, baseLight(), 2, 3, 10, chunkBB);
        placeBlock(level, baseLight(), 5, 3, 10, chunkBB);
        if (south.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
        }
        if (south.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 1, 3, 7, 2, 4);
        }
        if (south.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 3, 0, 2, 4);
        }
        if (north.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 15, 4, 2, 15);
        }
        if (north.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 11, 0, 2, 12);
        }
        if (north.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 1, 11, 7, 2, 12);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentDoubleXYRoom.postProcess.
class MonumentDoubleXYRoomBehavior final : public MonumentPieceBase {
public:
    MonumentDoubleXYRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& west = room(m_roomIdx);
        const RoomDef& east = room(west.connections[D_EAST]);
        const RoomDef& westUp = room(west.connections[D_UP]);
        const RoomDef& eastUp = room(east.connections[D_UP]);
        if (west.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 8, 0, east.hasOpening[D_DOWN]);
            generateDefaultFloor(level, chunkBB, 0, 0, west.hasOpening[D_DOWN]);
        }
        if (westUp.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 7, 8, 6, baseGray());
        }
        if (eastUp.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 8, 8, 1, 14, 8, 6, baseGray());
        }
        for (int y = 1; y <= 7; ++y) {
            BlockState* block = baseLight();
            if (y == 2 || y == 6) {
                block = baseGray();
            }
            generateBox(level, chunkBB, 0, y, 0, 0, y, 7, block, block, false);
            generateBox(level, chunkBB, 15, y, 0, 15, y, 7, block, block, false);
            generateBox(level, chunkBB, 1, y, 0, 15, y, 0, block, block, false);
            generateBox(level, chunkBB, 1, y, 7, 14, y, 7, block, block, false);
        }
        generateBox(level, chunkBB, 2, 1, 3, 2, 7, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 1, 2, 4, 7, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 1, 5, 4, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 13, 1, 3, 13, 7, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 11, 1, 2, 12, 7, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 11, 1, 5, 12, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 3, 5, 3, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 1, 3, 10, 3, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 7, 2, 10, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 5, 2, 5, 7, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 5, 2, 10, 7, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 5, 5, 5, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 5, 5, 10, 7, 5, baseLight(), baseLight(), false);
        placeBlock(level, baseLight(), 6, 6, 2, chunkBB);
        placeBlock(level, baseLight(), 9, 6, 2, chunkBB);
        placeBlock(level, baseLight(), 6, 6, 5, chunkBB);
        placeBlock(level, baseLight(), 9, 6, 5, chunkBB);
        generateBox(level, chunkBB, 5, 4, 3, 6, 4, 4, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 4, 3, 10, 4, 4, baseLight(), baseLight(), false);
        placeBlock(level, lamp(), 5, 4, 2, chunkBB);
        placeBlock(level, lamp(), 5, 4, 5, chunkBB);
        placeBlock(level, lamp(), 10, 4, 2, chunkBB);
        placeBlock(level, lamp(), 10, 4, 5, chunkBB);
        if (west.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
        }
        if (west.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 7, 4, 2, 7);
        }
        if (west.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 3, 0, 2, 4);
        }
        if (east.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 1, 0, 12, 2, 0);
        }
        if (east.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 1, 7, 12, 2, 7);
        }
        if (east.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 15, 1, 3, 15, 2, 4);
        }
        if (westUp.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 5, 0, 4, 6, 0);
        }
        if (westUp.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 5, 7, 4, 6, 7);
        }
        if (westUp.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 5, 3, 0, 6, 4);
        }
        if (eastUp.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 5, 0, 12, 6, 0);
        }
        if (eastUp.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 11, 5, 7, 12, 6, 7);
        }
        if (eastUp.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 15, 5, 3, 15, 6, 4);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentDoubleYZRoom.postProcess.
class MonumentDoubleYZRoomBehavior final : public MonumentPieceBase {
public:
    MonumentDoubleYZRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int roomIdx)
        : MonumentPieceBase(orientation, std::move(data)), m_roomIdx(roomIdx) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        const RoomDef& south = room(m_roomIdx);
        const RoomDef& north = room(south.connections[D_NORTH]);
        const RoomDef& northUp = room(north.connections[D_UP]);
        const RoomDef& southUp = room(south.connections[D_UP]);
        if (south.index / 25 > 0) {
            generateDefaultFloor(level, chunkBB, 0, 8, north.hasOpening[D_DOWN]);
            generateDefaultFloor(level, chunkBB, 0, 0, south.hasOpening[D_DOWN]);
        }
        if (southUp.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 8, 1, 6, 8, 7, baseGray());
        }
        if (northUp.connections[D_UP] == -1) {
            generateBoxOnFillOnly(level, chunkBB, 1, 8, 8, 6, 8, 14, baseGray());
        }
        for (int y = 1; y <= 7; ++y) {
            BlockState* block = baseLight();
            if (y == 2 || y == 6) {
                block = baseGray();
            }
            generateBox(level, chunkBB, 0, y, 0, 0, y, 15, block, block, false);
            generateBox(level, chunkBB, 7, y, 0, 7, y, 15, block, block, false);
            generateBox(level, chunkBB, 1, y, 0, 6, y, 0, block, block, false);
            generateBox(level, chunkBB, 1, y, 15, 6, y, 15, block, block, false);
        }
        for (int y = 1; y <= 7; ++y) {
            BlockState* block = baseBlack();
            if (y == 2 || y == 6) {
                block = lamp();
            }
            generateBox(level, chunkBB, 3, y, 7, 4, y, 8, block, block, false);
        }
        if (south.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 0, 4, 2, 0);
        }
        if (south.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 1, 3, 7, 2, 4);
        }
        if (south.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 3, 0, 2, 4);
        }
        if (north.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 1, 15, 4, 2, 15);
        }
        if (north.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 1, 11, 0, 2, 12);
        }
        if (north.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 1, 11, 7, 2, 12);
        }
        if (southUp.hasOpening[D_SOUTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 5, 0, 4, 6, 0);
        }
        if (southUp.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 5, 3, 7, 6, 4);
            generateBox(level, chunkBB, 5, 4, 2, 6, 4, 5, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 2, 6, 3, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 5, 6, 3, 5, baseLight(), baseLight(), false);
        }
        if (southUp.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 5, 3, 0, 6, 4);
            generateBox(level, chunkBB, 1, 4, 2, 2, 4, 5, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 2, 1, 3, 2, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 5, 1, 3, 5, baseLight(), baseLight(), false);
        }
        if (northUp.hasOpening[D_NORTH]) {
            generateWaterBox(level, generator, chunkBB, 3, 5, 15, 4, 6, 15);
        }
        if (northUp.hasOpening[D_WEST]) {
            generateWaterBox(level, generator, chunkBB, 0, 5, 11, 0, 6, 12);
            generateBox(level, chunkBB, 1, 4, 10, 2, 4, 13, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 10, 1, 3, 10, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 1, 1, 13, 1, 3, 13, baseLight(), baseLight(), false);
        }
        if (northUp.hasOpening[D_EAST]) {
            generateWaterBox(level, generator, chunkBB, 7, 5, 11, 7, 6, 12);
            generateBox(level, chunkBB, 5, 4, 10, 6, 4, 13, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 10, 6, 3, 10, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 1, 13, 6, 3, 13, baseLight(), baseLight(), false);
        }
    }

private:
    int m_roomIdx;
};

// Reference: OceanMonumentCoreRoom.postProcess.
class MonumentCoreRoomBehavior final : public MonumentPieceBase {
public:
    MonumentCoreRoomBehavior(int orientation, std::shared_ptr<MonumentData> data)
        : MonumentPieceBase(orientation, std::move(data)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        generateBoxOnFillOnly(level, chunkBB, 1, 8, 0, 14, 8, 14, baseGray());
        BlockState* block = baseLight();
        generateBox(level, chunkBB, 0, 7, 0, 0, 7, 15, block, block, false);
        generateBox(level, chunkBB, 15, 7, 0, 15, 7, 15, block, block, false);
        generateBox(level, chunkBB, 1, 7, 0, 15, 7, 0, block, block, false);
        generateBox(level, chunkBB, 1, 7, 15, 14, 7, 15, block, block, false);
        for (int y = 1; y <= 6; ++y) {
            block = baseLight();
            if (y == 2 || y == 6) {
                block = baseGray();
            }
            for (int x = 0; x <= 15; x += 15) {
                generateBox(level, chunkBB, x, y, 0, x, y, 1, block, block, false);
                generateBox(level, chunkBB, x, y, 6, x, y, 9, block, block, false);
                generateBox(level, chunkBB, x, y, 14, x, y, 15, block, block, false);
            }
            generateBox(level, chunkBB, 1, y, 0, 1, y, 0, block, block, false);
            generateBox(level, chunkBB, 6, y, 0, 9, y, 0, block, block, false);
            generateBox(level, chunkBB, 14, y, 0, 14, y, 0, block, block, false);
            generateBox(level, chunkBB, 1, y, 15, 14, y, 15, block, block, false);
        }
        generateBox(level, chunkBB, 6, 3, 6, 9, 6, 9, baseBlack(), baseBlack(), false);
        BlockState* gold = Blocks::getDefaultState("minecraft:gold_block");
        generateBox(level, chunkBB, 7, 4, 7, 8, 5, 8, gold, gold, false);
        for (int y = 3; y <= 6; y += 3) {
            for (int x = 6; x <= 9; x += 3) {
                placeBlock(level, lamp(), x, y, 6, chunkBB);
                placeBlock(level, lamp(), x, y, 9, chunkBB);
            }
        }
        generateBox(level, chunkBB, 5, 1, 6, 5, 2, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 1, 9, 5, 2, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 1, 6, 10, 2, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 1, 9, 10, 2, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 1, 5, 6, 2, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 1, 5, 9, 2, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, 1, 10, 6, 2, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 1, 10, 9, 2, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 2, 5, 5, 6, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 2, 10, 5, 6, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 2, 5, 10, 6, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 2, 10, 10, 6, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 7, 1, 5, 7, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 7, 1, 10, 7, 6, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 5, 7, 9, 5, 7, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 10, 7, 9, 10, 7, 14, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 7, 5, 6, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 7, 10, 6, 7, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 7, 5, 14, 7, 5, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 7, 10, 14, 7, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 1, 2, 2, 1, 3, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 1, 2, 3, 1, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 13, 1, 2, 13, 1, 3, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 12, 1, 2, 12, 1, 2, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 2, 1, 12, 2, 1, 13, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 3, 1, 13, 3, 1, 13, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 13, 1, 12, 13, 1, 13, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 12, 1, 13, 12, 1, 13, baseLight(), baseLight(), false);
    }
};

// Reference: OceanMonumentWingRoom.postProcess.
class MonumentWingRoomBehavior final : public MonumentPieceBase {
public:
    MonumentWingRoomBehavior(int orientation, std::shared_ptr<MonumentData> data, int mainDesign)
        : MonumentPieceBase(orientation, std::move(data)), m_mainDesign(mainDesign) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator; (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        if (m_mainDesign == 0) {
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, 10 - i, 3 - i, 20 - i, 12 + i, 3 - i, 20,
                            baseLight(), baseLight(), false);
            }
            generateBox(level, chunkBB, 7, 0, 6, 15, 0, 16, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 6, 0, 6, 6, 3, 20, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 16, 0, 6, 16, 3, 20, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 1, 7, 7, 1, 20, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 15, 1, 7, 15, 1, 20, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 7, 1, 6, 9, 3, 6, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 13, 1, 6, 15, 3, 6, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 8, 1, 7, 9, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 13, 1, 7, 14, 1, 7, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 9, 0, 5, 13, 0, 5, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 10, 0, 7, 12, 0, 7, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 8, 0, 10, 8, 0, 12, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 14, 0, 10, 14, 0, 12, baseBlack(), baseBlack(), false);
            for (int z = 18; z >= 7; z -= 3) {
                placeBlock(level, lamp(), 6, 3, z, chunkBB);
                placeBlock(level, lamp(), 16, 3, z, chunkBB);
            }
            placeBlock(level, lamp(), 10, 0, 10, chunkBB);
            placeBlock(level, lamp(), 12, 0, 10, chunkBB);
            placeBlock(level, lamp(), 10, 0, 12, chunkBB);
            placeBlock(level, lamp(), 12, 0, 12, chunkBB);
            placeBlock(level, lamp(), 8, 3, 6, chunkBB);
            placeBlock(level, lamp(), 14, 3, 6, chunkBB);
            placeBlock(level, baseLight(), 4, 2, 4, chunkBB);
            placeBlock(level, lamp(), 4, 1, 4, chunkBB);
            placeBlock(level, baseLight(), 4, 0, 4, chunkBB);
            placeBlock(level, baseLight(), 18, 2, 4, chunkBB);
            placeBlock(level, lamp(), 18, 1, 4, chunkBB);
            placeBlock(level, baseLight(), 18, 0, 4, chunkBB);
            placeBlock(level, baseLight(), 4, 2, 18, chunkBB);
            placeBlock(level, lamp(), 4, 1, 18, chunkBB);
            placeBlock(level, baseLight(), 4, 0, 18, chunkBB);
            placeBlock(level, baseLight(), 18, 2, 18, chunkBB);
            placeBlock(level, lamp(), 18, 1, 18, chunkBB);
            placeBlock(level, baseLight(), 18, 0, 18, chunkBB);
            placeBlock(level, baseLight(), 9, 7, 20, chunkBB);
            placeBlock(level, baseLight(), 13, 7, 20, chunkBB);
            generateBox(level, chunkBB, 6, 0, 21, 7, 4, 21, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 15, 0, 21, 16, 4, 21, baseLight(), baseLight(), false);
            spawnElder(level, chunkBB, 11, 2, 16);
        } else if (m_mainDesign == 1) {
            generateBox(level, chunkBB, 9, 3, 18, 13, 3, 20, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 9, 0, 18, 9, 2, 18, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 13, 0, 18, 13, 2, 18, baseLight(), baseLight(), false);
            int x = 9;
            for (int i = 0; i < 2; ++i) {
                placeBlock(level, baseLight(), x, 6, 20, chunkBB);
                placeBlock(level, lamp(), x, 5, 20, chunkBB);
                placeBlock(level, baseLight(), x, 4, 20, chunkBB);
                x = 13;
            }
            generateBox(level, chunkBB, 7, 3, 7, 15, 3, 14, baseLight(), baseLight(), false);
            x = 10;
            for (int i = 0; i < 2; ++i) {
                generateBox(level, chunkBB, x, 0, 10, x, 6, 10, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, 0, 12, x, 6, 12, baseLight(), baseLight(), false);
                placeBlock(level, lamp(), x, 0, 10, chunkBB);
                placeBlock(level, lamp(), x, 0, 12, chunkBB);
                placeBlock(level, lamp(), x, 4, 10, chunkBB);
                placeBlock(level, lamp(), x, 4, 12, chunkBB);
                x = 12;
            }
            x = 8;
            for (int i = 0; i < 2; ++i) {
                generateBox(level, chunkBB, x, 0, 7, x, 2, 7, baseLight(), baseLight(), false);
                generateBox(level, chunkBB, x, 0, 14, x, 2, 14, baseLight(), baseLight(), false);
                x = 14;
            }
            generateBox(level, chunkBB, 8, 3, 8, 8, 3, 13, baseBlack(), baseBlack(), false);
            generateBox(level, chunkBB, 14, 3, 8, 14, 3, 13, baseBlack(), baseBlack(), false);
            spawnElder(level, chunkBB, 11, 5, 13);
        }
    }

private:
    int m_mainDesign;
};

// Reference: OceanMonumentPenthouse.postProcess.
class MonumentPenthouseBehavior final : public MonumentPieceBase {
public:
    MonumentPenthouseBehavior(int orientation, std::shared_ptr<MonumentData> data)
        : MonumentPieceBase(orientation, std::move(data)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)random; (void)chunkPos; (void)referencePos;
        bind(self);
        generateBox(level, chunkBB, 2, -1, 2, 11, -1, 11, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 0, -1, 0, 1, -1, 11, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 12, -1, 0, 13, -1, 11, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 2, -1, 0, 11, -1, 1, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 2, -1, 12, 11, -1, 13, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 0, 0, 0, 0, 0, 13, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 13, 0, 0, 13, 0, 13, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 0, 0, 12, 0, 0, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 1, 0, 13, 12, 0, 13, baseLight(), baseLight(), false);
        for (int i = 2; i <= 11; i += 3) {
            placeBlock(level, lamp(), 0, 0, i, chunkBB);
            placeBlock(level, lamp(), 13, 0, i, chunkBB);
            placeBlock(level, lamp(), i, 0, 0, chunkBB);
        }
        generateBox(level, chunkBB, 2, 0, 3, 4, 0, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 9, 0, 3, 11, 0, 9, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 4, 0, 9, 9, 0, 11, baseLight(), baseLight(), false);
        placeBlock(level, baseLight(), 5, 0, 8, chunkBB);
        placeBlock(level, baseLight(), 8, 0, 8, chunkBB);
        placeBlock(level, baseLight(), 10, 0, 10, chunkBB);
        placeBlock(level, baseLight(), 3, 0, 10, chunkBB);
        generateBox(level, chunkBB, 3, 0, 3, 3, 0, 7, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 10, 0, 3, 10, 0, 7, baseBlack(), baseBlack(), false);
        generateBox(level, chunkBB, 6, 0, 10, 7, 0, 10, baseBlack(), baseBlack(), false);
        int x = 3;
        for (int i = 0; i < 2; ++i) {
            for (int z = 2; z <= 8; z += 3) {
                generateBox(level, chunkBB, x, 0, z, x, 2, z, baseLight(), baseLight(), false);
            }
            x = 10;
        }
        generateBox(level, chunkBB, 5, 0, 10, 5, 2, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 8, 0, 10, 8, 2, 10, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 6, -1, 7, 7, -1, 8, baseBlack(), baseBlack(), false);
        generateWaterBox(level, generator, chunkBB, 6, -1, 3, 7, -1, 4);
        spawnElder(level, chunkBB, 6, 1, 6);
    }
};

// Reference: MonumentBuilding.postProcess (walls/wings/pillars + child loop).
class MonumentBuildingBehavior final : public MonumentPieceBase {
public:
    MonumentBuildingBehavior(int orientation, std::shared_ptr<MonumentData> data)
        : MonumentPieceBase(orientation, data) {
        for (const auto& child : data->children) {
            StructurePieceData childSelf;
            childSelf.boundingBox = child.box;
            m_childSelfs.push_back(childSelf);
            switch (child.kind) {
                case C_ENTRY:
                    m_childBehaviors.push_back(std::make_shared<MonumentEntryRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_CORE:
                    m_childBehaviors.push_back(std::make_shared<MonumentCoreRoomBehavior>(
                        orientation, data));
                    break;
                case C_SIMPLE:
                    m_childBehaviors.push_back(std::make_shared<MonumentSimpleRoomBehavior>(
                        orientation, data, child.roomIdx, child.mainDesign));
                    break;
                case C_SIMPLE_TOP:
                    m_childBehaviors.push_back(std::make_shared<MonumentSimpleTopRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_DOUBLE_Y:
                    m_childBehaviors.push_back(std::make_shared<MonumentDoubleYRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_DOUBLE_X:
                    m_childBehaviors.push_back(std::make_shared<MonumentDoubleXRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_DOUBLE_Z:
                    m_childBehaviors.push_back(std::make_shared<MonumentDoubleZRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_DOUBLE_XY:
                    m_childBehaviors.push_back(std::make_shared<MonumentDoubleXYRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_DOUBLE_YZ:
                    m_childBehaviors.push_back(std::make_shared<MonumentDoubleYZRoomBehavior>(
                        orientation, data, child.roomIdx));
                    break;
                case C_WING:
                    m_childBehaviors.push_back(std::make_shared<MonumentWingRoomBehavior>(
                        orientation, data, child.mainDesign));
                    break;
                default:
                    m_childBehaviors.push_back(std::make_shared<MonumentPenthouseBehavior>(
                        orientation, data));
                    break;
            }
        }
    }

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        bind(self);
        int waterHeight = std::max(generator->getSeaLevel(), 64) - self.boundingBox.minY;
        generateWaterBox(level, generator, chunkBB, 0, 0, 0, 58, waterHeight, 58);
        generateWing(false, 0, level, chunkBB, generator);
        generateWing(true, 33, level, chunkBB, generator);
        generateEntranceArchs(level, chunkBB, generator);
        generateEntranceWall(level, chunkBB, generator);
        generateRoofPiece(level, chunkBB, generator);
        generateLowerWall(level, chunkBB, generator);
        generateMiddleWall(level, chunkBB, generator);
        generateUpperWall(level, chunkBB, generator);

        for (int pillarX = 0; pillarX < 7; ++pillarX) {
            int pillarZ = 0;
            while (pillarZ < 7) {
                if (pillarZ == 0 && pillarX == 3) {
                    pillarZ = 6;
                }
                int bx = pillarX * 9;
                int bz = pillarZ * 9;
                for (int w = 0; w < 4; ++w) {
                    for (int d = 0; d < 4; ++d) {
                        placeBlock(level, baseLight(), bx + w, 0, bz + d, chunkBB);
                        fillColumnDown(level, baseLight(), bx + w, -1, bz + d, chunkBB);
                    }
                }
                if (pillarX != 0 && pillarX != 6) {
                    pillarZ += 6;
                } else {
                    ++pillarZ;
                }
            }
        }

        for (int i = 0; i < 5; ++i) {
            generateWaterBox(level, generator, chunkBB, -1 - i, 0 + i * 2, -1 - i, -1 - i, 23, 58 + i);
            generateWaterBox(level, generator, chunkBB, 58 + i, 0 + i * 2, -1 - i, 58 + i, 23, 58 + i);
            generateWaterBox(level, generator, chunkBB, 0 - i, 0 + i * 2, -1 - i, 57 + i, 23, -1 - i);
            generateWaterBox(level, generator, chunkBB, 0 - i, 0 + i * 2, 58 + i, 57 + i, 23, 58 + i);
        }

        for (size_t i = 0; i < m_childBehaviors.size(); ++i) {
            if (m_childSelfs[i].boundingBox.intersects(chunkBB)) {
                m_childBehaviors[i]->postProcess(level, generator, random, chunkBB,
                                                 chunkPos, referencePos, m_childSelfs[i]);
            }
        }
    }

private:
    std::vector<std::shared_ptr<StructurePieceBehavior>> m_childBehaviors;
    std::vector<StructurePieceData> m_childSelfs;

    // Reference: MonumentBuilding.generateWing.
    void generateWing(bool isFlipped, int xoff, WorldGenLevel* level,
                      const BoundingBox& chunkBB, ChunkGenerator* generator) {
        if (!chunkIntersects(chunkBB, xoff, 0, xoff + 23, 20)) return;
        generateBox(level, chunkBB, xoff + 0, 0, 0, xoff + 24, 0, 20, baseGray(), baseGray(), false);
        generateWaterBox(level, generator, chunkBB, xoff + 0, 1, 0, xoff + 24, 10, 20);
        for (int i = 0; i < 4; ++i) {
            generateBox(level, chunkBB, xoff + i, i + 1, i, xoff + i, i + 1, 20,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xoff + i + 7, i + 5, i + 7, xoff + i + 7, i + 5, 20,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xoff + 17 - i, i + 5, i + 7, xoff + 17 - i, i + 5, 20,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xoff + 24 - i, i + 1, i, xoff + 24 - i, i + 1, 20,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xoff + i + 1, i + 1, i, xoff + 23 - i, i + 1, i,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, xoff + i + 8, i + 5, i + 7, xoff + 16 - i, i + 5, i + 7,
                        baseLight(), baseLight(), false);
        }
        generateBox(level, chunkBB, xoff + 4, 4, 4, xoff + 6, 4, 20, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, xoff + 7, 4, 4, xoff + 17, 4, 6, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, xoff + 18, 4, 4, xoff + 20, 4, 20, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, xoff + 11, 8, 11, xoff + 13, 8, 20, baseGray(), baseGray(), false);
        placeBlock(level, dotDeco(), xoff + 12, 9, 12, chunkBB);
        placeBlock(level, dotDeco(), xoff + 12, 9, 15, chunkBB);
        placeBlock(level, dotDeco(), xoff + 12, 9, 18, chunkBB);
        int leftPos = xoff + (isFlipped ? 19 : 5);
        int rightPos = xoff + (isFlipped ? 5 : 19);
        for (int z = 20; z >= 5; z -= 3) {
            placeBlock(level, dotDeco(), leftPos, 5, z, chunkBB);
        }
        for (int z = 19; z >= 7; z -= 3) {
            placeBlock(level, dotDeco(), rightPos, 5, z, chunkBB);
        }
        for (int i = 0; i < 4; ++i) {
            int pos = isFlipped ? xoff + 24 - (17 - i * 3) : xoff + 17 - i * 3;
            placeBlock(level, dotDeco(), pos, 5, 5, chunkBB);
        }
        placeBlock(level, dotDeco(), rightPos, 5, 5, chunkBB);
        generateBox(level, chunkBB, xoff + 11, 1, 12, xoff + 13, 7, 12, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, xoff + 12, 1, 11, xoff + 12, 7, 13, baseGray(), baseGray(), false);
    }

    // Reference: MonumentBuilding.generateEntranceArchs.
    void generateEntranceArchs(WorldGenLevel* level, const BoundingBox& chunkBB,
                               ChunkGenerator* generator) {
        if (!chunkIntersects(chunkBB, 22, 5, 35, 17)) return;
        generateWaterBox(level, generator, chunkBB, 25, 0, 0, 32, 8, 20);
        for (int i = 0; i < 4; ++i) {
            generateBox(level, chunkBB, 24, 2, 5 + i * 4, 24, 4, 5 + i * 4,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 22, 4, 5 + i * 4, 23, 4, 5 + i * 4,
                        baseLight(), baseLight(), false);
            placeBlock(level, baseLight(), 25, 5, 5 + i * 4, chunkBB);
            placeBlock(level, baseLight(), 26, 6, 5 + i * 4, chunkBB);
            placeBlock(level, lamp(), 26, 5, 5 + i * 4, chunkBB);
            generateBox(level, chunkBB, 33, 2, 5 + i * 4, 33, 4, 5 + i * 4,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 34, 4, 5 + i * 4, 35, 4, 5 + i * 4,
                        baseLight(), baseLight(), false);
            placeBlock(level, baseLight(), 32, 5, 5 + i * 4, chunkBB);
            placeBlock(level, baseLight(), 31, 6, 5 + i * 4, chunkBB);
            placeBlock(level, lamp(), 31, 5, 5 + i * 4, chunkBB);
            generateBox(level, chunkBB, 27, 6, 5 + i * 4, 30, 6, 5 + i * 4,
                        baseGray(), baseGray(), false);
        }
    }

    // Reference: MonumentBuilding.generateEntranceWall.
    void generateEntranceWall(WorldGenLevel* level, const BoundingBox& chunkBB,
                              ChunkGenerator* generator) {
        if (!chunkIntersects(chunkBB, 15, 20, 42, 21)) return;
        generateBox(level, chunkBB, 15, 0, 21, 42, 0, 21, baseGray(), baseGray(), false);
        generateWaterBox(level, generator, chunkBB, 26, 1, 21, 31, 3, 21);
        generateBox(level, chunkBB, 21, 12, 21, 36, 12, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 17, 11, 21, 40, 11, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 16, 10, 21, 41, 10, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 15, 7, 21, 42, 9, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 16, 6, 21, 41, 6, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 17, 5, 21, 40, 5, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 21, 4, 21, 36, 4, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 22, 3, 21, 26, 3, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 31, 3, 21, 35, 3, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 23, 2, 21, 25, 2, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 32, 2, 21, 34, 2, 21, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 28, 4, 20, 29, 4, 21, baseLight(), baseLight(), false);
        placeBlock(level, baseLight(), 27, 3, 21, chunkBB);
        placeBlock(level, baseLight(), 30, 3, 21, chunkBB);
        placeBlock(level, baseLight(), 26, 2, 21, chunkBB);
        placeBlock(level, baseLight(), 31, 2, 21, chunkBB);
        placeBlock(level, baseLight(), 25, 1, 21, chunkBB);
        placeBlock(level, baseLight(), 32, 1, 21, chunkBB);
        for (int i = 0; i < 7; ++i) {
            placeBlock(level, baseBlack(), 28 - i, 6 + i, 21, chunkBB);
            placeBlock(level, baseBlack(), 29 + i, 6 + i, 21, chunkBB);
        }
        for (int i = 0; i < 4; ++i) {
            placeBlock(level, baseBlack(), 28 - i, 9 + i, 21, chunkBB);
            placeBlock(level, baseBlack(), 29 + i, 9 + i, 21, chunkBB);
        }
        placeBlock(level, baseBlack(), 28, 12, 21, chunkBB);
        placeBlock(level, baseBlack(), 29, 12, 21, chunkBB);
        for (int i = 0; i < 3; ++i) {
            placeBlock(level, baseBlack(), 22 - i * 2, 8, 21, chunkBB);
            placeBlock(level, baseBlack(), 22 - i * 2, 9, 21, chunkBB);
            placeBlock(level, baseBlack(), 35 + i * 2, 8, 21, chunkBB);
            placeBlock(level, baseBlack(), 35 + i * 2, 9, 21, chunkBB);
        }
        generateWaterBox(level, generator, chunkBB, 15, 13, 21, 42, 15, 21);
        generateWaterBox(level, generator, chunkBB, 15, 1, 21, 15, 6, 21);
        generateWaterBox(level, generator, chunkBB, 16, 1, 21, 16, 5, 21);
        generateWaterBox(level, generator, chunkBB, 17, 1, 21, 20, 4, 21);
        generateWaterBox(level, generator, chunkBB, 21, 1, 21, 21, 3, 21);
        generateWaterBox(level, generator, chunkBB, 22, 1, 21, 22, 2, 21);
        generateWaterBox(level, generator, chunkBB, 23, 1, 21, 24, 1, 21);
        generateWaterBox(level, generator, chunkBB, 42, 1, 21, 42, 6, 21);
        generateWaterBox(level, generator, chunkBB, 41, 1, 21, 41, 5, 21);
        generateWaterBox(level, generator, chunkBB, 37, 1, 21, 40, 4, 21);
        generateWaterBox(level, generator, chunkBB, 36, 1, 21, 36, 3, 21);
        generateWaterBox(level, generator, chunkBB, 33, 1, 21, 34, 1, 21);
        generateWaterBox(level, generator, chunkBB, 35, 1, 21, 35, 2, 21);
    }

    // Reference: MonumentBuilding.generateRoofPiece.
    void generateRoofPiece(WorldGenLevel* level, const BoundingBox& chunkBB,
                           ChunkGenerator* generator) {
        if (!chunkIntersects(chunkBB, 21, 21, 36, 36)) return;
        generateBox(level, chunkBB, 21, 0, 22, 36, 0, 36, baseGray(), baseGray(), false);
        generateWaterBox(level, generator, chunkBB, 21, 1, 22, 36, 23, 36);
        for (int i = 0; i < 4; ++i) {
            generateBox(level, chunkBB, 21 + i, 13 + i, 21 + i, 36 - i, 13 + i, 21 + i,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 21 + i, 13 + i, 36 - i, 36 - i, 13 + i, 36 - i,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 21 + i, 13 + i, 22 + i, 21 + i, 13 + i, 35 - i,
                        baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 36 - i, 13 + i, 22 + i, 36 - i, 13 + i, 35 - i,
                        baseLight(), baseLight(), false);
        }
        generateBox(level, chunkBB, 25, 16, 25, 32, 16, 32, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 25, 17, 25, 25, 19, 25, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 32, 17, 25, 32, 19, 25, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 25, 17, 32, 25, 19, 32, baseLight(), baseLight(), false);
        generateBox(level, chunkBB, 32, 17, 32, 32, 19, 32, baseLight(), baseLight(), false);
        placeBlock(level, baseLight(), 26, 20, 26, chunkBB);
        placeBlock(level, baseLight(), 27, 21, 27, chunkBB);
        placeBlock(level, lamp(), 27, 20, 27, chunkBB);
        placeBlock(level, baseLight(), 26, 20, 31, chunkBB);
        placeBlock(level, baseLight(), 27, 21, 30, chunkBB);
        placeBlock(level, lamp(), 27, 20, 30, chunkBB);
        placeBlock(level, baseLight(), 31, 20, 31, chunkBB);
        placeBlock(level, baseLight(), 30, 21, 30, chunkBB);
        placeBlock(level, lamp(), 30, 20, 30, chunkBB);
        placeBlock(level, baseLight(), 31, 20, 26, chunkBB);
        placeBlock(level, baseLight(), 30, 21, 27, chunkBB);
        placeBlock(level, lamp(), 30, 20, 27, chunkBB);
        generateBox(level, chunkBB, 28, 21, 27, 29, 21, 27, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 27, 21, 28, 27, 21, 29, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 28, 21, 30, 29, 21, 30, baseGray(), baseGray(), false);
        generateBox(level, chunkBB, 30, 21, 28, 30, 21, 29, baseGray(), baseGray(), false);
    }

    // Reference: MonumentBuilding.generateLowerWall.
    void generateLowerWall(WorldGenLevel* level, const BoundingBox& chunkBB,
                           ChunkGenerator* generator) {
        if (chunkIntersects(chunkBB, 0, 21, 6, 58)) {
            generateBox(level, chunkBB, 0, 0, 21, 6, 0, 57, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 0, 1, 21, 6, 7, 57);
            generateBox(level, chunkBB, 4, 4, 21, 6, 4, 53, baseGray(), baseGray(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i,
                            baseLight(), baseLight(), false);
            }
            for (int z = 23; z < 53; z += 3) {
                placeBlock(level, dotDeco(), 5, 5, z, chunkBB);
            }
            placeBlock(level, dotDeco(), 5, 5, 52, chunkBB);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, i, i + 1, 21, i, i + 1, 57 - i,
                            baseLight(), baseLight(), false);
            }
            generateBox(level, chunkBB, 4, 1, 52, 6, 3, 52, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 5, 1, 51, 5, 3, 53, baseGray(), baseGray(), false);
        }
        if (chunkIntersects(chunkBB, 51, 21, 58, 58)) {
            generateBox(level, chunkBB, 51, 0, 21, 57, 0, 57, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 51, 1, 21, 57, 7, 57);
            generateBox(level, chunkBB, 51, 4, 21, 53, 4, 53, baseGray(), baseGray(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, 57 - i, i + 1, 21, 57 - i, i + 1, 57 - i,
                            baseLight(), baseLight(), false);
            }
            for (int z = 23; z < 53; z += 3) {
                placeBlock(level, dotDeco(), 52, 5, z, chunkBB);
            }
            placeBlock(level, dotDeco(), 52, 5, 52, chunkBB);
            generateBox(level, chunkBB, 51, 1, 52, 53, 3, 52, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 52, 1, 51, 52, 3, 53, baseGray(), baseGray(), false);
        }
        if (chunkIntersects(chunkBB, 0, 51, 57, 57)) {
            generateBox(level, chunkBB, 7, 0, 51, 50, 0, 57, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 7, 1, 51, 50, 10, 57);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, i + 1, i + 1, 57 - i, 56 - i, i + 1, 57 - i,
                            baseLight(), baseLight(), false);
            }
        }
    }

    // Reference: MonumentBuilding.generateMiddleWall.
    void generateMiddleWall(WorldGenLevel* level, const BoundingBox& chunkBB,
                            ChunkGenerator* generator) {
        if (chunkIntersects(chunkBB, 7, 21, 13, 50)) {
            generateBox(level, chunkBB, 7, 0, 21, 13, 0, 50, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 7, 1, 21, 13, 10, 50);
            generateBox(level, chunkBB, 11, 8, 21, 13, 8, 53, baseGray(), baseGray(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, i + 7, i + 5, 21, i + 7, i + 5, 54,
                            baseLight(), baseLight(), false);
            }
            for (int z = 21; z <= 45; z += 3) {
                placeBlock(level, dotDeco(), 12, 9, z, chunkBB);
            }
        }
        if (chunkIntersects(chunkBB, 44, 21, 50, 54)) {
            generateBox(level, chunkBB, 44, 0, 21, 50, 0, 50, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 44, 1, 21, 50, 10, 50);
            generateBox(level, chunkBB, 44, 8, 21, 46, 8, 53, baseGray(), baseGray(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, 50 - i, i + 5, 21, 50 - i, i + 5, 54,
                            baseLight(), baseLight(), false);
            }
            for (int z = 21; z <= 45; z += 3) {
                placeBlock(level, dotDeco(), 45, 9, z, chunkBB);
            }
        }
        if (chunkIntersects(chunkBB, 8, 44, 49, 54)) {
            generateBox(level, chunkBB, 14, 0, 44, 43, 0, 50, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 14, 1, 44, 43, 10, 50);
            for (int x = 12; x <= 45; x += 3) {
                placeBlock(level, dotDeco(), x, 9, 45, chunkBB);
                placeBlock(level, dotDeco(), x, 9, 52, chunkBB);
                if (x == 12 || x == 18 || x == 24 || x == 33 || x == 39 || x == 45) {
                    placeBlock(level, dotDeco(), x, 9, 47, chunkBB);
                    placeBlock(level, dotDeco(), x, 9, 50, chunkBB);
                    placeBlock(level, dotDeco(), x, 10, 45, chunkBB);
                    placeBlock(level, dotDeco(), x, 10, 46, chunkBB);
                    placeBlock(level, dotDeco(), x, 10, 51, chunkBB);
                    placeBlock(level, dotDeco(), x, 10, 52, chunkBB);
                    placeBlock(level, dotDeco(), x, 11, 47, chunkBB);
                    placeBlock(level, dotDeco(), x, 11, 50, chunkBB);
                    placeBlock(level, dotDeco(), x, 12, 48, chunkBB);
                    placeBlock(level, dotDeco(), x, 12, 49, chunkBB);
                }
            }
            for (int i = 0; i < 3; ++i) {
                generateBox(level, chunkBB, 8 + i, 5 + i, 54, 49 - i, 5 + i, 54,
                            baseGray(), baseGray(), false);
            }
            generateBox(level, chunkBB, 11, 8, 54, 46, 8, 54, baseLight(), baseLight(), false);
            generateBox(level, chunkBB, 14, 8, 44, 43, 8, 53, baseGray(), baseGray(), false);
        }
    }

    // Reference: MonumentBuilding.generateUpperWall.
    void generateUpperWall(WorldGenLevel* level, const BoundingBox& chunkBB,
                           ChunkGenerator* generator) {
        if (chunkIntersects(chunkBB, 14, 21, 20, 43)) {
            generateBox(level, chunkBB, 14, 0, 21, 20, 0, 43, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 14, 1, 22, 20, 14, 43);
            generateBox(level, chunkBB, 18, 12, 22, 20, 12, 39, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 18, 12, 21, 20, 12, 21, baseLight(), baseLight(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, i + 14, i + 9, 21, i + 14, i + 9, 43 - i,
                            baseLight(), baseLight(), false);
            }
            for (int z = 23; z <= 39; z += 3) {
                placeBlock(level, dotDeco(), 19, 13, z, chunkBB);
            }
        }
        if (chunkIntersects(chunkBB, 37, 21, 43, 43)) {
            generateBox(level, chunkBB, 37, 0, 21, 43, 0, 43, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 37, 1, 22, 43, 14, 43);
            generateBox(level, chunkBB, 37, 12, 22, 39, 12, 39, baseGray(), baseGray(), false);
            generateBox(level, chunkBB, 37, 12, 21, 39, 12, 21, baseLight(), baseLight(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, 43 - i, i + 9, 21, 43 - i, i + 9, 43 - i,
                            baseLight(), baseLight(), false);
            }
            for (int z = 23; z <= 39; z += 3) {
                placeBlock(level, dotDeco(), 38, 13, z, chunkBB);
            }
        }
        if (chunkIntersects(chunkBB, 15, 37, 42, 43)) {
            generateBox(level, chunkBB, 21, 0, 37, 36, 0, 43, baseGray(), baseGray(), false);
            generateWaterBox(level, generator, chunkBB, 21, 1, 37, 36, 14, 43);
            generateBox(level, chunkBB, 21, 12, 37, 36, 12, 39, baseGray(), baseGray(), false);
            for (int i = 0; i < 4; ++i) {
                generateBox(level, chunkBB, 15 + i, i + 9, 43 - i, 42 - i, i + 9, 43 - i,
                            baseLight(), baseLight(), false);
            }
            for (int x = 21; x <= 36; x += 3) {
                placeBlock(level, dotDeco(), x, 13, 38, chunkBB);
            }
        }
    }
};

} // namespace

std::shared_ptr<StructurePieceBehavior> monumentBuilding(
    int orientation, const BoundingBox& pieceBox, LegacyRandomSource& random) {
    Transform parent{orientation, pieceBox};
    std::shared_ptr<MonumentData> data = buildMonument(parent, random);
    return std::make_shared<MonumentBuildingBehavior>(orientation, std::move(data));
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
