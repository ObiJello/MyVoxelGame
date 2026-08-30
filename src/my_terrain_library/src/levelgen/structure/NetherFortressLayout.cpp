#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/ChunkGenerator.h"

#include <optional>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// NetherFortressStructure.java + NetherFortressPieces.java (layout half).
// LAYOUT ONLY - postProcess comes in Part D2.
//
// Generation shape: StartPiece (a BridgeCrossing at (blockX(2), 64, blockZ(2)),
// direction nextInt(4) over [N,E,S,W]) -> start.addChildren -> pendingChildren
// BFS drained via nextInt(size) removal -> moveInsideHeights(random, 48, 70).
//
// Piece selection (generatePiece): up to 5 attempts, each drawing
// nextInt(totalWeight) and walking the weight table SUBTRACTING weights.
// When the selection lands on an entry:
//   - !doPlace(depth) or (entry == previousPiece && !allowInRow) -> BREAK the
//     table walk (attempt wasted, redraw).
//   - factory returns null (box collision / minY <= 10) -> CONTINUE the walk:
//     weightSelection stays negative so every subsequent entry is tried in
//     table order (the stronghold cascade).
// All 5 attempts exhausted (or empty/deep) -> BridgeEndFiller (ctor draws
// nextInt() for its self seed when its box is ok).
//
// generateAndAddPiece range guard: |footX - start.minX| <= 112 AND
// |footZ - start.minZ| <= 112; OUT of range -> BridgeEndFiller.createPiece is
// still CONSTRUCTED (drawing nextInt() if its box passes) but the result is
// DISCARDED (never added to the piece list).
//
// Ctor draws (after box + collision pass): BridgeEndFiller nextInt();
// CastleSmallCorridorRightTurn / LeftTurn nextInt(3) (isNeedingChest);
// BridgeStraight / CastleEntrance take random but draw nothing.

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

// Reference: StructurePiece.makeBoundingBox() (StartPiece at y=64).
BoundingBox makeStartBox(int x, int y, int z, Dir direction, int width, int height, int depth) {
    bool axisZ = (direction == Dir::NORTH || direction == Dir::SOUTH);
    if (axisZ) return BoundingBox(x, y, z, x + width - 1, y + height - 1, z + depth - 1);
    return BoundingBox(x, y, z, x + depth - 1, y + height - 1, z + width - 1);
}

enum Kind {
    K_START, K_BRIDGE_STRAIGHT, K_BRIDGE_CROSSING, K_ROOM_CROSSING, K_STAIRS_ROOM,
    K_MONSTER_THRONE, K_CASTLE_ENTRANCE, K_SMALL_CORRIDOR, K_SMALL_CORRIDOR_CROSSING,
    K_SMALL_CORRIDOR_RIGHT_TURN, K_SMALL_CORRIDOR_LEFT_TURN, K_CORRIDOR_STAIRS,
    K_CORRIDOR_T_BALCONY, K_STALK_ROOM, K_BRIDGE_END_FILLER
};

// Registry ids: lowercased Java short names.
const char* kindTypeId(int kind) {
    switch (kind) {
        // StartPiece chains through the BridgeCrossing(west,north,dir) ctor,
        // which passes NETHER_FORTRESS_BRIDGE_CROSSING - so its runtime type
        // (and P-line id) is nebcr, NOT nestart (that id only appears on NBT
        // load).
        case K_START: return "minecraft:nebcr";
        case K_BRIDGE_STRAIGHT: return "minecraft:nebs";
        case K_BRIDGE_CROSSING: return "minecraft:nebcr";
        case K_ROOM_CROSSING: return "minecraft:nerc";
        case K_STAIRS_ROOM: return "minecraft:nesr";
        case K_MONSTER_THRONE: return "minecraft:nemt";
        case K_CASTLE_ENTRANCE: return "minecraft:nece";
        case K_SMALL_CORRIDOR: return "minecraft:nesc";
        case K_SMALL_CORRIDOR_CROSSING: return "minecraft:nescsc";
        case K_SMALL_CORRIDOR_RIGHT_TURN: return "minecraft:nescrt";
        case K_SMALL_CORRIDOR_LEFT_TURN: return "minecraft:nesclt";
        case K_CORRIDOR_STAIRS: return "minecraft:neccs";
        case K_CORRIDOR_T_BALCONY: return "minecraft:nectb";
        case K_STALK_ROOM: return "minecraft:necsr";
        case K_BRIDGE_END_FILLER: return "minecraft:nebef";
    }
    return "minecraft:unknown";
}

struct FPiece {
    int kind;
    BoundingBox box;
    int genDepth;
    Dir orientation;
    // BridgeEndFiller selfSeed / turn-piece isNeedingChest (D2).
    int selfSeed = 0;
    bool needsChest = false;
};

struct PieceWeightEntry {
    int kind;
    int weight;
    int maxPlaceCount;
    bool allowInRow;
    int placeCount = 0;
    bool doPlace() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
    bool isValid() const { return maxPlaceCount == 0 || placeCount < maxPlaceCount; }
};

struct Generator {
    std::vector<FPiece> pieces;
    std::vector<PieceWeightEntry> bridgePieces;
    std::vector<PieceWeightEntry> castlePieces;
    // previousPiece: PieceWeight object identity; kinds are unique per class.
    std::optional<int> previousPieceKind;
    std::vector<size_t> pendingChildren;

    void reset() {
        pieces.clear();
        pendingChildren.clear();
        previousPieceKind.reset();
        // Reference: BRIDGE_PIECE_WEIGHTS
        bridgePieces = {
            {K_BRIDGE_STRAIGHT, 30, 0, true}, {K_BRIDGE_CROSSING, 10, 4, false},
            {K_ROOM_CROSSING, 10, 4, false}, {K_STAIRS_ROOM, 10, 3, false},
            {K_MONSTER_THRONE, 5, 2, false}, {K_CASTLE_ENTRANCE, 5, 1, false},
        };
        // Reference: CASTLE_PIECE_WEIGHTS
        castlePieces = {
            {K_SMALL_CORRIDOR, 25, 0, true}, {K_SMALL_CORRIDOR_CROSSING, 15, 5, false},
            {K_SMALL_CORRIDOR_RIGHT_TURN, 5, 10, false}, {K_SMALL_CORRIDOR_LEFT_TURN, 5, 10, false},
            {K_CORRIDOR_STAIRS, 10, 3, true}, {K_CORRIDOR_T_BALCONY, 7, 2, false},
            {K_STALK_ROOM, 5, 2, false},
        };
    }

    bool collides(const BoundingBox& box) const {
        for (const auto& piece : pieces) {
            if (piece.box.intersects(box)) return true;
        }
        return false;
    }

    // Reference: NetherBridgePiece.updatePieceWeight()
    int updatePieceWeight(const std::vector<PieceWeightEntry>& currentPieces) const {
        bool hasAnyPieces = false;
        int totalWeight = 0;
        for (const auto& piece : currentPieces) {
            if (piece.maxPlaceCount > 0 && piece.placeCount < piece.maxPlaceCount) {
                hasAnyPieces = true;
            }
            totalWeight += piece.weight;
        }
        return hasAnyPieces ? totalWeight : -1;
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

    // Reference: StructurePiecesBuilder.moveInsideHeights()
    void moveInsideHeights(LegacyRandomSource& random, int lowestAllowed, int highestAllowed) {
        BoundingBox boundingBox = getBoundingBox();
        int heightSpan = highestAllowed - lowestAllowed + 1 - boundingBox.getYSpan();
        int y0Pos;
        if (heightSpan > 1) {
            y0Pos = lowestAllowed + random.nextInt(heightSpan);
        } else {
            y0Pos = lowestAllowed;
        }
        offsetPiecesVertically(y0Pos - boundingBox.minY);
    }
};

bool isOkBox(const BoundingBox& box) { return box.minY > 10; }

// Reference: findAndCreateBridgePieceFactory + the per-class createPiece
// methods. Returns the piece index or -1. Ctor draws happen only after box +
// collision checks pass.
int createPiece(Generator& gen, int kind, LegacyRandomSource& random,
                int footX, int footY, int footZ, Dir direction, int genDepth) {
    auto tryAdd = [&](const BoundingBox& box, bool okCheck) -> int {
        if (okCheck && !isOkBox(box)) return -1;
        if (gen.collides(box)) return -1;
        FPiece piece;
        piece.kind = kind;
        piece.box = box;
        piece.genDepth = genDepth;
        piece.orientation = direction;
        gen.pieces.push_back(piece);
        return static_cast<int>(gen.pieces.size()) - 1;
    };

    switch (kind) {
        case K_BRIDGE_STRAIGHT: {
            // orientBox(-1,-3,0, 5,10,19); ctor takes random, draws nothing.
            return tryAdd(orientBox(footX, footY, footZ, -1, -3, 0, 5, 10, 19, direction), true);
        }
        case K_BRIDGE_CROSSING: {
            return tryAdd(orientBox(footX, footY, footZ, -8, -3, 0, 19, 10, 19, direction), true);
        }
        case K_ROOM_CROSSING: {
            return tryAdd(orientBox(footX, footY, footZ, -2, 0, 0, 7, 9, 7, direction), true);
        }
        case K_STAIRS_ROOM: {
            return tryAdd(orientBox(footX, footY, footZ, -2, 0, 0, 7, 11, 7, direction), true);
        }
        case K_MONSTER_THRONE: {
            return tryAdd(orientBox(footX, footY, footZ, -2, 0, 0, 7, 8, 9, direction), true);
        }
        case K_CASTLE_ENTRANCE: {
            // ctor takes random, draws nothing.
            return tryAdd(orientBox(footX, footY, footZ, -5, -3, 0, 13, 14, 13, direction), true);
        }
        case K_SMALL_CORRIDOR: {
            return tryAdd(orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction), true);
        }
        case K_SMALL_CORRIDOR_CROSSING: {
            return tryAdd(orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction), true);
        }
        case K_SMALL_CORRIDOR_RIGHT_TURN: {
            int index = tryAdd(orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction), true);
            if (index >= 0) {
                gen.pieces[static_cast<size_t>(index)].needsChest = random.nextInt(3) == 0;
            }
            return index;
        }
        case K_SMALL_CORRIDOR_LEFT_TURN: {
            int index = tryAdd(orientBox(footX, footY, footZ, -1, 0, 0, 5, 7, 5, direction), true);
            if (index >= 0) {
                gen.pieces[static_cast<size_t>(index)].needsChest = random.nextInt(3) == 0;
            }
            return index;
        }
        case K_CORRIDOR_STAIRS: {
            return tryAdd(orientBox(footX, footY, footZ, -1, -7, 0, 5, 14, 10, direction), true);
        }
        case K_CORRIDOR_T_BALCONY: {
            return tryAdd(orientBox(footX, footY, footZ, -3, 0, 0, 9, 7, 9, direction), true);
        }
        case K_STALK_ROOM: {
            return tryAdd(orientBox(footX, footY, footZ, -5, -3, 0, 13, 14, 13, direction), true);
        }
        case K_BRIDGE_END_FILLER: {
            int index = tryAdd(orientBox(footX, footY, footZ, -1, -3, 0, 5, 10, 8, direction), true);
            if (index >= 0) {
                // ctor: this.selfSeed = random.nextInt();
                gen.pieces[static_cast<size_t>(index)].selfSeed = random.nextInt();
            }
            return index;
        }
    }
    return -1;
}

// Reference: NetherBridgePiece.generatePiece(). Returns index or -1; the
// BridgeEndFiller fallback is created here (and added by the caller).
int generatePiece(Generator& gen, std::vector<PieceWeightEntry>& currentPieces,
                  LegacyRandomSource& random,
                  int footX, int footY, int footZ, Dir direction, int depth) {
    int totalWeight = gen.updatePieceWeight(currentPieces);
    bool doStuff = totalWeight > 0 && depth <= 30;
    int numAttempts = 0;

    while (numAttempts < 5 && doStuff) {
        ++numAttempts;
        int weightSelection = random.nextInt(totalWeight);

        for (size_t entryIndex = 0; entryIndex < currentPieces.size(); ++entryIndex) {
            PieceWeightEntry& piece = currentPieces[entryIndex];
            weightSelection -= piece.weight;
            if (weightSelection < 0) {
                // BREAK semantics (unlike a successful cascade continue):
                if (!piece.doPlace() ||
                    (gen.previousPieceKind.has_value() &&
                     *gen.previousPieceKind == piece.kind && !piece.allowInRow)) {
                    break;
                }

                int newIndex = createPiece(gen, piece.kind, random,
                                           footX, footY, footZ, direction, depth);
                if (newIndex >= 0) {
                    ++piece.placeCount;
                    gen.previousPieceKind = piece.kind;
                    if (!piece.isValid()) {
                        currentPieces.erase(currentPieces.begin() +
                                            static_cast<long>(entryIndex));
                    }
                    return newIndex;
                }
                // factory null -> CASCADE: weightSelection stays negative,
                // the loop continues to the next entry.
            }
        }
    }

    return createPiece(gen, K_BRIDGE_END_FILLER, random, footX, footY, footZ,
                       direction, depth);
}

// Reference: NetherBridgePiece.generateAndAddPiece().
void generateAndAddPiece(Generator& gen, LegacyRandomSource& random,
                         int footX, int footY, int footZ, Dir direction, int depth,
                         bool isCastle) {
    const BoundingBox& startBox = gen.pieces.front().box;
    if (std::abs(footX - startBox.minX) <= 112 && std::abs(footZ - startBox.minZ) <= 112) {
        int newIndex = generatePiece(gen, isCastle ? gen.castlePieces : gen.bridgePieces,
                                     random, footX, footY, footZ, direction, depth + 1);
        if (newIndex >= 0) {
            gen.pendingChildren.push_back(static_cast<size_t>(newIndex));
        }
    } else {
        // Out of range: the filler is CONSTRUCTED (drawing its self seed when
        // the box passes) but discarded - emulate the draw without keeping it.
        BoundingBox box = orientBox(footX, footY, footZ, -1, -3, 0, 5, 10, 8, direction);
        if (isOkBox(box) && !gen.collides(box)) {
            random.nextInt();
        }
    }
}

// Reference: generateChildForward/Left/Right.
void childForward(Generator& gen, LegacyRandomSource& random, size_t index,
                  int xOff, int yOff, bool isCastle) {
    const FPiece piece = gen.pieces[index];
    const BoundingBox& b = piece.box;
    switch (piece.orientation) {
        case Dir::NORTH:
            generateAndAddPiece(gen, random, b.minX + xOff, b.minY + yOff, b.minZ - 1,
                                Dir::NORTH, piece.genDepth, isCastle);
            break;
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, b.minX + xOff, b.minY + yOff, b.maxZ + 1,
                                Dir::SOUTH, piece.genDepth, isCastle);
            break;
        case Dir::WEST:
            generateAndAddPiece(gen, random, b.minX - 1, b.minY + yOff, b.minZ + xOff,
                                Dir::WEST, piece.genDepth, isCastle);
            break;
        case Dir::EAST:
            generateAndAddPiece(gen, random, b.maxX + 1, b.minY + yOff, b.minZ + xOff,
                                Dir::EAST, piece.genDepth, isCastle);
            break;
    }
}

void childLeft(Generator& gen, LegacyRandomSource& random, size_t index,
               int yOff, int zOff, bool isCastle) {
    const FPiece piece = gen.pieces[index];
    const BoundingBox& b = piece.box;
    switch (piece.orientation) {
        case Dir::NORTH:
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, b.minX - 1, b.minY + yOff, b.minZ + zOff,
                                Dir::WEST, piece.genDepth, isCastle);
            break;
        case Dir::WEST:
        case Dir::EAST:
            generateAndAddPiece(gen, random, b.minX + zOff, b.minY + yOff, b.minZ - 1,
                                Dir::NORTH, piece.genDepth, isCastle);
            break;
    }
}

void childRight(Generator& gen, LegacyRandomSource& random, size_t index,
                int yOff, int zOff, bool isCastle) {
    const FPiece piece = gen.pieces[index];
    const BoundingBox& b = piece.box;
    switch (piece.orientation) {
        case Dir::NORTH:
        case Dir::SOUTH:
            generateAndAddPiece(gen, random, b.maxX + 1, b.minY + yOff, b.minZ + zOff,
                                Dir::EAST, piece.genDepth, isCastle);
            break;
        case Dir::WEST:
        case Dir::EAST:
            generateAndAddPiece(gen, random, b.minX + zOff, b.minY + yOff, b.maxZ + 1,
                                Dir::SOUTH, piece.genDepth, isCastle);
            break;
    }
}

// Reference: the per-class addChildren methods.
void addChildren(Generator& gen, size_t index, LegacyRandomSource& random) {
    switch (gen.pieces[index].kind) {
        case K_START:
        case K_BRIDGE_CROSSING:
            childForward(gen, random, index, 8, 3, false);
            childLeft(gen, random, index, 3, 8, false);
            childRight(gen, random, index, 3, 8, false);
            break;
        case K_BRIDGE_STRAIGHT:
            childForward(gen, random, index, 1, 3, false);
            break;
        case K_ROOM_CROSSING:
            childForward(gen, random, index, 2, 0, false);
            childLeft(gen, random, index, 0, 2, false);
            childRight(gen, random, index, 0, 2, false);
            break;
        case K_STAIRS_ROOM:
            childRight(gen, random, index, 6, 2, false);
            break;
        case K_MONSTER_THRONE:
            // no children
            break;
        case K_CASTLE_ENTRANCE:
            childForward(gen, random, index, 5, 3, true);
            break;
        case K_STALK_ROOM:
            childForward(gen, random, index, 5, 3, true);
            childForward(gen, random, index, 5, 11, true);
            break;
        case K_SMALL_CORRIDOR:
            childForward(gen, random, index, 1, 0, true);
            break;
        case K_SMALL_CORRIDOR_CROSSING:
            childForward(gen, random, index, 1, 0, true);
            childLeft(gen, random, index, 0, 1, true);
            childRight(gen, random, index, 0, 1, true);
            break;
        case K_SMALL_CORRIDOR_RIGHT_TURN:
            childRight(gen, random, index, 0, 1, true);
            break;
        case K_SMALL_CORRIDOR_LEFT_TURN:
            childLeft(gen, random, index, 0, 1, true);
            break;
        case K_CORRIDOR_STAIRS:
            childForward(gen, random, index, 1, 0, true);
            break;
        case K_CORRIDOR_T_BALCONY: {
            // zOff = (orientation W or N) ? 5 : 1; then two draws for the
            // isCastle flags (nextInt(8) > 0), evaluated per call.
            int zOff = 1;
            Dir orientation = gen.pieces[index].orientation;
            if (orientation == Dir::WEST || orientation == Dir::NORTH) {
                zOff = 5;
            }
            bool leftCastle = random.nextInt(8) > 0;
            childLeft(gen, random, index, 0, zOff, leftCastle);
            bool rightCastle = random.nextInt(8) > 0;
            childRight(gen, random, index, 0, zOff, rightCastle);
            break;
        }
        case K_BRIDGE_END_FILLER:
            // no children
            break;
    }
}

}  // namespace

namespace StructureLayouts {

bool generateNetherFortress(const StructureInfo& info, GenerationContext& ctx,
                            StructureStartData& out) {
    (void)info;
    // Reference: NetherFortressStructure.generatePieces(). The stub biome
    // check happened BEFORE this (lazy consumer; caller's duty).
    Generator gen;
    gen.reset();

    // StartPiece(random, chunkPos.getBlockX(2), chunkPos.getBlockZ(2)):
    // direction nextInt(4), BridgeCrossing box (19,10,19) at y=64.
    Dir direction = randomHorizontal(ctx.random);
    FPiece start;
    start.kind = K_START;
    start.box = makeStartBox(ctx.chunkX * 16 + 2, 64, ctx.chunkZ * 16 + 2,
                             direction, 19, 10, 19);
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

    // Reference: builder.moveInsideHeights(context.random(), 48, 70)
    gen.moveInsideHeights(ctx.random, 48, 70);

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
        out.behaviors.push_back(PieceBehaviors::fortressPiece(
            kindTypeId(piece.kind), coreDir(piece.orientation),
            piece.selfSeed, piece.needsChest));
    }
    return true;
}

}  // namespace StructureLayouts

}  // namespace structure
}  // namespace levelgen
}  // namespace minecraft
