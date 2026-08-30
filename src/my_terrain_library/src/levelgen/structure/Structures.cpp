#include "levelgen/structure/Structures.h"

#include "levelgen/structure/StructureLayouts.h"
#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"

#include <algorithm>
#include <functional>
#include <limits>

// Reference: net/minecraft/world/level/levelgen/structure/Structure.java,
// SinglePieceStructure.java, ScatteredFeaturePiece.java, StructurePiece.java,
// structures/{SwampHutStructure, DesertPyramidStructure, JungleTempleStructure,
// BuriedTreasureStructure, BuriedTreasurePieces}.java

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

// Reference: ChunkGenerator.getFirstOccupiedHeight() = getBaseHeight() - 1
int32_t getFirstOccupiedHeight(GenerationContext& ctx, int32_t x, int32_t z,
                               Heightmap::Types type) {
    return ctx.generator->getBaseHeight(x, z, type, ctx.randomState) - 1;
}

// Reference: Structure.getCornerHeights() - WORLD_SURFACE_WG at the 4 corners.
void getCornerHeights(GenerationContext& ctx, int32_t minX, int32_t sizeX,
                      int32_t minZ, int32_t sizeZ, int32_t out[4]) {
    out[0] = getFirstOccupiedHeight(ctx, minX, minZ, Heightmap::Types::WORLD_SURFACE_WG);
    out[1] = getFirstOccupiedHeight(ctx, minX, minZ + sizeZ, Heightmap::Types::WORLD_SURFACE_WG);
    out[2] = getFirstOccupiedHeight(ctx, minX + sizeX, minZ, Heightmap::Types::WORLD_SURFACE_WG);
    out[3] = getFirstOccupiedHeight(ctx, minX + sizeX, minZ + sizeZ, Heightmap::Types::WORLD_SURFACE_WG);
}

// Reference: Structure.getLowestY(context, sizeX, sizeZ) from the chunk min corner.
int32_t getLowestY(GenerationContext& ctx, int32_t sizeX, int32_t sizeZ) {
    int32_t minX = ctx.chunkX * 16;
    int32_t minZ = ctx.chunkZ * 16;
    int32_t corners[4];
    getCornerHeights(ctx, minX, sizeX, minZ, sizeZ, corners);
    return std::min(std::min(corners[0], corners[1]), std::min(corners[2], corners[3]));
}

// Reference: Structure.isValidBiome() - sample at the STUB position quarts.
bool isValidBiome(GenerationContext& ctx, int32_t x, int32_t y, int32_t z) {
    world::biome::BiomeKey biome =
        ctx.biomeSource->getNoiseBiome(x >> 2, y >> 2, z >> 2, *ctx.sampler);
    return ctx.validBiomes->find(biome) != ctx.validBiomes->end();
}

// Reference: StructurePiece.getRandomHorizontalDirection() ->
// Direction.Plane.HORIZONTAL faces = [NORTH, EAST, SOUTH, WEST]; nextInt(4).
enum class Direction4 { NORTH = 0, EAST = 1, SOUTH = 2, WEST = 3 };

Direction4 getRandomHorizontalDirection(LegacyRandomSource& random) {
    return static_cast<Direction4>(random.nextInt(4));
}

// Reference: StructurePiece.setOrientation() rotation mapping:
// SOUTH -> NONE, WEST -> CLOCKWISE_90, EAST -> CLOCKWISE_90, NORTH -> NONE.
const char* rotationName(Direction4 direction) {
    switch (direction) {
        case Direction4::WEST:
        case Direction4::EAST:
            return "CLOCKWISE_90";
        case Direction4::NORTH:
        case Direction4::SOUTH:
            return "NONE";
    }
    return "NONE";
}

// Reference: StructurePiece.makeBoundingBox() - width/depth swap on the X axis
// (EAST/WEST orientations).
BoundingBox makeBoundingBox(int32_t x, int32_t y, int32_t z, Direction4 direction,
                            int32_t width, int32_t height, int32_t depth) {
    bool axisZ = (direction == Direction4::NORTH || direction == Direction4::SOUTH);
    if (axisZ) {
        return BoundingBox(x, y, z, x + width - 1, y + height - 1, z + depth - 1);
    }
    return BoundingBox(x, y, z, x + depth - 1, y + height - 1, z + width - 1);
}

BoundingBox encapsulate(const std::vector<StructurePieceData>& pieces) {
    BoundingBox result(std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(),
                       std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
                       std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min());
    for (const auto& piece : pieces) {
        result.minX = std::min(result.minX, piece.boundingBox.minX);
        result.minY = std::min(result.minY, piece.boundingBox.minY);
        result.minZ = std::min(result.minZ, piece.boundingBox.minZ);
        result.maxX = std::max(result.maxX, piece.boundingBox.maxX);
        result.maxY = std::max(result.maxY, piece.boundingBox.maxY);
        result.maxZ = std::max(result.maxZ, piece.boundingBox.maxZ);
    }
    return result;
}

// Adds one scattered piece (ScatteredFeaturePiece ctor: x=minBlockX, y=64,
// z=minBlockZ, random direction, then setOrientation).
// Direction4 -> core::Direction value (N=2, S=3, W=4, E=5), the orientation
// encoding the placement behaviors take.
int coreDirection(Direction4 direction) {
    switch (direction) {
        case Direction4::NORTH: return static_cast<int>(core::Direction::NORTH);
        case Direction4::EAST: return static_cast<int>(core::Direction::EAST);
        case Direction4::SOUTH: return static_cast<int>(core::Direction::SOUTH);
        case Direction4::WEST: return static_cast<int>(core::Direction::WEST);
    }
    return static_cast<int>(core::Direction::NORTH);
}

StructurePieceData makeScatteredPiece(GenerationContext& ctx, const char* typeId,
                                      int32_t width, int32_t height, int32_t depth,
                                      int* outOrientation = nullptr) {
    Direction4 direction = getRandomHorizontalDirection(ctx.random);
    StructurePieceData piece;
    piece.pieceType = typeId;
    piece.boundingBox = makeBoundingBox(ctx.chunkX * 16, 64, ctx.chunkZ * 16,
                                        direction, width, height, depth);
    piece.rotation = rotationName(direction);
    piece.genDepth = 0;
    if (outOrientation != nullptr) {
        *outOrientation = coreDirection(direction);
    }
    return piece;
}

// Shared flow for onTopOfChunkCenter structures: stub at the chunk middle on
// the given heightmap, biome check there, THEN build pieces (lazy consumer).
bool generateOnTopOfChunkCenter(GenerationContext& ctx, Heightmap::Types heightmap,
                                const std::function<void(std::vector<StructurePieceData>&)>& buildPieces,
                                StructureStartData& out) {
    int32_t blockX = ctx.chunkX * 16 + 8;  // ChunkPos.getMiddleBlockX
    int32_t blockZ = ctx.chunkZ * 16 + 8;
    int32_t blockY = getFirstOccupiedHeight(ctx, blockX, blockZ, heightmap);
    if (!isValidBiome(ctx, blockX, blockY, blockZ)) {
        return false;
    }
    buildPieces(out.pieces);
    return !out.pieces.empty();
}

} // namespace

namespace Structures {

bool isImplemented(const StructureInfo& info) {
    return info.type == "minecraft:buried_treasure"
        || info.type == "minecraft:swamp_hut"
        || info.type == "minecraft:desert_pyramid"
        || info.type == "minecraft:jungle_temple"
        || info.type == "minecraft:mineshaft"
        || info.type == "minecraft:stronghold"
        || info.type == "minecraft:fortress"
        || info.type == "minecraft:nether_fossil"
        || info.type == "minecraft:end_city"
        || info.type == "minecraft:ocean_monument"
        || info.type == "minecraft:igloo"
        || info.type == "minecraft:shipwreck"
        || info.type == "minecraft:ocean_ruin"
        || info.type == "minecraft:ruined_portal"
        || info.type == "minecraft:woodland_mansion"
        || info.type == "minecraft:jigsaw";
}

bool generate(const StructureInfo& info, GenerationContext& ctx,
              int32_t references, StructureStartData& out) {
    out.structureName = info.name;
    out.startChunkPos = world::ChunkPos(ctx.chunkX, ctx.chunkZ);
    out.references = references;
    out.pieces.clear();

    bool built = false;
    if (info.type == "minecraft:buried_treasure") {
        // Reference: BuriedTreasureStructure - OCEAN_FLOOR_WG center; single
        // point piece at (blockX(9), 90, blockZ(9)), no orientation.
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::OCEAN_FLOOR_WG,
            [&](std::vector<StructurePieceData>& pieces) {
                StructurePieceData piece;
                piece.pieceType = "minecraft:btp";
                int32_t x = ctx.chunkX * 16 + 9;
                int32_t z = ctx.chunkZ * 16 + 9;
                piece.boundingBox = BoundingBox(x, 90, z, x, 90, z);
                pieces.push_back(std::move(piece));
            }, out);
        if (built) {
            out.behaviors = {PieceBehaviors::buriedTreasure()};
        }
    } else if (info.type == "minecraft:swamp_hut") {
        // Reference: SwampHutStructure - WORLD_SURFACE_WG center, no lowestY gate.
        int orientation = -1;
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::WORLD_SURFACE_WG,
            [&](std::vector<StructurePieceData>& pieces) {
                pieces.push_back(
                    makeScatteredPiece(ctx, "minecraft:tesh", 7, 7, 9, &orientation));
            }, out);
        if (built) {
            out.behaviors = {PieceBehaviors::swampHut(orientation)};
        }
    } else if (info.type == "minecraft:desert_pyramid") {
        // Reference: SinglePieceStructure(DesertPyramidPiece::new, 21, 21) -
        // reject when the 4-corner lowest WORLD_SURFACE_WG height is below sea level.
        if (getLowestY(ctx, 21, 21) < ctx.generator->getSeaLevel()) return false;
        int orientation = -1;
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::WORLD_SURFACE_WG,
            [&](std::vector<StructurePieceData>& pieces) {
                pieces.push_back(
                    makeScatteredPiece(ctx, "minecraft:tedp", 21, 15, 21, &orientation));
            }, out);
        if (built) {
            out.behaviors = {PieceBehaviors::desertPyramid(orientation, out.afterPlace)};
        }
    } else if (info.type == "minecraft:jungle_temple") {
        // Reference: SinglePieceStructure(JungleTemplePiece::new, 12, 15).
        if (getLowestY(ctx, 12, 15) < ctx.generator->getSeaLevel()) return false;
        int orientation = -1;
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::WORLD_SURFACE_WG,
            [&](std::vector<StructurePieceData>& pieces) {
                pieces.push_back(
                    makeScatteredPiece(ctx, "minecraft:tejp", 12, 10, 15, &orientation));
            }, out);
        if (built) {
            out.behaviors = {PieceBehaviors::jungleTemple(orientation)};
        }
    } else if (info.type == "minecraft:igloo") {
        // Reference: IglooStructure - onTopOfChunkCenter(WORLD_SURFACE_WG),
        // lazy pieces (biome check first, then rotation + lab-branch draws).
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::WORLD_SURFACE_WG,
            [&](std::vector<StructurePieceData>&) {
                StructureLayouts::generateIgloo(info, ctx, out);
            }, out);
    } else if (info.type == "minecraft:shipwreck") {
        // Reference: ShipwreckStructure - is_beached selects the heightmap;
        // structure names: minecraft:shipwreck_beached vs minecraft:shipwreck.
        bool isBeached = (info.name == "minecraft:shipwreck_beached");
        built = generateOnTopOfChunkCenter(
            ctx, isBeached ? Heightmap::Types::WORLD_SURFACE_WG : Heightmap::Types::OCEAN_FLOOR_WG,
            [&](std::vector<StructurePieceData>&) {
                StructureLayouts::generateShipwreck(info, ctx, out, isBeached);
            }, out);
    } else if (info.type == "minecraft:ocean_ruin") {
        // Reference: OceanRuinStructure - onTopOfChunkCenter(OCEAN_FLOOR_WG).
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::OCEAN_FLOOR_WG,
            [&](std::vector<StructurePieceData>&) {
                StructureLayouts::generateOceanRuin(info, ctx, out);
            }, out);
    } else if (info.type == "minecraft:jigsaw") {
        built = StructureLayouts::generateJigsaw(info, ctx, out,
            [&](int x, int y, int z) { return isValidBiome(ctx, x, y, z); });
    } else if (info.type == "minecraft:ruined_portal") {
        built = StructureLayouts::generateRuinedPortal(info, ctx, out,
            [&](int x, int y, int z) { return isValidBiome(ctx, x, y, z); });
    } else if (info.type == "minecraft:woodland_mansion") {
        // Reference: WoodlandMansionStructure.findGenerationPoint.
        int rotation = ctx.random.nextInt(4);  // Rotation.getRandom
        // getLowestYIn5by5BoxOffset7Blocks: offsets flip by rotation
        // (CLOCKWISE_90: x -5; CLOCKWISE_180: both -5; CCW_90: z -5).
        int offsetX = 5, offsetZ = 5;
        if (rotation == 1) offsetX = -5;
        else if (rotation == 2) { offsetX = -5; offsetZ = -5; }
        else if (rotation == 3) offsetZ = -5;
        int blockX = ctx.chunkX * 16 + 7;
        int blockZ = ctx.chunkZ * 16 + 7;
        // getLowestY(context, blockX, blockZ, offsetX, offsetZ) -> corner
        // heights at (minX, minZ), (minX, minZ+sizeZ), (minX+sizeX, minZ),
        // (minX+sizeX, minZ+sizeZ) via WORLD_SURFACE_WG firstOccupied.
        int corners[4];
        getCornerHeights(ctx, blockX, offsetX, blockZ, offsetZ, corners);
        int startY = std::min(std::min(corners[0], corners[1]), std::min(corners[2], corners[3]));
        if (startY < 60) return false;
        if (!isValidBiome(ctx, blockX, startY, blockZ)) return false;
        built = StructureLayouts::generateMansion(info, ctx, out, rotation, blockX, startY, blockZ);
    } else if (info.type == "minecraft:end_city") {
        // Reference: EndCityStructure.findGenerationPoint - same
        // getLowestYIn5by5BoxOffset7Blocks flow as the mansion.
        int rotation = ctx.random.nextInt(4);  // Rotation.getRandom
        int offsetX = 5, offsetZ = 5;
        if (rotation == 1) offsetX = -5;
        else if (rotation == 2) { offsetX = -5; offsetZ = -5; }
        else if (rotation == 3) offsetZ = -5;
        int blockX = ctx.chunkX * 16 + 7;
        int blockZ = ctx.chunkZ * 16 + 7;
        int corners[4];
        getCornerHeights(ctx, blockX, offsetX, blockZ, offsetZ, corners);
        int startY = std::min(std::min(corners[0], corners[1]), std::min(corners[2], corners[3]));
        if (startY < 60) return false;
        if (!isValidBiome(ctx, blockX, startY, blockZ)) return false;
        built = StructureLayouts::generateEndCity(info, ctx, out, rotation, blockX, startY, blockZ);
    } else if (info.type == "minecraft:mineshaft") {
        built = StructureLayouts::generateMineshaft(info, ctx, out,
            [&](int x, int y, int z) { return isValidBiome(ctx, x, y, z); });
    } else if (info.type == "minecraft:stronghold") {
        // Reference: StrongholdStructure - stub at (minBlockX, 0, minBlockZ),
        // LAZY pieces: biome check first, generation after.
        if (!isValidBiome(ctx, ctx.chunkX * 16, 0, ctx.chunkZ * 16)) return false;
        built = StructureLayouts::generateStronghold(info, ctx, out);
    } else if (info.type == "minecraft:nether_fossil") {
        built = StructureLayouts::generateNetherFossil(info, ctx, out,
            [&](int x, int y, int z) { return isValidBiome(ctx, x, y, z); });
    } else if (info.type == "minecraft:fortress") {
        // Reference: NetherFortressStructure - stub at (minBlockX, 64,
        // minBlockZ), LAZY pieces: biome check first, generation after.
        if (!isValidBiome(ctx, ctx.chunkX * 16, 64, ctx.chunkZ * 16)) return false;
        built = StructureLayouts::generateNetherFortress(info, ctx, out);
    } else if (info.type == "minecraft:ocean_monument") {
        // Reference: OceanMonumentStructure.findGenerationPoint - EVERY biome
        // within 29 blocks of (blockX(9), seaLevel, blockZ(9)) must be in
        // #required_ocean_monument_surrounding (draw-free, before the stub).
        {
            int32_t ox = ctx.chunkX * 16 + 9;
            int32_t oz = ctx.chunkZ * 16 + 9;
            int32_t seaLevel = ctx.generator->getSeaLevel();
            const auto& surrounding =
                BiomeTags::resolve("#minecraft:required_ocean_monument_surrounding");
            bool ringOk = true;
            for (int32_t qz = (oz - 29) >> 2; qz <= (oz + 29) >> 2 && ringOk; ++qz) {
                for (int32_t qx = (ox - 29) >> 2; qx <= (ox + 29) >> 2 && ringOk; ++qx) {
                    for (int32_t qy = (seaLevel - 29) >> 2; qy <= (seaLevel + 29) >> 2; ++qy) {
                        if (!surrounding.count(ctx.biomeSource->getNoiseBiome(qx, qy, qz, *ctx.sampler))) {
                            ringOk = false;
                            break;
                        }
                    }
                }
            }
            if (!ringOk) return false;
        }
        // onTopOfChunkCenter(OCEAN_FLOOR_WG); single MonumentBuilding piece at
        // (minBlockX-29, 39, minBlockZ-29), 58x23x58, random direction. The
        // MonumentBuilding ctor consumes the SAME layout random (room graph +
        // fitters + wing designs) - reproduced inside monumentBuilding().
        std::shared_ptr<StructurePieceBehavior> monumentBehavior;
        built = generateOnTopOfChunkCenter(ctx, Heightmap::Types::OCEAN_FLOOR_WG,
            [&](std::vector<StructurePieceData>& pieces) {
                Direction4 direction = getRandomHorizontalDirection(ctx.random);
                StructurePieceData piece;
                piece.pieceType = "minecraft:omb";
                piece.boundingBox = makeBoundingBox(ctx.chunkX * 16 - 29, 39, ctx.chunkZ * 16 - 29,
                                                    direction, 58, 23, 58);
                piece.rotation = rotationName(direction);
                piece.genDepth = 0;
                monumentBehavior = PieceBehaviors::monumentBuilding(
                    coreDirection(direction), piece.boundingBox, ctx.random);
                pieces.push_back(std::move(piece));
            }, out);
        if (built) {
            out.behaviors = {monumentBehavior};
        }
    } else {
        return false;  // not yet implemented (see isImplemented)
    }

    if (!built) return false;
    out.boundingBox = encapsulate(out.pieces);
    // Reference: StructureStart.getBoundingBox() applies
    // Structure.adjustBoundingBox(): inflate by 12 when terrainAdaptation is
    // not NONE (e.g. stronghold "bury"). This inflated box is what Java uses
    // everywhere at start level - S lines AND createReferences intersection.
    if (info.terrainAdaptation != "none") {
        out.boundingBox.minX -= 12; out.boundingBox.minY -= 12; out.boundingBox.minZ -= 12;
        out.boundingBox.maxX += 12; out.boundingBox.maxY += 12; out.boundingBox.maxZ += 12;
    }
    return true;
}

} // namespace Structures

} // namespace structure
} // namespace levelgen
} // namespace minecraft
