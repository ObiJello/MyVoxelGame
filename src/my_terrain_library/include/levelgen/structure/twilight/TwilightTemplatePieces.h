#pragma once

#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/Heightmap.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "random/LegacyRandomSource.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Twilight Forest 4.9 — the shared template/jigsaw machinery of the TF
// structure ports (naga courtyard, quest grove, lich tower):
//
//   TwilightTemplateStructurePiece.java   TemplatePieceBehavior
//   TwilightDoubleTemplateStructurePiece  DoubleTemplatePieceBehavior
//   TwilightJigsawPiece.java              JigsawPieceBehavior + JigsawPiece
//   util/jigsaw/{JigsawRecord,JigsawPlaceContext,JigsawUtil}.java
//   util/StructureTemplateDefinitions.java + TemplatePoolInstance.java
//                                         (data/twilightforest/twilight/
//                                          template_definition/**)
//   world/components/processors/*.java    the processors as TemplateEngine
//                                         processor lambdas
//   util/RotationUtil.java, util/BoundingBoxUtils.java, util/WorldUtil.java
//                                         (adjustForTerrain)
//
// Everything here is either immutable after construction or computed per
// call; behaviors hold only their layout-time configuration, so a start's
// behaviors may be shared by chunk copies of the start.

namespace minecraft {
namespace nbt { class CompoundTag; }
namespace world { namespace level { namespace block { class Block; } } }

namespace levelgen {
class ChunkGenerator;
class WorldGenLevel;
class WorldgenRandom;
namespace structure {
struct GenerationContext;

namespace twilight_template {

using Block = ::minecraft::world::level::block::Block;

/** One TemplateEngine processor (see TemplatePlaceSettings::processors). */
using Processor = std::function<BlockState*(const core::BlockPos&, BlockState*,
                                            const core::BlockPos&, BlockState*,
                                            const core::BlockPos&)>;

// ============================================================================
// Rotation / direction helpers (net.minecraft.world.level.block.Rotation,
// twilightforest.util.RotationUtil). Rotation ordinals: NONE 0,
// CLOCKWISE_90 1, CLOCKWISE_180 2, COUNTERCLOCKWISE_90 3.
// ============================================================================
enum : int { ROT_NONE = 0, ROT_CW90 = 1, ROT_CW180 = 2, ROT_CCW90 = 3 };

const char* rotationName(int rotation);
/** Rotation.rotate(Direction) — horizontal directions turn, vertical stay. */
core::Direction rotate(int rotation, core::Direction dir);
/** Rotation.getRotated(Rotation). */
inline int rotated(int rotation, int by) { return (rotation + by) & 3; }
/** Rotation.getRandom(random): values()[nextInt(4)]. */
int randomRotation(LegacyRandomSource& random);
/** RotationUtil.getRelativeRotation(original, destination). */
int relativeRotation(core::Direction original, core::Direction destination);
/** Direction.getClockWise() / getCounterClockWise() about Y (horizontal only). */
core::Direction clockWise(core::Direction dir);
core::Direction counterClockWise(core::Direction dir);
bool isVertical(core::Direction dir);
/** DirectionUtil.fromStringOrElse (serialized lowercase names). */
std::optional<core::Direction> directionFromName(const std::string& name);

/** RandomSource.nextIntBetweenInclusive(min, max). */
inline int32_t nextIntBetweenInclusive(LegacyRandomSource& random, int32_t min, int32_t max) {
    return random.nextInt(max - min + 1) + min;
}

// ============================================================================
// BoundingBox helpers (util/BoundingBoxUtils.java + vanilla BoundingBox).
// ============================================================================
BoundingBox inflatedBy(const BoundingBox& box, int amount);
BoundingBox inflatedBy(const BoundingBox& box, int x, int y, int z);
BoundingBox moved(const BoundingBox& box, int dx, int dy, int dz);
BoundingBox cloneWithAdjustments(const BoundingBox& box, int x1, int y1, int z1,
                                 int x2, int y2, int z2);
BoundingBox extrusionFrom(const BoundingBox& box, core::Direction direction, int length);
BoundingBox safeRetract(const BoundingBox& box, core::Direction direction, int length);
std::optional<BoundingBox> intersection(const BoundingBox& a, const BoundingBox& b);
/** BoundingBox.fromCorners. */
BoundingBox fromCorners(const core::BlockPos& a, const core::BlockPos& b);
/** BoundingBox.getCenter(). */
core::BlockPos center(const BoundingBox& box);
core::BlockPos bottomCenterOf(const BoundingBox& box);
core::BlockPos clampedInside(const BoundingBox& box, const core::BlockPos& pos);
int greatestAxalDistance(const BoundingBox& box, const core::BlockPos& pos);
int horizontalManhattanDistance(const BoundingBox& box, const core::BlockPos& pos);
BoundingBox wrappedCoordinates(int padding, const core::BlockPos& a, const core::BlockPos& b);
BoundingBox setY(const BoundingBox& box, int minY, int maxY);
int span(const BoundingBox& box, core::Axis axis);
/** BoundingBoxUtils.lerpPosInside. */
core::BlockPos lerpPosInside(const BoundingBox& box, core::Axis axis, float delta);
/** Mth.lerpDiscrete(float, int, int) in Java float arithmetic. */
int lerpDiscrete(float alpha, int p0, int p1);

// ============================================================================
// Raw templates — the NBT as written (palette names/properties, block nbt).
// TemplateEngine resolves palettes to engine states (TF names to stand-ins);
// the jigsaw orientation, the TF block names and properties the structure
// code reads (wrought_iron_fence POST) come from here instead.
// ============================================================================
struct RawPaletteEntry {
    std::string name;                                        // normalized id
    std::unordered_map<std::string, std::string> properties;
};

struct RawBlock {
    core::BlockPos pos;
    int32_t state = 0;
    std::shared_ptr<nbt::CompoundTag> nbt;                   // null = none
};

struct RawTemplate {
    int32_t sizeX = 0, sizeY = 0, sizeZ = 0;
    std::vector<std::vector<RawPaletteEntry>> palettes;
    std::vector<RawBlock> blocks;                            // file order
    bool empty() const { return palettes.empty(); }
};

/** Cached; a missing file is an empty template (getOrCreate). */
const RawTemplate& rawTemplate(const std::string& templateId);

struct FilteredBlock {
    core::BlockPos pos;
    const RawPaletteEntry* entry;
    const nbt::CompoundTag* nbt;
};

/**
 * StructureTemplate.filterBlocks(position, settings, block, absolute): the
 * blocks named `blockName` of the palette picked at `position`, positions
 * transformed (absolute) or template-local, clipped to `clip` when given.
 */
std::vector<FilteredBlock> filterBlocks(const std::string& templateId,
                                        const core::BlockPos& position,
                                        const TemplatePlaceSettings& settings,
                                        const std::string& blockName, bool absolute,
                                        const BoundingBox* clip = nullptr);

/** StructureTemplate.getSize(rotation) as {x, y, z}. */
std::array<int, 3> templateSize(const std::string& templateId, int rotation);

/** StructureTemplate.getBoundingBox(settings, position). */
BoundingBox templateBoundingBox(const std::string& templateId,
                                const TemplatePlaceSettings& settings,
                                const core::BlockPos& position);

// ============================================================================
// Blocks
// ============================================================================

/**
 * The engine block a TF name resolves to when it is REALLY registered (not a
 * stand-in); null otherwise. Processors that test "block == TFBlocks.X" use
 * this, so a stand-in shared by several TF names never matches the wrong one.
 */
Block* realTwilightBlock(const std::string& name);

/** FeaturePlacers.transferAllStateKeys(stateIn, blockOut) (cached). */
BlockState* transferAllStateKeys(BlockState* in, const std::string& targetName);

/** A state with one property replaced (no-op when the block lacks it). */
BlockState* withProperty(BlockState* state, const std::string& key, const std::string& value);

/** BlockState.rotate(Rotation) including ROTATION_16 (skulls, heads). */
BlockState* rotateBlockState(BlockState* state, int rotation);

/** LevelAccessor.removeBlock(pos, false): the cell's fluid, else air. */
void removeBlock(WorldGenLevel* level, const core::BlockPos& pos);

/** Writes a block-entity payload (save-format SNBT) at pos. */
void setBlockEntity(WorldGenLevel* level, const core::BlockPos& pos, const std::string& snbt);

/** RandomizableContainer.setLootTable payload for a container block entity. */
std::string lootContainerPayload(const std::string& beId, const std::string& lootTable, int64_t seed);

// ============================================================================
// Processors (world/components/processors/*.java). Each is pure per block:
// its random is StructurePlaceSettings.getRandom(pos) = LegacyRandomSource(
// Mth.getSeed(pos)), created fresh per call as in Java.
// ============================================================================
Processor nagastoneVariants();                 // NagastoneVariants
Processor stoneBricksVariants();               // StoneBricksVariants
Processor smoothStoneVariants();               // SmoothStoneVariants
Processor cobbleVariants();                    // CobbleVariants
Processor infestBlocks();                      // InfestBlocksProcessor
Processor blockRot(float integrity);           // vanilla BlockRotProcessor
Processor ignoreAir();                         // BlockIgnoreProcessor.AIR
/** TargetedRotProcessor: only exact states in blocksToRot may rot. */
Processor targetedRot(std::vector<BlockState*> blocksToRot, float integrity);
/** VerticalDecayProcessor (banisters draw from the cell below). */
Processor verticalDecay(std::vector<std::string> blockNames, float chance);
/** UpdateMarkingProcessor.forBlocks — marks the cell for post-processing. */
Processor updateMarking(std::vector<std::string> blockNames, WorldGenLevel* level);
/** CourtyardTerraceTemplateProcessor (reads the world at each cell). */
Processor courtyardTerrace(WorldGenLevel* level);
/** SoftReplaceProcessor (keeps solid world blocks under air/leaves). */
Processor softReplace(WorldGenLevel* level);

// ============================================================================
// Jigsaws (util/jigsaw/*.java). FrontAndTop as two directions.
// ============================================================================
struct FrontAndTop {
    core::Direction front = core::Direction::NORTH;
    core::Direction top = core::Direction::UP;
};
FrontAndTop rotateFrontAndTop(const FrontAndTop& orientation, int rotation);
/** JigsawUtil.getAbsoluteHorizontal. */
core::Direction absoluteHorizontal(const FrontAndTop& orientation);

/** JigsawRecord(priority, orientation, pos, pool, name, target). */
struct JigsawRecord {
    int priority = 0;
    FrontAndTop orientation;
    core::BlockPos pos{0, 0, 0};   // template-relative (configured)
    std::string pool;
    std::string name;
    std::string target;
};

/** JigsawPlaceContext. */
struct JigsawPlaceContext {
    core::BlockPos templatePos{0, 0, 0};
    int rotation = ROT_NONE;
    core::BlockPos pivot{0, 0, 0};
    JigsawRecord seedJigsaw;
    std::vector<JigsawRecord> spareJigsaws;
    std::string templateLocation;

    TemplatePlaceSettings settings() const;
    BoundingBox makeBoundingBox() const;
    const JigsawRecord* findFirst(const std::string& name) const;
};

/**
 * JigsawPlaceContext.pickPlaceableJunction(parentTemplatePos, sourceJigsawPos,
 * sourceOrientation, manager, templateLocation, jigsawNameLabel, random).
 * Empty templateLocation (Java null) returns nullopt without drawing.
 */
std::optional<JigsawPlaceContext> pickPlaceableJunction(const core::BlockPos& parentTemplatePos,
                                                        const core::BlockPos& sourceJigsawPos,
                                                        const FrontAndTop& sourceOrientation,
                                                        const std::string& templateLocation,
                                                        const std::string& jigsawNameLabel,
                                                        LegacyRandomSource& random);

// ============================================================================
// StructureTemplateDefinitions (data/twilightforest/twilight/
// template_definition/**): template id -> {pool id: TemplatePoolInstance}.
// ============================================================================
struct TemplatePoolInstance {
    int weight = 0;
    std::string terrainAdaptation = "none";
    bool ignoreWorldWaterlog = false;
    bool hasHeightAdjustment = false;
    std::string heightmap;
    int yOffset = 0;
    std::optional<int> groundJunctionDiffClamp;
    bool hasProcessors = false;              // "processors" / randomized ones
    std::string markerHandlers;
    std::unordered_map<std::string, std::string> poolAliases;
};

struct PoolEntry {
    std::string templateId;
    TemplatePoolInstance instance;
};

/** StructureTemplateDefinitions.getRandomEntry (WeightedList.getRandom). */
const PoolEntry* randomPoolEntry(LegacyRandomSource& random, const std::string& poolId);
/** getRandomTemplate: "" when the pool is missing/empty (Java null). */
std::string randomTemplate(LegacyRandomSource& random, const std::string& poolId);
/** getShuffledSequence: weighted reservoir sampling (-ln(u)/w ascending). */
std::vector<std::string> shuffledSequence(LegacyRandomSource& random, const std::string& poolId);

// ============================================================================
// WorldUtil.adjustForTerrain.
// ============================================================================
int adjustForTerrain(GenerationContext& ctx, int xMin, int zMin, int xMax, int zMax,
                     int gridLength, Heightmap::Types heightmapType);
int adjustForTerrain(GenerationContext& ctx, int xInCenterChunk, int zInCenterChunk,
                     int radiusFromCenterChunk, int gridLength);
/** ChunkGenerator.getFirstOccupiedHeight. */
int firstOccupiedHeight(GenerationContext& ctx, int x, int z, Heightmap::Types heightmapType);

// ============================================================================
// Piece records.
// ============================================================================
StructurePieceData makePiece(const std::string& pieceType, const BoundingBox& box,
                             int rotation, int genDepth, const std::string& detail);

/**
 * NeoForge PieceBeardifierModifier as the engine's Beardifier reads pieces:
 * NONE -> the piece contributes nothing (poolElement, non-rigid); anything
 * else -> a rigid box with the ground delta. The engine applies the
 * STRUCTURE's terrain adjustment kind to it (per-piece kinds are an engine
 * gap: BEARD_BOX / BURY pieces beard like the structure's BEARD_THIN).
 */
void applyBeardifierModifier(StructurePieceData& piece, bool adjusts, int groundLevelDelta);

// ============================================================================
// TwilightTemplateStructurePiece.
// ============================================================================
class TemplatePieceBehavior : public StructurePieceBehavior {
public:
    /** Builds the processor chain (after STRUCTURE_BLOCK ignore). */
    using ProcessorFactory = std::function<void(std::vector<Processor>&, WorldGenLevel*)>;

    struct Config {
        std::string templateId;
        int rotation = ROT_NONE;
        int mirror = 0;
        core::BlockPos pivot{0, 0, 0};
        core::BlockPos templatePosition{0, 0, 0};
        bool knownShape = true;          // TwilightTemplateStructurePiece sets it
        bool keepLiquids = true;         // LiquidSettings.APPLY_WATERLOGGING
        bool jigsawReplacement = false;  // JigsawReplacementProcessor
        int adjustY = 0;                 // placePieceAdjusted dY (0 = plain)
        ProcessorFactory processors;
    };

    explicit TemplatePieceBehavior(Config config) : m_config(std::move(config)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override;

    const Config& config() const { return m_config; }
    TemplatePlaceSettings makeSettings(WorldGenLevel* level) const;

protected:
    /** customPostProcess: place, then the data markers inside chunkBB. */
    void placeTemplate(WorldGenLevel* level, ChunkGenerator* generator,
                       WorldgenRandom& random, const BoundingBox& chunkBB,
                       const core::BlockPos& referencePos, StructurePieceData& self);

    /** TwilightTemplateStructurePiece.handleDataMarker (default no-op). */
    virtual void handleDataMarker(const std::string& label, const core::BlockPos& pos,
                                  WorldGenLevel* level, WorldgenRandom& random,
                                  const BoundingBox& chunkBB, ChunkGenerator* generator,
                                  int rotation) {
        (void)label; (void)pos; (void)level; (void)random; (void)chunkBB;
        (void)generator; (void)rotation;
    }

    Config m_config;
};

/** TwilightDoubleTemplateStructurePiece: the template, then an overlay. */
class DoubleTemplatePieceBehavior : public TemplatePieceBehavior {
public:
    DoubleTemplatePieceBehavior(Config base, std::string overlayId,
                                ProcessorFactory overlayProcessors)
        : TemplatePieceBehavior(std::move(base)), m_overlayId(std::move(overlayId)),
          m_overlayProcessors(std::move(overlayProcessors)) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override;

private:
    std::string m_overlayId;
    ProcessorFactory m_overlayProcessors;
};

// ============================================================================
// TwilightJigsawPiece — layout half (the Java object during generation).
// ============================================================================
struct JigsawPiece {
    std::string pieceType;
    std::string templateId;
    int genDepth = 0;
    JigsawPlaceContext context;
    BoundingBox boundingBox;

    const core::BlockPos& templatePosition() const { return context.templatePos; }
    int rotation() const { return context.rotation; }
    const JigsawRecord& sourceJigsaw() const { return context.seedJigsaw; }
    const std::vector<JigsawRecord>& spareJigsaws() const { return context.spareJigsaws; }
    core::BlockPos sourcePosition() const {
        return context.templatePos.offset(context.seedJigsaw.pos);
    }
    int firstMatchIndex(const std::function<bool(const JigsawRecord&)>& filter) const;
    std::vector<JigsawRecord> matchSpareJigsaws(
        const std::function<bool(const JigsawRecord&)>& filter) const;
};

/** TwilightJigsawPiece(type, genDepth, manager, template, context): bbox from the template. */
JigsawPiece makeJigsawPiece(const std::string& pieceType, int genDepth,
                            const std::string& templateId, const JigsawPlaceContext& context);

/**
 * TwilightJigsawPiece.addJigsaws' reseed:
 * random.setSeed(random.nextLong() ^ (seed * templatePosition.asLong())).
 */
void reseedForJigsaws(LegacyRandomSource& random, int64_t worldSeed,
                      const core::BlockPos& templatePosition);

/** TwilightJigsawPiece.postProcess placement (template + marker handlers). */
class JigsawPieceBehavior : public TemplatePieceBehavior {
public:
    JigsawPieceBehavior(const JigsawPiece& piece, ProcessorFactory processors,
                        bool keepLiquids = true);
    const JigsawPlaceContext& jigsawContext() const { return m_context; }

protected:
    JigsawPlaceContext m_context;
};

} // namespace twilight_template
} // namespace structure
} // namespace levelgen
} // namespace minecraft
