#pragma once

#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/TemplatePool.h"
#include "world/ChunkPos.h"
#include "core/BlockPos.h"
#include <functional>
#include <memory>
#include <vector>

// Per-family StructurePieceBehavior factories (B6 block placement).
// Implementations live in src/levelgen/structure/PieceBehaviors.cpp and
// per-family files as batches land.

namespace minecraft {
class LegacyRandomSource;
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

/** Reference: OceanMonumentPieces.MonumentBuilding - the ctor consumes the
 *  LAYOUT random (room graph, fitters, wing designs); call at layout time
 *  right after the piece bbox is known. orientation = core::Direction. */
std::shared_ptr<StructurePieceBehavior> monumentBuilding(
    int orientation, const BoundingBox& pieceBox, LegacyRandomSource& random);

/** Reference: BuriedTreasurePieces.BuriedTreasurePiece.postProcess. */
std::shared_ptr<StructurePieceBehavior> buriedTreasure();

/** Reference: SwampHutPiece.postProcess. orientation = core::Direction. */
std::shared_ptr<StructurePieceBehavior> swampHut(int orientation);

/** Reference: JungleTemplePiece.postProcess. */
std::shared_ptr<StructurePieceBehavior> jungleTemple(int orientation);

/** Reference: MineshaftPieces - placement behaviors fed by MineshaftLayout.
 *  mesa = MineshaftStructure.Type.MESA; orientation = core::Direction. */
std::shared_ptr<StructurePieceBehavior> mineshaftRoom(
    bool mesa, std::vector<BoundingBox> entrances);
std::shared_ptr<StructurePieceBehavior> mineshaftCorridor(
    bool mesa, int orientation, bool hasRails, bool spiderCorridor, int numSections);
std::shared_ptr<StructurePieceBehavior> mineshaftCrossing(bool mesa, bool isTwoFloored);
std::shared_ptr<StructurePieceBehavior> mineshaftStairs(bool mesa, int orientation);

/** Reference: StrongholdPieces - placement behaviors fed by StrongholdLayout.
 *  orientation = core::Direction; door = SmallDoorType ordinal
 *  (OPENING=0, WOOD_DOOR=1, GRATES=2, IRON_DOOR=3). */
std::shared_ptr<StructurePieceBehavior> strongholdStairsDown(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdStraight(
    int orientation, int door, bool leftChild, bool rightChild);
std::shared_ptr<StructurePieceBehavior> strongholdChestCorridor(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdStraightStairsDown(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdLeftTurn(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdRightTurn(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdRoomCrossing(
    int orientation, int door, int type);
std::shared_ptr<StructurePieceBehavior> strongholdPrisonHall(int orientation, int door);
std::shared_ptr<StructurePieceBehavior> strongholdLibrary(
    int orientation, int door, bool isTall);
std::shared_ptr<StructurePieceBehavior> strongholdFiveCrossing(
    int orientation, int door, bool leftLow, bool leftHigh, bool rightLow, bool rightHigh);
std::shared_ptr<StructurePieceBehavior> strongholdPortalRoom(int orientation);
std::shared_ptr<StructurePieceBehavior> strongholdFillerCorridor(int orientation, int steps);

/** Reference: PoolElementStructurePiece + StructurePoolElement.place - the
 *  element is COPIED (kind/location/processors/projection/listElements);
 *  keepLiquids comes from the structure JSON liquid_settings. */
std::shared_ptr<StructurePieceBehavior> jigsawPiece(
    const PoolElement& element, const core::BlockPos& position, int rotation,
    bool keepLiquids);

/** Reference: WoodlandMansionPieces.WoodlandMansionPiece - rotation/mirror
 *  are Java ordinals; templateName is the SHORT name. */
std::shared_ptr<StructurePieceBehavior> mansionPiece(
    const std::string& templateName, int rotation, int mirror,
    const core::BlockPos& templatePosition);

/** Reference: WoodlandMansionStructure.afterPlace - cobblestone fill below. */
std::function<void(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&,
                   const BoundingBox&, const ::world::ChunkPos&,
                   StructureStartData&)> mansionAfterPlace();

/** Reference: RuinedPortalPiece. */
std::shared_ptr<StructurePieceBehavior> ruinedPortal(
    const std::string& templateId, int rotation, bool mirrorFrontBack,
    const core::BlockPos& pivot, const core::BlockPos& templatePosition,
    const std::string& placement, bool cold, float mossiness, bool airPocket,
    bool overgrown, bool vines, bool replaceWithBlackstone);

/** Reference: OceanRuinPieces.OceanRuinPiece. */
std::shared_ptr<StructurePieceBehavior> oceanRuin(const std::string& templateId, int rotation,
                                                  float integrity, bool warm, bool isLarge,
                                                  const core::BlockPos& templatePosition);

/** Reference: ShipwreckPieces.ShipwreckPiece. */
std::shared_ptr<StructurePieceBehavior> shipwreck(const std::string& templateId, int rotation,
                                                  bool isBeached,
                                                  const core::BlockPos& templatePosition);

/** Reference: IglooPieces.IglooPiece - rotation is the Rotation ordinal. */
std::shared_ptr<StructurePieceBehavior> igloo(const std::string& templateId, int rotation,
                                              const core::BlockPos& pivot,
                                              const core::BlockPos& offset,
                                              const core::BlockPos& templatePosition);

/** Reference: NetherFortressPieces block placement. Keyed by the piece type
 *  registry id; selfSeed = BridgeEndFiller ctor draw; needsChest = the
 *  corridor turn pieces' ctor draw. */
std::shared_ptr<StructurePieceBehavior> fortressPiece(
    const std::string& pieceTypeId, int orientation, int selfSeed, bool needsChest);

/** Reference: NetherFossilPieces.NetherFossilPiece (encapsulated placement +
 *  placeDriedGhast positional roll). */
std::shared_ptr<StructurePieceBehavior> netherFossil(const std::string& templateId,
                                                     int rotation,
                                                     const core::BlockPos& templatePosition);

/** Reference: EndCityPieces.EndCityPiece (overwrite selects the ignore
 *  processor; Chest markers draw the loot seed). */
std::shared_ptr<StructurePieceBehavior> endCity(const std::string& templateId, int rotation,
                                                bool overwrite,
                                                const core::BlockPos& templatePosition);

/**
 * Reference: DesertPyramidPiece.postProcess + DesertPyramidStructure
 * .afterPlace (suspicious sand). The afterPlace closure (sharing the piece
 * instance's sand list and collapsed-roof pos) is returned via outAfterPlace
 * for StructureStartData::afterPlace.
 */
std::shared_ptr<StructurePieceBehavior> desertPyramid(
    int orientation,
    std::function<void(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&,
                       const BoundingBox&, const ::world::ChunkPos&,
                       StructureStartData&)>& outAfterPlace);

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
