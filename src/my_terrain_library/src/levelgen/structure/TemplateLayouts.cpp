#include "util/TerrainProfiling.h"
#include "levelgen/structure/StructureLayouts.h"

#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/ChunkGenerator.h"
#include "core/BlockPos.h"
#include "nbt/AllTags.h"
#include "nbt/NbtIo.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/
// StructureTemplate.java (getBoundingBox/transform) plus the template-based
// structure layouts, starting with IglooStructure/IglooPieces.
// LAYOUT ONLY: templates are loaded just for their SIZE here; palettes/blocks
// come with B6 block placement.

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

namespace fs = std::filesystem;

// Same discovery contract as the other loaders.
fs::path templateDataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) {
        return fs::path(env);
    }
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found for structure templates");
}

struct TemplateSize { int32_t x, y, z; };

// Template size cache: "minecraft:igloo/top" ->
// data/minecraft/structure/igloo/top.nbt root "size" [x,y,z].
const TemplateSize& templateSize(const std::string& templateId) {
    static std::mutex s_mutex;
    static std::unordered_map<std::string, TemplateSize> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(templateId);
    if (it != s_cache.end()) return it->second;

    size_t colon = templateId.find(':');
    std::string ns = colon == std::string::npos ? "minecraft" : templateId.substr(0, colon);
    std::string path = colon == std::string::npos ? templateId : templateId.substr(colon + 1);
    fs::path file = templateDataRoot() / ns / "structure" / (path + ".nbt");
    TERRAIN_ZONE_N("Tmpl.LoadSize");
    TERRAIN_ZONE_TEXT(templateId.c_str(), templateId.size());
    auto root = nbt::NbtIo::readCompressedFromFile(file.string());
    if (!root) {
        throw std::runtime_error("Cannot read structure template: " + file.string());
    }
    nbt::ListTag* sizeList = root->getListPtr("size");
    if (sizeList == nullptr || sizeList->size() != 3) {
        throw std::runtime_error("Structure template missing size: " + templateId);
    }
    TemplateSize size{
        static_cast<nbt::IntTag*>(sizeList->get(0))->getValue(),
        static_cast<nbt::IntTag*>(sizeList->get(1))->getValue(),
        static_cast<nbt::IntTag*>(sizeList->get(2))->getValue(),
    };
    return s_cache.emplace(templateId, size).first->second;
}

// Template rotations in Rotation.values() order (Rotation.getRandom uses
// nextInt(4) into this order).
enum class TRot { NONE = 0, CLOCKWISE_90 = 1, CLOCKWISE_180 = 2, COUNTERCLOCKWISE_90 = 3 };

const char* trotName(TRot r) {
    switch (r) {
        case TRot::NONE: return "NONE";
        case TRot::CLOCKWISE_90: return "CLOCKWISE_90";
        case TRot::CLOCKWISE_180: return "CLOCKWISE_180";
        case TRot::COUNTERCLOCKWISE_90: return "COUNTERCLOCKWISE_90";
    }
    return "NONE";
}

struct P3 { int32_t x, y, z; };

// Reference: StructureTemplate.transform() - FRONT_BACK mirror flips x
// BEFORE rotation (LEFT_RIGHT flips z; unused by current layouts).
P3 transformFull(P3 pos, bool mirrorFrontBack, TRot rotation, P3 pivot) {
    if (mirrorFrontBack) pos.x = -pos.x;
    switch (rotation) {
        case TRot::COUNTERCLOCKWISE_90:
            return {pivot.x - pivot.z + pos.z, pos.y, pivot.x + pivot.z - pos.x};
        case TRot::CLOCKWISE_90:
            return {pivot.x + pivot.z - pos.z, pos.y, pivot.z - pivot.x + pos.x};
        case TRot::CLOCKWISE_180:
            return {pivot.x + pivot.x - pos.x, pos.y, pivot.z + pivot.z - pos.z};
        case TRot::NONE:
        default:
            return pos;
    }
}

// Reference: StructureTemplate.transform() with Mirror.NONE.
P3 transformNoMirror(P3 pos, TRot rotation, P3 pivot) {
    switch (rotation) {
        case TRot::COUNTERCLOCKWISE_90:
            return {pivot.x - pivot.z + pos.z, pos.y, pivot.x + pivot.z - pos.x};
        case TRot::CLOCKWISE_90:
            return {pivot.x + pivot.z - pos.z, pos.y, pivot.z - pivot.x + pos.x};
        case TRot::CLOCKWISE_180:
            return {pivot.x + pivot.x - pos.x, pos.y, pivot.z + pivot.z - pos.z};
        case TRot::NONE:
        default:
            return pos;
    }
}

// Reference: StructureTemplate.getBoundingBox() - corners transformed around
// the pivot, min/max'd, moved by position.
BoundingBox templateBoundingBox(const std::string& templateId, TRot rotation, P3 pivot, P3 position) {
    const TemplateSize& size = templateSize(templateId);
    P3 corner1 = transformNoMirror({0, 0, 0}, rotation, pivot);
    P3 corner2 = transformNoMirror({size.x - 1, size.y - 1, size.z - 1}, rotation, pivot);
    BoundingBox box(std::min(corner1.x, corner2.x), std::min(corner1.y, corner2.y),
                    std::min(corner1.z, corner2.z), std::max(corner1.x, corner2.x),
                    std::max(corner1.y, corner2.y), std::max(corner1.z, corner2.z));
    box.move(position.x, position.y, position.z);
    return box;
}

StructurePieceData makeTemplatePiece(const char* pieceTypeId, const std::string& templateId,
                                     TRot rotation, P3 pivot, P3 position, int genDepth) {
    StructurePieceData piece;
    piece.pieceType = pieceTypeId;
    piece.boundingBox = templateBoundingBox(templateId, rotation, pivot, position);
    piece.rotation = trotName(rotation);  // TemplateStructurePiece.getRotation()
    piece.genDepth = genDepth;
    piece.detail = templateId;            // TemplateStructurePiece.templateName
    return piece;
}

} // namespace

// Shared with MansionLayout.cpp: template bbox with pivot ZERO and full
// mirror support (1 = LEFT_RIGHT flips z, 2 = FRONT_BACK flips x).
namespace template_detail {

BoundingBox mansionTemplateBox(const std::string& shortName, int rotation, int mirror,
                               int posX, int posY, int posZ) {
    const TemplateSize& size = templateSize("minecraft:woodland_mansion/" + shortName);
    auto tf = [&](int x, int y, int z) -> P3 {
        if (mirror == 1) z = -z;
        if (mirror == 2) x = -x;
        switch (rotation & 3) {
            case 1: return {-z, y, x};   // CLOCKWISE_90, pivot ZERO
            case 2: return {-x, y, -z};  // CLOCKWISE_180
            case 3: return {z, y, -x};   // COUNTERCLOCKWISE_90
            default: return {x, y, z};
        }
    };
    P3 c1 = tf(0, 0, 0);
    P3 c2 = tf(size.x - 1, size.y - 1, size.z - 1);
    BoundingBox box(std::min(c1.x, c2.x), std::min(c1.y, c2.y), std::min(c1.z, c2.z),
                    std::max(c1.x, c2.x), std::max(c1.y, c2.y), std::max(c1.z, c2.z));
    box.move(posX, posY, posZ);
    return box;
}

} // namespace template_detail

namespace StructureLayouts {

bool generateShipwreck(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                       bool isBeached) {
    (void)info;
    // Reference: ShipwreckStructure.generatePieces + ShipwreckPieces.
    // Rotation.getRandom, then Util.getRandom over the variant array
    // (beached: 11 entries, ocean: 20), pivot (4,0,15), pos (minX, 90, minZ).
    static const char* kBeached[11] = {
        "minecraft:shipwreck/with_mast", "minecraft:shipwreck/sideways_full",
        "minecraft:shipwreck/sideways_fronthalf", "minecraft:shipwreck/sideways_backhalf",
        "minecraft:shipwreck/rightsideup_full", "minecraft:shipwreck/rightsideup_fronthalf",
        "minecraft:shipwreck/rightsideup_backhalf", "minecraft:shipwreck/with_mast_degraded",
        "minecraft:shipwreck/rightsideup_full_degraded",
        "minecraft:shipwreck/rightsideup_fronthalf_degraded",
        "minecraft:shipwreck/rightsideup_backhalf_degraded",
    };
    static const char* kOcean[20] = {
        "minecraft:shipwreck/with_mast", "minecraft:shipwreck/upsidedown_full",
        "minecraft:shipwreck/upsidedown_fronthalf", "minecraft:shipwreck/upsidedown_backhalf",
        "minecraft:shipwreck/sideways_full", "minecraft:shipwreck/sideways_fronthalf",
        "minecraft:shipwreck/sideways_backhalf", "minecraft:shipwreck/rightsideup_full",
        "minecraft:shipwreck/rightsideup_fronthalf", "minecraft:shipwreck/rightsideup_backhalf",
        "minecraft:shipwreck/with_mast_degraded", "minecraft:shipwreck/upsidedown_full_degraded",
        "minecraft:shipwreck/upsidedown_fronthalf_degraded",
        "minecraft:shipwreck/upsidedown_backhalf_degraded",
        "minecraft:shipwreck/sideways_full_degraded",
        "minecraft:shipwreck/sideways_fronthalf_degraded",
        "minecraft:shipwreck/sideways_backhalf_degraded",
        "minecraft:shipwreck/rightsideup_full_degraded",
        "minecraft:shipwreck/rightsideup_fronthalf_degraded",
        "minecraft:shipwreck/rightsideup_backhalf_degraded",
    };

    TRot rotation = static_cast<TRot>(ctx.random.nextInt(4));
    const char* templateId = isBeached ? kBeached[ctx.random.nextInt(11)]
                                       : kOcean[ctx.random.nextInt(20)];
    P3 position{ctx.chunkX * 16, 90, ctx.chunkZ * 16};
    StructurePieceData piece = makeTemplatePiece("minecraft:shipwreck", templateId,
                                                 rotation, P3{4, 0, 15}, position, 0);

    // Reference: isTooBigToFitInWorldGenRegion (template x or y > 32) - never
    // true for the vanilla template set; the height-adjust branch draws RNG,
    // so fail LOUD if a data change ever makes it reachable.
    const TemplateSize& size = templateSize(templateId);
    if (size.x > 32 || size.y > 32) {
        throw std::runtime_error("shipwreck too-big branch reached - port the "
                                 "height adjustment (draws RNG) before proceeding");
    }
    out.pieces.push_back(std::move(piece));
    out.behaviors.push_back(PieceBehaviors::shipwreck(
        templateId, static_cast<int>(rotation), isBeached,
        core::BlockPos(position.x, position.y, position.z)));
    return true;
}

bool generateRuinedPortal(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                          const std::function<bool(int x, int y, int z)>& validBiomeAt) {
    // Reference: RuinedPortalStructure.findGenerationPoint. ALL draws happen
    // BEFORE the stub biome check (stub y = projectedY from findSuitableY).
    static const char* kPortals[10] = {
        "minecraft:ruined_portal/portal_1", "minecraft:ruined_portal/portal_2",
        "minecraft:ruined_portal/portal_3", "minecraft:ruined_portal/portal_4",
        "minecraft:ruined_portal/portal_5", "minecraft:ruined_portal/portal_6",
        "minecraft:ruined_portal/portal_7", "minecraft:ruined_portal/portal_8",
        "minecraft:ruined_portal/portal_9", "minecraft:ruined_portal/portal_10"};
    static const char* kGiant[3] = {
        "minecraft:ruined_portal/giant_portal_1", "minecraft:ruined_portal/giant_portal_2",
        "minecraft:ruined_portal/giant_portal_3"};

    // Weighted float setup pick (only when >1 setups; overworld variant has 2).
    const RuinedPortalSetup* setup = nullptr;
    if (info.portalSetups.size() > 1) {
        float total = 0.0f;
        for (const auto& s : info.portalSetups) total += s.weight;
        float pick = ctx.random.nextFloat();
        for (const auto& s : info.portalSetups) {
            pick -= s.weight / total;
            if (pick < 0.0f) { setup = &s; break; }
        }
    } else {
        setup = &info.portalSetups.at(0);
    }
    if (setup == nullptr) throw std::runtime_error("ruined portal setup pick failed");

    // Reference: sample() - draws ONLY when 0 < p < 1.
    bool airPocket;
    if (setup->airPocketProbability == 0.0f) airPocket = false;
    else if (setup->airPocketProbability == 1.0f) airPocket = true;
    else airPocket = ctx.random.nextFloat() < setup->airPocketProbability;

    const char* templateId = (ctx.random.nextFloat() < 0.05f)
        ? kGiant[ctx.random.nextInt(3)] : kPortals[ctx.random.nextInt(10)];
    const TemplateSize& size = templateSize(templateId);
    TRot rotation = static_cast<TRot>(ctx.random.nextInt(4));
    bool mirrorFrontBack = !(ctx.random.nextFloat() < 0.5f);  // NONE : FRONT_BACK
    P3 pivot{size.x / 2, 0, size.z / 2};
    P3 basePosition{ctx.chunkX * 16, 0, ctx.chunkZ * 16};

    // bbox at y=0 first (basePosition y is 0 = getWorldPosition).
    P3 corner1 = transformFull({0, 0, 0}, mirrorFrontBack, rotation, pivot);
    P3 corner2 = transformFull({size.x - 1, size.y - 1, size.z - 1}, mirrorFrontBack, rotation, pivot);
    BoundingBox box(std::min(corner1.x, corner2.x), std::min(corner1.y, corner2.y),
                    std::min(corner1.z, corner2.z), std::max(corner1.x, corner2.x),
                    std::max(corner1.y, corner2.y), std::max(corner1.z, corner2.z));
    box.move(basePosition.x, basePosition.y, basePosition.z);

    // Reference: RuinedPortalPiece.getHeightMapType().
    Heightmap::Types heightmapType = (setup->placement == "on_ocean_floor")
        ? Heightmap::Types::OCEAN_FLOOR_WG : Heightmap::Types::WORLD_SURFACE_WG;
    int32_t surfaceY = ctx.generator->getBaseHeight(box.centerX(), box.centerZ(),
                                                    heightmapType, ctx.randomState) - 1;

    // Reference: findSuitableY.
    // Reference: heightAccessor.getMinY() + 15 - the LEVEL, not the generator.
    int32_t minY = ctx.generator->getLevelMinY() + 15;
    int32_t ySpan = box.getYSpan();
    auto randomBetween = [&](int a, int b) { return ctx.random.nextInt(b - a + 1) + a; };
    auto randomWithinInterval = [&](int minPreferred, int max) {
        return minPreferred < max ? randomBetween(minPreferred, max) : max;
    };
    int32_t newY;
    if (setup->placement == "in_nether") {
        if (airPocket) newY = randomBetween(32, 100);
        else if (ctx.random.nextFloat() < 0.5f) newY = randomBetween(27, 29);
        else newY = randomBetween(29, 100);
    } else if (setup->placement == "in_mountain") {
        newY = randomWithinInterval(70, surfaceY - ySpan);
    } else if (setup->placement == "underground") {
        newY = randomWithinInterval(minY, surfaceY - ySpan);
    } else if (setup->placement == "partly_buried") {
        newY = surfaceY - ySpan + randomBetween(2, 8);
    } else {
        newY = surfaceY;
    }

    // 4 bottom-corner noise columns; walk down until 3 corners are opaque per
    // the (ocean-floor ? motion-blocking : non-air) predicate.
    struct Corner { int32_t x, z; };
    Corner corners[4] = {{box.minX, box.minZ}, {box.maxX, box.minZ},
                         {box.minX, box.maxZ}, {box.maxX, box.maxZ}};
    std::vector<minecraft::BlockState*> columns[4];
    for (int i = 0; i < 4; ++i) {
        ctx.generator->getBaseColumn(corners[i].x, corners[i].z, ctx.randomState, columns[i]);
    }
    Heightmap::Types opaqueType = (setup->placement == "on_ocean_floor")
        ? Heightmap::Types::OCEAN_FLOOR_WG : Heightmap::Types::WORLD_SURFACE_WG;
    auto isOpaque = Heightmap::getOpaquePredicate(opaqueType);
    // Column anchor - Java NoiseColumn.minY (level-anchored for flat).
    int32_t generatorMinY = ctx.generator->getBaseColumnMinY();
    int32_t projectedY = newY;
    for (; projectedY > minY; --projectedY) {
        int cornersOnSolidGround = 0;
        bool found = false;
        for (int i = 0; i < 4; ++i) {
            int32_t index = projectedY - generatorMinY;
            if (index < 0 || index >= static_cast<int32_t>(columns[i].size())) continue;
            minecraft::BlockState* state = columns[i][static_cast<size_t>(index)];
            if (state != nullptr && isOpaque(state)) {
                ++cornersOnSolidGround;
                if (cornersOnSolidGround == 3) { found = true; break; }
            }
        }
        if (found) break;
    }

    // Stub at (baseX, projectedY, baseZ); biome check BEFORE the piece build.
    if (!validBiomeAt(basePosition.x, projectedY, basePosition.z)) {
        return false;
    }

    // Piece: template bbox at origin (properties.cold biome sample draws nothing).
    P3 origin{basePosition.x, projectedY, basePosition.z};
    StructurePieceData piece;
    piece.pieceType = "minecraft:rupo";
    {
        P3 c1 = transformFull({0, 0, 0}, mirrorFrontBack, rotation, pivot);
        P3 c2 = transformFull({size.x - 1, size.y - 1, size.z - 1}, mirrorFrontBack, rotation, pivot);
        BoundingBox pieceBox(std::min(c1.x, c2.x), std::min(c1.y, c2.y), std::min(c1.z, c2.z),
                             std::max(c1.x, c2.x), std::max(c1.y, c2.y), std::max(c1.z, c2.z));
        pieceBox.move(origin.x, origin.y, origin.z);
        piece.boundingBox = pieceBox;
    }
    piece.rotation = trotName(rotation);
    piece.genDepth = 0;
    piece.detail = templateId;
    out.pieces.push_back(std::move(piece));

    // Reference: the lazy piece builder - properties.cold = canBeCold &&
    // isCold(origin, noiseBiome(origin quarts), seaLevel); no RNG draws.
    bool cold = false;
    if (setup->canBeCold) {
        world::biome::BiomeKey biomeKey = ctx.biomeSource->getNoiseBiome(
            origin.x >> 2, projectedY >> 2, origin.z >> 2, *ctx.sampler);
        const world::biome::Biome* biome = world::biome::Biomes::get(biomeKey);
        if (biome != nullptr) {
            cold = biome->coldEnoughToSnow(
                core::BlockPos(origin.x, projectedY, origin.z),
                ctx.generator->getSeaLevel());
        }
    }
    out.behaviors.push_back(PieceBehaviors::ruinedPortal(
        templateId, static_cast<int>(rotation), mirrorFrontBack,
        core::BlockPos(pivot.x, pivot.y, pivot.z),
        core::BlockPos(origin.x, origin.y, origin.z), setup->placement, cold,
        setup->mossiness, airPocket, setup->overgrown, setup->vines,
        setup->replaceWithBlackstone));
    return true;
}

bool generateOceanRuin(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out) {
    // Reference: OceanRuinStructure.generatePieces + OceanRuinPieces.
    // From structure JSON: biome_temp, large_probability, cluster_probability.
    bool warm = (info.name == "minecraft:ocean_ruin_warm");
    float largeProbability = warm ? 0.3f : 0.3f;    // both JSONs: 0.3
    float clusterProbability = warm ? 0.9f : 0.9f;  // both JSONs: 0.9

    static const char* kWarm[8] = {
        "minecraft:underwater_ruin/warm_1", "minecraft:underwater_ruin/warm_2",
        "minecraft:underwater_ruin/warm_3", "minecraft:underwater_ruin/warm_4",
        "minecraft:underwater_ruin/warm_5", "minecraft:underwater_ruin/warm_6",
        "minecraft:underwater_ruin/warm_7", "minecraft:underwater_ruin/warm_8"};
    static const char* kBigWarm[4] = {
        "minecraft:underwater_ruin/big_warm_4", "minecraft:underwater_ruin/big_warm_5",
        "minecraft:underwater_ruin/big_warm_6", "minecraft:underwater_ruin/big_warm_7"};
    static const char* kBrick[8] = {
        "minecraft:underwater_ruin/brick_1", "minecraft:underwater_ruin/brick_2",
        "minecraft:underwater_ruin/brick_3", "minecraft:underwater_ruin/brick_4",
        "minecraft:underwater_ruin/brick_5", "minecraft:underwater_ruin/brick_6",
        "minecraft:underwater_ruin/brick_7", "minecraft:underwater_ruin/brick_8"};
    static const char* kCracked[8] = {
        "minecraft:underwater_ruin/cracked_1", "minecraft:underwater_ruin/cracked_2",
        "minecraft:underwater_ruin/cracked_3", "minecraft:underwater_ruin/cracked_4",
        "minecraft:underwater_ruin/cracked_5", "minecraft:underwater_ruin/cracked_6",
        "minecraft:underwater_ruin/cracked_7", "minecraft:underwater_ruin/cracked_8"};
    static const char* kMossy[8] = {
        "minecraft:underwater_ruin/mossy_1", "minecraft:underwater_ruin/mossy_2",
        "minecraft:underwater_ruin/mossy_3", "minecraft:underwater_ruin/mossy_4",
        "minecraft:underwater_ruin/mossy_5", "minecraft:underwater_ruin/mossy_6",
        "minecraft:underwater_ruin/mossy_7", "minecraft:underwater_ruin/mossy_8"};
    static const char* kBigBrick[4] = {
        "minecraft:underwater_ruin/big_brick_1", "minecraft:underwater_ruin/big_brick_2",
        "minecraft:underwater_ruin/big_brick_3", "minecraft:underwater_ruin/big_brick_8"};
    static const char* kBigCracked[4] = {
        "minecraft:underwater_ruin/big_cracked_1", "minecraft:underwater_ruin/big_cracked_2",
        "minecraft:underwater_ruin/big_cracked_3", "minecraft:underwater_ruin/big_cracked_8"};
    static const char* kBigMossy[4] = {
        "minecraft:underwater_ruin/big_mossy_1", "minecraft:underwater_ruin/big_mossy_2",
        "minecraft:underwater_ruin/big_mossy_3", "minecraft:underwater_ruin/big_mossy_8"};

    // Reference: addPiece() - pivot defaults to ZERO (no setRotationPivot).
    // Cold ruins stack brick/cracked/mossy pieces with integrities
    // (base, 0.7, 0.5); warm ruins use one piece at base integrity.
    auto addPieceAt = [&](P3 position, TRot rotation, bool isLarge) {
        float base = isLarge ? 0.9f : 0.8f;
        core::BlockPos bp(position.x, position.y, position.z);
        if (warm) {
            const char* templateId = isLarge ? kBigWarm[ctx.random.nextInt(4)]
                                             : kWarm[ctx.random.nextInt(8)];
            out.pieces.push_back(makeTemplatePiece("minecraft:orp", templateId,
                                                   rotation, P3{0, 0, 0}, position, 0));
            out.behaviors.push_back(PieceBehaviors::oceanRuin(
                templateId, static_cast<int>(rotation), base, true, isLarge, bp));
        } else {
            const char** bricks = isLarge ? kBigBrick : kBrick;
            const char** cracked = isLarge ? kBigCracked : kCracked;
            const char** mossy = isLarge ? kBigMossy : kMossy;
            int idx = ctx.random.nextInt(isLarge ? 4 : 8);
            out.pieces.push_back(makeTemplatePiece("minecraft:orp", bricks[idx],
                                                   rotation, P3{0, 0, 0}, position, 0));
            out.behaviors.push_back(PieceBehaviors::oceanRuin(
                bricks[idx], static_cast<int>(rotation), base, false, isLarge, bp));
            out.pieces.push_back(makeTemplatePiece("minecraft:orp", cracked[idx],
                                                   rotation, P3{0, 0, 0}, position, 0));
            out.behaviors.push_back(PieceBehaviors::oceanRuin(
                cracked[idx], static_cast<int>(rotation), 0.7f, false, isLarge, bp));
            out.pieces.push_back(makeTemplatePiece("minecraft:orp", mossy[idx],
                                                   rotation, P3{0, 0, 0}, position, 0));
            out.behaviors.push_back(PieceBehaviors::oceanRuin(
                mossy[idx], static_cast<int>(rotation), 0.5f, false, isLarge, bp));
        }
    };

    // Mth.nextInt(random, a, b) = nextInt(b - a + 1) + a
    auto nextIntBetween = [&](int a, int b) { return ctx.random.nextInt(b - a + 1) + a; };

    TRot rotation = static_cast<TRot>(ctx.random.nextInt(4));
    P3 position{ctx.chunkX * 16, 90, ctx.chunkZ * 16};

    bool isLarge = ctx.random.nextFloat() <= largeProbability;
    float baseIntegrity = isLarge ? 0.9f : 0.8f;
    (void)baseIntegrity;  // integrity feeds BlockRot in B6, not layout
    addPieceAt(position, rotation, isLarge);

    if (isLarge && ctx.random.nextFloat() <= clusterProbability) {
        // Reference: addClusterRuins().
        P3 parentPos{position.x, 90, position.z};
        P3 parentCorner = transformNoMirror({15, 0, 15}, rotation, {0, 0, 0});
        parentCorner = {parentCorner.x + parentPos.x, parentCorner.y + parentPos.y,
                        parentCorner.z + parentPos.z};
        BoundingBox parentBB(std::min(parentPos.x, parentCorner.x), std::min(parentPos.y, parentCorner.y),
                             std::min(parentPos.z, parentCorner.z), std::max(parentPos.x, parentCorner.x),
                             std::max(parentPos.y, parentCorner.y), std::max(parentPos.z, parentCorner.z));
        P3 origin{std::min(parentPos.x, parentCorner.x), parentPos.y,
                  std::min(parentPos.z, parentCorner.z)};

        // allPositions(): 8 candidates, 16 draws in this fixed order.
        std::vector<P3> allPositions;
        allPositions.push_back({origin.x - 16 + nextIntBetween(1, 8), origin.y, origin.z + 16 + nextIntBetween(1, 7)});
        allPositions.push_back({origin.x - 16 + nextIntBetween(1, 8), origin.y, origin.z + nextIntBetween(1, 7)});
        allPositions.push_back({origin.x - 16 + nextIntBetween(1, 8), origin.y, origin.z - 16 + nextIntBetween(4, 8)});
        allPositions.push_back({origin.x + nextIntBetween(1, 7), origin.y, origin.z + 16 + nextIntBetween(1, 7)});
        allPositions.push_back({origin.x + nextIntBetween(1, 7), origin.y, origin.z - 16 + nextIntBetween(4, 6)});
        allPositions.push_back({origin.x + 16 + nextIntBetween(1, 7), origin.y, origin.z + 16 + nextIntBetween(3, 8)});
        allPositions.push_back({origin.x + 16 + nextIntBetween(1, 7), origin.y, origin.z + nextIntBetween(1, 7)});
        allPositions.push_back({origin.x + 16 + nextIntBetween(1, 7), origin.y, origin.z - 16 + nextIntBetween(4, 8)});

        int ruins = nextIntBetween(4, 8);
        for (int i = 0; i < ruins; ++i) {
            if (allPositions.empty()) continue;
            int idx = ctx.random.nextInt(static_cast<int32_t>(allPositions.size()));
            P3 pos = allPositions[static_cast<size_t>(idx)];
            allPositions.erase(allPositions.begin() + idx);
            TRot nextRotation = static_cast<TRot>(ctx.random.nextInt(4));
            P3 nextCorner = transformNoMirror({5, 0, 6}, nextRotation, {0, 0, 0});
            nextCorner = {nextCorner.x + pos.x, nextCorner.y + pos.y, nextCorner.z + pos.z};
            BoundingBox nextBB(std::min(pos.x, nextCorner.x), std::min(pos.y, nextCorner.y),
                               std::min(pos.z, nextCorner.z), std::max(pos.x, nextCorner.x),
                               std::max(pos.y, nextCorner.y), std::max(pos.z, nextCorner.z));
            if (!nextBB.intersects(parentBB)) {
                addPieceAt(pos, nextRotation, false);
            }
        }
    }
    return !out.pieces.empty();
}

bool generateIgloo(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out) {
    (void)info;
    // Reference: IglooStructure.generatePieces + IglooPieces.addPieces.
    // startPos = (minBlockX, 90, minBlockZ); rotation = Rotation.getRandom.
    P3 startPos{ctx.chunkX * 16, 90, ctx.chunkZ * 16};
    TRot rotation = static_cast<TRot>(ctx.random.nextInt(4));

    // PIVOTS / OFFSETS tables (IglooPieces statics).
    const P3 pivotTop{3, 5, 5}, pivotMiddle{1, 3, 1}, pivotBottom{3, 6, 7};
    const P3 offsetTop{0, 0, 0}, offsetMiddle{2, -3, 4}, offsetBottom{0, -3, -2};

    auto position = [&](P3 offset, int below) {
        return P3{startPos.x + offset.x, startPos.y + offset.y - below, startPos.z + offset.z};
    };

    auto addPiece = [&](const char* templateId, TRot rot, P3 pivot, P3 offset, P3 pos) {
        out.pieces.push_back(makeTemplatePiece("minecraft:iglu", templateId, rot, pivot, pos, 0));
        out.behaviors.push_back(PieceBehaviors::igloo(
            templateId, static_cast<int>(rot),
            core::BlockPos(pivot.x, pivot.y, pivot.z),
            core::BlockPos(offset.x, offset.y, offset.z),
            core::BlockPos(pos.x, pos.y, pos.z)));
    };
    if (ctx.random.nextDouble() < 0.5) {
        int depth = ctx.random.nextInt(8) + 4;
        addPiece("minecraft:igloo/bottom", rotation, pivotBottom, offsetBottom,
                 position(offsetBottom, depth * 3));
        for (int i = 0; i < depth - 1; ++i) {
            addPiece("minecraft:igloo/middle", rotation, pivotMiddle, offsetMiddle,
                     position(offsetMiddle, i * 3));
        }
    }
    addPiece("minecraft:igloo/top", rotation, pivotTop, offsetTop, position(offsetTop, 0));
    return !out.pieces.empty();
}

// ============================================================================
// END CITY - Reference: EndCityStructure + EndCityPieces (layout half).
// Piece graph over end_city templates. Pieces carry a random "childTag" as
// genDepth (collision-group id) - P lines must reproduce those ints exactly.
// ============================================================================

namespace endcity {

struct ECPiece {
    std::string templateName;   // short name ("base_floor")
    P3 position;
    TRot rotation;
    bool overwrite;
    int genDepth = 0;
    BoundingBox box;
};

struct ECContext {
    std::vector<ECPiece> pieces;
    bool shipCreated = false;   // TOWER_BRIDGE_GENERATOR state (init() clears)
};

std::string ecTemplateId(const std::string& shortName) {
    return "minecraft:end_city/" + shortName;
}

// Rotation.getRotated = ordinal addition mod 4 ([NONE, CW90, CW180, CCW90]).
TRot rotAdd(TRot a, TRot b) {
    return static_cast<TRot>((static_cast<int>(a) + static_cast<int>(b)) & 3);
}

ECPiece makeECPiece(const std::string& shortName, P3 position, TRot rotation, bool overwrite) {
    ECPiece piece;
    piece.templateName = shortName;
    piece.position = position;
    piece.rotation = rotation;
    piece.overwrite = overwrite;
    piece.genDepth = 0;
    piece.box = templateBoundingBox(ecTemplateId(shortName), rotation, P3{0, 0, 0}, position);
    return piece;
}

// Reference: EndCityPieces.addPiece: child pos = parent.templatePosition +
// calculateConnectedPosition(parentSettings, offset, childSettings, ZERO)
// = parent.pos + transform(offset, parentRot, pivot ZERO) (child term is ZERO).
ECPiece addPiece(const ECPiece& parent, P3 offset, const std::string& shortName,
                 TRot rotation, bool overwrite) {
    P3 transformed = transformNoMirror(offset, parent.rotation, P3{0, 0, 0});
    P3 position{parent.position.x + transformed.x,
                parent.position.y + transformed.y,
                parent.position.z + transformed.z};
    return makeECPiece(shortName, position, rotation, overwrite);
}

bool recursiveChildren(ECContext& ec, int generatorKind, int genDepth,
                       const ECPiece& parent, const P3* offset,
                       std::vector<ECPiece>& pieces, LegacyRandomSource& random);

// Generator kinds: 0 = HOUSE_TOWER, 1 = TOWER, 2 = TOWER_BRIDGE, 3 = FAT_TOWER.
// Each returns whether generation succeeded; child pieces append to out.
bool runGenerator(ECContext& ec, int generatorKind, int genDepth,
                  const ECPiece& parent, const P3* offset,
                  std::vector<ECPiece>& out, LegacyRandomSource& random) {
    auto add = [&](const ECPiece& piece) -> const ECPiece& {
        out.push_back(piece);
        return out.back();
    };

    switch (generatorKind) {
        case 0: {  // HOUSE_TOWER_GENERATOR
            if (genDepth > 8) return false;
            TRot rotation = parent.rotation;
            // NOTE: parent for the offset transform, then children chain.
            ECPiece lastPiece = add(addPiece(parent, *offset, "base_floor", rotation, true));
            int numFloors = random.nextInt(3);
            if (numFloors == 0) {
                add(addPiece(lastPiece, P3{-1, 4, -1}, "base_roof", rotation, true));
            } else if (numFloors == 1) {
                lastPiece = add(addPiece(lastPiece, P3{-1, 0, -1}, "second_floor_2", rotation, false));
                lastPiece = add(addPiece(lastPiece, P3{-1, 8, -1}, "second_roof", rotation, false));
                recursiveChildren(ec, 1, genDepth + 1, lastPiece, nullptr, out, random);
            } else if (numFloors == 2) {
                lastPiece = add(addPiece(lastPiece, P3{-1, 0, -1}, "second_floor_2", rotation, false));
                lastPiece = add(addPiece(lastPiece, P3{-1, 4, -1}, "third_floor_2", rotation, false));
                lastPiece = add(addPiece(lastPiece, P3{-1, 8, -1}, "third_roof", rotation, true));
                recursiveChildren(ec, 1, genDepth + 1, lastPiece, nullptr, out, random);
            }
            return true;
        }
        case 1: {  // TOWER_GENERATOR
            TRot rotation = parent.rotation;
            // Offset draws in arg order: x then z.
            int offX = 3 + random.nextInt(2);
            int offZ = 3 + random.nextInt(2);
            ECPiece lastPiece = add(addPiece(parent, P3{offX, -3, offZ}, "tower_base", rotation, true));
            lastPiece = add(addPiece(lastPiece, P3{0, 7, 0}, "tower_piece", rotation, true));
            bool haveBridgePiece = random.nextInt(3) == 0;
            ECPiece bridgePiece = lastPiece;  // valid only when haveBridgePiece
            int towerHeight = 1 + random.nextInt(3);
            for (int i = 0; i < towerHeight; ++i) {
                lastPiece = add(addPiece(lastPiece, P3{0, 4, 0}, "tower_piece", rotation, true));
                if (i < towerHeight - 1 && random.nextBoolean()) {
                    haveBridgePiece = true;
                    bridgePiece = lastPiece;
                }
            }
            if (haveBridgePiece) {
                // TOWER_BRIDGES: (NONE,(1,-1,0)) (CW90,(6,-1,1))
                // (CCW90,(0,-1,5)) (CW180,(5,-1,6))
                static const struct { TRot rot; P3 off; } TOWER_BRIDGES[4] = {
                    {TRot::NONE, {1, -1, 0}}, {TRot::CLOCKWISE_90, {6, -1, 1}},
                    {TRot::COUNTERCLOCKWISE_90, {0, -1, 5}}, {TRot::CLOCKWISE_180, {5, -1, 6}},
                };
                for (const auto& bridge : TOWER_BRIDGES) {
                    if (random.nextBoolean()) {
                        ECPiece bridgeStart = add(addPiece(bridgePiece, bridge.off, "bridge_end",
                                                           rotAdd(rotation, bridge.rot), true));
                        recursiveChildren(ec, 2, genDepth + 1, bridgeStart, nullptr, out, random);
                    }
                }
                add(addPiece(lastPiece, P3{-1, 4, -1}, "tower_top", rotation, true));
            } else {
                if (genDepth != 7) {
                    return recursiveChildren(ec, 3, genDepth + 1, lastPiece, nullptr, out, random);
                }
                add(addPiece(lastPiece, P3{-1, 4, -1}, "tower_top", rotation, true));
            }
            return true;
        }
        case 2: {  // TOWER_BRIDGE_GENERATOR
            TRot rotation = parent.rotation;
            int bridgeLength = random.nextInt(4) + 1;
            ECPiece lastPiece = add(addPiece(parent, P3{0, 0, -4}, "bridge_piece", rotation, true));
            out.back().genDepth = -1;
            int nextY = 0;
            for (int i = 0; i < bridgeLength; ++i) {
                if (random.nextBoolean()) {
                    lastPiece = add(addPiece(lastPiece, P3{0, nextY, -4}, "bridge_piece", rotation, true));
                    nextY = 0;
                } else {
                    if (random.nextBoolean()) {
                        lastPiece = add(addPiece(lastPiece, P3{0, nextY, -4}, "bridge_steep_stairs", rotation, true));
                    } else {
                        lastPiece = add(addPiece(lastPiece, P3{0, nextY, -8}, "bridge_gentle_stairs", rotation, true));
                    }
                    nextY = 4;
                }
            }
            if (!ec.shipCreated && random.nextInt(10 - genDepth) == 0) {
                // Offset draws in arg order: x (-8+nextInt(8)) then z (-70+nextInt(10)).
                int shipX = -8 + random.nextInt(8);
                int shipZ = -70 + random.nextInt(10);
                add(addPiece(lastPiece, P3{shipX, nextY, shipZ}, "ship", rotation, true));
                ec.shipCreated = true;
            } else {
                P3 houseOffset{-3, nextY + 1, -11};
                if (!recursiveChildren(ec, 0, genDepth + 1, lastPiece, &houseOffset, out, random)) {
                    return false;
                }
            }
            add(addPiece(lastPiece, P3{4, nextY, 0}, "bridge_end",
                         rotAdd(rotation, TRot::CLOCKWISE_180), true));
            out.back().genDepth = -1;
            return true;
        }
        case 3: {  // FAT_TOWER_GENERATOR
            TRot rotation = parent.rotation;
            ECPiece lastPiece = add(addPiece(parent, P3{-3, 4, -3}, "fat_tower_base", rotation, true));
            lastPiece = add(addPiece(lastPiece, P3{0, 4, 0}, "fat_tower_middle", rotation, true));
            static const struct { TRot rot; P3 off; } FAT_TOWER_BRIDGES[4] = {
                {TRot::NONE, {4, -1, 0}}, {TRot::CLOCKWISE_90, {12, -1, 4}},
                {TRot::COUNTERCLOCKWISE_90, {0, -1, 8}}, {TRot::CLOCKWISE_180, {8, -1, 12}},
            };
            // Loop condition draws nextInt(3) each iteration.
            for (int i = 0; i < 2 && random.nextInt(3) != 0; ++i) {
                lastPiece = add(addPiece(lastPiece, P3{0, 8, 0}, "fat_tower_middle", rotation, true));
                for (const auto& bridge : FAT_TOWER_BRIDGES) {
                    if (random.nextBoolean()) {
                        ECPiece bridgeStart = add(addPiece(lastPiece, bridge.off, "bridge_end",
                                                           rotAdd(rotation, bridge.rot), true));
                        recursiveChildren(ec, 2, genDepth + 1, bridgeStart, nullptr, out, random);
                    }
                }
            }
            add(addPiece(lastPiece, P3{-2, 8, -2}, "fat_tower_top", rotation, true));
            return true;
        }
    }
    return false;
}

// Reference: EndCityPieces.recursiveChildren. `pieces` is THIS level's list
// (Java threads each generate's childPieces down as the next level's list);
// collision checks run against it, NOT the global list. childTag overwrites
// any tags nested levels applied (their pieces were addAll'd into
// childPieces before this level's tagging).
bool recursiveChildren(ECContext& ec, int generatorKind, int genDepth,
                       const ECPiece& parent, const P3* offset,
                       std::vector<ECPiece>& pieces, LegacyRandomSource& random) {
    if (genDepth > 8) return false;
    std::vector<ECPiece> childPieces;
    if (runGenerator(ec, generatorKind, genDepth, parent, offset, childPieces, random)) {
        bool collision = false;
        int childTag = random.nextInt();
        for (auto& child : childPieces) {
            child.genDepth = childTag;
            for (const auto& existing : pieces) {
                if (existing.box.intersects(child.box) &&
                    existing.genDepth != parent.genDepth) {
                    collision = true;
                    break;
                }
            }
            if (collision) break;
        }
        if (!collision) {
            for (auto& child : childPieces) {
                pieces.push_back(std::move(child));
            }
            return true;
        }
    }
    return false;
}

}  // namespace endcity

bool generateEndCity(const StructureInfo& info, GenerationContext& ctx,
                     StructureStartData& out, int rotationIn,
                     int blockX, int startY, int blockZ) {
    (void)info;
    using namespace endcity;
    // Reference: EndCityStructure.generatePieces -> startHouseTower.
    // The rotation draw + 5x5 lowest-y stub + y<60 reject + biome check
    // happened in the DISPATCH (mirrors the mansion flow).
    TRot rotation = static_cast<TRot>(rotationIn);
    P3 origin{blockX, startY, blockZ};

    ECContext ec;
    ec.shipCreated = false;  // all four generators' init()

    ECPiece lastPiece = makeECPiece("base_floor", origin, rotation, true);
    ec.pieces.push_back(lastPiece);
    lastPiece = addPiece(lastPiece, P3{-1, 0, -1}, "second_floor_1", rotation, false);
    ec.pieces.push_back(lastPiece);
    lastPiece = addPiece(lastPiece, P3{-1, 4, -1}, "third_floor_1", rotation, false);
    ec.pieces.push_back(lastPiece);
    lastPiece = addPiece(lastPiece, P3{-1, 8, -1}, "third_roof", rotation, true);
    ec.pieces.push_back(lastPiece);
    recursiveChildren(ec, 1, 1, lastPiece, nullptr, ec.pieces, ctx.random);

    out.pieces.clear();
    out.pieces.reserve(ec.pieces.size());
    for (const auto& piece : ec.pieces) {
        StructurePieceData data;
        data.pieceType = "minecraft:ecp";
        data.boundingBox = piece.box;
        data.rotation = trotName(piece.rotation);
        data.genDepth = piece.genDepth;
        // EndCityPiece.templateName is the SHORT name (like the mansion).
        data.detail = piece.templateName;
        out.pieces.push_back(std::move(data));
        out.behaviors.push_back(PieceBehaviors::endCity(
            ecTemplateId(piece.templateName), static_cast<int>(piece.rotation),
            piece.overwrite,
            core::BlockPos(piece.position.x, piece.position.y, piece.position.z)));
    }
    return !out.pieces.empty();
}

bool generateNetherFossil(const StructureInfo& info, GenerationContext& ctx,
                          StructureStartData& out,
                          std::function<bool(int, int, int)> biomeCheck) {
    (void)info;
    // Reference: NetherFossilStructure.findGenerationPoint +
    // NetherFossilPieces.addPieces.
    // Draws: nextInt(16) x, nextInt(16) z, UniformHeight(absolute(32),
    // belowTop(2)) sample, [column walk, no draws], BIOME CHECK, then
    // Rotation.getRandom nextInt(4) + Util.getRandom(FOSSILS) nextInt(14).
    int32_t blockX = ctx.chunkX * 16 + ctx.random.nextInt(16);
    int32_t blockZ = ctx.chunkZ * 16 + ctx.random.nextInt(16);
    int32_t seaLevel = ctx.generator->getSeaLevel();

    // WorldGenerationContext(generator, heightAccessor): genDepth =
    // min(dimension height, noise settings height) = 128 in the nether ->
    // belowTop(2) resolves to genMinY + genDepth - 1 - 2.
    int32_t genMinY = ctx.generator->getBaseColumnMinY();
    int32_t genDepth = ctx.generator->getGenDepth();
    int32_t minInclusive = 32;
    int32_t maxInclusive = genMinY + genDepth - 1 - 2;
    int32_t y = (minInclusive > maxInclusive)
        ? minInclusive
        : ctx.random.nextInt(maxInclusive - minInclusive + 1) + minInclusive;

    // NoiseColumn walk-down - Reference: lines 37-45. The base column only
    // holds default block / fluid / air, so Java's
    // (below is SOUL_SAND || below.isFaceSturdy(UP)) reduces to
    // "solid non-fluid" (lava is NOT sturdy; soul_sand never appears).
    std::vector<minecraft::BlockState*> column;
    ctx.generator->getBaseColumn(blockX, blockZ, ctx.randomState, column);
    auto columnBlock = [&](int32_t worldY) -> minecraft::BlockState* {
        int32_t index = worldY - genMinY;
        if (index < 0 || index >= static_cast<int32_t>(column.size())) return nullptr;
        return column[static_cast<size_t>(index)];
    };
    while (y > seaLevel) {
        minecraft::BlockState* current = columnBlock(y);
        --y;
        minecraft::BlockState* below = columnBlock(y);
        bool currentAir = current == nullptr || current->isAir();
        bool belowSturdy = below != nullptr && !below->isAir() && !below->isFluid();
        if (below != nullptr && below->getIdentifier() == "minecraft:soul_sand") {
            belowSturdy = true;
        }
        if (currentAir && belowSturdy) {
            break;
        }
    }
    if (y <= seaLevel) {
        return false;
    }

    // Stub at (blockX, y, blockZ); biome check BEFORE the lazy piece build.
    if (!biomeCheck(blockX, y, blockZ)) {
        return false;
    }

    TRot rotation = static_cast<TRot>(ctx.random.nextInt(4));
    static const char* const FOSSILS[14] = {
        "minecraft:nether_fossils/fossil_1", "minecraft:nether_fossils/fossil_2",
        "minecraft:nether_fossils/fossil_3", "minecraft:nether_fossils/fossil_4",
        "minecraft:nether_fossils/fossil_5", "minecraft:nether_fossils/fossil_6",
        "minecraft:nether_fossils/fossil_7", "minecraft:nether_fossils/fossil_8",
        "minecraft:nether_fossils/fossil_9", "minecraft:nether_fossils/fossil_10",
        "minecraft:nether_fossils/fossil_11", "minecraft:nether_fossils/fossil_12",
        "minecraft:nether_fossils/fossil_13", "minecraft:nether_fossils/fossil_14"
    };
    const char* templateId = FOSSILS[ctx.random.nextInt(14)];

    out.pieces.push_back(makeTemplatePiece("minecraft:nefos", templateId, rotation,
                                           P3{0, 0, 0}, P3{blockX, y, blockZ}, 0));
    out.behaviors.push_back(PieceBehaviors::netherFossil(
        templateId, static_cast<int>(rotation), core::BlockPos(blockX, y, blockZ)));
    return true;
}

} // namespace StructureLayouts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
