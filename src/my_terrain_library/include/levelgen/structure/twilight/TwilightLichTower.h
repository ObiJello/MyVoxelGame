#pragma once

#include "levelgen/structure/TwilightStructures.h"
#include "levelgen/structure/twilight/TwilightTemplatePieces.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "core/BlockPos.h"
#include "core/Direction.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Twilight Forest 4.9 — the revamped Lich Tower ("twilightforest:lich_tower"):
// type/LichTowerStructure.java + lichtowerrevamp/*.java.
//
//   TwilightLichTower.cpp        piece graph (LichTowerStructure.getFirstPiece,
//                                every piece's processJigsaw, LichTowerSegment.
//                                buildTowerBySegments, LichTowerWingBridge /
//                                WingRoom room-and-bridge logic, the yard:
//                                LichYardBox.beginYard, LichPerimeterFence),
//                                the LandmarkStructure stub sort and the
//                                terraformer (getStructureTerraformer).
//   TwilightLichTowerPieces.cpp  postProcess of every piece: template placement
//                                with LichTowerUtil's processors, the data-marker
//                                handlers (chests with the TF loot tables,
//                                spawners, candles, heads, lecterns, ...), the
//                                yard paths / dirt / graves / lights.
//
// No lich: tower_boss_room's lich_boss_spawner palette entry is placed by the
// template loader as its stand-in marker and never configured.
//
// Piece types (TFStructurePieceTypes, lower-cased):
//   twilightforest:tflttfoy       LichTowerFoyer
//   twilightforest:tfltctbase     LichTowerBase
//   twilightforest:tfltcttrim     LichTowerBaseTrim
//   twilightforest:tfltctseg      LichTowerSegment
//   twilightforest:tfltmobbridge  LichTowerSpawnerBridge
//   twilightforest:tfltbridge     LichTowerWingBridge
//   twilightforest:tflttroof      LichTowerWingRoof
//   twilightforest:tflttbeard     LichTowerWingBeard
//   twilightforest:tflttroom      LichTowerWingRoom
//   twilightforest:tflttdecor     LichTowerRoomDecor
//   twilightforest:tflttgallery   LichTowerMagicGallery
//   twilightforest:tflttfoyd      LichTowerFoyerDecor
//   twilightforest:tflttboss      LichBossRoom
//   twilightforest:tflttbossroof  LichBossRoof
//   twilightforest:tfltfence      LichPerimeterFence
//   twilightforest:tfltpath       LichYardBox
//   twilightforest:tfltgrave      LichYardGrave
//   twilightforest:tfltlight      LichYardLights
//   twilightforest:tfutilitypiece UtilityPiece (fence ladder tree clearance)

namespace minecraft {
namespace levelgen {
namespace structure {
namespace lich_tower {

enum class LichKind {
    Foyer,
    Base,
    BaseTrim,
    Segment,
    SpawnerBridge,
    WingBridge,
    WingRoof,
    WingBeard,
    WingRoom,
    RoomDecor,
    MagicGallery,
    FoyerDecor,
    BossRoom,
    BossRoof,
    PerimeterFence,
    YardBox,
    YardGrave,
    YardLights,
    Utility,
};

/** One lich tower piece as the Java objects hold it during generation. */
struct LichPiece {
    LichKind kind = LichKind::Utility;
    // Jigsaw pieces (TwilightJigsawPiece subclasses): template, context,
    // genDepth; `jigsaw.boundingBox` is the piece box (LichTowerBase's is
    // raised by 30 at construction).
    twilight_template::JigsawPiece jigsaw;
    bool isJigsaw = false;
    // Plain StructurePieces (yard boxes, lights, utility): box + depth.
    BoundingBox plainBox;
    int plainDepth = 0;

    // LichTowerFoyer
    bool putChest = false;
    bool chestSide = false;
    std::vector<core::BlockPos> shelfPositions;   // generation-time only
    // LichTowerBase
    int casketWingIndex = -1;
    // LichTowerSegment
    bool putMobBridge = false;
    bool putWings = false;
    bool putGallery = false;
    // LichTowerSpawnerBridge
    bool invertedPalette = false;
    // LichTowerWingBridge
    bool fromCentral = false;
    // LichTowerWingBeard (isTrim) / LichTowerWingRoom
    bool generateGround = false;
    // LichTowerWingRoom
    int roomSize = 0;
    int ladderIndex = -1;
    std::string jigsawLadderTarget;
    int roofFallback = -1;
    std::vector<int> allowedCeilingPlacements;
    // LichPerimeterFence
    std::optional<core::BlockPos> leashPos;
    // LichYardBox
    float edgeFeatheringRange = 0.0f;
    core::Direction direction = core::Direction::UP;
    bool doDirtMotley = false;
    float scale = 0.0f;
    float offset = 0.0f;
    // LichYardLights
    core::Axis placeAxis = core::Axis::Y;

    const BoundingBox& box() const { return isJigsaw ? jigsaw.boundingBox : plainBox; }
    int genDepth() const { return isJigsaw ? jigsaw.genDepth : plainDepth; }
    const std::string& templateName() const { return jigsaw.templateId; }
    /** SortablePiece.getSortKey (0 for unsortable pieces). */
    int sortKey() const;
    /** PieceBeardifierModifier terrain adjustment != NONE, and its delta. */
    bool beardAdjusts() const;
    int groundLevelDelta() const;
    const char* pieceType() const;
};

/** The postProcess behavior for a finished piece (TwilightLichTowerPieces.cpp). */
std::shared_ptr<StructurePieceBehavior> makeBehavior(const LichPiece& piece);

} // namespace lich_tower
} // namespace structure
} // namespace levelgen
} // namespace minecraft
