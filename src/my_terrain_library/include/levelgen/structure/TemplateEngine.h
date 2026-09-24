#pragma once

#include "levelgen/structure/StructureStartData.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace minecraft {
namespace nbt {
class CompoundTag;
}
}

// Reference: net/minecraft/world/level/levelgen/structure/templatesystem/
// StructureTemplate.java - the full-block half of the template system (B6).
// JigsawTemplates.h keeps the layout-only jigsaw extraction; this module
// loads complete block lists with resolved BlockStates and implements
// placeInWorld (processors, waterlogging, neighbor shape updates).

namespace minecraft {

namespace world { namespace level { namespace block {
class Block;
namespace state { class BlockState; }
}}}
using BlockState = world::level::block::state::BlockState;

namespace levelgen {
class WorldGenLevel;
class WorldgenRandom;
namespace structure {

/** One entry of the NBT "blocks" list. All palettes share positions and
 *  state indices; the chosen palette maps stateIdx to a BlockState. */
struct TemplateBlockInfo {
    int32_t x, y, z;
    int32_t stateIdx;
    bool hasNbt = false;
    bool isDataMarker = false;      // structure_block with mode DATA
    std::string metadata;           // data marker string
    std::string jigsawFinalState;   // jigsaw final_state ("" when not jigsaw)
    bool isLootContainer = false;   // RandomizableContainer -> seed draw
    // Full template BE nbt (B8), retained when hasNbt; shared across the
    // cached FullTemplateData copies.
    std::shared_ptr<nbt::CompoundTag> nbt;
};

/** One entry of the NBT "entities" list. Reference:
 *  StructureTemplate.StructureEntityInfo - pos (template-local, fractional),
 *  blockPos (the block it stands in) and the saved entity compound. */
struct TemplateEntityInfo {
    double x = 0.0, y = 0.0, z = 0.0;
    int32_t blockX = 0, blockY = 0, blockZ = 0;
    std::shared_ptr<nbt::CompoundTag> nbt;
};

struct FullTemplateData {
    int32_t sizeX = 0, sizeY = 0, sizeZ = 0;
    // Palette states resolved against the C++ registry (throws if a palette
    // entry's block/property set cannot be resolved - silent air would be an
    // invisible parity hole).
    std::vector<std::vector<BlockState*>> palettes;
    std::vector<TemplateBlockInfo> blocks;
    // Reference: StructureTemplate.entityInfoList (loaded from "entities";
    // entries without an "nbt" compound are skipped, as Java's ifPresent).
    std::vector<TemplateEntityInfo> entities;

    bool empty() const { return palettes.empty(); }
};

/** Reference: StructurePlaceSettings - the fields placement needs. */
struct TemplatePlaceSettings {
    int rotation = 0;               // Rot ordinal (0..3)
    // StructurePlaceSettings.setRandom(r): the palette comes from that random
    // (drawn by the caller, e.g. TemplateFeature); -1 = seeded by position.
    int paletteIndex = -1;
    int mirror = 0;                 // Mirror ordinal (0..2)
    core::BlockPos rotationPivot{0, 0, 0};
    bool ignoreAir = false;         // BlockIgnoreProcessor.STRUCTURE_AND_AIR
    bool keepLiquids = true;        // LiquidSettings.APPLY_WATERLOGGING
    // Extra processors (BlockRot etc.) as a callable chain; nullptr entry
    // drops the block. Applied in order after the ignore processor.
    // Signature: (worldPos, state, templateLocalPos, originalState,
    //             structureReferencePos) -> state or nullptr.
    //
    // originalState is the raw palette state before jigsaw replacement (Java
    // passes originalBlockInfo through the whole chain; BlockRotProcessor's
    // rottable_blocks tag tests it, not the processed state).
    //
    // structureReferencePos is Java's `pos` argument to
    // StructureProcessor.processBlock — the structure's reference position, as
    // distinct from `worldPos` (this block) and `templateLocalPos` (this block
    // inside the template). Only the positional rule predicates read it, and
    // they are the reason it is here: AxisAlignedLinearPosTest measures how far
    // a block is from the structure's origin along one axis, which is
    // unanswerable from the other three.
    std::vector<std::function<BlockState*(const core::BlockPos&, BlockState*,
                                          const core::BlockPos&, BlockState*,
                                          const core::BlockPos&)>>
        processors;

    // Reference: CappedProcessor(RuleProcessor(BlockMatch from -> to),
    // ConstantInt limit) - finalizeProcessing: positional random at the
    // template position picks up to `limit` matching survivors (shuffled
    // index order) and replaces their state (ocean ruin archaeology).
    struct CappedReplace {
        std::string fromBlock;   // exact block match ("" when fromTag set)
        std::string fromTag;     // block tag name (trail_ruins archaeology)
        std::string toBlock;     // default state of this block ("" when toState)
        BlockState* toState = nullptr;  // exact output state (overrides toBlock)
        int limit = 5;
        // B8: the delegate rule's append_loot modifier - each replaced block
        // gets {LootTable, LootTableSeed} with seed = the FIRST nextLong of
        // LegacyRandomSource(Mth.getSeed(worldPos)) (the RuleProcessor's
        // per-block positional random; the rule tests draw nothing).
        std::string lootTable;   // "" = no BE payload
        std::string beId = "minecraft:brushable_block";
    };
    std::vector<CappedReplace> cappedReplaces;

    // Reference: RuleProcessor(ProcessorRule(BlockMatchTest(block),
    // AlwaysTrueTest, PosAlwaysTrueTest, block.defaultBlockState(),
    // AppendLoot(lootTable))) - every matching block becomes its default state
    // with {LootTable, LootTableSeed}; seed = the first nextLong of
    // LegacyRandomSource(Mth.getSeed(worldPos)) (the desert well's suspicious
    // sand).
    struct AppendLootRule {
        std::string block;
        std::string lootTable;
        std::string beId = "minecraft:brushable_block";
    };
    std::vector<AppendLootRule> appendLootRules;

    // Reference: StructurePlaceSettings.setKnownShape(true) - pool elements
    // skip BOTH shape-update passes entirely.
    bool knownShape = false;

    // Reference: StructurePlaceSettings.ignoreEntities (default false) -
    // WoodlandMansionPiece and EndCityPiece set it; everything else places
    // the template's entities (placeEntities).
    bool ignoreEntities = false;
    // Reference: StructurePlaceSettings.finalizeEntities (default false) -
    // SinglePoolElement.getSettings sets it, so jigsaw-placed mobs (village
    // villagers, golems, cats, animals; bastion piglins and hoglins; outpost
    // cage golems and allays) get finalizeSpawn(STRUCTURE). Template pieces
    // (the igloo's villager and zombie villager) do not.
    bool finalizeEntities = false;

    // Reference: JigsawReplacementProcessor - jigsaw blocks with nbt become
    // their parsed final_state (default "minecraft:air"); a structure_void
    // result drops the block. Applied AFTER the ignore processor and BEFORE
    // the `processors` lambdas.
    bool jigsawReplacement = false;

    // Reference: GravityProcessor (TERRAIN_MATCHING projection) - applied
    // AFTER the `processors` lambdas: final y = heightmap(worldX, worldZ) +
    // gravityOffset + ORIGINAL (untransformed) template-local y. The rule
    // lambdas see the PRE-gravity world position (their positional randoms
    // seed from it, matching Java's processor order).
    bool terrainMatchingGravity = false;
    int gravityOffset = -1;
};

namespace TemplateEngine {

/** Cached full-template load. Missing file = empty template (vanilla
 *  getOrCreate semantics). Throws on malformed data. */
const FullTemplateData& get(const std::string& templateId);

/**
 * Reference: StructureTemplate.placeInWorld(level, position, referencePos,
 * settings, random, 2). chunkBB is the settings bounding box. Returns false
 * for empty templates. `random` draws the loot-table seeds.
 */
bool placeInWorld(WorldGenLevel* level, const std::string& templateId,
                  const core::BlockPos& position, const core::BlockPos& referencePos,
                  const TemplatePlaceSettings& settings, WorldgenRandom& random,
                  const BoundingBox& chunkBB);

/** Reference: StructureTemplate.transform(Vec3, mirror, rotation, pivot) -
 *  the fractional twin of calculateRelativePosition (mirror flips about the
 *  block's far face: 1 - x, and rotations add the +1). */
void transformVec(const TemplatePlaceSettings& settings, double& x, double& y, double& z);

/** Reference: StructureTemplate.calculateRelativePosition (transform of a
 *  template-local pos with the settings' mirror/rotation/pivot). */
core::BlockPos calculateRelativePosition(const TemplatePlaceSettings& settings,
                                         const core::BlockPos& localPos);

/** Data markers (structure_block mode DATA) of the chosen palette, with
 *  world positions. Reference: template.filterBlocks(..., STRUCTURE_BLOCK) -
 *  Java's filterBlocks clips to settings.getBoundingBox() (the chunkBB set by
 *  TemplateStructurePiece.postProcess), so markers outside the decorating
 *  chunk's writable box never fire for that chunk's pass (a multi-chunk piece
 *  fires each marker exactly once, in the pass of the chunk containing it). */
struct DataMarker {
    core::BlockPos pos;
    std::string metadata;
};
std::vector<DataMarker> dataMarkers(const std::string& templateId,
                                    const core::BlockPos& position,
                                    const TemplatePlaceSettings& settings,
                                    const BoundingBox& chunkBB);

/**
 * B8: canonical save-format E payload for a template-placed block entity
 * (load->save round trip modeled per type; see B8_BE_INVENTORY.md). Returns
 * "" for unmodeled types (the pending {id:"DUMMY"} tag then remains).
 * `placementRotation` is the piece's rotation ordinal (0..3): engine block
 * entities that must know how their structure was turned record it (the
 * Aurelith resonance engine's Rotation — see its branch).
 */
std::string blockEntityPayloadFor(const std::string& blockId,
                                  const nbt::CompoundTag* templateNbt,
                                  std::optional<int64_t> lootSeed,
                                  std::optional<int> placementRotation = std::nullopt);

/**
 * Reference: TemplateStructurePiece.postProcess jigsaw pass - every jigsaw
 * block of the template becomes its final_state (parsed "block[k=v,...]"),
 * set with flag 3, unclipped. Call after dataMarkers.
 */
void applyJigsawFinalStates(WorldGenLevel* level, const std::string& templateId,
                            const core::BlockPos& position,
                            const TemplatePlaceSettings& settings);

/**
 * Reference: BlockState.updateShape dispatch for structure-placed block
 * families (walls, fences, panes, stairs, torches, doors, ...). Exposed for
 * TreeFeature's updateShapeAtEdge sweep, which in Java runs the same
 * per-block virtuals when a tree's shape borders structure blocks.
 */
BlockState* updateShapeForBlock(BlockState* state, WorldGenLevel* level,
                                const core::BlockPos& pos, core::Direction dir,
                                const core::BlockPos& neighborPos,
                                BlockState* neighborState);

/**
 * A "namespace:block[k=v,...]" spec resolved like BlockStateParser: listed
 * properties over the block's default state. Throws for an unregistered
 * block or an unknown property value. Exposed for code-placed engine pieces
 * (AurelithOutskirts' roads) that name their blocks the way templates do.
 */
BlockState* parseBlockStateSpec(const std::string& spec);

} // namespace TemplateEngine

} // namespace structure
} // namespace levelgen
} // namespace minecraft
