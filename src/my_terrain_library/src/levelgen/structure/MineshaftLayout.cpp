#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/ChunkGenerator.h"
#include "core/Direction.h"

#include <optional>
#include <vector>

// Reference: net/minecraft/world/level/levelgen/structure/structures/
// MineshaftStructure.java + MineshaftPieces.java (layout-relevant halves) and
// pieces/StructurePiecesBuilder.java. LAYOUT ONLY - postProcess comes in B6.
//
// RNG draw-order notes (all LegacyRandomSource via GenerationContext.random):
// - findGenerationPoint burns one nextDouble() first.
// - Room bbox: three nextInt(6) draws in Java argument order maxX, maxY, maxZ.
// - Corridor ctor: nextInt(3) for rails, then nextInt(23) ONLY when no rails
//   (Java && short-circuit).
// - Java nested-call arguments draw left-to-right BEFORE the callee runs
//   (e.g. minY - 1 + random.nextInt(3) inside generateAndAddPiece calls).

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

enum class Dir { NORTH, SOUTH, WEST, EAST };

const char* dirRotationName(Dir d) {
    // StructurePiece.setOrientation(): SOUTH/NORTH -> NONE, WEST/EAST -> CLOCKWISE_90.
    return (d == Dir::WEST || d == Dir::EAST) ? "CLOCKWISE_90" : "NONE";
}

// Internal mutable piece graph (converted to StructurePieceData at the end).
struct MPiece {
    int kind;  // 0 room, 1 corridor, 2 crossing, 3 stairs
    BoundingBox box;
    int genDepth;
    std::optional<Dir> orientation;  // corridor/stairs setOrientation; room/crossing none
    // corridor state (kept for B6 block placement parity later)
    bool hasRails = false;
    bool spiderCorridor = false;
    int numSections = 0;
    // crossing state
    Dir crossingDirection = Dir::NORTH;
    bool isTwoFloored = false;
    // room state (Java MineShaftRoom.childEntranceBoxes; moved with the room)
    std::vector<BoundingBox> childEntranceBoxes;
};

struct Builder {
    std::vector<MPiece> pieces;

    const MPiece* findCollisionPiece(const BoundingBox& box) const {
        for (const auto& piece : pieces) {
            if (piece.box.intersects(box)) return &piece;
        }
        return nullptr;
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
        for (auto& piece : pieces) {
            piece.box.move(0, dy, 0);
            // Java MineShaftRoom.move() also moves the entrance boxes.
            for (auto& entrance : piece.childEntranceBoxes) entrance.move(0, dy, 0);
        }
    }

    // Reference: StructurePiecesBuilder.moveBelowSeaLevel()
    int moveBelowSeaLevel(int seaLevel, int minY, LegacyRandomSource& random, int offset) {
        int maxY = seaLevel - offset;
        BoundingBox boundingBox = getBoundingBox();
        int y1Pos = boundingBox.getYSpan() + minY + 1;
        if (y1Pos < maxY) {
            y1Pos += random.nextInt(maxY - y1Pos);
        }
        int dy = y1Pos - boundingBox.maxY;
        offsetPiecesVertically(dy);
        return dy;
    }
};

// The recursion below mirrors Java's virtual addChildren dispatch. Indices are
// used instead of pointers because pieces vector reallocation invalidates refs.
void addChildren(Builder& builder, size_t pieceIndex, LegacyRandomSource& random);

// Reference: MineShaftCorridor.findCorridorSize()
std::optional<BoundingBox> findCorridorSize(const Builder& builder, LegacyRandomSource& random,
                                            int footX, int footY, int footZ, Dir direction) {
    for (int corridorLength = random.nextInt(3) + 2; corridorLength > 0; --corridorLength) {
        int blockLength = corridorLength * 5;
        BoundingBox box;
        switch (direction) {
            case Dir::NORTH: default: box = BoundingBox(0, 0, -(blockLength - 1), 2, 2, 0); break;
            case Dir::SOUTH: box = BoundingBox(0, 0, 0, 2, 2, blockLength - 1); break;
            case Dir::WEST:  box = BoundingBox(-(blockLength - 1), 0, 0, 0, 2, 2); break;
            case Dir::EAST:  box = BoundingBox(0, 0, 0, blockLength - 1, 2, 2); break;
        }
        box.move(footX, footY, footZ);
        if (builder.findCollisionPiece(box) == nullptr) {
            return box;
        }
    }
    return std::nullopt;
}

// Reference: MineShaftCrossing.findCrossing()
std::optional<BoundingBox> findCrossing(const Builder& builder, LegacyRandomSource& random,
                                        int footX, int footY, int footZ, Dir direction) {
    int y1 = (random.nextInt(4) == 0) ? 6 : 2;
    BoundingBox box;
    switch (direction) {
        case Dir::NORTH: default: box = BoundingBox(-1, 0, -4, 3, y1, 0); break;
        case Dir::SOUTH: box = BoundingBox(-1, 0, 0, 3, y1, 4); break;
        case Dir::WEST:  box = BoundingBox(-4, 0, -1, 0, y1, 3); break;
        case Dir::EAST:  box = BoundingBox(0, 0, -1, 4, y1, 3); break;
    }
    box.move(footX, footY, footZ);
    return builder.findCollisionPiece(box) != nullptr ? std::nullopt : std::optional<BoundingBox>(box);
}

// Reference: MineShaftStairs.findStairs()
std::optional<BoundingBox> findStairs(const Builder& builder, LegacyRandomSource& random,
                                      int footX, int footY, int footZ, Dir direction) {
    (void)random;  // Java draws nothing here
    BoundingBox box;
    switch (direction) {
        case Dir::NORTH: default: box = BoundingBox(0, -5, -8, 2, 2, 0); break;
        case Dir::SOUTH: box = BoundingBox(0, -5, 0, 2, 2, 8); break;
        case Dir::WEST:  box = BoundingBox(-8, -5, 0, 0, 2, 2); break;
        case Dir::EAST:  box = BoundingBox(0, -5, 0, 8, 2, 2); break;
    }
    box.move(footX, footY, footZ);
    return builder.findCollisionPiece(box) != nullptr ? std::nullopt : std::optional<BoundingBox>(box);
}

// Reference: MineshaftPieces.createRandomShaftPiece(). Returns index or -1.
int createRandomShaftPiece(Builder& builder, LegacyRandomSource& random,
                           int footX, int footY, int footZ, Dir direction, int genDepth) {
    int randomSelection = random.nextInt(100);
    if (randomSelection >= 80) {
        auto box = findCrossing(builder, random, footX, footY, footZ, direction);
        if (box) {
            MPiece piece;
            piece.kind = 2;
            piece.box = *box;
            piece.genDepth = genDepth;
            piece.crossingDirection = direction;   // stored field, NOT setOrientation
            piece.isTwoFloored = box->getYSpan() > 3;
            builder.pieces.push_back(piece);
            return static_cast<int>(builder.pieces.size()) - 1;
        }
    } else if (randomSelection >= 70) {
        auto box = findStairs(builder, random, footX, footY, footZ, direction);
        if (box) {
            MPiece piece;
            piece.kind = 3;
            piece.box = *box;
            piece.genDepth = genDepth;
            piece.orientation = direction;
            builder.pieces.push_back(piece);
            return static_cast<int>(builder.pieces.size()) - 1;
        }
    } else {
        auto box = findCorridorSize(builder, random, footX, footY, footZ, direction);
        if (box) {
            MPiece piece;
            piece.kind = 1;
            piece.box = *box;
            piece.genDepth = genDepth;
            piece.orientation = direction;
            piece.hasRails = random.nextInt(3) == 0;
            piece.spiderCorridor = !piece.hasRails && random.nextInt(23) == 0;
            piece.numSections = (direction == Dir::NORTH || direction == Dir::SOUTH)
                ? box->getZSpan() / 5 : box->getXSpan() / 5;
            builder.pieces.push_back(piece);
            return static_cast<int>(builder.pieces.size()) - 1;
        }
    }
    return -1;
}

// Reference: MineshaftPieces.generateAndAddPiece(). startPiece is pieces[0].
int generateAndAddPiece(Builder& builder, LegacyRandomSource& random,
                        int footX, int footY, int footZ, Dir direction, int depth) {
    if (depth > 8) return -1;
    const BoundingBox& startBox = builder.pieces.front().box;
    if (std::abs(footX - startBox.minX) > 80 || std::abs(footZ - startBox.minZ) > 80) {
        return -1;
    }
    int newIndex = createRandomShaftPiece(builder, random, footX, footY, footZ, direction, depth + 1);
    if (newIndex >= 0) {
        addChildren(builder, static_cast<size_t>(newIndex), random);
    }
    return newIndex;
}

// Reference: MineShaftRoom.addChildren() - one axis-side loop per direction.
// The nextInt(span) draw happens on every loop entry, even the breaking one.
void addRoomChildren(Builder& builder, size_t roomIndex, LegacyRandomSource& random) {
    BoundingBox roomBox = builder.pieces[roomIndex].box;  // copy: vector may realloc
    int depth = builder.pieces[roomIndex].genDepth;
    int heightSpace = roomBox.getYSpan() - 3 - 1;
    if (heightSpace <= 0) heightSpace = 1;

    // Java records an entrance box per successful child (used by the room's
    // postProcess); the room piece may have moved in the vector - re-index.
    auto recordEntrance = [&builder, roomIndex](const BoundingBox& box) {
        builder.pieces[roomIndex].childEntranceBoxes.push_back(box);
    };

    for (int pos = 0; pos < roomBox.getXSpan(); pos += 4) {
        pos += random.nextInt(roomBox.getXSpan());
        if (pos + 3 > roomBox.getXSpan()) break;
        int child = generateAndAddPiece(builder, random, roomBox.minX + pos,
                            roomBox.minY + random.nextInt(heightSpace) + 1,
                            roomBox.minZ - 1, Dir::NORTH, depth);
        if (child >= 0) {
            const BoundingBox& childBox = builder.pieces[static_cast<size_t>(child)].box;
            recordEntrance(BoundingBox(childBox.minX, childBox.minY, roomBox.minZ,
                                       childBox.maxX, childBox.maxY, roomBox.minZ + 1));
        }
    }
    for (int pos = 0; pos < roomBox.getXSpan(); pos += 4) {
        pos += random.nextInt(roomBox.getXSpan());
        if (pos + 3 > roomBox.getXSpan()) break;
        int child = generateAndAddPiece(builder, random, roomBox.minX + pos,
                            roomBox.minY + random.nextInt(heightSpace) + 1,
                            roomBox.maxZ + 1, Dir::SOUTH, depth);
        if (child >= 0) {
            const BoundingBox& childBox = builder.pieces[static_cast<size_t>(child)].box;
            recordEntrance(BoundingBox(childBox.minX, childBox.minY, roomBox.maxZ - 1,
                                       childBox.maxX, childBox.maxY, roomBox.maxZ));
        }
    }
    for (int pos = 0; pos < roomBox.getZSpan(); pos += 4) {
        pos += random.nextInt(roomBox.getZSpan());
        if (pos + 3 > roomBox.getZSpan()) break;
        int child = generateAndAddPiece(builder, random, roomBox.minX - 1,
                            roomBox.minY + random.nextInt(heightSpace) + 1,
                            roomBox.minZ + pos, Dir::WEST, depth);
        if (child >= 0) {
            const BoundingBox& childBox = builder.pieces[static_cast<size_t>(child)].box;
            recordEntrance(BoundingBox(roomBox.minX, childBox.minY, childBox.minZ,
                                       roomBox.minX + 1, childBox.maxY, childBox.maxZ));
        }
    }
    for (int pos = 0; pos < roomBox.getZSpan(); pos += 4) {
        pos += random.nextInt(roomBox.getZSpan());
        if (pos + 3 > roomBox.getZSpan()) break;
        int child = generateAndAddPiece(builder, random, roomBox.maxX + 1,
                            roomBox.minY + random.nextInt(heightSpace) + 1,
                            roomBox.minZ + pos, Dir::EAST, depth);
        if (child >= 0) {
            const BoundingBox& childBox = builder.pieces[static_cast<size_t>(child)].box;
            recordEntrance(BoundingBox(roomBox.maxX - 1, childBox.minY, childBox.minZ,
                                       roomBox.maxX, childBox.maxY, childBox.maxZ));
        }
    }
}

// Reference: MineShaftCorridor.addChildren()
void addCorridorChildren(Builder& builder, size_t index, LegacyRandomSource& random) {
    BoundingBox box = builder.pieces[index].box;
    int depth = builder.pieces[index].genDepth;
    Dir orientation = *builder.pieces[index].orientation;

    int endSelection = random.nextInt(4);
    switch (orientation) {
        case Dir::NORTH: default:
            if (endSelection <= 1) {
                generateAndAddPiece(builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.minZ - 1, orientation, depth);
            } else if (endSelection == 2) {
                generateAndAddPiece(builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.minZ, Dir::WEST, depth);
            } else {
                generateAndAddPiece(builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.minZ, Dir::EAST, depth);
            }
            break;
        case Dir::SOUTH:
            if (endSelection <= 1) {
                generateAndAddPiece(builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.maxZ + 1, orientation, depth);
            } else if (endSelection == 2) {
                generateAndAddPiece(builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.maxZ - 3, Dir::WEST, depth);
            } else {
                generateAndAddPiece(builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.maxZ - 3, Dir::EAST, depth);
            }
            break;
        case Dir::WEST:
            if (endSelection <= 1) {
                generateAndAddPiece(builder, random, box.minX - 1, box.minY - 1 + random.nextInt(3), box.minZ, orientation, depth);
            } else if (endSelection == 2) {
                generateAndAddPiece(builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.minZ - 1, Dir::NORTH, depth);
            } else {
                generateAndAddPiece(builder, random, box.minX, box.minY - 1 + random.nextInt(3), box.maxZ + 1, Dir::SOUTH, depth);
            }
            break;
        case Dir::EAST:
            if (endSelection <= 1) {
                generateAndAddPiece(builder, random, box.maxX + 1, box.minY - 1 + random.nextInt(3), box.minZ, orientation, depth);
            } else if (endSelection == 2) {
                generateAndAddPiece(builder, random, box.maxX - 3, box.minY - 1 + random.nextInt(3), box.minZ - 1, Dir::NORTH, depth);
            } else {
                generateAndAddPiece(builder, random, box.maxX - 3, box.minY - 1 + random.nextInt(3), box.maxZ + 1, Dir::SOUTH, depth);
            }
            break;
    }

    if (depth < 8) {
        if (orientation != Dir::NORTH && orientation != Dir::SOUTH) {
            for (int x = box.minX + 3; x + 3 <= box.maxX; x += 5) {
                int selection = random.nextInt(5);
                if (selection == 0) {
                    generateAndAddPiece(builder, random, x, box.minY, box.minZ - 1, Dir::NORTH, depth + 1);
                } else if (selection == 1) {
                    generateAndAddPiece(builder, random, x, box.minY, box.maxZ + 1, Dir::SOUTH, depth + 1);
                }
            }
        } else {
            for (int z = box.minZ + 3; z + 3 <= box.maxZ; z += 5) {
                int selection = random.nextInt(5);
                if (selection == 0) {
                    generateAndAddPiece(builder, random, box.minX - 1, box.minY, z, Dir::WEST, depth + 1);
                } else if (selection == 1) {
                    generateAndAddPiece(builder, random, box.maxX + 1, box.minY, z, Dir::EAST, depth + 1);
                }
            }
        }
    }
}

// Reference: MineShaftCrossing.addChildren()
void addCrossingChildren(Builder& builder, size_t index, LegacyRandomSource& random) {
    BoundingBox box = builder.pieces[index].box;
    int depth = builder.pieces[index].genDepth;
    Dir direction = builder.pieces[index].crossingDirection;
    bool isTwoFloored = builder.pieces[index].isTwoFloored;

    switch (direction) {
        case Dir::NORTH: default:
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.minZ - 1, Dir::NORTH, depth);
            generateAndAddPiece(builder, random, box.minX - 1, box.minY, box.minZ + 1, Dir::WEST, depth);
            generateAndAddPiece(builder, random, box.maxX + 1, box.minY, box.minZ + 1, Dir::EAST, depth);
            break;
        case Dir::SOUTH:
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.maxZ + 1, Dir::SOUTH, depth);
            generateAndAddPiece(builder, random, box.minX - 1, box.minY, box.minZ + 1, Dir::WEST, depth);
            generateAndAddPiece(builder, random, box.maxX + 1, box.minY, box.minZ + 1, Dir::EAST, depth);
            break;
        case Dir::WEST:
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.minZ - 1, Dir::NORTH, depth);
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.maxZ + 1, Dir::SOUTH, depth);
            generateAndAddPiece(builder, random, box.minX - 1, box.minY, box.minZ + 1, Dir::WEST, depth);
            break;
        case Dir::EAST:
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.minZ - 1, Dir::NORTH, depth);
            generateAndAddPiece(builder, random, box.minX + 1, box.minY, box.maxZ + 1, Dir::SOUTH, depth);
            generateAndAddPiece(builder, random, box.maxX + 1, box.minY, box.minZ + 1, Dir::EAST, depth);
            break;
    }

    if (isTwoFloored) {
        if (random.nextBoolean()) {
            generateAndAddPiece(builder, random, box.minX + 1, box.minY + 3 + 1, box.minZ - 1, Dir::NORTH, depth);
        }
        if (random.nextBoolean()) {
            generateAndAddPiece(builder, random, box.minX - 1, box.minY + 3 + 1, box.minZ + 1, Dir::WEST, depth);
        }
        if (random.nextBoolean()) {
            generateAndAddPiece(builder, random, box.maxX + 1, box.minY + 3 + 1, box.minZ + 1, Dir::EAST, depth);
        }
        if (random.nextBoolean()) {
            generateAndAddPiece(builder, random, box.minX + 1, box.minY + 3 + 1, box.maxZ + 1, Dir::SOUTH, depth);
        }
    }
}

// Reference: MineShaftStairs.addChildren()
void addStairsChildren(Builder& builder, size_t index, LegacyRandomSource& random) {
    BoundingBox box = builder.pieces[index].box;
    int depth = builder.pieces[index].genDepth;
    Dir orientation = *builder.pieces[index].orientation;

    switch (orientation) {
        case Dir::NORTH: default:
            generateAndAddPiece(builder, random, box.minX, box.minY, box.minZ - 1, Dir::NORTH, depth);
            break;
        case Dir::SOUTH:
            generateAndAddPiece(builder, random, box.minX, box.minY, box.maxZ + 1, Dir::SOUTH, depth);
            break;
        case Dir::WEST:
            generateAndAddPiece(builder, random, box.minX - 1, box.minY, box.minZ, Dir::WEST, depth);
            break;
        case Dir::EAST:
            generateAndAddPiece(builder, random, box.maxX + 1, box.minY, box.minZ, Dir::EAST, depth);
            break;
    }
}

void addChildren(Builder& builder, size_t pieceIndex, LegacyRandomSource& random) {
    switch (builder.pieces[pieceIndex].kind) {
        case 0: addRoomChildren(builder, pieceIndex, random); break;
        case 1: addCorridorChildren(builder, pieceIndex, random); break;
        case 2: addCrossingChildren(builder, pieceIndex, random); break;
        case 3: addStairsChildren(builder, pieceIndex, random); break;
    }
}

const char* pieceTypeId(int kind) {
    switch (kind) {
        case 0: return "minecraft:msroom";
        case 1: return "minecraft:mscorridor";
        case 2: return "minecraft:mscrossing";
        case 3: return "minecraft:msstairs";
    }
    return "?";
}

} // namespace

namespace StructureLayouts {

bool generateMineshaft(const StructureInfo& info, GenerationContext& ctx,
                       StructureStartData& out,
                       const std::function<bool(int x, int y, int z)>& validBiomeAt) {
    // Reference: MineshaftStructure.findGenerationPoint()
    ctx.random.nextDouble();  // burned draw

    Builder builder;

    // MineShaftRoom ctor: BoundingBox args evaluate left-to-right ->
    // maxX draw, then maxY, then maxZ.
    int west = ctx.chunkX * 16 + 2;   // chunkPos.getBlockX(2)
    int north = ctx.chunkZ * 16 + 2;  // chunkPos.getBlockZ(2)
    int maxX = west + 7 + ctx.random.nextInt(6);
    int maxY = 54 + ctx.random.nextInt(6);
    int maxZ = north + 7 + ctx.random.nextInt(6);
    MPiece room;
    room.kind = 0;
    room.box = BoundingBox(west, 50, north, maxX, maxY, maxZ);
    room.genDepth = 0;
    builder.pieces.push_back(room);
    addChildren(builder, 0, ctx.random);

    int seaLevel = ctx.generator->getSeaLevel();
    int yOffset;
    if (info.mineshaftType == "mesa") {
        // Reference: MESA branch - center height probe + randomBetweenInclusive.
        BoundingBox total = builder.getBoundingBox();
        int centerX = total.centerX();
        int centerY = total.centerY();
        int centerZ = total.centerZ();
        int surfaceHeight = ctx.generator->getBaseHeight(centerX, centerZ,
            Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState);
        int targetYForCenter = surfaceHeight <= seaLevel
            ? seaLevel
            : ctx.random.nextInt(surfaceHeight - seaLevel + 1) + seaLevel;  // Mth.randomBetweenInclusive
        yOffset = targetYForCenter - centerY;
        builder.offsetPiecesVertically(yOffset);
    } else {
        yOffset = builder.moveBelowSeaLevel(seaLevel, ctx.generator->getMinY(), ctx.random, 10);
    }

    // Stub position: (middleBlockX, 50, minBlockZ) offset by yOffset; pieces
    // were built EAGERLY (Either.right), so the biome check happens last.
    int stubX = ctx.chunkX * 16 + 8;
    int stubY = 50 + yOffset;
    int stubZ = ctx.chunkZ * 16;
    if (!validBiomeAt(stubX, stubY, stubZ)) {
        return false;
    }

    bool mesa = info.mineshaftType == "mesa";
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
    out.pieces.reserve(builder.pieces.size());
    out.behaviors.clear();
    out.behaviors.reserve(builder.pieces.size());
    for (const auto& piece : builder.pieces) {
        StructurePieceData data;
        data.pieceType = pieceTypeId(piece.kind);
        data.boundingBox = piece.box;
        data.rotation = piece.orientation ? dirRotationName(*piece.orientation) : "-";
        data.genDepth = piece.genDepth;
        out.pieces.push_back(std::move(data));
        switch (piece.kind) {
            case 0:
                out.behaviors.push_back(
                    PieceBehaviors::mineshaftRoom(mesa, piece.childEntranceBoxes));
                break;
            case 1:
                out.behaviors.push_back(PieceBehaviors::mineshaftCorridor(
                    mesa, coreDir(*piece.orientation), piece.hasRails,
                    piece.spiderCorridor, piece.numSections));
                break;
            case 2:
                out.behaviors.push_back(
                    PieceBehaviors::mineshaftCrossing(mesa, piece.isTwoFloored));
                break;
            default:
                out.behaviors.push_back(
                    PieceBehaviors::mineshaftStairs(mesa, coreDir(*piece.orientation)));
                break;
        }
    }
    return !out.pieces.empty();
}

} // namespace StructureLayouts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
