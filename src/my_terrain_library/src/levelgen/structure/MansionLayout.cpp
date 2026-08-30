#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/ChunkGenerator.h"
#include "levelgen/structure/PieceBehaviors.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// WoodlandMansionPieces.java (full layout transcription: MansionGrid grid
// randomization + MansionPiecePlacer piece emission). LAYOUT ONLY.
//
// All pieces are WoodlandMansionPiece (minecraft:wmp): template SHORT name in
// the P detail (Java templateName field), placeSettings rotation in the P
// rotation, pivot ZERO, mirrors affect only the bbox.

namespace minecraft {
namespace levelgen {
namespace structure {

// From TemplateLayouts.cpp (shared template helpers).
namespace template_detail {
BoundingBox mansionTemplateBox(const std::string& shortName, int rotation, int mirror,
                               int posX, int posY, int posZ);
}

namespace {

// Directions with Java semantics. Order irrelevant internally; conversions are
// explicit. data2d: SOUTH=0, WEST=1, NORTH=2, EAST=3.
enum class D { NORTH, SOUTH, WEST, EAST, UP };

D from2DData(int data) {
    static const D by2d[4] = {D::SOUTH, D::WEST, D::NORTH, D::EAST};
    return by2d[data & 3];
}
D opposite(D d) {
    switch (d) {
        case D::NORTH: return D::SOUTH;
        case D::SOUTH: return D::NORTH;
        case D::WEST: return D::EAST;
        case D::EAST: return D::WEST;
        default: return D::UP;
    }
}
D clockWise(D d) {
    switch (d) {
        case D::NORTH: return D::EAST;
        case D::EAST: return D::SOUTH;
        case D::SOUTH: return D::WEST;
        case D::WEST: return D::NORTH;
        default: return d;
    }
}
D counterClockWise(D d) { return clockWise(clockWise(clockWise(d))); }
int stepX(D d) { return d == D::EAST ? 1 : (d == D::WEST ? -1 : 0); }
int stepZ(D d) { return d == D::SOUTH ? 1 : (d == D::NORTH ? -1 : 0); }

// Rotations as ints 0..3 = NONE, CW90, CW180, CCW90 (Rotation.values order).
int getRotated(int a, int b) { return (a + b) & 3; }
const char* rotName(int r) {
    static const char* names[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
    return names[r & 3];
}
// Reference: Rotation.rotate(Direction)
D rotate(int rotation, D d) {
    if (d == D::UP) return d;
    for (int i = 0; i < (rotation & 3); ++i) d = clockWise(d);
    return d;
}

// Mirrors: 0 NONE, 1 LEFT_RIGHT (flip z), 2 FRONT_BACK (flip x).
struct BP {
    int x = 0, y = 0, z = 0;
    BP relative(D d, int n) const { return {x + stepX(d) * n, y, z + stepZ(d) * n}; }
    BP above(int n = 1) const { return {x, y + n, z}; }
    BP offset(int dx, int dy, int dz) const { return {x + dx, y + dy, z + dz}; }
    // Reference: BlockPos.rotate(Rotation)
    BP rotated(int rotation) const {
        switch (rotation & 3) {
            case 1: return {-z, y, x};
            case 2: return {-x, y, -z};
            case 3: return {z, y, -x};
            default: return *this;
        }
    }
};

struct MPiece {
    std::string name;  // template short name
    BP pos;
    int rotation;
    int mirror;
};

// Reference: SimpleGrid
struct SimpleGrid {
    int width, height, valueIfOutside;
    std::vector<int> cells;
    SimpleGrid(int w, int h, int outside)
        : width(w), height(h), valueIfOutside(outside), cells(static_cast<size_t>(w * h), 0) {}
    void set(int x, int y, int value) {
        if (x >= 0 && x < width && y >= 0 && y < height) cells[static_cast<size_t>(x * height + y)] = value;
    }
    void set(int x0, int y0, int x1, int y1, int value) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) set(x, y, value);
    }
    int get(int x, int y) const {
        return (x >= 0 && x < width && y >= 0 && y < height)
            ? cells[static_cast<size_t>(x * height + y)] : valueIfOutside;
    }
    void setif(int x, int y, int ifValue, int value) {
        if (get(x, y) == ifValue) set(x, y, value);
    }
    bool edgesTo(int x, int y, int ifValue) const {
        return get(x - 1, y) == ifValue || get(x + 1, y) == ifValue
            || get(x, y + 1) == ifValue || get(x, y - 1) == ifValue;
    }
};

bool isHouse(const SimpleGrid& grid, int x, int y) {
    int v = grid.get(x, y);
    return v == 1 || v == 2 || v == 3 || v == 4;
}

// Reference: MansionGrid
struct MansionGrid {
    LegacyRandomSource& random;
    SimpleGrid baseGrid{11, 11, 5};
    SimpleGrid thirdFloorGrid{11, 11, 5};
    std::vector<SimpleGrid> floorRooms;
    int entranceX = 7, entranceY = 4;

    explicit MansionGrid(LegacyRandomSource& rng) : random(rng) {
        baseGrid.set(entranceX, entranceY, entranceX + 1, entranceY + 1, 3);
        baseGrid.set(entranceX - 1, entranceY, entranceX - 1, entranceY + 1, 2);
        baseGrid.set(entranceX + 2, entranceY - 2, entranceX + 3, entranceY + 3, 5);
        baseGrid.set(entranceX + 1, entranceY - 2, entranceX + 1, entranceY - 1, 1);
        baseGrid.set(entranceX + 1, entranceY + 2, entranceX + 1, entranceY + 3, 1);
        baseGrid.set(entranceX - 1, entranceY - 1, 1);
        baseGrid.set(entranceX - 1, entranceY + 2, 1);
        baseGrid.set(0, 0, 11, 1, 5);
        baseGrid.set(0, 9, 11, 11, 5);
        recursiveCorridor(baseGrid, entranceX, entranceY - 2, D::WEST, 6);
        recursiveCorridor(baseGrid, entranceX, entranceY + 3, D::WEST, 6);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY - 1, D::WEST, 3);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY + 2, D::WEST, 3);
        while (cleanEdges(baseGrid)) {}
        floorRooms.assign(3, SimpleGrid(11, 11, 5));
        identifyRooms(baseGrid, floorRooms[0]);
        identifyRooms(baseGrid, floorRooms[1]);
        floorRooms[0].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
        floorRooms[1].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 8388608);
        setupThirdFloor();
        identifyRooms(thirdFloorGrid, floorRooms[2]);
    }

    bool isRoomId(const SimpleGrid&, int x, int y, int floor, int roomId) const {
        return (floorRooms[static_cast<size_t>(floor)].get(x, y) & 0xFFFF) == roomId;
    }

    // Direction.Plane.HORIZONTAL iteration = [NORTH, EAST, SOUTH, WEST].
    static constexpr D kHorizontal[4] = {D::NORTH, D::EAST, D::SOUTH, D::WEST};

    bool get1x2RoomDirection(const SimpleGrid& grid, int x, int y, int floorNum, int roomId, D& out) const {
        for (D direction : kHorizontal) {
            if (isRoomId(grid, x + stepX(direction), y + stepZ(direction), floorNum, roomId)) {
                out = direction;
                return true;
            }
        }
        return false;
    }

    void recursiveCorridor(SimpleGrid& grid, int x, int y, D heading, int depth) {
        if (depth <= 0) return;
        grid.set(x, y, 1);
        grid.setif(x + stepX(heading), y + stepZ(heading), 0, 1);
        for (int attempts = 0; attempts < 8; ++attempts) {
            D nextDir = from2DData(random.nextInt(4));
            if (nextDir != opposite(heading) && (nextDir != D::EAST || !random.nextBoolean())) {
                int nx = x + stepX(heading);
                int ny = y + stepZ(heading);
                if (grid.get(nx + stepX(nextDir), ny + stepZ(nextDir)) == 0
                    && grid.get(nx + stepX(nextDir) * 2, ny + stepZ(nextDir) * 2) == 0) {
                    recursiveCorridor(grid, x + stepX(heading) + stepX(nextDir),
                                      y + stepZ(heading) + stepZ(nextDir), nextDir, depth - 1);
                    break;
                }
            }
        }
        D cw = clockWise(heading);
        D ccw = counterClockWise(heading);
        grid.setif(x + stepX(cw), y + stepZ(cw), 0, 2);
        grid.setif(x + stepX(ccw), y + stepZ(ccw), 0, 2);
        grid.setif(x + stepX(heading) + stepX(cw), y + stepZ(heading) + stepZ(cw), 0, 2);
        grid.setif(x + stepX(heading) + stepX(ccw), y + stepZ(heading) + stepZ(ccw), 0, 2);
        grid.setif(x + stepX(heading) * 2, y + stepZ(heading) * 2, 0, 2);
        grid.setif(x + stepX(cw) * 2, y + stepZ(cw) * 2, 0, 2);
        grid.setif(x + stepX(ccw) * 2, y + stepZ(ccw) * 2, 0, 2);
    }

    bool cleanEdges(SimpleGrid& grid) {
        bool touched = false;
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                if (grid.get(x, y) != 0) continue;
                int direct = (isHouse(grid, x + 1, y) ? 1 : 0) + (isHouse(grid, x - 1, y) ? 1 : 0)
                           + (isHouse(grid, x, y + 1) ? 1 : 0) + (isHouse(grid, x, y - 1) ? 1 : 0);
                if (direct >= 3) {
                    grid.set(x, y, 2);
                    touched = true;
                } else if (direct == 2) {
                    int diag = (isHouse(grid, x + 1, y + 1) ? 1 : 0) + (isHouse(grid, x - 1, y + 1) ? 1 : 0)
                             + (isHouse(grid, x + 1, y - 1) ? 1 : 0) + (isHouse(grid, x - 1, y - 1) ? 1 : 0);
                    if (diag <= 1) {
                        grid.set(x, y, 2);
                        touched = true;
                    }
                }
            }
        }
        return touched;
    }

    void setupThirdFloor() {
        std::vector<std::pair<int, int>> potentialRooms;
        SimpleGrid& floor = floorRooms[1];
        for (int y = 0; y < thirdFloorGrid.height; ++y) {
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                int roomData = floor.get(x, y);
                if ((roomData & 983040) == 131072 && (roomData & 2097152) == 2097152) {
                    potentialRooms.emplace_back(x, y);
                }
            }
        }
        if (potentialRooms.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
            return;
        }
        auto roomPos = potentialRooms[static_cast<size_t>(
            random.nextInt(static_cast<int32_t>(potentialRooms.size())))];
        int roomData = floor.get(roomPos.first, roomPos.second);
        floor.set(roomPos.first, roomPos.second, roomData | 4194304);
        D roomDir = D::NORTH;
        get1x2RoomDirection(baseGrid, roomPos.first, roomPos.second, 1, roomData & 0xFFFF, roomDir);
        int roomEndX = roomPos.first + stepX(roomDir);
        int roomEndY = roomPos.second + stepZ(roomDir);
        for (int y = 0; y < thirdFloorGrid.height; ++y) {
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                if (!isHouse(baseGrid, x, y)) {
                    thirdFloorGrid.set(x, y, 5);
                } else if (x == roomPos.first && y == roomPos.second) {
                    thirdFloorGrid.set(x, y, 3);
                } else if (x == roomEndX && y == roomEndY) {
                    thirdFloorGrid.set(x, y, 3);
                    floorRooms[2].set(x, y, 8388608);
                }
            }
        }
        std::vector<D> potentialCorridors;
        for (D direction : kHorizontal) {
            if (thirdFloorGrid.get(roomEndX + stepX(direction), roomEndY + stepZ(direction)) == 0) {
                potentialCorridors.push_back(direction);
            }
        }
        if (potentialCorridors.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
            floor.set(roomPos.first, roomPos.second, roomData);
        } else {
            D corridorDir = potentialCorridors[static_cast<size_t>(
                random.nextInt(static_cast<int32_t>(potentialCorridors.size())))];
            recursiveCorridor(thirdFloorGrid, roomEndX + stepX(corridorDir),
                              roomEndY + stepZ(corridorDir), corridorDir, 4);
            while (cleanEdges(thirdFloorGrid)) {}
        }
    }

    void identifyRooms(const SimpleGrid& fromGrid, SimpleGrid& roomGrid) {
        std::vector<std::pair<int, int>> roomPos;
        for (int y = 0; y < fromGrid.height; ++y) {
            for (int x = 0; x < fromGrid.width; ++x) {
                if (fromGrid.get(x, y) == 2) roomPos.emplace_back(x, y);
            }
        }
        // Reference: Util.shuffle - reverse Fisher-Yates.
        for (int i = static_cast<int>(roomPos.size()); i > 1; --i) {
            int swapTo = random.nextInt(i);
            std::swap(roomPos[static_cast<size_t>(i - 1)], roomPos[static_cast<size_t>(swapTo)]);
        }
        int roomId = 10;
        for (const auto& pos : roomPos) {
            int x = pos.first, y = pos.second;
            if (roomGrid.get(x, y) != 0) continue;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            int type = 65536;
            if (roomGrid.get(x + 1, y) == 0 && roomGrid.get(x, y + 1) == 0 && roomGrid.get(x + 1, y + 1) == 0
                && fromGrid.get(x + 1, y) == 2 && fromGrid.get(x, y + 1) == 2 && fromGrid.get(x + 1, y + 1) == 2) {
                x1 = x + 1; y1 = y + 1; type = 262144;
            } else if (roomGrid.get(x - 1, y) == 0 && roomGrid.get(x, y + 1) == 0 && roomGrid.get(x - 1, y + 1) == 0
                && fromGrid.get(x - 1, y) == 2 && fromGrid.get(x, y + 1) == 2 && fromGrid.get(x - 1, y + 1) == 2) {
                x0 = x - 1; y1 = y + 1; type = 262144;
            } else if (roomGrid.get(x - 1, y) == 0 && roomGrid.get(x, y - 1) == 0 && roomGrid.get(x - 1, y - 1) == 0
                && fromGrid.get(x - 1, y) == 2 && fromGrid.get(x, y - 1) == 2 && fromGrid.get(x - 1, y - 1) == 2) {
                x0 = x - 1; y0 = y - 1; type = 262144;
            } else if (roomGrid.get(x + 1, y) == 0 && fromGrid.get(x + 1, y) == 2) {
                x1 = x + 1; type = 131072;
            } else if (roomGrid.get(x, y + 1) == 0 && fromGrid.get(x, y + 1) == 2) {
                y1 = y + 1; type = 131072;
            } else if (roomGrid.get(x - 1, y) == 0 && fromGrid.get(x - 1, y) == 2) {
                x0 = x - 1; type = 131072;
            } else if (roomGrid.get(x, y - 1) == 0 && fromGrid.get(x, y - 1) == 2) {
                y0 = y - 1; type = 131072;
            }
            int doorX = random.nextBoolean() ? x0 : x1;
            int doorY = random.nextBoolean() ? y0 : y1;
            int doorFlag = 2097152;
            if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                doorX = doorX == x0 ? x1 : x0;
                doorY = doorY == y0 ? y1 : y0;
                if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                    doorY = doorY == y0 ? y1 : y0;
                    if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                        doorX = doorX == x0 ? x1 : x0;
                        doorY = doorY == y0 ? y1 : y0;
                        if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                            doorFlag = 0;
                            doorX = x0;
                            doorY = y0;
                        }
                    }
                }
            }
            for (int ry = y0; ry <= y1; ++ry) {
                for (int rx = x0; rx <= x1; ++rx) {
                    if (rx == doorX && ry == doorY) {
                        roomGrid.set(rx, ry, 1048576 | doorFlag | type | roomId);
                    } else {
                        roomGrid.set(rx, ry, type | roomId);
                    }
                }
            }
            ++roomId;
        }
    }
};

constexpr D MansionGrid::kHorizontal[4];

// Reference: FloorRoomCollection subclasses.
struct RoomCollection {
    int floor;  // 0/1/2 (2 == ThirdFloor == SecondFloor behavior)
    std::string get1x1(LegacyRandomSource& r) const {
        return (floor == 0 ? "1x1_a" : "1x1_b") + std::to_string(r.nextInt(5) + 1);
    }
    std::string get1x1Secret(LegacyRandomSource& r) const {
        return "1x1_as" + std::to_string(r.nextInt(4) + 1);
    }
    std::string get1x2SideEntrance(LegacyRandomSource& r, bool isStairsRoom) const {
        if (floor == 0) return "1x2_a" + std::to_string(r.nextInt(9) + 1);
        if (isStairsRoom) return "1x2_c_stairs";
        return "1x2_c" + std::to_string(r.nextInt(4) + 1);
    }
    std::string get1x2FrontEntrance(LegacyRandomSource& r, bool isStairsRoom) const {
        if (floor == 0) return "1x2_b" + std::to_string(r.nextInt(5) + 1);
        if (isStairsRoom) return "1x2_d_stairs";
        return "1x2_d" + std::to_string(r.nextInt(5) + 1);
    }
    std::string get1x2Secret(LegacyRandomSource& r) const {
        if (floor == 0) return "1x2_s" + std::to_string(r.nextInt(2) + 1);
        return "1x2_se" + std::to_string(r.nextInt(1) + 1);
    }
    std::string get2x2(LegacyRandomSource& r) const {
        return (floor == 0 ? "2x2_a" + std::to_string(r.nextInt(4) + 1)
                           : "2x2_b" + std::to_string(r.nextInt(5) + 1));
    }
    std::string get2x2Secret(LegacyRandomSource&) const { return "2x2_s1"; }
};

// Reference: StructureTemplate.getZeroPositionWithTransform.
BP getZeroPositionWithTransform(BP zeroPos, int mirror, int rotation, int sizeX, int sizeZ) {
    --sizeX; --sizeZ;
    int mirrorDeltaX = mirror == 2 ? sizeX : 0;  // FRONT_BACK
    int mirrorDeltaZ = mirror == 1 ? sizeZ : 0;  // LEFT_RIGHT
    switch (rotation & 3) {
        case 3: return zeroPos.offset(mirrorDeltaZ, 0, sizeX - mirrorDeltaX);   // CCW90
        case 1: return zeroPos.offset(sizeZ - mirrorDeltaZ, 0, mirrorDeltaX);   // CW90
        case 2: return zeroPos.offset(sizeX - mirrorDeltaX, 0, sizeZ - mirrorDeltaZ);
        default: return zeroPos.offset(mirrorDeltaX, 0, mirrorDeltaZ);
    }
}

// Reference: MansionPiecePlacer.
struct Placer {
    LegacyRandomSource& random;
    std::vector<MPiece>& pieces;
    int startX = 0, startY = 0;

    struct PlacementData {
        int rotation;
        BP position;
        std::string wallType;
    };

    void add(const std::string& name, BP pos, int rotation, int mirror = 0) {
        pieces.push_back({name, pos, rotation, mirror});
    }

    void entrance(PlacementData& data) {
        add("entrance", data.position.relative(rotate(data.rotation, D::WEST), 9), data.rotation);
        data.position = data.position.relative(rotate(data.rotation, D::SOUTH), 16);
    }

    void traverseWallPiece(PlacementData& data) {
        add(data.wallType, data.position.relative(rotate(data.rotation, D::EAST), 7), data.rotation);
        data.position = data.position.relative(rotate(data.rotation, D::SOUTH), 8);
    }

    void traverseTurn(PlacementData& data) {
        data.position = data.position.relative(rotate(data.rotation, D::SOUTH), -1);
        add("wall_corner", data.position, data.rotation);
        data.position = data.position.relative(rotate(data.rotation, D::SOUTH), -7);
        data.position = data.position.relative(rotate(data.rotation, D::WEST), -6);
        data.rotation = getRotated(data.rotation, 1);  // CLOCKWISE_90
    }

    void traverseInnerTurn(PlacementData& data) {
        data.position = data.position.relative(rotate(data.rotation, D::SOUTH), 6);
        data.position = data.position.relative(rotate(data.rotation, D::EAST), 8);
        data.rotation = getRotated(data.rotation, 3);  // COUNTERCLOCKWISE_90
    }

    void traverseOuterWalls(PlacementData& data, const SimpleGrid& grid, D gridDirection,
                            int sx, int sy, int endX, int endY) {
        int gridX = sx, gridY = sy;
        D startDirection = gridDirection;
        do {
            if (!isHouse(grid, gridX + stepX(gridDirection), gridY + stepZ(gridDirection))) {
                traverseTurn(data);
                gridDirection = clockWise(gridDirection);
                if (gridX != endX || gridY != endY || startDirection != gridDirection) {
                    traverseWallPiece(data);
                }
            } else if (isHouse(grid, gridX + stepX(gridDirection), gridY + stepZ(gridDirection))
                    && isHouse(grid, gridX + stepX(gridDirection) + stepX(counterClockWise(gridDirection)),
                               gridY + stepZ(gridDirection) + stepZ(counterClockWise(gridDirection)))) {
                traverseInnerTurn(data);
                gridX += stepX(gridDirection);
                gridY += stepZ(gridDirection);
                gridDirection = counterClockWise(gridDirection);
            } else {
                gridX += stepX(gridDirection);
                gridY += stepZ(gridDirection);
                if (gridX != endX || gridY != endY || startDirection != gridDirection) {
                    traverseWallPiece(data);
                }
            }
        } while (gridX != endX || gridY != endY || startDirection != gridDirection);
    }

    void createRoof(BP roofOrigin, int rotation, const SimpleGrid& grid, const SimpleGrid* aboveGrid) {
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                BP position = roofOrigin.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                position = position.relative(rotate(rotation, D::EAST), (x - startX) * 8);
                bool isAbove = aboveGrid != nullptr && isHouse(*aboveGrid, x, y);
                if (isHouse(grid, x, y) && !isAbove) {
                    add("roof", position.above(3), rotation);
                    if (!isHouse(grid, x + 1, y)) {
                        add("roof_front", position.relative(rotate(rotation, D::EAST), 6), rotation);
                    }
                    if (!isHouse(grid, x - 1, y)) {
                        BP p2 = position.relative(rotate(rotation, D::EAST), 0);
                        p2 = p2.relative(rotate(rotation, D::SOUTH), 7);
                        add("roof_front", p2, getRotated(rotation, 2));
                    }
                    if (!isHouse(grid, x, y - 1)) {
                        add("roof_front", position.relative(rotate(rotation, D::WEST), 1), getRotated(rotation, 3));
                    }
                    if (!isHouse(grid, x, y + 1)) {
                        BP p2 = position.relative(rotate(rotation, D::EAST), 6);
                        p2 = p2.relative(rotate(rotation, D::SOUTH), 6);
                        add("roof_front", p2, getRotated(rotation, 1));
                    }
                }
            }
        }
        if (aboveGrid != nullptr) {
            for (int y = 0; y < grid.height; ++y) {
                for (int x = 0; x < grid.width; ++x) {
                    BP position = roofOrigin.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                    position = position.relative(rotate(rotation, D::EAST), (x - startX) * 8);
                    bool isAbove = isHouse(*aboveGrid, x, y);
                    if (isHouse(grid, x, y) && isAbove) {
                        if (!isHouse(grid, x + 1, y)) {
                            add("small_wall", position.relative(rotate(rotation, D::EAST), 7), rotation);
                        }
                        if (!isHouse(grid, x - 1, y)) {
                            BP p2 = position.relative(rotate(rotation, D::WEST), 1);
                            p2 = p2.relative(rotate(rotation, D::SOUTH), 6);
                            add("small_wall", p2, getRotated(rotation, 2));
                        }
                        if (!isHouse(grid, x, y - 1)) {
                            BP p2 = position.relative(rotate(rotation, D::WEST), 0);
                            p2 = p2.relative(rotate(rotation, D::NORTH), 1);
                            add("small_wall", p2, getRotated(rotation, 3));
                        }
                        if (!isHouse(grid, x, y + 1)) {
                            BP p2 = position.relative(rotate(rotation, D::EAST), 6);
                            p2 = p2.relative(rotate(rotation, D::SOUTH), 7);
                            add("small_wall", p2, getRotated(rotation, 1));
                        }
                        if (!isHouse(grid, x + 1, y)) {
                            if (!isHouse(grid, x, y - 1)) {
                                BP p2 = position.relative(rotate(rotation, D::EAST), 7);
                                p2 = p2.relative(rotate(rotation, D::NORTH), 2);
                                add("small_wall_corner", p2, rotation);
                            }
                            if (!isHouse(grid, x, y + 1)) {
                                BP p2 = position.relative(rotate(rotation, D::EAST), 8);
                                p2 = p2.relative(rotate(rotation, D::SOUTH), 7);
                                add("small_wall_corner", p2, getRotated(rotation, 1));
                            }
                        }
                        if (!isHouse(grid, x - 1, y)) {
                            if (!isHouse(grid, x, y - 1)) {
                                BP p2 = position.relative(rotate(rotation, D::WEST), 2);
                                p2 = p2.relative(rotate(rotation, D::NORTH), 1);
                                add("small_wall_corner", p2, getRotated(rotation, 3));
                            }
                            if (!isHouse(grid, x, y + 1)) {
                                BP p2 = position.relative(rotate(rotation, D::WEST), 1);
                                p2 = p2.relative(rotate(rotation, D::SOUTH), 8);
                                add("small_wall_corner", p2, getRotated(rotation, 2));
                            }
                        }
                    }
                }
            }
        }
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                BP base = roofOrigin.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                base = base.relative(rotate(rotation, D::EAST), (x - startX) * 8);
                bool isAbove = aboveGrid != nullptr && isHouse(*aboveGrid, x, y);
                if (isHouse(grid, x, y) && !isAbove) {
                    if (!isHouse(grid, x + 1, y)) {
                        BP p2 = base.relative(rotate(rotation, D::EAST), 6);
                        if (!isHouse(grid, x, y + 1)) {
                            add("roof_corner", p2.relative(rotate(rotation, D::SOUTH), 6), rotation);
                        } else if (isHouse(grid, x + 1, y + 1)) {
                            add("roof_inner_corner", p2.relative(rotate(rotation, D::SOUTH), 5), rotation);
                        }
                        if (!isHouse(grid, x, y - 1)) {
                            add("roof_corner", p2, getRotated(rotation, 3));
                        } else if (isHouse(grid, x + 1, y - 1)) {
                            BP p3 = base.relative(rotate(rotation, D::EAST), 9);
                            p3 = p3.relative(rotate(rotation, D::NORTH), 2);
                            add("roof_inner_corner", p3, getRotated(rotation, 1));
                        }
                    }
                    if (!isHouse(grid, x - 1, y)) {
                        BP p2 = base.relative(rotate(rotation, D::EAST), 0);
                        p2 = p2.relative(rotate(rotation, D::SOUTH), 0);
                        if (!isHouse(grid, x, y + 1)) {
                            add("roof_corner", p2.relative(rotate(rotation, D::SOUTH), 6), getRotated(rotation, 1));
                        } else if (isHouse(grid, x - 1, y + 1)) {
                            BP p3 = p2.relative(rotate(rotation, D::SOUTH), 8);
                            p3 = p3.relative(rotate(rotation, D::WEST), 3);
                            add("roof_inner_corner", p3, getRotated(rotation, 3));
                        }
                        if (!isHouse(grid, x, y - 1)) {
                            add("roof_corner", p2, getRotated(rotation, 2));
                        } else if (isHouse(grid, x - 1, y - 1)) {
                            add("roof_inner_corner", p2.relative(rotate(rotation, D::SOUTH), 1), getRotated(rotation, 2));
                        }
                    }
                }
            }
        }
    }

    void addRoom1x1(BP roomPos, int rotation, D doorDir, const RoomCollection& rooms) {
        int pieceRot = 0;  // Rotation.NONE
        std::string roomType = rooms.get1x1(random);
        if (doorDir != D::EAST) {
            if (doorDir == D::NORTH) pieceRot = getRotated(pieceRot, 3);
            else if (doorDir == D::WEST) pieceRot = getRotated(pieceRot, 2);
            else if (doorDir == D::SOUTH) pieceRot = getRotated(pieceRot, 1);
            else roomType = rooms.get1x1Secret(random);
        }
        BP orientation = getZeroPositionWithTransform({1, 0, 0}, 0, pieceRot, 7, 7);
        pieceRot = getRotated(pieceRot, rotation);
        orientation = orientation.rotated(rotation);
        BP pos = roomPos.offset(orientation.x, 0, orientation.z);
        add(roomType, pos, pieceRot);
    }

    void addRoom1x2(BP roomPos, int rotation, D roomDir, D doorDir,
                    const RoomCollection& rooms, bool isStairsRoom) {
        if (doorDir == D::EAST && roomDir == D::SOUTH) {
            add(rooms.get1x2SideEntrance(random, isStairsRoom),
                roomPos.relative(rotate(rotation, D::EAST), 1), rotation);
        } else if (doorDir == D::EAST && roomDir == D::NORTH) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 1);
            pos = pos.relative(rotate(rotation, D::SOUTH), 6);
            add(rooms.get1x2SideEntrance(random, isStairsRoom), pos, rotation, 1);
        } else if (doorDir == D::WEST && roomDir == D::NORTH) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 7);
            pos = pos.relative(rotate(rotation, D::SOUTH), 6);
            add(rooms.get1x2SideEntrance(random, isStairsRoom), pos, getRotated(rotation, 2));
        } else if (doorDir == D::WEST && roomDir == D::SOUTH) {
            add(rooms.get1x2SideEntrance(random, isStairsRoom),
                roomPos.relative(rotate(rotation, D::EAST), 7), rotation, 2);
        } else if (doorDir == D::SOUTH && roomDir == D::EAST) {
            add(rooms.get1x2SideEntrance(random, isStairsRoom),
                roomPos.relative(rotate(rotation, D::EAST), 1), getRotated(rotation, 1), 1);
        } else if (doorDir == D::SOUTH && roomDir == D::WEST) {
            add(rooms.get1x2SideEntrance(random, isStairsRoom),
                roomPos.relative(rotate(rotation, D::EAST), 7), getRotated(rotation, 1));
        } else if (doorDir == D::NORTH && roomDir == D::WEST) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 7);
            pos = pos.relative(rotate(rotation, D::SOUTH), 6);
            add(rooms.get1x2SideEntrance(random, isStairsRoom), pos, getRotated(rotation, 1), 2);
        } else if (doorDir == D::NORTH && roomDir == D::EAST) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 1);
            pos = pos.relative(rotate(rotation, D::SOUTH), 6);
            add(rooms.get1x2SideEntrance(random, isStairsRoom), pos, getRotated(rotation, 3));
        } else if (doorDir == D::SOUTH && roomDir == D::NORTH) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 1);
            pos = pos.relative(rotate(rotation, D::NORTH), 8);
            add(rooms.get1x2FrontEntrance(random, isStairsRoom), pos, rotation);
        } else if (doorDir == D::NORTH && roomDir == D::SOUTH) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 7);
            pos = pos.relative(rotate(rotation, D::SOUTH), 14);
            add(rooms.get1x2FrontEntrance(random, isStairsRoom), pos, getRotated(rotation, 2));
        } else if (doorDir == D::WEST && roomDir == D::EAST) {
            add(rooms.get1x2FrontEntrance(random, isStairsRoom),
                roomPos.relative(rotate(rotation, D::EAST), 15), getRotated(rotation, 1));
        } else if (doorDir == D::EAST && roomDir == D::WEST) {
            BP pos = roomPos.relative(rotate(rotation, D::WEST), 7);
            pos = pos.relative(rotate(rotation, D::SOUTH), 6);
            add(rooms.get1x2FrontEntrance(random, isStairsRoom), pos, getRotated(rotation, 3));
        } else if (doorDir == D::UP && roomDir == D::EAST) {
            add(rooms.get1x2Secret(random),
                roomPos.relative(rotate(rotation, D::EAST), 15), getRotated(rotation, 1));
        } else if (doorDir == D::UP && roomDir == D::SOUTH) {
            BP pos = roomPos.relative(rotate(rotation, D::EAST), 1);
            pos = pos.relative(rotate(rotation, D::NORTH), 0);
            add(rooms.get1x2Secret(random), pos, rotation);
        }
    }

    void addRoom2x2(BP roomPos, int rotation, D roomDir, D doorDir, const RoomCollection& rooms) {
        int east = 0, south = 0;
        int rot = rotation;
        int mirror = 0;
        if (doorDir == D::EAST && roomDir == D::SOUTH) {
            east = -7;
        } else if (doorDir == D::EAST && roomDir == D::NORTH) {
            east = -7; south = 6; mirror = 1;
        } else if (doorDir == D::NORTH && roomDir == D::EAST) {
            east = 1; south = 14; rot = getRotated(rotation, 3);
        } else if (doorDir == D::NORTH && roomDir == D::WEST) {
            east = 7; south = 14; rot = getRotated(rotation, 3); mirror = 1;
        } else if (doorDir == D::SOUTH && roomDir == D::WEST) {
            east = 7; south = -8; rot = getRotated(rotation, 1);
        } else if (doorDir == D::SOUTH && roomDir == D::EAST) {
            east = 1; south = -8; rot = getRotated(rotation, 1); mirror = 1;
        } else if (doorDir == D::WEST && roomDir == D::NORTH) {
            east = 15; south = 6; rot = getRotated(rotation, 2);
        } else if (doorDir == D::WEST && roomDir == D::SOUTH) {
            east = 15; mirror = 2;
        }
        BP pos = roomPos.relative(rotate(rotation, D::EAST), east);
        pos = pos.relative(rotate(rotation, D::SOUTH), south);
        add(rooms.get2x2(random), pos, rot, mirror);
    }

    void addRoom2x2Secret(BP roomPos, int rotation, const RoomCollection& rooms) {
        add(rooms.get2x2Secret(random), roomPos.relative(rotate(rotation, D::EAST), 1), rotation, 0);
    }

    void createMansion(BP origin, int rotation, MansionGrid& mansion) {
        PlacementData data{rotation, origin, "wall_flat"};
        PlacementData secondData{rotation, origin, "wall_window"};
        entrance(data);
        secondData.position = data.position.above(8);
        secondData.rotation = data.rotation;
        // (Java re-reads secondData fields after entrance() mutated data.)
        SimpleGrid& baseGrid = mansion.baseGrid;
        SimpleGrid& thirdGrid = mansion.thirdFloorGrid;
        startX = mansion.entranceX + 1;
        startY = mansion.entranceY + 1;
        int endX = mansion.entranceX + 1;
        int endY = mansion.entranceY;
        traverseOuterWalls(data, baseGrid, D::SOUTH, startX, startY, endX, endY);
        traverseOuterWalls(secondData, baseGrid, D::SOUTH, startX, startY, endX, endY);
        PlacementData thirdData{rotation, origin.above(19), "wall_window"};
        bool done = false;
        for (int y = 0; y < thirdGrid.height && !done; ++y) {
            for (int x = thirdGrid.width - 1; x >= 0 && !done; --x) {
                if (isHouse(thirdGrid, x, y)) {
                    thirdData.position = thirdData.position.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                    thirdData.position = thirdData.position.relative(rotate(rotation, D::EAST), (x - startX) * 8);
                    traverseWallPiece(thirdData);
                    traverseOuterWalls(thirdData, thirdGrid, D::SOUTH, x, y, x, y);
                    done = true;
                }
            }
        }
        createRoof(origin.above(16), rotation, baseGrid, &thirdGrid);
        createRoof(origin.above(27), rotation, thirdGrid, nullptr);

        RoomCollection roomCollections[3] = {{0}, {1}, {2}};
        for (int floorNum = 0; floorNum < 3; ++floorNum) {
            BP floorOrigin = origin.above(8 * floorNum + (floorNum == 2 ? 3 : 0));
            SimpleGrid& rooms = mansion.floorRooms[static_cast<size_t>(floorNum)];
            SimpleGrid& grid = floorNum == 2 ? thirdGrid : baseGrid;
            std::string southPiece = floorNum == 0 ? "carpet_south_1" : "carpet_south_2";
            std::string westPiece = floorNum == 0 ? "carpet_west_1" : "carpet_west_2";
            for (int y = 0; y < grid.height; ++y) {
                for (int x = 0; x < grid.width; ++x) {
                    if (grid.get(x, y) != 1) continue;
                    BP pos = floorOrigin.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                    pos = pos.relative(rotate(rotation, D::EAST), (x - startX) * 8);
                    add("corridor_floor", pos, rotation);
                    if (grid.get(x, y - 1) == 1 || (rooms.get(x, y - 1) & 8388608) == 8388608) {
                        add("carpet_north", pos.relative(rotate(rotation, D::EAST), 1).above(), rotation);
                    }
                    if (grid.get(x + 1, y) == 1 || (rooms.get(x + 1, y) & 8388608) == 8388608) {
                        add("carpet_east",
                            pos.relative(rotate(rotation, D::SOUTH), 1).relative(rotate(rotation, D::EAST), 5).above(),
                            rotation);
                    }
                    if (grid.get(x, y + 1) == 1 || (rooms.get(x, y + 1) & 8388608) == 8388608) {
                        add(southPiece,
                            pos.relative(rotate(rotation, D::SOUTH), 5).relative(rotate(rotation, D::WEST), 1),
                            rotation);
                    }
                    if (grid.get(x - 1, y) == 1 || (rooms.get(x - 1, y) & 8388608) == 8388608) {
                        add(westPiece,
                            pos.relative(rotate(rotation, D::WEST), 1).relative(rotate(rotation, D::NORTH), 1),
                            rotation);
                    }
                }
            }
            std::string wallPiece = floorNum == 0 ? "indoors_wall_1" : "indoors_wall_2";
            std::string doorPiece = floorNum == 0 ? "indoors_door_1" : "indoors_door_2";
            std::vector<D> doorDirs;
            for (int y = 0; y < grid.height; ++y) {
                for (int x = 0; x < grid.width; ++x) {
                    bool thirdFloorStartRoom = floorNum == 2 && grid.get(x, y) == 3;
                    if (grid.get(x, y) != 2 && !thirdFloorStartRoom) continue;
                    int roomData = rooms.get(x, y);
                    int roomType = roomData & 983040;
                    int roomId = roomData & 0xFFFF;
                    thirdFloorStartRoom = thirdFloorStartRoom && (roomData & 8388608) == 8388608;
                    doorDirs.clear();
                    if ((roomData & 2097152) == 2097152) {
                        for (D direction : MansionGrid::kHorizontal) {
                            if (grid.get(x + stepX(direction), y + stepZ(direction)) == 1) {
                                doorDirs.push_back(direction);
                            }
                        }
                    }
                    bool hasDoorDir = false;
                    D doorDir = D::NORTH;
                    if (!doorDirs.empty()) {
                        doorDir = doorDirs[static_cast<size_t>(
                            random.nextInt(static_cast<int32_t>(doorDirs.size())))];
                        hasDoorDir = true;
                    } else if ((roomData & 1048576) == 1048576) {
                        doorDir = D::UP;
                        hasDoorDir = true;
                    }
                    BP roomPos = floorOrigin.relative(rotate(rotation, D::SOUTH), 8 + (y - startY) * 8);
                    roomPos = roomPos.relative(rotate(rotation, D::EAST), -1 + (x - startX) * 8);
                    if (isHouse(grid, x - 1, y) && !mansion.isRoomId(grid, x - 1, y, floorNum, roomId)) {
                        add((hasDoorDir && doorDir == D::WEST) ? doorPiece : wallPiece, roomPos, rotation);
                    }
                    if (grid.get(x + 1, y) == 1 && !thirdFloorStartRoom) {
                        add((hasDoorDir && doorDir == D::EAST) ? doorPiece : wallPiece,
                            roomPos.relative(rotate(rotation, D::EAST), 8), rotation);
                    }
                    if (isHouse(grid, x, y + 1) && !mansion.isRoomId(grid, x, y + 1, floorNum, roomId)) {
                        BP pos = roomPos.relative(rotate(rotation, D::SOUTH), 7);
                        pos = pos.relative(rotate(rotation, D::EAST), 7);
                        add((hasDoorDir && doorDir == D::SOUTH) ? doorPiece : wallPiece, pos, getRotated(rotation, 1));
                    }
                    if (grid.get(x, y - 1) == 1 && !thirdFloorStartRoom) {
                        BP pos = roomPos.relative(rotate(rotation, D::NORTH), 1);
                        pos = pos.relative(rotate(rotation, D::EAST), 7);
                        add((hasDoorDir && doorDir == D::NORTH) ? doorPiece : wallPiece, pos, getRotated(rotation, 1));
                    }
                    if (roomType == 65536) {
                        // Java doorDir may be null here; null and UP both fall
                        // into addRoom1x1's final else (secret room), so map
                        // null -> UP (behaviorally identical).
                        addRoom1x1(roomPos, rotation, hasDoorDir ? doorDir : D::UP,
                                   roomCollections[floorNum]);
                    } else if (roomType == 131072 && hasDoorDir) {
                        D roomDir = D::NORTH;
                        mansion.get1x2RoomDirection(grid, x, y, floorNum, roomId, roomDir);
                        bool isStairsRoom = (roomData & 4194304) == 4194304;
                        addRoom1x2(roomPos, rotation, roomDir, doorDir, roomCollections[floorNum], isStairsRoom);
                    } else if (roomType == 262144 && hasDoorDir && doorDir != D::UP) {
                        D roomDir = clockWise(doorDir);
                        if (!mansion.isRoomId(grid, x + stepX(roomDir), y + stepZ(roomDir), floorNum, roomId)) {
                            roomDir = opposite(roomDir);
                        }
                        addRoom2x2(roomPos, rotation, roomDir, doorDir, roomCollections[floorNum]);
                    } else if (roomType == 262144 && hasDoorDir && doorDir == D::UP) {
                        addRoom2x2Secret(roomPos, rotation, roomCollections[floorNum]);
                    }
                }
            }
        }
    }
};

} // namespace

namespace StructureLayouts {

bool generateMansion(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                     int rotation, int startPosX, int startPosY, int startPosZ) {
    (void)info;
    // Reference: WoodlandMansionStructure.generatePieces - MansionGrid draws
    // first (ctor), then MansionPiecePlacer.createMansion.
    std::vector<MPiece> mpieces;
    MansionGrid grid(ctx.random);
    Placer placer{ctx.random, mpieces};
    placer.createMansion({startPosX, startPosY, startPosZ}, rotation, grid);

    out.pieces.clear();
    out.pieces.reserve(mpieces.size());
    out.behaviors.clear();
    out.behaviors.reserve(mpieces.size());
    for (const auto& mp : mpieces) {
        StructurePieceData data;
        data.pieceType = "minecraft:wmp";
        data.boundingBox = template_detail::mansionTemplateBox(mp.name, mp.rotation, mp.mirror,
                                                               mp.pos.x, mp.pos.y, mp.pos.z);
        data.rotation = rotName(mp.rotation);
        data.genDepth = 0;
        data.detail = mp.name;  // templateName = SHORT name for mansion pieces
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(PieceBehaviors::mansionPiece(
            mp.name, mp.rotation, mp.mirror,
            core::BlockPos(mp.pos.x, mp.pos.y, mp.pos.z)));
    }
    out.afterPlace = PieceBehaviors::mansionAfterPlace();
    return !out.pieces.empty();
}

} // namespace StructureLayouts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
