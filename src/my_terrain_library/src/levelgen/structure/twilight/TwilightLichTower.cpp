#include "levelgen/structure/twilight/TwilightLichTower.h"

#include "levelgen/structure/twilight/TwilightTemplatePieces.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/Structures.h"
#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "core/BlockPos.h"
#include "core/Direction.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — type/LichTowerStructure.java and the generation half
// of lichtowerrevamp/*.java: LichTowerPieces (pool ids), LichTowerUtil (pool
// rolls), LichTowerFoyer, LichTowerBase, LichTowerBaseTrim, LichTowerSegment,
// LichTowerSpawnerBridge, LichTowerWingBridge, LichTowerWingRoom,
// LichTowerWingRoof, LichTowerWingBeard, LichTowerRoomDecor,
// LichTowerMagicGallery, LichTowerFoyerDecor, LichBossRoom, LichBossRoof,
// LichPerimeterFence, LichYardBox, LichYardGrave, LichYardLights, UtilityPiece.

namespace minecraft {
namespace levelgen {
namespace structure {

namespace lich_tower {

namespace tt = twilight_template;
using core::BlockPos;
using core::Direction;

// ============================================================================
// LichPiece properties
// ============================================================================

int LichPiece::sortKey() const {
    switch (kind) {
        case LichKind::YardBox: return doDirtMotley ? INT_MIN : INT_MIN + 255;
        case LichKind::BaseTrim: return -2;
        case LichKind::WingBeard: return -1;
        case LichKind::Base: return 1;
        case LichKind::RoomDecor: return 2;    // after LichTowerBase
        case LichKind::WingBridge: return 2;
        case LichKind::PerimeterFence: return jigsaw.boundingBox.maxY;
        default: return 0;
    }
}

bool LichPiece::beardAdjusts() const {
    switch (kind) {
        case LichKind::Foyer:
        case LichKind::Base:
        case LichKind::BaseTrim:
        case LichKind::PerimeterFence:
        case LichKind::YardGrave:
            return true;                       // BEARD_BOX
        case LichKind::WingBeard: return generateGround;
        case LichKind::YardBox: return doDirtMotley;
        default: return false;                 // NONE
    }
}

int LichPiece::groundLevelDelta() const {
    switch (kind) {
        case LichKind::Foyer:
        case LichKind::Base:
        case LichKind::BaseTrim:
        case LichKind::YardGrave:
        case LichKind::WingBridge:
            return 1;
        case LichKind::PerimeterFence: return 2;
        case LichKind::WingBeard: return 4;
        default: return 0;
    }
}

const char* LichPiece::pieceType() const {
    switch (kind) {
        case LichKind::Foyer: return "twilightforest:tflttfoy";
        case LichKind::Base: return "twilightforest:tfltctbase";
        case LichKind::BaseTrim: return "twilightforest:tfltcttrim";
        case LichKind::Segment: return "twilightforest:tfltctseg";
        case LichKind::SpawnerBridge: return "twilightforest:tfltmobbridge";
        case LichKind::WingBridge: return "twilightforest:tfltbridge";
        case LichKind::WingRoof: return "twilightforest:tflttroof";
        case LichKind::WingBeard: return "twilightforest:tflttbeard";
        case LichKind::WingRoom: return "twilightforest:tflttroom";
        case LichKind::RoomDecor: return "twilightforest:tflttdecor";
        case LichKind::MagicGallery: return "twilightforest:tflttgallery";
        case LichKind::FoyerDecor: return "twilightforest:tflttfoyd";
        case LichKind::BossRoom: return "twilightforest:tflttboss";
        case LichKind::BossRoof: return "twilightforest:tflttbossroof";
        case LichKind::PerimeterFence: return "twilightforest:tfltfence";
        case LichKind::YardBox: return "twilightforest:tfltpath";
        case LichKind::YardGrave: return "twilightforest:tfltgrave";
        case LichKind::YardLights: return "twilightforest:tfltlight";
        case LichKind::Utility: default: return "twilightforest:tfutilitypiece";
    }
}

namespace {

// ============================================================================
// LichTowerPieces — pool ids.
// ============================================================================
const std::string kP = "twilightforest:lich_tower/";

std::string roomPool(int size) {
    switch (size) {
        case 0: return kP + "3x3";
        case 1: return kP + "5x5";
        case 2: return kP + "7x7";
        case 3: return kP + "9x9";
        default: return "";
    }
}

std::string sizedPool(int size, const char* suffix, int minSize) {
    if (size < minSize || size > 3) return "";
    static const char* const kDirs[4] = {"3x3/", "5x5/", "7x7/", "9x9/"};
    return kP + kDirs[size] + suffix;
}

// LichTowerPieces.ladderPlacements1/2/3.
bool isLadderPlacementForSize(int size, const std::string& target) {
    static const std::set<std::string> k1 = {"twilightforest:ladder_below/0", "twilightforest:ladder_below/2"};
    static const std::set<std::string> k2 = {"twilightforest:ladder_below/0", "twilightforest:ladder_below/1",
                                             "twilightforest:ladder_below/3", "twilightforest:ladder_below/4"};
    static const std::set<std::string> k3 = {"twilightforest:ladder_below/1", "twilightforest:ladder_below/2",
                                             "twilightforest:ladder_below/4", "twilightforest:ladder_below/5"};
    switch (size) {
        case 1: return k1.count(target) != 0;
        case 2: return k2.count(target) != 0;
        case 3: return k3.count(target) != 0;
        default: return false;
    }
}

// LichTowerPieces.ladderRooms: size -> ladder offset -> pool.
std::string ladderRoomPool(int size, int ladderOffset) {
    switch (size) {
        case 1:
            if (ladderOffset == 0) return kP + "5x5/ladder_0";
            if (ladderOffset == 2) return kP + "5x5/ladder_2";
            return "";
        case 2:
            if (ladderOffset == 0) return kP + "7x7/ladder_0";
            if (ladderOffset == 1) return kP + "7x7/ladder_1";
            if (ladderOffset == 3) return kP + "7x7/ladder_3";
            if (ladderOffset == 4) return kP + "7x7/ladder_4";
            return "";
        case 3:
            if (ladderOffset == 1) return kP + "9x9/ladder_1";
            if (ladderOffset == 2) return kP + "9x9/ladder_2";
            if (ladderOffset == 4) return kP + "9x9/ladder_4";
            if (ladderOffset == 5) return kP + "9x9/ladder_5";
            return "";
        default:
            return "";
    }
}

// Java String.split(regex) for a literal single-character delimiter:
// trailing empty strings removed; a string without the delimiter is itself.
std::vector<std::string> javaSplit(const std::string& s, char delim) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t at = s.find(delim, start);
        if (at == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, at - start));
        start = at + 1;
    }
    if (parts.size() == 1) return parts;  // no match: [input]
    while (!parts.empty() && parts.back().empty()) parts.pop_back();
    return parts;
}

// StringUtils.isNumeric: non-empty, all digits.
bool isNumeric(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool isBlank(const std::string& s) {
    for (char c : s) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

int mthCeil(float value) {
    const int i = static_cast<int>(value);
    return value > static_cast<float>(i) ? i + 1 : i;
}

// Direction.fromAxisAndDirection for a horizontal axis.
Direction fromAxisAndDirection(core::Axis axis, bool positive) {
    if (axis == core::Axis::X) return positive ? Direction::EAST : Direction::WEST;
    return positive ? Direction::SOUTH : Direction::NORTH;
}

float mthLerp(float delta, float start, float end) {
    return start + delta * (end - start);
}

// ============================================================================
// Generation state: StructurePiecesBuilder + Structure.GenerationContext.
// ============================================================================
using PiecePtr = std::unique_ptr<LichPiece>;

class LichGen {
public:
    LichGen(GenerationContext& ctx) : m_ctx(ctx), m_random(ctx.random) {}

    std::vector<PiecePtr>& pieces() { return m_pieces; }
    LegacyRandomSource& random() { return m_random; }

    LichPiece* add(PiecePtr piece) {
        m_pieces.push_back(std::move(piece));
        return m_pieces.back().get();
    }

    // StructurePiecesBuilder.findCollisionPiece.
    bool collides(const BoundingBox& box) const {
        for (const PiecePtr& piece : m_pieces) {
            if (piece->box().intersects(box)) return true;
        }
        return false;
    }

    // JigsawPlaceContext.isWithoutCollision with the upward extrusion
    // (Mth.ceil(ySpan * 1.5f)) the room code applies.
    bool withoutCollisionExtruded(const tt::JigsawPlaceContext& context) const {
        const BoundingBox box = context.makeBoundingBox();
        const BoundingBox extruded = tt::extrusionFrom(
            box, Direction::UP, mthCeil(static_cast<float>(box.getYSpan()) * 1.5f));
        return !collides(extruded);
    }

    std::optional<tt::JigsawPlaceContext> pick(const BlockPos& parentTemplatePos, const BlockPos& sourcePos,
                                               const tt::FrontAndTop& orientation,
                                               const std::string& templateLocation,
                                               const std::string& label) {
        return tt::pickPlaceableJunction(parentTemplatePos, sourcePos, orientation, templateLocation,
                                         label, m_random);
    }

    // ------------------------------------------------------------------
    // Piece constructors.
    // ------------------------------------------------------------------
    PiecePtr jigsawPiece(LichKind kind, int genDepth, const std::string& templateId,
                         const tt::JigsawPlaceContext& context) {
        auto piece = std::make_unique<LichPiece>();
        piece->kind = kind;
        piece->isJigsaw = true;
        piece->jigsaw = tt::makeJigsawPiece("", genDepth, templateId, context);
        piece->jigsaw.pieceType = piece->pieceType();
        return piece;
    }

    PiecePtr plainPiece(LichKind kind, int genDepth, const BoundingBox& box) {
        auto piece = std::make_unique<LichPiece>();
        piece->kind = kind;
        piece->isJigsaw = false;
        piece->plainBox = box;
        piece->plainDepth = genDepth;
        return piece;
    }

    // LichTowerBase(manager, context).
    PiecePtr makeBase(const tt::JigsawPlaceContext& context) {
        PiecePtr base = jigsawPiece(LichKind::Base, 1, kP + "tower_base", context);
        base->jigsaw.boundingBox = tt::cloneWithAdjustments(base->jigsaw.boundingBox, 0, 0, 0, 0, 30, 0);
        base->casketWingIndex = base->jigsaw.firstMatchIndex(
            [](const tt::JigsawRecord& r) { return r.target == "twilightforest:lich_tower/bridge"; });
        return base;
    }

    // LichTowerWingRoom(manager, genDepth, context, roomId, roomSize,
    // generateGround, canGenerateLadder, random).
    PiecePtr makeRoom(int genDepth, const tt::JigsawPlaceContext& context, const std::string& roomId,
                      int roomSize, bool generateGround, bool canGenerateLadder) {
        PiecePtr room = jigsawPiece(LichKind::WingRoom, genDepth, roomId, context);
        room->roomSize = roomSize;
        room->generateGround = generateGround;
        const auto& spare = room->jigsaw.spareJigsaws();
        room->ladderIndex = -1;
        if (canGenerateLadder) {
            for (size_t i = 0; i < spare.size(); ++i) {
                if (isLadderPlacementForSize(roomSize, spare[i].target)) {
                    room->ladderIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        room->jigsawLadderTarget = room->ladderIndex >= 0
            ? spare[static_cast<size_t>(room->ladderIndex)].target : std::string();
        room->roofFallback = -1;
        if (canGenerateLadder) {
            for (size_t i = 0; i < spare.size(); ++i) {
                if (spare[i].target == "twilightforest:lich_tower/roof") {
                    room->roofFallback = static_cast<int>(i);
                    break;
                }
            }
        }
        room->allowedCeilingPlacements = generateCeilingPlacements(*room);
        return room;
    }

    // LichPerimeterFence(manager, context, templateId, random).
    PiecePtr makeFence(const tt::JigsawPlaceContext& context, const std::string& templateId) {
        PiecePtr fence = jigsawPiece(LichKind::PerimeterFence, 0, templateId, context);
        std::vector<tt::FilteredBlock> fenceBlocks;
        if (!(m_random.nextFloat() > 0.25)) {
            fenceBlocks = tt::filterBlocks(templateId, BlockPos(0, 0, 0), context.settings(),
                                           "twilightforest:wrought_iron_fence", true);
        }
        if (!fenceBlocks.empty()) {
            fenceBlocks.erase(std::remove_if(fenceBlocks.begin(), fenceBlocks.end(),
                                             [](const tt::FilteredBlock& info) {
                                                 auto it = info.entry->properties.find("post");
                                                 // Palette default is POST.
                                                 const std::string post = it != info.entry->properties.end()
                                                     ? it->second : std::string("post");
                                                 return post != "post";
                                             }),
                              fenceBlocks.end());
            javaShuffle(fenceBlocks);
        }
        if (!fenceBlocks.empty()) {
            fence->leashPos = fence->jigsaw.templatePosition().offset(fenceBlocks.front().pos);
        }
        return fence;
    }

    // ------------------------------------------------------------------
    // TwilightJigsawPiece.addJigsaws (+ overrides).
    // ------------------------------------------------------------------
    void addJigsaws(LichPiece& piece, LichPiece& parent) {
        tt::reseedForJigsaws(m_random, m_ctx.seed, piece.jigsaw.templatePosition());
        // addChildren is the no-op default for every lich piece.
        const std::vector<tt::JigsawRecord> jigsaws = piece.jigsaw.spareJigsaws();
        for (size_t i = 0; i < jigsaws.size(); ++i) {
            processJigsaw(piece, parent, jigsaws[i], static_cast<int>(i));
        }
        if (piece.kind == LichKind::PerimeterFence) {
            // LichPerimeterFence.addJigsaws: the tree-clearance utility piece
            // around the escape ladder of a fence end.
            const Direction ladderDirection = core::getOpposite(piece.jigsaw.sourceJigsaw().orientation.top);
            const BlockPos ladderColumnPos = piece.jigsaw.sourcePosition().relative(ladderDirection, 2).above(2);
            if (piece.jigsaw.spareJigsaws().size() == 1 && !tt::isVertical(ladderDirection)) {
                const BoundingBox box = tt::inflatedBy(
                    BoundingBox(ladderColumnPos.getX(), ladderColumnPos.getY(), ladderColumnPos.getZ(),
                                ladderColumnPos.getX(), ladderColumnPos.getY(), ladderColumnPos.getZ()), 3);
                add(plainPiece(LichKind::Utility, piece.genDepth() + 1, box));
                // UtilityPiece.addChildren: no-op.
            }
        }
    }

    void processJigsaw(LichPiece& piece, LichPiece& parent, const tt::JigsawRecord& connection, int jigsawIndex) {
        switch (piece.kind) {
            case LichKind::Foyer: processFoyer(piece, connection); break;
            case LichKind::Base: processBase(piece, connection, jigsawIndex); break;
            case LichKind::Segment: processSegment(piece, connection, jigsawIndex); break;
            case LichKind::WingRoom: processRoom(piece, parent, connection, jigsawIndex); break;
            case LichKind::MagicGallery: processGallery(piece, connection); break;
            case LichKind::BossRoom: processBossRoom(piece, connection); break;
            default: break;   // BaseTrim, bridges, roofs, beards, decor, fences, graves: no-op
        }
    }

    // ------------------------------------------------------------------
    // LichTowerFoyer.processJigsaw.
    // ------------------------------------------------------------------
    void processFoyer(LichPiece& foyer, const tt::JigsawRecord& connection) {
        const BlockPos templatePos = foyer.jigsaw.templatePosition();
        if (connection.target == "twilightforest:lich_tower/tower_base") {
            auto context = pick(templatePos, connection.pos, connection.orientation, kP + "tower_base",
                                "twilightforest:lich_tower/tower_base");
            if (!context) return;
            LichPiece* base = add(makeBase(*context));
            addJigsaws(*base, foyer);
        } else if (connection.target == "twilightforest:shelf" && m_random.nextFloat() <= 0.5f) {
            auto context = pick(templatePos, connection.pos, connection.orientation, kP + "foyer_decor",
                                "twilightforest:shelf");
            // Don't want to place next to an existing shelf.
            if (!context || hasShelfNeighbor(foyer, connection.pos)) return;
            LichPiece* decor = add(jigsawPiece(LichKind::FoyerDecor, foyer.genDepth() + 1, kP + "foyer_decor", *context));
            addJigsaws(*decor, foyer);
            foyer.shelfPositions.push_back(connection.pos);
        }
    }

    static bool hasShelfNeighbor(const LichPiece& foyer, const BlockPos& pos) {
        for (const BlockPos& occupied : foyer.shelfPositions) {
            if (static_cast<float>(occupied.distManhattan(pos)) < 1.5f) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // LichTowerBase.processJigsaw.
    // ------------------------------------------------------------------
    void processBase(LichPiece& base, const tt::JigsawRecord& connection, int jigsawIndex) {
        const BlockPos templatePos = base.jigsaw.templatePosition();
        if (connection.target == "twilightforest:lich_tower/tower_below") {
            const int segments = tt::nextIntBetweenInclusive(m_random, 12, 15);
            buildTowerBySegments(connection.pos, connection.orientation, base, segments);
        } else if (connection.target == "twilightforest:lich_tower/bridge") {
            std::string room;
            if (jigsawIndex == base.casketWingIndex) {
                room = tt::randomTemplate(m_random, kP + "9x9/special");   // getKeepsakeCasketRoom
            }
            if (!room.empty() || connection.pos.getY() < 6) {
                tryRoomAndBridge(base, connection, true, 4, true, base.genDepth() + 1, room);
            }
        } else if (connection.target == "twilightforest:lich_tower/decor") {
            const std::string decorId = tt::randomTemplate(m_random, kP + "center_decor");
            auto context = pick(templatePos, connection.pos, connection.orientation, decorId,
                                "twilightforest:lich_tower/decor");
            if (context) {
                LichPiece* decor = add(jigsawPiece(LichKind::RoomDecor, base.genDepth() + 1, decorId, *context));
                addJigsaws(*decor, base);
            }
        } else if (connection.target == "twilightforest:lich_tower/tower_trim") {
            const std::string decorId = kP + "central_trim";
            auto context = pick(templatePos, connection.pos, connection.orientation, decorId,
                                "twilightforest:lich_tower/tower_trim");
            if (context) {
                LichPiece* trim = add(jigsawPiece(LichKind::BaseTrim, 1, decorId, *context));
                addJigsaws(*trim, base);
            }
        }
    }

    // ------------------------------------------------------------------
    // LichTowerSegment.buildTowerBySegments.
    // ------------------------------------------------------------------
    PiecePtr makeSegment(int genDepth, const tt::JigsawPlaceContext& context, bool putMobBridge,
                         bool putWings, bool putGallery, const std::string& templateId) {
        PiecePtr segment = jigsawPiece(LichKind::Segment, genDepth, templateId, context);
        segment->putMobBridge = putMobBridge;
        segment->putWings = putWings;
        segment->putGallery = putGallery;
        return segment;
    }

    void buildTowerBySegments(const BlockPos& sourceJigsawPos, const tt::FrontAndTop& sourceOrientation,
                              LichPiece& parentBase, int segments) {
        const std::string segmentId = kP + "tower_slice";
        std::vector<LichPiece*> built;
        LichPiece* priorPiece = &parentBase;
        BlockPos priorJigsawOffset = sourceJigsawPos;
        tt::FrontAndTop priorOrientation = sourceOrientation;
        int mobBridge = tt::nextIntBetweenInclusive(m_random, 0, 5);

        for (int stackIndex = 0; stackIndex < segments; ++stackIndex) {
            auto context = pick(priorPiece->jigsaw.templatePosition(), priorJigsawOffset, priorOrientation,
                                segmentId, "twilightforest:lich_tower/tower_below");
            if (!context) continue;
            const bool putWings = stackIndex > (segments >> 1);
            const bool putGallery = stackIndex == segments - 1;
            LichPiece* segment = add(makeSegment(priorPiece->genDepth() + 1, *context, mobBridge == 0,
                                                 putWings, putGallery, segmentId));
            // Children come later: the tower must reach the boss room before
            // side towers begin from the base upwards.
            built.push_back(segment);
            const tt::JigsawRecord* firstJunction = context->findFirst("twilightforest:lich_tower/tower_above");
            if (firstJunction == nullptr) break;
            priorPiece = segment;
            priorJigsawOffset = firstJunction->pos;
            priorOrientation = firstJunction->orientation;
            mobBridge = mobBridge == 0 ? tt::nextIntBetweenInclusive(m_random, 2, 5) : (mobBridge - 1);
        }

        // The boss room is wider than the segments: adding it sooner helps
        // prevent collisions.
        auto bossRoomJunction = pick(priorPiece->jigsaw.templatePosition(), priorJigsawOffset, priorOrientation,
                                     kP + "tower_boss_room", "twilightforest:lich_tower/tower_below");
        if (bossRoomJunction) {
            LichPiece* bossRoom = add(jigsawPiece(LichKind::BossRoom, 1, kP + "tower_boss_room", *bossRoomJunction));
            addJigsaws(*bossRoom, *priorPiece);
            LichPiece* boundary = add(makeSegment(priorPiece->genDepth() + 1, *bossRoomJunction, false, false,
                                                  false, kP + "tower_boss_boundary"));
            addJigsaws(*boundary, *priorPiece);
        }

        if (built.empty()) return;

        // The topmost segment first, so the Magic Gallery does not compete
        // with side-tower clearances.
        LichPiece* top = built.back();
        built.pop_back();
        addJigsaws(*top, built.empty() ? parentBase : *built.back());

        // Now the rest, bottom up, so the side towers generate.
        LichPiece* prior = &parentBase;
        for (LichPiece* piece : built) {
            addJigsaws(*piece, *prior);
            prior = piece;
        }
    }

    // ------------------------------------------------------------------
    // LichTowerSegment.processJigsaw.
    // ------------------------------------------------------------------
    void processSegment(LichPiece& segment, const tt::JigsawRecord& connection, int jigsawIndex) {
        if (connection.target == "twilightforest:lich_tower/bridge") {
            if (!segment.putWings) return;
            // The top segment places only the gallery, so the normal side
            // towers place lower and generate taller.
            if (segment.putGallery) {
                if (jigsawIndex == 2 && m_random.nextInt(10) == 0) {
                    const std::string galleryId = tt::randomTemplate(m_random, kP + "gallery");
                    tryPlaceGallery(galleryId, connection, segment, segment.genDepth() + 1,
                                    "twilightforest:lich_tower/bridge_center");
                }
            } else {
                tryRoomAndBridge(segment, connection, true, 4, false, segment.genDepth() + 1, "");
            }
        } else if (connection.target == "twilightforest:mob_bridge") {
            if (!segment.putMobBridge) return;
            const tt::FrontAndTop orientation = connection.orientation;
            // Keep the jigsaw rotation or spin it 180 ("flips" a few bridges).
            const tt::FrontAndTop forPlacement = m_random.nextBoolean()
                ? orientation
                : tt::FrontAndTop{orientation.front, core::getOpposite(orientation.top)};
            const std::string mobBridgeLocation = tt::randomTemplate(m_random, kP + "mob_bridge");
            auto context = pick(segment.jigsaw.templatePosition(), connection.pos, forPlacement,
                                mobBridgeLocation, "twilightforest:mob_bridge");
            if (context) {
                PiecePtr bridge = jigsawPiece(LichKind::SpawnerBridge, segment.genDepth() + 1, mobBridgeLocation, *context);
                bridge->invertedPalette = m_random.nextBoolean();
                LichPiece* added = add(std::move(bridge));
                addJigsaws(*added, segment);
            }
        }
    }

    // ------------------------------------------------------------------
    // LichTowerMagicGallery.tryPlaceGallery / processJigsaw.
    // ------------------------------------------------------------------
    void tryPlaceGallery(const std::string& roomId, const tt::JigsawRecord& connection, LichPiece& parent,
                         int newDepth, const std::string& jigsawLabel) {
        auto context = pick(parent.jigsaw.templatePosition(), connection.pos, connection.orientation, roomId,
                            jigsawLabel);
        if (context) {
            LichPiece* gallery = add(jigsawPiece(LichKind::MagicGallery, newDepth, roomId, *context));
            addJigsaws(*gallery, parent);
        }
    }

    void processGallery(LichPiece& gallery, const tt::JigsawRecord& connection) {
        if (connection.target != "twilightforest:lich_tower/roof") return;
        // LichTowerUtil.rollGalleryRoof: odd/even by the narrower span.
        const BoundingBox& box = gallery.box();
        const bool odd = (std::min(box.getXSpan(), box.getZSpan()) & 1) == 1;
        const std::string fallbackRoof = tt::randomTemplate(m_random, kP + (odd ? "gallery_odd" : "gallery_even"));
        const tt::FrontAndTop orientationToMatch = verticalOrientation(connection, Direction::UP, gallery);
        tryRoof(connection, fallbackRoof, orientationToMatch, true, gallery, gallery.genDepth() + 1);
    }

    // ------------------------------------------------------------------
    // LichBossRoom.processJigsaw.
    // ------------------------------------------------------------------
    void processBossRoom(LichPiece& bossRoom, const tt::JigsawRecord& connection) {
        if (connection.target != "twilightforest:lich_tower/tower_below") return;
        auto context = pick(bossRoom.jigsaw.templatePosition(), connection.pos, connection.orientation,
                            kP + "tower_boss_roof", "twilightforest:lich_tower/tower_below");
        if (!context) return;
        LichPiece* roof = add(jigsawPiece(LichKind::BossRoof, 1, kP + "tower_boss_roof", *context));
        addJigsaws(*roof, bossRoom);
    }

    // ------------------------------------------------------------------
    // LichTowerWingBridge.
    // ------------------------------------------------------------------
    void tryRoomAndBridge(LichPiece& parent, const tt::JigsawRecord& connection, bool fromCentralTower,
                          int roomMaxSize, bool generateGround, int newDepth, const std::string& override) {
        if (!generateGround) {
            if (fromCentralTower) {
                for (const std::string& bridgeId : tt::shuffledSequence(m_random, kP + "bridge_from_central")) {
                    if (tryBridge(parent, connection.pos, connection.orientation, true, roomMaxSize, false,
                                  newDepth, bridgeId, true, override, false)) {
                        return;
                    }
                }
            } else {
                for (const std::string& bridgeId : tt::shuffledSequence(m_random, kP + "room_bridge")) {
                    if (tryBridge(parent, connection.pos, connection.orientation, false, roomMaxSize, false,
                                  newDepth, bridgeId, false, override, false)) {
                        return;
                    }
                }
                for (const std::string& bridgeId : tt::shuffledSequence(m_random, kP + "end_bridge")) {
                    if (tryBridge(parent, connection.pos, connection.orientation, false, 0, false, newDepth,
                                  bridgeId, false, override, true)) {
                        return;
                    }
                }
            }
        }
        if (fromCentralTower) {
            const std::string enclosed = tt::randomTemplate(m_random, kP + "bridge_from_central_fallback");
            tryBridge(parent, connection.pos, connection.orientation, true, roomMaxSize, generateGround, newDepth,
                      enclosed, true, override, false);
        } else {
            const std::string direct = tt::randomTemplate(m_random, kP + "room_bridge_fallback");
            if (!tryBridge(parent, connection.pos, connection.orientation, false, roomMaxSize, generateGround,
                           newDepth, direct, true, override, true)) {
                // No room fitted: a wall covers where the bridge would have been.
                putCover(parent, connection.pos, connection.orientation, generateGround, newDepth);
            }
        }
    }

    bool tryBridge(LichPiece& parent, const BlockPos& sourceJigsawPos, const tt::FrontAndTop& sourceOrientation,
                   bool fromCentralTower, int roomMaxSize, bool generateGround, int newDepth,
                   const std::string& bridgeId, bool allowClipping, const std::string& override, bool tiny) {
        auto context = pick(parent.jigsaw.templatePosition(), sourceJigsawPos, sourceOrientation, bridgeId,
                            fromCentralTower ? "twilightforest:lich_tower/bridge_center"
                                             : "twilightforest:lich_tower/bridge");
        if (context) {
            PiecePtr bridge = jigsawPiece(LichKind::WingBridge, newDepth, bridgeId, *context);
            bridge->fromCentral = fromCentralTower;
            if ((allowClipping || !collides(bridge->box()))
                && tryGenerateRoom(*bridge, roomMaxSize, generateGround, override, tiny)) {
                // The bridge & room fit: add the bridge too.
                LichPiece* added = add(std::move(bridge));
                addJigsaws(*added, parent);
                return true;
            }
        }
        return false;
    }

    void putCover(LichPiece& parent, const BlockPos& sourceJigsawPos, const tt::FrontAndTop& sourceOrientation,
                  bool noWindow, int newDepth) {
        const BlockPos parentTemplatePos = parent.jigsaw.templatePosition();
        const BoundingBox clearance = tt::fromCorners(
            parentTemplatePos.offset(sourceJigsawPos.relative(sourceOrientation.front, 1)),
            parentTemplatePos.offset(sourceJigsawPos.relative(sourceOrientation.front, 3)));
        const bool onlyCobbleStopper = noWindow || collides(clearance);
        const std::string coverLocation = onlyCobbleStopper
            ? tt::randomTemplate(m_random, kP + "door_stopper_fallback")
            : tt::randomTemplate(m_random, kP + "door_stopper");
        auto context = pick(parentTemplatePos, sourceJigsawPos, sourceOrientation, coverLocation,
                            "twilightforest:lich_tower/bridge");
        if (context) {
            LichPiece* cover = add(jigsawPiece(LichKind::WingBridge, newDepth, coverLocation, *context));
            cover->fromCentral = false;
            addJigsaws(*cover, parent);
        }
    }

    bool tryGenerateRoom(LichPiece& bridge, int roomMaxSize, bool generateGround, const std::string& override,
                         bool tiny) {
        const std::vector<tt::JigsawRecord> spareJigsaws = bridge.jigsaw.spareJigsaws();
        if (spareJigsaws.empty()) return false;
        if (!override.empty()) {
            return tryPlaceRoom(override, spareJigsaws.front(), 3, generateGround, false, bridge,
                                bridge.genDepth() + 1, "twilightforest:lich_tower/room");
        }
        const int minSize = tiny ? 0 : 1;
        for (const tt::JigsawRecord& generatingPoint : spareJigsaws) {
            for (int roomSize = std::max(0, roomMaxSize - 1); roomSize >= minSize; --roomSize) {
                const std::string roomId = tt::randomTemplate(m_random, roomPool(roomSize));
                if (tryPlaceRoom(roomId, generatingPoint, roomSize, generateGround, false, bridge,
                                 bridge.genDepth() + 1, "twilightforest:lich_tower/room")) {
                    return true;
                }
            }
        }
        return false;
    }

    bool tryPlaceRoom(const std::string& roomId, const tt::JigsawRecord& connection, int roomSize,
                      bool canPutGround, bool allowClipping, LichPiece& parent, int newDepth,
                      const std::string& jigsawLabel) {
        auto context = pick(parent.jigsaw.templatePosition(), connection.pos, connection.orientation, roomId,
                            jigsawLabel);
        if (!context) return false;
        const bool generateGround = canPutGround && connection.pos.getY() < 4;
        const bool doLadder = withoutCollisionExtruded(*context);
        PiecePtr room = makeRoom(newDepth, *context, roomId, roomSize, generateGround, doLadder);
        if (allowClipping || !collides(room->box())) {
            LichPiece* added = add(std::move(room));
            addJigsaws(*added, parent);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    // LichTowerWingRoom.
    // ------------------------------------------------------------------
    // generateCeilingPlacements: x-indexed z offsets of the rope/chain
    // markers kept after removing aligned ones (template-local coordinates,
    // as the mod reads them).
    std::vector<int> generateCeilingPlacements(const LichPiece& room) {
        const BoundingBox& box = room.box();
        const int width = std::max(box.getXSpan(), box.getZSpan());
        if (width <= 0) return {};
        std::vector<tt::FilteredBlock> infos = tt::filterBlocks(
            room.jigsaw.templateId, BlockPos(0, 0, 0), room.jigsaw.context.settings(),
            "minecraft:structure_block", false);
        infos.erase(std::remove_if(infos.begin(), infos.end(), [](const tt::FilteredBlock& info) {
                        if (info.nbt == nullptr || info.nbt->isEmpty()) return false;
                        const std::string metadata = info.nbt->getStringOr("metadata", "");
                        return !(startsWith(metadata, "rope") || startsWith(metadata, "chain"));
                    }),
                    infos.end());
        if (infos.empty()) return {};
        // filterAlignedPieces.
        javaShuffle(infos);
        std::set<int> xDistinct;
        std::set<int> zDistinct;
        for (size_t i = 0; i < infos.size(); ++i) {
            const tt::FilteredBlock& info = infos[i];
            if (info.nbt == nullptr || filterMetadata(*info.nbt)) continue;
            const BlockPos& pos = info.pos;
            if (xDistinct.count(pos.getX()) != 0 || zDistinct.count(pos.getZ()) != 0) {
                infos.erase(infos.begin() + static_cast<std::ptrdiff_t>(i));
                --i;
            } else {
                xDistinct.insert(pos.getX());
                zDistinct.insert(pos.getZ());
            }
        }
        std::vector<int> xIndexedZOffsets(static_cast<size_t>(width), -1);
        for (const tt::FilteredBlock& info : infos) {
            const int x = info.pos.getX();
            if (x >= 0 && x < width) xIndexedZOffsets[static_cast<size_t>(x)] = info.pos.getZ();
        }
        return xIndexedZOffsets;
    }

    // filterMetadata: blank chance keeps; a numeric chance keeps with that
    // percentage (nextFloat drawn only for numeric chances).
    bool filterMetadata(const nbt::CompoundTag& nbt) {
        if (nbt.isEmpty() || !nbt.contains("metadata")) return true;
        // split("%", 1)[0] is the whole string (limit 1).
        const std::string metadata = nbt.getStringOr("metadata", "");
        const std::string chance = startsWith(metadata, "rope") ? metadata.substr(4)
                                 : (metadata.size() >= 5 ? metadata.substr(5) : std::string());
        if (isBlank(chance)) return true;
        return isNumeric(chance) && m_random.nextFloat() > static_cast<float>(std::stoi(chance)) * 0.01f;
    }

    static bool shouldLadderUpwards(const LichPiece& room) { return room.ladderIndex >= 0; }
    static bool hasLadderBelowRoom(const LichPiece& room) {
        return room.jigsaw.sourceJigsaw().orientation.front == Direction::DOWN;
    }
    static int towerStackIndex(const LichPiece& room) {
        const bool hasRoomAbove = shouldLadderUpwards(room);
        const bool hasRoomBelow = hasLadderBelowRoom(room);
        return hasRoomAbove && !hasRoomBelow ? 0 : hasRoomAbove ? 1 : 2;
    }

    static tt::FrontAndTop verticalOrientation(const tt::JigsawRecord& connection, Direction vertical,
                                               const LichPiece& towerRoom) {
        (void)connection;
        const Direction sourceDirection = tt::absoluteHorizontal(towerRoom.jigsaw.sourceJigsaw().orientation);
        return tt::FrontAndTop{vertical, core::getOpposite(sourceDirection)};
    }

    void processRoom(LichPiece& room, LichPiece& parent, const tt::JigsawRecord& connection, int jigsawIndex) {
        if (connection.target == "twilightforest:lich_tower/bridge") {
            if (room.roomSize < 1) return;
            const bool terminate = room.genDepth() > 30
                || m_random.nextInt(towerStackIndex(room) * 2 + 1) == 0;
            const bool tooCloseToGround = room.generateGround
                || (room.box().getYSpan() > 11 && connection.pos.getY() < 7);
            if (terminate || tooCloseToGround) {
                putCover(room, connection.pos, connection.orientation, true, room.genDepth() + 1);
            } else {
                const int maxSize = room.roomSize - m_random.nextInt(2);
                tryRoomAndBridge(room, connection, false, maxSize, false, room.genDepth() + 1, "");
            }
            return;
        }
        if (connection.target == "twilightforest:lich_tower/roof") {
            if (!shouldLadderUpwards(room)) putRoof(room, connection);
            return;
        }
        if (connection.target == "twilightforest:lich_tower/beard") {
            if (hasLadderBelowRoom(room)) {
                // The Beardifier makes ground here, or a ladder enters from below.
                return;
            }
            const tt::FrontAndTop orientationToMatch = verticalOrientation(connection, Direction::DOWN, room);
            if (room.generateGround) {
                const std::string trim = tt::randomTemplate(m_random, sizedPool(room.roomSize, "trim", 1));
                tryBeard(room, connection, trim, orientationToMatch, true, true);
            } else {
                bool placed = false;
                for (const std::string& beardLocation :
                     tt::shuffledSequence(m_random, sizedPool(room.roomSize, "beard", 1))) {
                    if (tryBeard(room, connection, beardLocation, orientationToMatch, false, false)) {
                        placed = true;
                        break;
                    }
                }
                if (placed) return;
                const std::string fallbackBeard = tt::randomTemplate(m_random, sizedPool(room.roomSize, "beard_fallback", 1));
                tryBeard(room, connection, fallbackBeard, orientationToMatch, true, false);
            }
        } else if (connection.target == "twilightforest:lich_tower/decor") {
            addDecor(room, connection, room.genDepth() + 1);
        }

        if (room.ladderIndex == jigsawIndex && room.jigsawLadderTarget == connection.target) {
            const int ladderOffset = room.jigsawLadderTarget.back() - '0';
            std::string roomId = tt::randomTemplate(m_random, ladderRoomPool(room.roomSize, ladderOffset));
            if (!roomId.empty() && (room.templateName() == roomId || parent.templateName() == roomId)) {
                // One re-roll if the template repeats this room or its parent.
                roomId = tt::randomTemplate(m_random, ladderRoomPool(room.roomSize, ladderOffset));
            }
            const BlockPos topPos = connection.pos.offset(0, room.box().getYSpan() - connection.pos.getY() - 1, 0);
            auto context = pick(room.jigsaw.templatePosition(), topPos, connection.orientation, roomId,
                                connection.target);
            if (context) {
                const bool canFitRoomAbove = withoutCollisionExtruded(*context);
                const bool canGenerateLadder = canFitRoomAbove
                    && !tt::isVertical(room.jigsaw.sourceJigsaw().orientation.front)
                    && m_random.nextBoolean();
                PiecePtr above = makeRoom(room.genDepth() + 1, *context, roomId, room.roomSize, false,
                                          canGenerateLadder);
                const BoundingBox shrunk = tt::cloneWithAdjustments(above->box(), 1, 0, 1, -1, 0, -1);
                if (!collides(shrunk)) {
                    LichPiece* added = add(std::move(above));
                    addJigsaws(*added, room);
                    return;
                }
            }
            if (room.roofFallback >= 0) {
                // The room above cannot generate: the roof goes on instead.
                const tt::JigsawRecord roofJigsaw =
                    room.jigsaw.spareJigsaws()[static_cast<size_t>(room.roofFallback)];
                putRoof(room, roofJigsaw);
            }
        }
    }

    bool putRoof(LichPiece& room, const tt::JigsawRecord& connection) {
        const tt::FrontAndTop orientationToMatch = verticalOrientation(connection, Direction::UP, room);
        const BoundingBox& box = room.box();
        const BoundingBox roofExtension = tt::extrusionFrom(
            BoundingBox(box.minX, box.maxY + 1, box.minZ, box.maxX, box.maxY + 1, box.maxZ),
            core::getOpposite(orientationToMatch.top), 1);
        const bool doSideAttachment = !tt::isVertical(connection.orientation.front) && collides(roofExtension);
        const std::string pool = sizedPool(room.roomSize, doSideAttachment ? "side_roof" : "roof", 0);
        for (const std::string& roofLocation : tt::shuffledSequence(m_random, pool)) {
            if (tryRoof(connection, roofLocation, orientationToMatch, false, room, room.genDepth() + 1)) {
                return true;
            }
        }
        // getFallbackRoof: the side fallbacks are the roof_fallback pools.
        const std::string fallbackRoof = tt::randomTemplate(m_random, sizedPool(room.roomSize, "roof_fallback", 0));
        tryRoof(connection, fallbackRoof, orientationToMatch, true, room, room.genDepth() + 1);
        return false;
    }

    // LichTowerWingRoof.generationCollisionBox.
    static BoundingBox roofCollisionBox(const BoundingBox& box) {
        if (box.getXSpan() < 2 || box.getYSpan() < 2 || box.getZSpan() < 2) {
            return tt::safeRetract(box, Direction::DOWN, 1);
        }
        return tt::cloneWithAdjustments(box, 1, 1, 1, -1, 0, -1);
    }

    // LichTowerWingBeard.generationCollisionBox.
    static BoundingBox beardCollisionBox(const BoundingBox& box) {
        if (box.getXSpan() < 2 || box.getYSpan() < 2 || box.getZSpan() < 2) {
            return tt::safeRetract(box, Direction::UP, 1);
        }
        return tt::cloneWithAdjustments(box, 1, 0, 1, -1, -1, -1);
    }

    bool tryRoof(const tt::JigsawRecord& connection, const std::string& roofLocation,
                 const tt::FrontAndTop& orientationToMatch, bool allowClipping, LichPiece& parent, int newDepth) {
        auto context = pick(parent.jigsaw.templatePosition(), connection.pos, orientationToMatch, roofLocation,
                            "twilightforest:lich_tower/roof");
        if (context) {
            PiecePtr roof = jigsawPiece(LichKind::WingRoof, newDepth, roofLocation, *context);
            if (allowClipping || !collides(roofCollisionBox(roof->box()))) {
                LichPiece* added = add(std::move(roof));
                addJigsaws(*added, parent);
                return true;
            }
        }
        return false;
    }

    bool tryBeard(LichPiece& room, const tt::JigsawRecord& connection, const std::string& beardLocation,
                  const tt::FrontAndTop& orientationToMatch, bool allowClipping, bool generateGround) {
        auto context = pick(room.jigsaw.templatePosition(), connection.pos, orientationToMatch, beardLocation,
                            "twilightforest:lich_tower/beard");
        if (context) {
            PiecePtr beard = jigsawPiece(LichKind::WingBeard, room.genDepth() + 1, beardLocation, *context);
            beard->generateGround = generateGround;
            if (allowClipping || !collides(beardCollisionBox(beard->box()))) {
                LichPiece* added = add(std::move(beard));
                addJigsaws(*added, room);
                return true;
            }
        }
        return false;
    }

    // LichTowerRoomDecor.addDecor.
    void addDecor(LichPiece& parent, const tt::JigsawRecord& connection, int newDepth) {
        const std::string decorId = tt::randomTemplate(m_random, kP + "room_decor");
        auto context = pick(parent.jigsaw.templatePosition(), connection.pos, connection.orientation, decorId,
                            "twilightforest:lich_tower/decor");
        if (context) {
            LichPiece* decor = add(jigsawPiece(LichKind::RoomDecor, newDepth, decorId, *context));
            addJigsaws(*decor, parent);
        }
    }

    // ------------------------------------------------------------------
    // The yard: LichYardBox.beginYard / generateYard / addDecoration and
    // LichPerimeterFence.generateFence.
    // ------------------------------------------------------------------
    void beginYard(LichPiece& foyer) {
        const int ySurface = foyer.box().minY + foyer.groundLevelDelta();
        const std::vector<tt::JigsawRecord> paths = foyer.jigsaw.matchSpareJigsaws(
            [](const tt::JigsawRecord& r) { return r.target == "twilightforest:lich_tower/path"; });
        if (paths.empty()) return;
        const tt::JigsawRecord path = paths.front();
        const int pathLength = 24 + m_random.nextInt(32 - 24);  // nextInt(24, 32)
        const Direction direction = tt::rotate(foyer.jigsaw.rotation(), Direction::SOUTH);
        const BlockPos generatePos = foyer.jigsaw.templatePosition().offset(path.pos);
        const BlockPos fenceCenter = generatePos.relative(direction, pathLength);
        generateFence(foyer, direction, fenceCenter.atY(ySurface));
        const BlockPos nearVestibule = generatePos.relative(direction, 1).above(4);
        const BlockPos nearFence = fenceCenter.relative(core::getOpposite(direction), 6).below(4);
        generateYard(foyer, nearVestibule, nearFence, direction);

        // The dirt yard encloses the foyer and every fence post.
        std::vector<BlockPos> positions;
        positions.push_back(tt::center(foyer.box()).above(10));
        positions.push_back(tt::bottomCenterOf(foyer.box()).below(10));
        for (const PiecePtr& piece : m_pieces) {
            if (piece->kind != LichKind::PerimeterFence) continue;
            for (const BlockPos& post : fencePostPositions(*piece)) positions.push_back(post);
        }
        BoundingBox fullYard(positions.front().getX(), positions.front().getY(), positions.front().getZ(),
                             positions.front().getX(), positions.front().getY(), positions.front().getZ());
        for (const BlockPos& pos : positions) {
            fullYard.minX = std::min(fullYard.minX, pos.getX());
            fullYard.minY = std::min(fullYard.minY, pos.getY());
            fullYard.minZ = std::min(fullYard.minZ, pos.getZ());
            fullYard.maxX = std::max(fullYard.maxX, pos.getX());
            fullYard.maxY = std::max(fullYard.maxY, pos.getY());
            fullYard.maxZ = std::max(fullYard.maxZ, pos.getZ());
        }
        const BoundingBox yardBox = tt::safeRetract(
            tt::setY(tt::inflatedBy(fullYard, 3), ySurface, ySurface + 10),
            core::getOpposite(foyer.jigsaw.sourceJigsaw().orientation.top), 5);
        LichPiece* dirt = add(makeYardBox(yardBox, 8.0f, Direction::UP, true, 0.1f, 0.0f));
        addDecoration(*dirt, foyer);
    }

    PiecePtr makeYardBox(const BoundingBox& box, float feather, Direction direction, bool dirtMotley,
                         float scale, float offset) {
        PiecePtr yard = plainPiece(LichKind::YardBox, 0, box);
        yard->edgeFeatheringRange = feather;
        yard->direction = direction;
        yard->doDirtMotley = dirtMotley;
        yard->scale = scale;
        yard->offset = offset;
        return yard;
    }

    void generateYard(LichPiece& foyer, const BlockPos& nearVestibule, const BlockPos& nearFence,
                      Direction dirFromVestibule) {
        const int ySurface = foyer.box().minY + foyer.groundLevelDelta();
        std::vector<LichPiece*> paths;

        // First path, from the vestibule.
        const BoundingBox firstPathBox = tt::setY(
            tt::inflatedBy(tt::wrappedCoordinates(3, nearVestibule, nearFence), 1), ySurface, ySurface + 10);
        const core::Axis axisFromVestibule = core::getAxis(dirFromVestibule);
        paths.push_back(add(makeYardBox(firstPathBox, 2.5f, dirFromVestibule, false, 0.35f, -1.0f)));

        // Second path, crossing the first.
        const float delta = mthLerp(m_random.nextFloat(), 0.2f, 0.8f);
        const BlockPos randomPos(tt::lerpDiscrete(delta, nearVestibule.getX(), nearFence.getX()),
                                 tt::lerpDiscrete(delta, nearVestibule.getY(), nearFence.getY()),
                                 tt::lerpDiscrete(delta, nearVestibule.getZ(), nearFence.getZ()));
        const int crossPathSpan = 24;
        const BlockPos pathLeft = randomPos.relative(tt::clockWise(dirFromVestibule), crossPathSpan);
        const BlockPos pathRight = randomPos.relative(tt::counterClockWise(dirFromVestibule), crossPathSpan);
        const BoundingBox crossPathBox = tt::setY(tt::wrappedCoordinates(1, pathLeft, pathRight), ySurface, ySurface + 10);
        paths.push_back(add(makeYardBox(crossPathBox, -1.0f, tt::clockWise(dirFromVestibule), false, 0.0f, 0.0f)));

        // Last two paths, to the sides of the vestibule.
        paths.push_back(putSidePath(nearVestibule.atY(ySurface), dirFromVestibule,
                                    tt::clockWise(dirFromVestibule), pathLeft, crossPathSpan));
        paths.push_back(putSidePath(nearVestibule.atY(ySurface), dirFromVestibule,
                                    tt::counterClockWise(dirFromVestibule), pathRight, crossPathSpan));

        // Lights before the graves.
        const BoundingBox boxLightPlace = tt::inflatedBy(firstPathBox,
                                                         axisFromVestibule == core::Axis::Z ? 3 : 0, 0,
                                                         axisFromVestibule == core::Axis::X ? 3 : 0);
        PiecePtr lights = plainPiece(LichKind::YardLights, 0, boxLightPlace);
        lights->placeAxis = axisFromVestibule;
        add(std::move(lights));

        // All paths exist: now the graves.
        for (LichPiece* piece : paths) addDecoration(*piece, foyer);
    }

    LichPiece* putSidePath(const BlockPos& nearVestibule, Direction dirFromVestibule, Direction sideDirection,
                           const BlockPos& pathEnd, int spread) {
        const BlockPos fromVestibule = nearVestibule.relative(sideDirection, 24);
        const BoundingBox pathBox = tt::setY(
            tt::wrappedCoordinates(1, pathEnd, fromVestibule.relative(core::getOpposite(dirFromVestibule), spread)),
            nearVestibule.getY(), nearVestibule.getY() + 10);
        return add(makeYardBox(pathBox, -1.0f, dirFromVestibule, false, 0.0f, 0.0f));
    }

    // LichYardBox.addDecoration: graves along the paths.
    void addDecoration(LichPiece& yard, LichPiece& parent) {
        const core::Axis axis = core::getAxis(yard.direction);
        if (axis == core::Axis::Y || yard.scale != 0.0f) return;
        const int baseY = yard.box().minY;
        for (int i = 0; i < 5; ++i) {
            const Direction side = tt::clockWise(fromAxisAndDirection(axis, !m_random.nextBoolean()));
            const BlockPos lerped = tt::lerpPosInside(yard.box(), axis, mthLerp(m_random.nextFloat(), 0.05f, 0.95f));
            const BlockPos randomPos = lerped.relative(side, tt::nextIntBetweenInclusive(m_random, 2, 4));
            const tt::FrontAndTop orientation{side, Direction::UP};
            const std::string templateId = tt::randomTemplate(m_random, kP + "grave");
            auto context = pick(randomPos.atY(baseY - 1), BlockPos(0, 0, 0), orientation, templateId,
                                "twilightforest:lich_tower/grave");
            if (!context) continue;
            PiecePtr grave = jigsawPiece(LichKind::YardGrave, 0, templateId, *context);
            if (!collides(grave->box())) {
                LichPiece* added = add(std::move(grave));
                addJigsaws(*added, parent);
            }
        }
    }

    // LichPerimeterFence helpers.
    static std::vector<tt::JigsawRecord> leftJunctions(const LichPiece& fence) {
        return fence.jigsaw.matchSpareJigsaws(
            [](const tt::JigsawRecord& r) { return r.name == "twilightforest:lich_tower/fence_edge_left"; });
    }
    static std::vector<tt::JigsawRecord> rightJunctions(const LichPiece& fence) {
        return fence.jigsaw.matchSpareJigsaws(
            [](const tt::JigsawRecord& r) { return r.name == "twilightforest:lich_tower/fence_edge_right"; });
    }
    static std::vector<BlockPos> fencePostPositions(const LichPiece& fence) {
        std::vector<BlockPos> result;
        for (const auto& r : leftJunctions(fence)) result.push_back(fence.jigsaw.templatePosition().offset(r.pos));
        for (const auto& r : rightJunctions(fence)) result.push_back(fence.jigsaw.templatePosition().offset(r.pos));
        return result;
    }

    using JunctionGetter = std::vector<tt::JigsawRecord> (*)(const LichPiece&);

    void generateFence(LichPiece& vestibule, Direction direction, const BlockPos& fenceCenter) {
        LichPiece* frontFence = startPerimeterFence(vestibule, direction, fenceCenter);
        if (frontFence == nullptr) return;
        const LichPiece* base = nullptr;
        for (const PiecePtr& piece : m_pieces) {
            if (piece->kind == LichKind::Base) {
                base = piece.get();
                break;
            }
        }
        if (base == nullptr) return;
        const BlockPos baseBottomCenter = tt::bottomCenterOf(base->box());
        const Direction sourceJigsawFront = base->jigsaw.sourceJigsaw().orientation.front;
        const std::optional<BoundingBox> leftDest =
            closestTrimOnGround(baseBottomCenter.relative(tt::clockWise(sourceJigsawFront), 64));
        const std::optional<BoundingBox> rightDest =
            closestTrimOnGround(baseBottomCenter.relative(tt::counterClockWise(sourceJigsawFront), 64));
        if (!leftDest || !rightDest) return;
        generatePerimeter(*frontFence, tt::inflatedBy(*leftDest, -1), tt::inflatedBy(*rightDest, -1));
    }

    std::optional<BoundingBox> closestTrimOnGround(const BlockPos& pos) const {
        const LichPiece* closest = nullptr;
        float minDist = std::numeric_limits<float>::max();
        for (const PiecePtr& piece : m_pieces) {
            if (piece->kind != LichKind::WingBeard || !piece->generateGround) continue;
            const BlockPos bottomCenter = tt::bottomCenterOf(piece->box());
            const int dX = pos.getX() - bottomCenter.getX();
            const int dZ = pos.getZ() - bottomCenter.getZ();
            const float dist = Mth::sqrt(static_cast<float>(dX * dX + dZ * dZ));
            if (dist < minDist) {
                minDist = dist;
                closest = piece.get();
            }
        }
        if (closest == nullptr) return std::nullopt;
        return closest->box();
    }

    LichPiece* startPerimeterFence(LichPiece& vestibule, Direction direction, const BlockPos& fenceCenter) {
        const tt::FrontAndTop orientation{Direction::UP, direction};
        const int baseY = fenceCenter.getY();
        const std::string templateId = kP + "outer_fence_7";
        auto context = pick(fenceCenter.atY(baseY - 2), BlockPos(0, 0, 0), orientation, templateId,
                            "twilightforest:lich_tower/fence_source");
        if (!context) return nullptr;
        LichPiece* fence = add(makeFence(*context, templateId));
        addJigsaws(*fence, vestibule);
        return fence;
    }

    void generatePerimeter(LichPiece& frontFence, const BoundingBox& leftDest, const BoundingBox& rightDest) {
        const std::string fullFenceId = kP + "outer_fence_7";
        generateSidedPerimeter(frontFence, fullFenceId, leftDest, &leftJunctions, tt::ROT_CW90);
        generateSidedPerimeter(frontFence, fullFenceId, rightDest, &rightJunctions, tt::ROT_CCW90);
    }

    void generateSidedPerimeter(LichPiece& frontFence, const std::string& fullFenceId, const BoundingBox& destination,
                                JunctionGetter junctionGetter, int rotation) {
        LichPiece* fence = nextFence(frontFence, junctionGetter(frontFence), tt::ROT_NONE, fullFenceId, destination);
        if (fence == nullptr) return;
        fence = generateUntilNearDest(destination, 4, fence, rotation, junctionGetter, fullFenceId);
        // (Java reads the junctions before its null test and would throw on
        // a null fence; a failed chain simply ends here.)
        if (fence == nullptr) return;
        const std::vector<tt::JigsawRecord> fenceJunctions = junctionGetter(*fence);
        if (fenceJunctions.empty()) return;
        // Generate until collision.
        const tt::JigsawRecord first = fenceJunctions.front();
        const BlockPos fencePostPos = fence->jigsaw.templatePosition().offset(first.pos);
        for (int distance = std::min(tt::greatestAxalDistance(destination, fencePostPos) + 1, 32); distance > 2;) {
            const int stepSize = std::min(distance, 7);
            distance -= stepSize;
            fence = nextFence(*fence, junctionGetter(*fence), rotation, kP + "outer_fence_" + std::to_string(stepSize),
                              destination);
            if (fence == nullptr) return;
            rotation = tt::ROT_NONE;
        }
    }

    LichPiece* generateUntilNearDest(const BoundingBox& destBox, int turnAtIndex, LichPiece* fence, int turn,
                                     JunctionGetter junctionGetter, const std::string& templateId) {
        int infoldedPieces = 0;
        int counterRotatedPieces = 0;
        bool foldNext = false;
        const int foldAt = m_random.nextInt(turnAtIndex - 1);
        const int maximumPosts = 16;
        for (int idx = 0; idx < maximumPosts; ++idx) {
            const bool makeTurn = idx == turnAtIndex;
            const bool marchTowardsDest = idx > turnAtIndex;
            if (fence == nullptr) break;
            const std::vector<tt::JigsawRecord> junctions = junctionGetter(*fence);
            if (junctions.empty()) break;
            if (marchTowardsDest) {
                // Does the spare fence post align with the side tower?
                const tt::JigsawRecord& first = junctions.front();
                const BlockPos checkPos = fence->jigsaw.templatePosition().offset(first.pos);
                const BlockPos destPos = tt::clampedInside(destBox, checkPos);
                const int offX = destPos.getX() - checkPos.getX();
                const int offZ = destPos.getZ() - checkPos.getZ();
                const Direction target = tt::rotate(turn, first.orientation.top);
                if (offX * core::getStepX(target) + offZ * core::getStepZ(target) == 0) break;
            } else if (idx == foldAt) {
                infoldedPieces = tt::nextIntBetweenInclusive(m_random, 1, idx + 1);
                counterRotatedPieces = turnAtIndex - infoldedPieces;
                turnAtIndex += infoldedPieces;
                foldNext = true;
            }
            if (infoldedPieces > 0) {
                const int nextTurn = foldNext ? turn : tt::ROT_NONE;
                foldNext = false;
                fence = nextFence(*fence, junctions, nextTurn, templateId, destBox);
                --infoldedPieces;
                if (infoldedPieces == 0) foldNext = true;
            } else if (counterRotatedPieces > 0) {
                const int nextTurn = foldNext ? tt::rotated(tt::ROT_CW180, turn) : (makeTurn ? turn : tt::ROT_NONE);
                foldNext = false;
                fence = nextFence(*fence, junctions, nextTurn, templateId, destBox);
                --counterRotatedPieces;
            } else {
                const int nextTurn = makeTurn ? turn : tt::ROT_NONE;
                fence = nextFence(*fence, junctions, nextTurn, templateId, destBox);
            }
        }
        return fence;
    }

    LichPiece* nextFence(LichPiece& parentFence, const std::vector<tt::JigsawRecord>& junctions, int rotation,
                         const std::string& templateId, const BoundingBox& destination) {
        if (junctions.empty()) return nullptr;
        const tt::JigsawRecord& junction = junctions.front();
        const tt::FrontAndTop orientation = junction.orientation;
        const tt::FrontAndTop connectOrientation{core::getOpposite(orientation.front),
                                                 tt::rotate(rotation, orientation.top)};
        const BlockPos parentTemplatePos = parentFence.jigsaw.templatePosition();
        const BlockPos postPos = parentTemplatePos.offset(junction.pos);
        const int horizontalAxalDistance = tt::horizontalManhattanDistance(destination, postPos);
        int dY = 0;
        if (horizontalAxalDistance <= 20) {
            // Home onto the fence's height near the trim (Mth.clamp:
            // min(max(value, min), max)).
            const int clamped = std::min(std::max(parentTemplatePos.getY() - 1, destination.minY + 2),
                                         destination.maxY - 2);
            dY = clamped - 1 - parentTemplatePos.getY();
        }
        const int sign = dY > 0 ? 1 : (dY < 0 ? -1 : 0);
        const BlockPos parentPos = parentTemplatePos.above(sign - 1);
        auto context = pick(parentPos, junction.pos, connectOrientation, templateId, junction.target);
        if (!context) return nullptr;
        LichPiece* next = add(makeFence(*context, templateId));
        addJigsaws(*next, parentFence);
        return next;
    }

    template <typename T>
    void javaShuffle(std::vector<T>& list) {
        for (size_t i = list.size(); i > 1; --i) {
            const size_t swapTo = static_cast<size_t>(m_random.nextInt(static_cast<int32_t>(i)));
            std::swap(list[i - 1], list[swapTo]);
        }
    }

private:
    GenerationContext& m_ctx;
    LegacyRandomSource& m_random;
    std::vector<PiecePtr> m_pieces;
};

// BoxDensityFunction (BURY, clamped to [-4, 4]).
struct BuryBox {
    int minX, minY, minZ, maxX, maxY, maxZ;
    double compute(int x, int y, int z) const {
        const int xDist = std::max(0, std::max(minX - x, x - maxX));
        const int zDist = std::max(0, std::max(minZ - z, z - maxZ));
        const int yDist = y - minY;   // BURY: distAboveBottom
        const double length = std::sqrt(static_cast<double>(xDist) * xDist
                                        + (yDist * 0.5) * (yDist * 0.5)
                                        + static_cast<double>(zDist) * zDist);
        const double density = Mth::clampedMap(length, 0.0, 6.0, 1.0, 0.0);  // getBuryContribution
        return std::clamp(density, -4.0, 4.0);
    }
};

} // namespace

} // namespace lich_tower

namespace twilight_pieces {

namespace tt = twilight_template;
using lich_tower::LichKind;
using lich_tower::LichPiece;

bool buildLichTower(const StructureInfo& info, GenerationContext& ctx,
                    LegacyRandomSource& firstPieceRandom,
                    int32_t x, int32_t y, int32_t z, StructureStartData& out) {
    (void)info;
    (void)y;
    // LichTowerStructure.adjustForTerrain: WorldUtil.adjustForTerrain(
    // context, x, z, 32, 4) (replaces the dispatcher's generic height).
    const int32_t towerY = tt::adjustForTerrain(ctx, x, z, 32, 4);

    // getFirstPiece.
    const core::Direction direction = tt::rotate(tt::randomRotation(firstPieceRandom), core::Direction::SOUTH);
    const core::BlockPos placePos = core::BlockPos(x, towerY, z).relative(direction, 24);
    const tt::FrontAndTop oriented{core::Direction::UP, direction};
    std::optional<tt::JigsawPlaceContext> placeContext = tt::pickPlaceableJunction(
        placePos, core::BlockPos(0, 0, 0), oriented, "twilightforest:lich_tower/tower_foyer",
        "twilightforest:lich_tower/vestibule", firstPieceRandom);
    if (!placeContext) return false;

    lich_tower::LichGen gen(ctx);
    lich_tower::PiecePtr foyerPtr = gen.jigsawPiece(LichKind::Foyer, 0, "twilightforest:lich_tower/tower_foyer",
                                                    *placeContext);
    foyerPtr->putChest = true;
    foyerPtr->chestSide = firstPieceRandom.nextBoolean();

    // generateFromStartingPiece.
    LichPiece* foyer = gen.add(std::move(foyerPtr));
    gen.addJigsaws(*foyer, *foyer);
    gen.beginYard(*foyer);

    // LandmarkStructure stub: stable sort by SortablePiece key.
    std::vector<lich_tower::PiecePtr>& pieces = gen.pieces();
    std::stable_sort(pieces.begin(), pieces.end(),
                     [](const lich_tower::PiecePtr& a, const lich_tower::PiecePtr& b) {
                         return a->sortKey() < b->sortKey();
                     });

    for (const lich_tower::PiecePtr& piece : pieces) {
        StructurePieceData data = tt::makePiece(piece->pieceType(), piece->box(),
                                                piece->isJigsaw ? piece->jigsaw.rotation() : tt::ROT_NONE,
                                                piece->genDepth(),
                                                piece->isJigsaw ? piece->jigsaw.templateId : std::string());
        if (piece->kind == LichKind::Utility) {
            // UtilityPiece(allowFeatures = false): a plain StructurePiece.
            data.allowFeatures = false;
        } else {
            tt::applyBeardifierModifier(data, piece->beardAdjusts(), piece->groundLevelDelta());
        }
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(lich_tower::makeBehavior(*piece));
    }
    return true;
}

TwilightTerraformer lichTowerTerraformer(const StructureInfo& info, const StructureStartData& start,
                                         const ::world::ChunkPos& chunkPos) {
    (void)info;
    (void)chunkPos;
    if (start.pieces.empty()) return TwilightTerraformer{};
    // LichTowerStructure.getStructureTerraformer.
    const int yBase = start.pieces.front().boundingBox.minY;
    std::vector<lich_tower::BuryBox> boxes;
    for (const StructurePieceData& piece : start.pieces) {
        const bool foyerOrTrim = piece.pieceType == "twilightforest:tflttfoy"
                              || piece.pieceType == "twilightforest:tfltcttrim";
        // LichTowerWingBeard.isTrim / LichYardBox terrain adjustment != NONE
        // are exactly the pieces recorded as rigid (BEARD_BOX) beard boxes.
        const bool trimBeard = piece.pieceType == "twilightforest:tflttbeard" && piece.rigidProjection;
        const bool adjustedYard = piece.pieceType == "twilightforest:tfltpath" && piece.rigidProjection;
        BoundingBox box = piece.boundingBox;
        if (foyerOrTrim || trimBeard) {
            // as is
        } else if (adjustedYard) {
            box.move(0, -5, 0);
        } else {
            continue;
        }
        // BoxDensityFunction.make(box, -5, -5, BURY).
        boxes.push_back({box.minX, box.minY - 5, box.minZ, box.maxX, box.maxY - 5, box.maxZ});
    }
    return [yBase, boxes](int32_t blockX, int32_t blockY, int32_t blockZ) -> double {
        // yClampedGradient(yBase - 2, yBase - 1, 1, 0) * combine(...).
        const double activator = Mth::clampedMap(static_cast<double>(blockY), static_cast<double>(yBase - 2),
                                                 static_cast<double>(yBase - 1), 1.0, 0.0);
        if (boxes.empty()) return activator * 0.0;
        // combine: make(b0); then add(make(bi), accumulated).
        double sum = boxes.front().compute(blockX, blockY, blockZ);
        for (size_t i = 1; i < boxes.size(); ++i) {
            sum = boxes[i].compute(blockX, blockY, blockZ) + sum;
        }
        return activator * sum;
    };
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
