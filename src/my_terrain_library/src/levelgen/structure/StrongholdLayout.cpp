#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/ChunkGenerator.h"
#include "levelgen/structure/PieceBehaviors.h"

#include <cstdlib>
#include <optional>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// StrongholdStructure.java + StrongholdPieces.java (layout halves).
// LAYOUT ONLY - postProcess comes in B6.
//
// Generation shape: retry loop reseeding context.random with
// setLargeFeatureSeed(seed + tries++, cx, cz) until pieces are nonempty AND a
// portal room was placed. Piece selection uses a per-run weighted class list
// with placeCount/maxPlaceCount state, an "imposedPiece" override (FiveCrossing
// forced after the start stairs), previousPiece repeat-suppression (PieceWeight
// object identity == kind here, kinds are unique in the list), and the start
// piece's pendingChildren queue drained in random order (nextInt(size) removal).
//
// Ctor draw orders (after the createPiece box+collision checks pass):
//   StairsDown: nextInt(5) door        Straight: door, nextInt(2), nextInt(2)
//   ChestCorridor/StraightStairsDown/LeftTurn/RightTurn/PrisonHall: door
//   RoomCrossing: door, nextInt(5)     Library: door (tall from box height)
//   FiveCrossing: door, nextBoolean x3, nextInt(3)
//   PortalRoom / FillerCorridor / source StairsDown (StartPiece): no draws
//   (StartPiece itself draws nextInt(4) for its direction first)

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

enum class Dir { NORTH, SOUTH, WEST, EAST };

const char* dirRotationName(Dir d) {
    return (d == Dir::WEST || d == Dir::EAST) ? "CLOCKWISE_90" : "NONE";
}

// Reference: Direction.Plane.HORIZONTAL = [NORTH, EAST, SOUTH, WEST]
Dir randomHorizontal(LegacyRandomSource& random) {
    static const Dir order[4] = {Dir::NORTH, Dir::EAST, Dir::SOUTH, Dir::WEST};
    return order[random.nextInt(4)];
}

// Reference: BoundingBox.orientBox()
BoundingBox orientBox(int footX, int footY, int footZ, int offX, int offY, int offZ,
                      int width, int height, int depth, Dir direction) {
    switch (direction) {
        case Dir::SOUTH: default:
            return BoundingBox(footX + offX, footY + offY, footZ + offZ,
                               footX + width - 1 + offX, footY + height - 1 + offY,
                               footZ + depth - 1 + offZ);
        case Dir::NORTH:
            return BoundingBox(footX + offX, footY + offY, footZ - depth + 1 + offZ,
                               footX + width - 1 + offX, footY + height - 1 + offY, footZ + offZ);
        case Dir::WEST:
            return BoundingBox(footX - depth + 1 + offZ, footY + offY, footZ + offX,
                               footX + offZ, footY + height - 1 + offY, footZ + width - 1 + offX);
        case Dir::EAST:
            return BoundingBox(footX + offZ, footY + offY, footZ + offX,
                               footX + depth - 1 + offZ, footY + height - 1 + offY,
                               footZ + width - 1 + offX);
    }
}

// Reference: StructurePiece.makeBoundingBox() (StartPiece stairs at y=64).
BoundingBox makeStairsBox(int x, int y, int z, Dir direction, int width, int height, int depth) {
    bool axisZ = (direction == Dir::NORTH || direction == Dir::SOUTH);
    if (axisZ) return BoundingBox(x, y, z, x + width - 1, y + height - 1, z + depth - 1);
    return BoundingBox(x, y, z, x + depth - 1, y + height - 1, z + width - 1);
}

enum Kind {
    K_START, K_STAIRS_DOWN, K_STRAIGHT, K_CHEST_CORRIDOR, K_STRAIGHT_STAIRS_DOWN,
    K_LEFT_TURN, K_RIGHT_TURN, K_ROOM_CROSSING, K_PRISON_HALL, K_FIVE_CROSSING,
    K_LIBRARY, K_PORTAL_ROOM, K_FILLER_CORRIDOR
};

const char* kindTypeId(int kind) {
    switch (kind) {
        case K_START: return "minecraft:shstart";
        case K_STAIRS_DOWN: return "minecraft:shsd";
        case K_STRAIGHT: return "minecraft:shs";
        case K_CHEST_CORRIDOR: return "minecraft:shcc";
        case K_STRAIGHT_STAIRS_DOWN: return "minecraft:shssd";
        case K_LEFT_TURN: return "minecraft:shlt";
        case K_RIGHT_TURN: return "minecraft:shrt";
        case K_ROOM_CROSSING: return "minecraft:shrc";
        case K_PRISON_HALL: return "minecraft:shph";
        case K_FIVE_CROSSING: return "minecraft:sh5c";
        case K_LIBRARY: return "minecraft:shli";
        case K_PORTAL_ROOM: return "minecraft:shpr";
        case K_FILLER_CORRIDOR: return "minecraft:shfc";
    }
    return "?";
}

struct SPiece {
    int kind;
    BoundingBox box;
    int genDepth;
    Dir orientation;   // every stronghold piece calls setOrientation
    // Straight: a=leftChild, b=rightChild. FiveCrossing: a=leftLow, b=leftHigh,
    // c=rightLow, d=rightHigh. Others unused.
    bool a = false, b = false, c = false, d = false;
    // B6: SmallDoorType ordinal (OPENING=0, WOOD_DOOR=1, GRATES=2, IRON_DOOR=3)
    // and RoomCrossing.type - captured from the ctor draws for postProcess.
    int door = 0;
    int rcType = 0;
};

// Reference: StrongholdPiece.randomSmallDoor - 0,1 -> OPENING; 2 -> WOOD_DOOR;
// 3 -> GRATES; 4 -> IRON_DOOR.
int randomSmallDoor(LegacyRandomSource& random) {
    switch (random.nextInt(5)) {
        case 2: return 1;
        case 3: return 2;
        case 4: return 3;
        default: return 0;
    }
}

struct PieceWeightEntry {
    int kind;
    int weight;
    int maxPlaceCount;
    int placeCount = 0;
    bool doPlace(int depth) const {
        bool base = maxPlaceCount == 0 || placeCount < maxPlaceCount;
        if (kind == K_LIBRARY) return base && depth > 4;
        if (kind == K_PORTAL_ROOM) return base && depth > 5;
        return base;
    }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

struct Generator {
    std::vector<SPiece> pieces;
    std::vector<PieceWeightEntry> currentPieces;
    int totalWeight = 0;
    std::optional<int> imposedPiece;        // kind (FiveCrossing after start)
    std::optional<int> previousPieceKind;   // StartPiece.previousPiece
    std::optional<size_t> portalRoomIndex;  // StartPiece.portalRoomPiece
    std::vector<size_t> pendingChildren;

    // Reference: resetPieces() - table order matters for the weighted scan.
    void reset() {
        pieces.clear();
        pendingChildren.clear();
        portalRoomIndex.reset();
        previousPieceKind.reset();
        imposedPiece.reset();
        currentPieces = {
            {K_STRAIGHT, 40, 0}, {K_PRISON_HALL, 5, 5}, {K_LEFT_TURN, 20, 0},
            {K_RIGHT_TURN, 20, 0}, {K_ROOM_CROSSING, 10, 6}, {K_STRAIGHT_STAIRS_DOWN, 5, 5},
            {K_STAIRS_DOWN, 5, 5}, {K_FIVE_CROSSING, 5, 4}, {K_CHEST_CORRIDOR, 5, 4},
            {K_LIBRARY, 10, 2}, {K_PORTAL_ROOM, 20, 1},
        };
    }

    const SPiece* findCollisionPiece(const BoundingBox& box) const {
        for (const auto& piece : pieces) {
            if (piece.box.intersects(box)) return &piece;
        }
        return nullptr;
    }

    bool updatePieceWeight() {
        bool hasAnyPieces = false;
        totalWeight = 0;
        for (const auto& piece : currentPieces) {
            if (piece.maxPlaceCount > 0 && piece.placeCount < piece.maxPlaceCount) {
                hasAnyPieces = true;
            }
            totalWeight += piece.weight;
        }
        return hasAnyPieces;
    }

    BoundingBox getBoundingBox() const {
        BoundingBox result = pieces.front().box;
        for (const auto& piece : pieces) {
            result.minX = std::min(result.minX, piece.box.minX);
            result.minY = std::min(result.minY, piece.box.minY);
            result.minZ = std::min(result.minZ, piece.box.minZ);
            result.maxX = std::max(result.maxX, piece.box.maxX);
            result.maxY = std::max(result.maxY, piece.box.maxY);
            result.maxZ = std::max(result.maxZ, piece.box.maxZ);
        }
        return result;
    }

    void offsetPiecesVertically(int dy) {
        for (auto& piece : pieces) piece.box.move(0, dy, 0);
    }

    // Reference: StructurePiecesBuilder.moveBelowSeaLevel()
    void moveBelowSeaLevel(int seaLevel, int minY, LegacyRandomSource& random, int offset) {
        int maxY = seaLevel - offset;
        BoundingBox boundingBox = getBoundingBox();
        int y1Pos = boundingBox.getYSpan() + minY + 1;
        if (y1Pos < maxY) {
            y1Pos += random.nextInt(maxY - y1Pos);
        }
        offsetPiecesVertically(y1Pos - boundingBox.maxY);
    }
};

bool isOkBox(const BoundingBox& box) { return box.minY > 10; }

// Reference: per-class createPiece. Returns piece index or -1. Ctor draws
// happen only after box + collision checks pass (Java constructs the piece
// object then, consuming its ctor RNG).
int createPieceOfKind(int kind, Generator& gen, LegacyRandomSource& random,
                      int footX, int footY, int footZ, Dir direction, int genDepth) {
    SPiece piece;
    piece.kind = kind;
    piece.genDepth = genDepth;
    piece.orientation = direction;

    switch (kind) {
        case K_STRAIGHT: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 7, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            piece.a = random.nextInt(2) == 0;     // leftChild
            piece.b = random.nextInt(2) == 0;     // rightChild
            break;
        }
        case K_PRISON_HALL: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 9, 5, 11, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            break;
        }
        case K_LEFT_TURN:
        case K_RIGHT_TURN: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 5, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            break;
        }
        case K_ROOM_CROSSING: {
            BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 11, 7, 11, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            piece.rcType = random.nextInt(5);     // RoomCrossing.type
            break;
        }
        case K_STRAIGHT_STAIRS_DOWN: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -7, 0, 5, 11, 8, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            break;
        }
        case K_STAIRS_DOWN: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -7, 0, 5, 11, 5, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            break;
        }
        case K_FIVE_CROSSING: {
            BoundingBox box = orientBox(footX, footY, footZ, -4, -3, 0, 10, 9, 11, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            piece.a = random.nextBoolean();       // leftLow
            piece.b = random.nextBoolean();       // leftHigh
            piece.c = random.nextBoolean();       // rightLow
            piece.d = random.nextInt(3) > 0;      // rightHigh
            break;
        }
        case K_CHEST_CORRIDOR: {
            BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 7, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            piece.door = randomSmallDoor(random);
            break;
        }
        case K_LIBRARY: {
            BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 14, 11, 15, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) {
                box = orientBox(footX, footY, footZ, -4, -1, 0, 14, 6, 15, direction);
                if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            }
            piece.box = box;
            piece.door = randomSmallDoor(random); // isTall from box
            break;
        }
        case K_PORTAL_ROOM: {
            // Reference: PortalRoom.createPiece has NO random parameter.
            BoundingBox box = orientBox(footX, footY, footZ, -4, -1, 0, 11, 8, 16, direction);
            if (!isOkBox(box) || gen.findCollisionPiece(box)) return -1;
            piece.box = box;
            break;
        }
        default:
            return -1;
    }

    gen.pieces.push_back(piece);
    return static_cast<int>(gen.pieces.size()) - 1;
}

// Reference: FillerCorridor.findPieceBox()
std::optional<BoundingBox> findFillerBox(Generator& gen, int footX, int footY, int footZ, Dir direction) {
    BoundingBox box = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, 4, direction);
    const SPiece* collisionPiece = gen.findCollisionPiece(box);
    if (collisionPiece == nullptr) return std::nullopt;
    if (collisionPiece->box.minY == box.minY) {
        for (int depth = 2; depth >= 1; --depth) {
            BoundingBox shorter = orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, depth, direction);
            if (!collisionPiece->box.intersects(shorter)) {
                return orientBox(footX, footY, footZ, -1, -1, 0, 5, 5, depth + 1, direction);
            }
        }
    }
    return std::nullopt;
}

// Reference: generatePieceFromSmallDoor()
int generatePieceFromSmallDoor(Generator& gen, LegacyRandomSource& random,
                               int footX, int footY, int footZ, Dir direction, int depth) {
    if (!gen.updatePieceWeight()) return -1;

    if (gen.imposedPiece.has_value()) {
        int imposedKind = *gen.imposedPiece;
        gen.imposedPiece.reset();
        int index = createPieceOfKind(imposedKind, gen, random, footX, footY, footZ, direction, depth);
        if (index >= 0) return index;
    }

    for (int numAttempts = 0; numAttempts < 5; ) {
        ++numAttempts;
        int weightSelection = random.nextInt(gen.totalWeight);
        for (auto& entry : gen.currentPieces) {
            weightSelection -= entry.weight;
            if (weightSelection < 0) {
                if (!entry.doPlace(depth)
                    || (gen.previousPieceKind.has_value() && *gen.previousPieceKind == entry.kind)) {
                    break;
                }
                int index = createPieceOfKind(entry.kind, gen, random, footX, footY, footZ, direction, depth);
                if (index >= 0) {
                    ++entry.placeCount;
                    gen.previousPieceKind = entry.kind;
                    if (!entry.isValid()) {
                        // Reference: currentPieces.remove(piece)
                        for (auto it = gen.currentPieces.begin(); it != gen.currentPieces.end(); ++it) {
                            if (it->kind == entry.kind) { gen.currentPieces.erase(it); break; }
                        }
                    }
                    return index;
                }
                // NO break on factory failure: Java's scan CASCADES - once
                // weightSelection is negative, every subsequent table entry is
                // also tried within this attempt (verified against the running
                // jar: nextInt(145)=94 selects RoomCrossing, collides, falls
                // through to StraightStairsDown, then StairsDown, which lands).
            }
        }
    }

    auto fillerBox = findFillerBox(gen, footX, footY, footZ, direction);
    if (fillerBox.has_value() && fillerBox->minY > 1) {
        SPiece piece;
        piece.kind = K_FILLER_CORRIDOR;
        piece.box = *fillerBox;
        piece.genDepth = depth;
        piece.orientation = direction;
        gen.pieces.push_back(piece);
        return static_cast<int>(gen.pieces.size()) - 1;
    }
    return -1;
}

// Reference: generateAndAddPiece() - also queues into pendingChildren.
int generateAndAddPiece(Generator& gen, LegacyRandomSource& random,
                        int footX, int footY, int footZ, Dir direction, int depth) {
    if (depth > 50) return -1;
    const BoundingBox& startBox = gen.pieces.front().box;
    if (std::abs(footX - startBox.minX) > 112 || std::abs(footZ - startBox.minZ) > 112) {
        return -1;
    }
    int index = generatePieceFromSmallDoor(gen, random, footX, footY, footZ, direction, depth + 1);
    if (index >= 0) {
        gen.pendingChildren.push_back(static_cast<size_t>(index));
    }
    return index;
}

// Reference: StrongholdPiece.generateSmallDoorChildForward/Left/Right
void doorChildForward(Generator& gen, LegacyRandomSource& random, size_t index, int xOff, int yOff) {
    const SPiece piece = gen.pieces[index];  // copy: vector may realloc
    switch (piece.orientation) {
        case Dir::NORTH:
            generateAndAddPiece(gen, random, piece.box.minX + xOff, piece.box.minY + yOff,
                                piece.box.minZ - 1, piece.orientation, piece.genDepth);
            break;
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, piece.box.minX + xOff, piece.box.minY + yOff,
                                piece.box.maxZ + 1, piece.orientation, piece.genDepth);
            break;
        case Dir::WEST:
            generateAndAddPiece(gen, random, piece.box.minX - 1, piece.box.minY + yOff,
                                piece.box.minZ + xOff, piece.orientation, piece.genDepth);
            break;
        case Dir::EAST:
            generateAndAddPiece(gen, random, piece.box.maxX + 1, piece.box.minY + yOff,
                                piece.box.minZ + xOff, piece.orientation, piece.genDepth);
            break;
    }
}

void doorChildLeft(Generator& gen, LegacyRandomSource& random, size_t index, int yOff, int zOff) {
    const SPiece piece = gen.pieces[index];
    switch (piece.orientation) {
        case Dir::NORTH:
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, piece.box.minX - 1, piece.box.minY + yOff,
                                piece.box.minZ + zOff, Dir::WEST, piece.genDepth);
            break;
        case Dir::WEST:
        case Dir::EAST:
            generateAndAddPiece(gen, random, piece.box.minX + zOff, piece.box.minY + yOff,
                                piece.box.minZ - 1, Dir::NORTH, piece.genDepth);
            break;
    }
}

void doorChildRight(Generator& gen, LegacyRandomSource& random, size_t index, int yOff, int zOff) {
    const SPiece piece = gen.pieces[index];
    switch (piece.orientation) {
        case Dir::NORTH:
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, piece.box.maxX + 1, piece.box.minY + yOff,
                                piece.box.minZ + zOff, Dir::EAST, piece.genDepth);
            break;
        case Dir::WEST:
        case Dir::EAST:
            generateAndAddPiece(gen, random, piece.box.minX + zOff, piece.box.minY + yOff,
                                piece.box.maxZ + 1, Dir::SOUTH, piece.genDepth);
            break;
    }
}

// Reference: the per-class addChildren methods.
void addChildren(Generator& gen, size_t index, LegacyRandomSource& random) {
    const SPiece piece = gen.pieces[index];  // copy
    switch (piece.kind) {
        case K_START:
            // Reference: StairsDown.addChildren with isSource == true.
            gen.imposedPiece = K_FIVE_CROSSING;
            doorChildForward(gen, random, index, 1, 1);
            break;
        case K_STAIRS_DOWN:
            doorChildForward(gen, random, index, 1, 1);
            break;
        case K_STRAIGHT:
            doorChildForward(gen, random, index, 1, 1);
            if (piece.a) doorChildLeft(gen, random, index, 1, 2);
            if (piece.b) doorChildRight(gen, random, index, 1, 2);
            break;
        case K_CHEST_CORRIDOR:
        case K_STRAIGHT_STAIRS_DOWN:
        case K_PRISON_HALL:
            doorChildForward(gen, random, index, 1, 1);
            break;
        case K_LEFT_TURN:
            if (piece.orientation != Dir::NORTH && piece.orientation != Dir::EAST) {
                doorChildRight(gen, random, index, 1, 1);
            } else {
                doorChildLeft(gen, random, index, 1, 1);
            }
            break;
        case K_RIGHT_TURN:
            if (piece.orientation != Dir::NORTH && piece.orientation != Dir::EAST) {
                doorChildLeft(gen, random, index, 1, 1);
            } else {
                doorChildRight(gen, random, index, 1, 1);
            }
            break;
        case K_ROOM_CROSSING:
            doorChildForward(gen, random, index, 4, 1);
            doorChildLeft(gen, random, index, 1, 4);
            doorChildRight(gen, random, index, 1, 4);
            break;
        case K_FIVE_CROSSING: {
            int zOffA = 3;
            int zOffB = 5;
            if (piece.orientation == Dir::WEST || piece.orientation == Dir::NORTH) {
                zOffA = 8 - zOffA;
                zOffB = 8 - zOffB;
            }
            doorChildForward(gen, random, index, 5, 1);
            if (piece.a) doorChildLeft(gen, random, index, zOffA, 1);
            if (piece.b) doorChildLeft(gen, random, index, zOffB, 7);
            if (piece.c) doorChildRight(gen, random, index, zOffA, 1);
            if (piece.d) doorChildRight(gen, random, index, zOffB, 7);
            break;
        }
        case K_LIBRARY:
            // Reference: Library has no addChildren override (dead end).
            break;
        case K_PORTAL_ROOM:
            gen.portalRoomIndex = index;
            break;
        case K_FILLER_CORRIDOR:
            // No addChildren override (dead end). NOTE: FillerCorridor is
            // returned by generatePieceFromSmallDoor and queued, so its
            // addChildren (the base no-op) still runs from the pending queue.
            break;
    }
}

} // namespace

namespace StructureLayouts {

bool generateStronghold(const StructureInfo& info, GenerationContext& ctx,
                        StructureStartData& out) {
    (void)info;
    // Reference: StrongholdStructure.generatePieces() retry loop. The stub
    // biome check happened BEFORE this (lazy consumer; caller's duty).
    Generator gen;
    int tries = 0;
    do {
        gen.reset();
        ctx.random.setLargeFeatureSeed(ctx.seed + static_cast<int64_t>(tries++), ctx.chunkX, ctx.chunkZ);

        // StartPiece: nextInt(4) direction; source stairs, no further draws.
        Dir direction = randomHorizontal(ctx.random);
        SPiece start;
        start.kind = K_START;
        start.box = makeStairsBox(ctx.chunkX * 16 + 2, 64, ctx.chunkZ * 16 + 2, direction, 5, 11, 5);
        start.genDepth = 0;
        start.orientation = direction;
        gen.pieces.push_back(start);
        addChildren(gen, 0, ctx.random);

        while (!gen.pendingChildren.empty()) {
            int pos = ctx.random.nextInt(static_cast<int32_t>(gen.pendingChildren.size()));
            size_t pieceIndex = gen.pendingChildren[static_cast<size_t>(pos)];
            gen.pendingChildren.erase(gen.pendingChildren.begin() + pos);
            addChildren(gen, pieceIndex, ctx.random);
        }

        gen.moveBelowSeaLevel(ctx.generator->getSeaLevel(), ctx.generator->getMinY(), ctx.random, 10);
    } while (gen.pieces.empty() || !gen.portalRoomIndex.has_value());

    auto coreDir = [](Dir d) {
        switch (d) {
            case Dir::NORTH: return static_cast<int>(core::Direction::NORTH);
            case Dir::SOUTH: return static_cast<int>(core::Direction::SOUTH);
            case Dir::WEST: return static_cast<int>(core::Direction::WEST);
            case Dir::EAST: return static_cast<int>(core::Direction::EAST);
        }
        return static_cast<int>(core::Direction::NORTH);
    };

    out.pieces.clear();
    out.pieces.reserve(gen.pieces.size());
    out.behaviors.clear();
    out.behaviors.reserve(gen.pieces.size());
    for (const auto& piece : gen.pieces) {
        StructurePieceData data;
        data.pieceType = kindTypeId(piece.kind);
        data.boundingBox = piece.box;
        data.rotation = dirRotationName(piece.orientation);
        data.genDepth = piece.genDepth;
        out.pieces.push_back(std::move(data));
        int orientation = coreDir(piece.orientation);
        switch (piece.kind) {
            case K_START:
            case K_STAIRS_DOWN:
                // Reference: StartPiece extends StairsDown (entryDoor OPENING
                // for the source piece, drawn otherwise) - same postProcess.
                out.behaviors.push_back(
                    PieceBehaviors::strongholdStairsDown(orientation, piece.door));
                break;
            case K_STRAIGHT:
                out.behaviors.push_back(PieceBehaviors::strongholdStraight(
                    orientation, piece.door, piece.a, piece.b));
                break;
            case K_CHEST_CORRIDOR:
                out.behaviors.push_back(
                    PieceBehaviors::strongholdChestCorridor(orientation, piece.door));
                break;
            case K_STRAIGHT_STAIRS_DOWN:
                out.behaviors.push_back(
                    PieceBehaviors::strongholdStraightStairsDown(orientation, piece.door));
                break;
            case K_LEFT_TURN:
                out.behaviors.push_back(
                    PieceBehaviors::strongholdLeftTurn(orientation, piece.door));
                break;
            case K_RIGHT_TURN:
                out.behaviors.push_back(
                    PieceBehaviors::strongholdRightTurn(orientation, piece.door));
                break;
            case K_ROOM_CROSSING:
                out.behaviors.push_back(PieceBehaviors::strongholdRoomCrossing(
                    orientation, piece.door, piece.rcType));
                break;
            case K_PRISON_HALL:
                out.behaviors.push_back(
                    PieceBehaviors::strongholdPrisonHall(orientation, piece.door));
                break;
            case K_FIVE_CROSSING:
                out.behaviors.push_back(PieceBehaviors::strongholdFiveCrossing(
                    orientation, piece.door, piece.a, piece.b, piece.c, piece.d));
                break;
            case K_LIBRARY:
                out.behaviors.push_back(PieceBehaviors::strongholdLibrary(
                    orientation, piece.door, piece.box.getYSpan() > 6));
                break;
            case K_PORTAL_ROOM:
                out.behaviors.push_back(PieceBehaviors::strongholdPortalRoom(orientation));
                break;
            case K_FILLER_CORRIDOR:
                // Reference: FillerCorridor ctor - steps from the box span
                // along the walk axis.
                out.behaviors.push_back(PieceBehaviors::strongholdFillerCorridor(
                    orientation,
                    (piece.orientation == Dir::NORTH || piece.orientation == Dir::SOUTH)
                        ? piece.box.getZSpan() : piece.box.getXSpan()));
                break;
        }
    }
    return !out.pieces.empty();
}

} // namespace StructureLayouts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
