#include "data/worldgen/features/TwilightTemplateFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "TwilightDecorCommon.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "random/XoroshiroRandomSource.h"
#include "random/LegacyRandomSource.h"
#include "world/IChunk.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — the NBT-template features
// (world/components/feature/templates/*.java) and the structure processors
// they use (world/components/processors/*.java, config/SwizzleConfig.java,
// util/woods/WoodPalette.java). Templates: data/twilightforest/structure/
// feature/{druid_hut,well,ruins,graveyard}/*.nbt through TemplateEngine.
//
// Random streams. TemplateFeature draws everything from world.getRandom()
// (the WorldGenRegion random), and StructurePlaceSettings.setRandom(random)
// makes the processors' settings.getRandom(pos) that same stream — so
// CobbleVariants / StoneBricksVariants reseed the region random block by
// block, exactly as in the mod. GraveyardFeature uses the feature random the
// same way. Java's placeInWorld and filterBlocks each draw
// getRandomPalette(...).nextInt(paletteCount) from it, and placeInWorld draws
// a LootTableSeed nextLong for every placed RandomizableContainer; the
// library's TemplateEngine picks palettes positionally and draws loot seeds
// from the random it is handed, so those draws are made here on the mod's
// stream (TemplateEngine gets a scratch random) and the container payloads
// are rewritten with the mod's seeds.
//
// TemplateEngine always drops structure blocks before the processor chain
// (BlockIgnoreProcessor.STRUCTURE_BLOCK); the mod's TemplateFeature has no
// ignore processor, so its processors also see (and draw for) the data
// markers. placeTemplate() replays the chain for every skipped marker in
// template order so the draw count matches; the markers themselves are
// replaced by processMarkers right after, as in the mod.
//
// Wood palettes (data/.../twilight/wood_palettes/*.json): a TF palette member
// the engine does not register resolves through TwilightBlocks; when that
// yields a block of another shape (the generic stone fallback), the oak
// member of the same shape is used instead so a swizzled hut keeps working
// doors, fences and buttons.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace twilight_decor;
using levelgen::placement::PlacedFeature;
using levelgen::structure::BoundingBox;
using levelgen::structure::FullTemplateData;
using levelgen::structure::TemplateBlockInfo;
using levelgen::structure::TemplatePlaceSettings;
namespace TemplateEngine = levelgen::structure::TemplateEngine;

namespace {

// Rotation / Mirror ordinals (Rotation.values(), Mirror.values()).
constexpr int kRotNone = 0, kRotCW90 = 1, kRotCW180 = 2, kRotCCW90 = 3;
constexpr int kMirrorNone = 0, kMirrorLeftRight = 1, kMirrorFrontBack = 2;

const std::string kLootBasement = "twilightforest:chests/basement";
const std::string kLootHutJunk = "twilightforest:chests/hut_junk";
const std::string kLootWell = "twilightforest:well";
const std::string kLootFancyWell = "twilightforest:fancy_well";
const std::string kLootGraveyard = "twilightforest:graveyard";

// ============================================================================
// Resolved blocks
// ============================================================================
struct TemplateBlocks {
    BlockState* air = nullptr;
    BlockState* spawner = nullptr;
    BlockState* cobblestone = nullptr;
    BlockState* mossyCobblestone = nullptr;
    BlockState* mossyCobblestoneStairs = nullptr;
    BlockState* mossyCobblestoneSlab = nullptr;
    BlockState* mossyCobblestoneWall = nullptr;
    BlockState* mossyStoneBricks = nullptr;
    BlockState* crackedStoneBricks = nullptr;
    BlockState* mossyStoneBrickStairs = nullptr;
    BlockState* mossyStoneBrickSlab = nullptr;
    BlockState* mossyStoneBrickWall = nullptr;
    BlockState* podzol = nullptr;
    BlockState* mycelium = nullptr;
    BlockState* dirtPath = nullptr;
    BlockState* coarseDirt = nullptr;
    BlockState* uberousSoil = nullptr;      // real TF block only (SmartGrassProcessor)
    BlockState* cobweb = nullptr;
    // [trapped][ChestType single/left/right][Direction ordinal]
    std::array<std::array<std::array<BlockState*, 6>, 3>, 2> chests{};
    std::array<BlockState*, 6> barrels{};   // by Direction ordinal
    std::array<BlockState*, 6> hoppers{};   // by Direction ordinal (no UP)
    BlockState* graveyardChest = nullptr;   // trapped_chest[facing=west]

    bool complete() const {
        return air && spawner && cobblestone && mossyCobblestone && mossyCobblestoneStairs && mossyCobblestoneSlab
            && mossyCobblestoneWall && mossyStoneBricks && crackedStoneBricks && mossyStoneBrickStairs
            && mossyStoneBrickSlab && mossyStoneBrickWall && podzol && mycelium && dirtPath && coarseDirt
            && cobweb && graveyardChest;
    }
};

TemplateBlocks s_blocks;

constexpr core::Direction kDirections[6] = {
    core::Direction::DOWN, core::Direction::UP, core::Direction::NORTH,
    core::Direction::SOUTH, core::Direction::WEST, core::Direction::EAST};

bool isBlock(BlockState* state, const char* id) {
    return state != nullptr && state->getIdentifier() == id;
}

bool sameBlock(BlockState* a, BlockState* b) {
    return a != nullptr && b != nullptr && a->getBlock() == b->getBlock();
}

// ============================================================================
// StatsAccumulator (Guava) — population standard deviation, mean, max
// ============================================================================
struct StatsAccumulator {
    int64_t count = 0;
    double mean = 0.0;
    double sumOfSquaresOfDeltas = 0.0;
    double maxValue = 0.0;

    void add(double value) {
        if (count == 0) {
            count = 1;
            mean = value;
            maxValue = value;
            return;
        }
        ++count;
        const double delta = value - mean;
        mean += delta / static_cast<double>(count);
        sumOfSquaresOfDeltas += delta * (value - mean);
        maxValue = std::max(maxValue, value);
    }

    double populationStandardDeviation() const {
        if (count <= 1) return 0.0;
        return std::sqrt(std::max(0.0, sumOfSquaresOfDeltas) / static_cast<double>(count));
    }
};

// StructureTemplate.getZeroPositionWithTransform(zeroPos, mirror, rotation, sizeX, sizeZ)
core::BlockPos zeroPositionWithTransform(const core::BlockPos& zeroPos, int mirror, int rotation, int sizeX, int sizeZ) {
    --sizeX;
    --sizeZ;
    const int mirrorDeltaX = mirror == kMirrorFrontBack ? sizeX : 0;
    const int mirrorDeltaZ = mirror == kMirrorLeftRight ? sizeZ : 0;
    switch (rotation) {
        case kRotCCW90: return zeroPos.offset(mirrorDeltaZ, 0, sizeX - mirrorDeltaX);
        case kRotCW90:  return zeroPos.offset(sizeZ - mirrorDeltaZ, 0, mirrorDeltaX);
        case kRotCW180: return zeroPos.offset(sizeX - mirrorDeltaX, 0, sizeZ - mirrorDeltaZ);
        default:        return zeroPos.offset(mirrorDeltaX, 0, mirrorDeltaZ);
    }
}

// StructureTemplate.getSize(rotation)
core::BlockPos rotatedSize(const FullTemplateData& data, int rotation) {
    if (rotation == kRotCW90 || rotation == kRotCCW90) return core::BlockPos(data.sizeZ, data.sizeY, data.sizeX);
    return core::BlockPos(data.sizeX, data.sizeY, data.sizeZ);
}

// ============================================================================
// Processor chain
// ============================================================================
struct BlockContext {
    size_t index;                    // template block index (palette order)
    core::BlockPos worldPos;         // processed block position
    core::BlockPos structurePos;     // processBlockInfos' `position` argument
};

// (context, processed state, original template state) -> state or null (drop)
using Processor = std::function<BlockState*(const BlockContext&, BlockState*, BlockState*)>;
using Chain = std::vector<Processor>;

void warnMultiPalette(const std::string& templateId) {
    static std::once_flag s_once;
    std::call_once(s_once, [&templateId] {
        std::fprintf(stderr, "[TwilightTemplateFeatures] %s has several palettes; TemplateEngine picks "
                             "positionally where the mod draws from the feature random\n", templateId.c_str());
    });
}

/**
 * StructureTemplate.placeInWorld(level, position, position, settings, random,
 * flags) on the mod's random stream: the palette draw, the processor chain
 * over EVERY template block in palette order (markers included), the block
 * placement through TemplateEngine, then the LootTableSeed draws for the
 * placed containers. `onPlaced` runs after TemplateEngine placed the blocks
 * and before the loot draws (processor side effects such as the graveyard's
 * relocated cobwebs).
 */
template <typename Rng>
void placeTemplate(WorldGenLevel& level, const std::string& templateId, const core::BlockPos& position,
                   int rotation, int mirror, const BoundingBox& bb, const Chain& chain, Rng& random,
                   const std::function<void()>& onPlaced = {}) {
    const FullTemplateData& data = TemplateEngine::get(templateId);
    if (data.palettes.empty()) return;
    // settings.getRandomPalette(palettes, position)
    (void)random.nextInt(static_cast<int32_t>(data.palettes.size()));
    if (data.palettes.size() > 1) warnMultiPalette(templateId);
    const std::vector<BlockState*>& palette = data.palettes[0];
    if (data.blocks.empty() || data.sizeX < 1 || data.sizeY < 1 || data.sizeZ < 1) return;

    TemplatePlaceSettings settings;
    settings.rotation = rotation;
    settings.mirror = mirror;

    const size_t n = data.blocks.size();
    auto packLocal = [](int x, int y, int z) {
        return (static_cast<int64_t>(x & 0x1FFFFF) << 42) | (static_cast<int64_t>(y & 0x1FFFFF) << 21)
             | static_cast<int64_t>(z & 0x1FFFFF);
    };
    std::unordered_map<int64_t, size_t> indexOf;
    indexOf.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const TemplateBlockInfo& info = data.blocks[i];
        indexOf.emplace(packLocal(info.x, info.y, info.z), i);
    }
    auto worldPosOf = [&](size_t i) {
        const TemplateBlockInfo& info = data.blocks[i];
        return TemplateEngine::calculateRelativePosition(settings, core::BlockPos(info.x, info.y, info.z))
            .offset(position.getX(), position.getY(), position.getZ());
    };
    auto isStructureBlock = [&](size_t i) {
        return isBlock(palette[static_cast<size_t>(data.blocks[i].stateIdx)], "minecraft:structure_block");
    };
    std::vector<char> dropped(n, 0);
    auto runChain = [&](size_t i, const core::BlockPos& worldPos, BlockState* state) -> BlockState* {
        BlockState* original = palette[static_cast<size_t>(data.blocks[i].stateIdx)];
        const BlockContext context{i, worldPos, position};
        for (const Processor& processor : chain) {
            state = processor(context, state, original);
            if (state == nullptr) {
                dropped[i] = 1;
                return nullptr;
            }
        }
        return state;
    };
    size_t cursor = 0;
    auto replaySkippedUpTo = [&](size_t end) {
        for (; cursor < end && cursor < n; ++cursor) {
            if (isStructureBlock(cursor)) {
                runChain(cursor, worldPosOf(cursor), palette[static_cast<size_t>(data.blocks[cursor].stateIdx)]);
            }
        }
    };

    settings.processors.push_back(
        [&](const core::BlockPos& worldPos, BlockState* state, const core::BlockPos& localPos,
            BlockState* /*originalState*/, const core::BlockPos& /*referencePos*/) -> BlockState* {
            auto it = indexOf.find(packLocal(localPos.getX(), localPos.getY(), localPos.getZ()));
            if (it == indexOf.end()) return state;
            const size_t index = it->second;
            if (index >= cursor) replaySkippedUpTo(index);
            cursor = std::max(cursor, index + 1);
            return runChain(index, worldPos, state);
        });

    WorldgenRandom scratch(static_cast<int64_t>(0));
    TemplateEngine::placeInWorld(&level, templateId, position, position, settings, scratch, bb);
    replaySkippedUpTo(n);

    if (onPlaced) onPlaced();

    // Placement loop: RandomizableContainer -> LootTableSeed = random.nextLong()
    for (size_t i = 0; i < n; ++i) {
        const TemplateBlockInfo& info = data.blocks[i];
        if (!info.hasNbt || !info.isLootContainer || dropped[i] || isStructureBlock(i)) continue;
        const core::BlockPos pos = worldPosOf(i);
        if (!bb.isInside(pos.getX(), pos.getY(), pos.getZ())) continue;
        BlockState* placed = level.getBlockState(pos);
        if (!sameBlock(placed, palette[static_cast<size_t>(info.stateIdx)])) continue;
        const int64_t seed = random.nextLong();
        const std::string payload = TemplateEngine::blockEntityPayloadFor(
            placed->getBlock()->getIdentifier(), info.nbt.get(), seed);
        if (payload.empty()) continue;
        if (::world::IChunk* chunk = level.getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
            chunk->setBlockEntityNbt(pos, payload);
        }
    }
}

/** StructureTemplate.filterBlocks(position, settings, STRUCTURE_BLOCK): the
 *  palette draw, then the DATA markers inside the settings' bounding box. */
template <typename Rng>
std::vector<TemplateEngine::DataMarker> templateMarkers(const std::string& templateId, const core::BlockPos& position,
                                                        int rotation, int mirror, const BoundingBox& bb, Rng& random) {
    const FullTemplateData& data = TemplateEngine::get(templateId);
    if (data.palettes.empty()) return {};
    (void)random.nextInt(static_cast<int32_t>(data.palettes.size()));
    TemplatePlaceSettings settings;
    settings.rotation = rotation;
    settings.mirror = mirror;
    return TemplateEngine::dataMarkers(templateId, position, settings, bb);
}

// ============================================================================
// Wood palettes — util/woods/WoodPalette.java
// ============================================================================
enum WoodShape { PLANKS = 0, STAIRS, SLAB, BUTTON, FENCE, GATE, PLATE, BANISTER, SHAPE_COUNT };

struct WoodPalette {
    std::string name;
    std::array<std::string, SHAPE_COUNT> ids;       // resolved block ids
    std::array<BlockState*, SHAPE_COUNT> states{};  // default states

    // getWoodShapeFromBlock: first matching member in declaration order
    int shapeOf(BlockState* state) const {
        if (state == nullptr) return -1;
        const std::string& id = state->getIdentifier();
        for (int shape = 0; shape < SHAPE_COUNT; ++shape) {
            if (!ids[static_cast<size_t>(shape)].empty() && ids[static_cast<size_t>(shape)] == id) return shape;
        }
        return -1;
    }

    // modifyBlockWithType(targetPalette, state)
    BlockState* modifyBlockWithType(const WoodPalette& target, BlockState* state) const {
        const int shape = target.shapeOf(state);
        if (shape < 0) return state;
        BlockState* replacement = states[static_cast<size_t>(shape)];
        if (replacement == nullptr) return state;
        if (shape == PLANKS) return replacement;
        return transferAllStateKeys(state, replacement);
    }
};

std::vector<std::unique_ptr<WoodPalette>> s_palettes;
std::unordered_map<std::string, const WoodPalette*> s_paletteByName;

bool endsWith(const std::string& s, const char* suffix) {
    const std::string x(suffix);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

const WoodPalette* buildPalette(const std::string& name, const std::array<const char*, SHAPE_COUNT>& members) {
    static const char* kOakMembers[SHAPE_COUNT] = {
        "minecraft:oak_planks", "minecraft:oak_stairs", "minecraft:oak_slab", "minecraft:oak_button",
        "minecraft:oak_fence", "minecraft:oak_fence_gate", "minecraft:oak_pressure_plate", "minecraft:oak_fence"};
    static const char* kShapeSuffix[SHAPE_COUNT] = {
        "_planks", "_stairs", "_slab", "_button", "_fence", "_fence_gate", "_pressure_plate", "_fence"};

    auto palette = std::make_unique<WoodPalette>();
    palette->name = name;
    for (int shape = 0; shape < SHAPE_COUNT; ++shape) {
        const std::string member = members[static_cast<size_t>(shape)];
        std::string id = levelgen::twilight_blocks::resolveName(member);
        const bool shapeOk = !id.empty()
            && (endsWith(id, kShapeSuffix[shape]) || (shape == BANISTER && endsWith(id, "_banister"))
                || (shape == PLANKS && endsWith(id, "_planks")));
        if (!shapeOk) {
            std::fprintf(stderr, "[TwilightTemplateFeatures] wood palette %s: %s -> %s (oak %s shape)\n",
                         name.c_str(), member.c_str(), kOakMembers[shape], kShapeSuffix[shape] + 1);
            id = levelgen::twilight_blocks::resolveName(kOakMembers[shape]);
        }
        palette->ids[static_cast<size_t>(shape)] = id;
        palette->states[static_cast<size_t>(shape)] =
            id.empty() ? nullptr : world::level::block::Blocks::getDefaultState(id);
    }
    const WoodPalette* raw = palette.get();
    s_paletteByName[name] = raw;
    s_palettes.push_back(std::move(palette));
    return raw;
}

void buildPalettes() {
    // data/{minecraft,twilightforest}/twilight/wood_palettes/*.json
    // Order of members: planks, stairs, slab, button, fence, gate, plate, banister.
    auto vanilla = [](const char* wood, const char* banister) {
        const std::string w(wood);
        const std::string prefix = "minecraft:" + w;
        static std::vector<std::unique_ptr<std::array<std::string, SHAPE_COUNT>>> s_names;
        auto names = std::make_unique<std::array<std::string, SHAPE_COUNT>>();
        (*names)[PLANKS] = prefix + "_planks";
        (*names)[STAIRS] = prefix + "_stairs";
        (*names)[SLAB] = prefix + "_slab";
        (*names)[BUTTON] = prefix + "_button";
        (*names)[FENCE] = prefix + "_fence";
        (*names)[GATE] = prefix + "_fence_gate";
        (*names)[PLATE] = prefix + "_pressure_plate";
        (*names)[BANISTER] = banister;
        std::array<const char*, SHAPE_COUNT> members{};
        for (int i = 0; i < SHAPE_COUNT; ++i) members[static_cast<size_t>(i)] = (*names)[static_cast<size_t>(i)].c_str();
        buildPalette("minecraft:" + w, members);
        s_names.push_back(std::move(names));
    };
    auto twilight = [](const char* paletteName, const char* wood) {
        const std::string w(wood);
        const std::string prefix = "twilightforest:" + w;
        static std::vector<std::unique_ptr<std::array<std::string, SHAPE_COUNT>>> s_names;
        auto names = std::make_unique<std::array<std::string, SHAPE_COUNT>>();
        (*names)[PLANKS] = prefix + "_planks";
        (*names)[STAIRS] = prefix + "_stairs";
        (*names)[SLAB] = prefix + "_slab";
        (*names)[BUTTON] = prefix + "_button";
        (*names)[FENCE] = prefix + "_fence";
        (*names)[GATE] = prefix + "_fence_gate";
        (*names)[PLATE] = prefix + "_pressure_plate";
        (*names)[BANISTER] = prefix + "_banister";
        std::array<const char*, SHAPE_COUNT> members{};
        for (int i = 0; i < SHAPE_COUNT; ++i) members[static_cast<size_t>(i)] = (*names)[static_cast<size_t>(i)].c_str();
        buildPalette(std::string("twilightforest:") + paletteName, members);
        s_names.push_back(std::move(names));
    };
    vanilla("oak", "twilightforest:oak_banister");
    vanilla("spruce", "twilightforest:spruce_banister");
    vanilla("birch", "twilightforest:birch_banister");
    vanilla("jungle", "twilightforest:jungle_banister");
    twilight("canopy", "canopy");
    twilight("darkwood", "dark");
    twilight("twilight_oak", "twilight_oak");
    twilight("mangrove", "mangrove");
    twilight("timewood", "time");
    twilight("transwood", "transformation");
    twilight("minewood", "mining");
    twilight("sortwood", "sorting");
}

const WoodPalette* palette(const char* name) {
    auto it = s_paletteByName.find(name);
    return it == s_paletteByName.end() ? nullptr : it->second;
}

// ============================================================================
// StateTransfiguringProcessor — processors/StateTransfiguringProcessor.java
// (rules: random_block_match input, always_true location)
// ============================================================================
struct TransfigRule {
    std::string inputId;        // RandomBlockMatchTest.block
    float probability;          // RandomBlockMatchTest.probability
    BlockState* output;         // ProcessorRule output_state (a template for transferAllStateKeys)
};

Processor stateTransfiguring(const std::vector<TransfigRule>* rules) {
    return [rules](const BlockContext& context, BlockState* state, BlockState*) -> BlockState* {
        // RandomSource.create(Mth.getSeed(pos)); i = nextLong(); per rule
        // setSeed(i * 3), i += 115, then the rule test.
        LegacyRandomSource random(Mth::getSeed(context.worldPos.getX(), context.worldPos.getY(), context.worldPos.getZ()));
        uint64_t i = static_cast<uint64_t>(random.nextLong());
        for (const TransfigRule& rule : *rules) {
            random.setSeed(static_cast<int64_t>(i * 3u));
            i += 115u;
            if (state != nullptr && state->getIdentifier() == rule.inputId && random.nextFloat() < rule.probability) {
                return transferAllStateKeys(state, rule.output);
            }
        }
        return state;
    };
}

// ============================================================================
// WoodPaletteSwizzle — processors/WoodPaletteSwizzle.java
// ============================================================================
Processor woodPaletteSwizzle(const WoodPalette* target, const WoodPalette* replacement) {
    return [target, replacement](const BlockContext&, BlockState* state, BlockState*) -> BlockState* {
        if (target == nullptr || replacement == nullptr) return state;
        return replacement->modifyBlockWithType(*target, state);
    };
}

// ============================================================================
// CobbleVariants / StoneBricksVariants — processors/{CobbleVariants,
// StoneBricksVariants}.java. settings.getRandom(pos) is the placement random.
// ============================================================================
template <typename Rng>
Processor cobbleVariants(Rng& random) {
    return [&random](const BlockContext&, BlockState* state, BlockState*) -> BlockState* {
        random.setSeed(static_cast<int64_t>(static_cast<uint64_t>(random.nextLong()) * 2u));
        if (isBlock(state, "minecraft:cobblestone") && random.nextBoolean())
            return s_blocks.mossyCobblestone;
        if (isBlock(state, "minecraft:cobblestone_stairs") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyCobblestoneStairs);
        if (isBlock(state, "minecraft:cobblestone_slab") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyCobblestoneSlab);
        if (isBlock(state, "minecraft:cobblestone_wall") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyCobblestoneWall);
        return state;
    };
}

template <typename Rng>
Processor stoneBricksVariants(Rng& random) {
    return [&random](const BlockContext&, BlockState* state, BlockState*) -> BlockState* {
        random.setSeed(static_cast<int64_t>(static_cast<uint64_t>(random.nextLong()) * 3u));
        if (isBlock(state, "minecraft:stone_bricks") && random.nextBoolean())
            return random.nextBoolean() ? s_blocks.mossyStoneBricks : s_blocks.crackedStoneBricks;
        if (isBlock(state, "minecraft:stone_brick_stairs") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyStoneBrickStairs);
        if (isBlock(state, "minecraft:stone_brick_slab") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyStoneBrickSlab);
        if (isBlock(state, "minecraft:stone_brick_wall") && random.nextBoolean())
            return transferAllStateKeys(state, s_blocks.mossyStoneBrickWall);
        return state;
    };
}

// ============================================================================
// SmartGrassProcessor — processors/SmartGrassProcessor.java
// ============================================================================
Processor smartGrass(WorldGenLevel& level) {
    return [&level](const BlockContext& context, BlockState* state, BlockState* original) -> BlockState* {
        if (!isBlock(original, "minecraft:grass_block")) return state;
        const core::BlockPos& pos = context.worldPos;
        if (isSubstrateOverworld(level.getBlockState(pos)) || !level.isEmptyBlock(pos.above())) return nullptr;

        // RotationUtil.CARDINALS: north, south, east, west
        static const core::Direction kCardinals[4] = {
            core::Direction::NORTH, core::Direction::SOUTH, core::Direction::EAST, core::Direction::WEST};
        for (core::Direction direction : kCardinals) {
            BlockState* at = level.getBlockState(pos.relative(direction));
            if (at == nullptr) continue;
            if (isBlock(at, "minecraft:podzol")) return s_blocks.podzol;
            if (isBlock(at, "minecraft:grass_block")) return state;
            if (isBlock(at, "minecraft:mycelium")) return s_blocks.mycelium;
            if (isBlock(at, "minecraft:dirt_path")) return s_blocks.dirtPath;
            if (isBlock(at, "minecraft:coarse_dirt")) return s_blocks.coarseDirt;
            if (s_blocks.uberousSoil != nullptr && sameBlock(at, s_blocks.uberousSoil)) return s_blocks.uberousSoil;
        }
        return state;
    };
}

// ============================================================================
// SwizzleConfig — feature/config/SwizzleConfig.java
// ============================================================================
struct SwizzleConfig {
    std::vector<const WoodPalette*> targets;                        // target_palettes (tag order)
    std::vector<std::pair<std::vector<const WoodPalette*>, int>> paletteChoices;   // (HolderSet, outer weight)
    std::vector<TransfigRule> preprocessingRules;

    // buildAddProcessors(settings, random)
    template <typename Rng>
    void buildAddProcessors(Chain& chain, Rng& random) const {
        if (!preprocessingRules.empty()) chain.push_back(stateTransfiguring(&preprocessingRules));
        int total = 0;
        for (const auto& choice : paletteChoices) total += choice.second;
        for (const WoodPalette* target : targets) {
            // paletteChoices().getRandom(random).get().value().getRandomElement(random).get()
            const WoodPalette* replacement = nullptr;
            if (total > 0) {
                int selection = random.nextInt(total);
                for (const auto& choice : paletteChoices) {
                    selection -= choice.second;
                    if (selection < 0) {
                        if (!choice.first.empty()) {
                            replacement = choice.first[static_cast<size_t>(
                                random.nextInt(static_cast<int32_t>(choice.first.size())))];
                        }
                        break;
                    }
                }
            }
            chain.push_back(woodPaletteSwizzle(target, replacement));
        }
    }
};

// ============================================================================
// TemplateFeature — templates/TemplateFeature.java
// ============================================================================
template <typename FC>
class TFTemplateFeature : public Feature<FC> {
public:
    bool place(FeaturePlaceContext<FC>& context) final {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        XoroshiroRandomSource& random = world->getRandom();
        const core::BlockPos& pos = context.origin();

        const std::string templateId = getTemplate(random);
        const FullTemplateData& data = TemplateEngine::get(templateId);

        const int rotation = random.nextInt(4);   // Rotation.getRandom
        const int mirror = random.nextInt(3);     // Util.getRandom(Mirror.values())

        const int chunkMinX = (pos.getX() >> 4) << 4;
        const int chunkMinZ = (pos.getZ() >> 4) << 4;
        // 26.x getMaxY() is inclusive (the library's is exclusive)
        const BoundingBox structureMask(chunkMinX, world->getMinY(), chunkMinZ,
                                        chunkMinX + 15, world->getMaxY() - 1, chunkMinZ + 15);

        const core::BlockPos transformedSize = rotatedSize(data, rotation);
        if (16 - transformedSize.getX() <= 0 || 16 - transformedSize.getZ() <= 0) return false;
        const int dx = random.nextInt(16 - transformedSize.getX());
        const int dz = random.nextInt(16 - transformedSize.getZ());
        core::BlockPos::MutableBlockPos startPos(chunkMinX + dx, pos.getY(), chunkMinZ + dz);

        if (!offsetToAverageGroundLevel(*world, startPos, transformedSize)) return false;
        startPos.move(0, yLevelOffset(), 0);

        const core::BlockPos placementPos = zeroPositionWithTransform(startPos, mirror, rotation, data.sizeX, data.sizeZ);

        Chain chain;
        modifySettings(chain, random, context.config());

        placeTemplate(*world, templateId, placementPos, rotation, mirror, structureMask, chain, random);
        for (const auto& marker : templateMarkers(templateId, placementPos, rotation, mirror, structureMask, random)) {
            processMarker(marker, *world, rotation, mirror, random);
        }

        postPlacement(*world, random, rotation, mirror, structureMask, chain, placementPos, context.config());
        return true;
    }

protected:
    virtual std::string getTemplate(XoroshiroRandomSource& random) = 0;
    virtual void modifySettings(Chain& chain, XoroshiroRandomSource& random, const FC& config) {
        (void)chain; (void)random; (void)config;
    }
    virtual void processMarker(const TemplateEngine::DataMarker& marker, WorldGenLevel& world, int rotation, int mirror,
                               XoroshiroRandomSource& random) {
        (void)marker; (void)world; (void)rotation; (void)mirror; (void)random;
    }
    virtual void postPlacement(WorldGenLevel& world, XoroshiroRandomSource& random, int rotation, int mirror,
                               const BoundingBox& structureMask, Chain& chain, const core::BlockPos& placementPos,
                               const FC& config) {
        (void)world; (void)random; (void)rotation; (void)mirror; (void)structureMask; (void)chain;
        (void)placementPos; (void)config;
    }
    virtual int yLevelOffset() const { return 0; }

    // Shared by subclasses: the template's markers through processMarker.
    void processTemplateMarkers(WorldGenLevel& world, const std::string& templateId, const core::BlockPos& position,
                                int rotation, int mirror, const BoundingBox& bb, XoroshiroRandomSource& random) {
        for (const auto& marker : templateMarkers(templateId, position, rotation, mirror, bb, random)) {
            processMarker(marker, world, rotation, mirror, random);
        }
    }

private:
    // TemplateFeature.offsetToAverageGroundLevel
    static bool offsetToAverageGroundLevel(WorldGenLevel& world, core::BlockPos::MutableBlockPos& startPos,
                                           const core::BlockPos& size) {
        StatsAccumulator heights;
        for (int dx = 0; dx < size.getX(); ++dx) {
            for (int dz = 0; dz < size.getZ(); ++dz) {
                const int x = startPos.getX() + dx;
                const int z = startPos.getZ() + dz;
                int y = world.getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
                while (y >= 0) {
                    BlockState* state = world.getBlockState(core::BlockPos(x, y, z));
                    if (isBlockNotOk(state)) return false;
                    if (isBlockOk(state)) break;
                    --y;
                }
                if (y < 0) return false;
                heights.add(static_cast<double>(y));
            }
        }
        // Guava throws for an empty accumulator; an empty template never places.
        if (heights.count == 0) return false;
        if (heights.populationStandardDeviation() > 2.0) return false;

        const int baseY = static_cast<int>(heights.mean + 0.5);
        const int maxY = static_cast<int>(heights.maxValue);
        startPos.set(startPos.getX(), baseY, startPos.getZ());

        // isAreaClear(startPos.above(maxY - baseY + 1), startPos.offset(size)): canBeReplaced everywhere
        const core::BlockPos a = startPos.above(maxY - baseY + 1);
        const core::BlockPos b = startPos.offset(size.getX(), size.getY(), size.getZ());
        for (int z = std::min(a.getZ(), b.getZ()); z <= std::max(a.getZ(), b.getZ()); ++z) {
            for (int y = std::min(a.getY(), b.getY()); y <= std::max(a.getY(), b.getY()); ++y) {
                for (int x = std::min(a.getX(), b.getX()); x <= std::max(a.getX(), b.getX()); ++x) {
                    BlockState* state = world.getBlockState(core::BlockPos(x, y, z));
                    if (state == nullptr || !state->canBeReplaced()) return false;
                }
            }
        }
        return true;
    }
};

// TFLootTables.generateLootContainer(world, pos, state, flags, table)
void generateLootContainer(WorldGenLevel& world, const core::BlockPos& pos, BlockState* state, int flags,
                           const std::string& lootTable) {
    if (state == nullptr) return;
    world.setBlock(pos, state, flags);
    BlockState* placed = world.getBlockState(pos);
    if (!sameBlock(placed, state)) return;
    setLootContainer(world, pos, state->getIdentifier(), lootTable, tfLootSeed(world, pos));
}

// The vanilla painting variants of an exact size (PaintingVariants.bootstrap;
// the mod registers none of its own): EntityUtil.getPaintingOfSize's
// valid.size() for exactMeasurements.
int paintingVariantCount(int width, int height) {
    struct Variant { int w, h; };
    static const Variant kVariants[] = {
        {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1},          // kebab .. wasteland
        {2, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1},                          // pool .. creebet
        {1, 2}, {1, 2},                                                  // wanderer, graham
        {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2}, {2, 2},                  // match .. wither
        {4, 2},                                                          // fighters
        {4, 4}, {4, 4}, {4, 4},                                          // pointer, pigscene, burning_skull
        {4, 3},                                                          // skeleton
        {2, 2}, {2, 2}, {2, 2}, {2, 2},                                  // earth, wind, water, fire
        {4, 3},                                                          // donkey_kong
        {2, 2}, {2, 2},                                                  // baroque, humble
        {1, 1}, {1, 2},                                                  // meditative, prairie_ride
        {4, 4}, {3, 4}, {3, 3}, {3, 3}, {4, 2}, {3, 3}, {3, 3}, {3, 3},  // unpacked .. fern
        {4, 2}, {4, 2}, {4, 4}, {3, 3}, {4, 2}, {3, 4}, {3, 3}, {3, 3},  // finding .. tides
        {3, 3}};                                                         // dennis
    int count = 0;
    for (const Variant& v : kVariants) {
        if (v.w == width && v.h == height) ++count;
    }
    return count;
}

// ============================================================================
// DruidHutFeature — templates/DruidHutFeature.java
// ============================================================================
class DruidHutFeature : public TFTemplateFeature<SwizzleConfig> {
protected:
    std::string getTemplate(XoroshiroRandomSource& random) override {
        static const char* kHuts[3] = {
            "twilightforest:feature/druid_hut/druid_hut",
            "twilightforest:feature/druid_hut/druid_sideways",
            "twilightforest:feature/druid_hut/druid_doubledeck"};
        return kHuts[random.nextInt(3)];
    }

    void modifySettings(Chain& chain, XoroshiroRandomSource& random, const SwizzleConfig& config) override {
        config.buildAddProcessors(chain, random);
    }

    void postPlacement(WorldGenLevel& world, XoroshiroRandomSource& random, int rotation, int mirror,
                       const BoundingBox& structureMask, Chain& chain, const core::BlockPos& placementPos,
                       const SwizzleConfig& config) override {
        if (!random.nextBoolean()) return;

        // BasementType.values()[nextInt(size)].getBasement(nextBoolean())
        static const char* kBasements[3][2] = {
            {"twilightforest:feature/druid_hut/basement_study", "twilightforest:feature/druid_hut/basement_study_trap"},
            {"twilightforest:feature/druid_hut/basement_shelves", "twilightforest:feature/druid_hut/basement_shelves_trap"},
            {"twilightforest:feature/druid_hut/basement_gallery", "twilightforest:feature/druid_hut/basement_gallery_trap"}};
        const int type = random.nextInt(3);
        const bool trapped = random.nextBoolean();
        const std::string templateId = kBasements[type][trapped ? 1 : 0];

        const core::BlockPos basementPos = placementPos.below(12)
            .relative(rotateDirection(rotation, mirrorDirection(mirror, core::Direction::NORTH)), 1)
            .relative(rotateDirection(rotation, mirrorDirection(mirror, core::Direction::EAST)), 1);

        chain.clear();
        config.buildAddProcessors(chain, random);
        chain.push_back(cobbleVariants(random));
        chain.push_back(stoneBricksVariants(random));

        placeTemplate(world, templateId, basementPos, rotation, mirror, structureMask, chain, random);
        processTemplateMarkers(world, templateId, basementPos, rotation, mirror, structureMask, random);
    }

    void processMarker(const TemplateEngine::DataMarker& marker, WorldGenLevel& world, int rotation, int mirror,
                       XoroshiroRandomSource& random) override {
        const std::string& s = marker.metadata;
        const core::BlockPos& blockPos = marker.pos;
        auto oriented = [&](core::Direction direction) { return rotateDirection(rotation, mirrorDirection(mirror, direction)); };

        if (s == "spawner") {
            if (removeBlock(world, blockPos) && world.setBlock(blockPos, s_blocks.spawner, kKnownShapeClients)) {
                setSpawnerEntity(world, blockPos, "twilightforest:skeleton_druid");
            }
        } else if (s.rfind("loot", 0) == 0) {
            if (s.size() < 6) return;   // the mod's substring(5, 6) would throw
            removeBlock(world, blockPos);
            const int trapped = (!s.empty() && s.back() == 'T') ? 1 : 0;
            int type = 0;   // ChestType SINGLE
            if (s[5] == 'L') type = mirror != kMirrorNone ? 2 : 1;
            else if (s[5] == 'R') type = mirror != kMirrorNone ? 1 : 2;
            core::Direction facing;
            switch (s[4]) {
                case 'W': facing = oriented(core::Direction::WEST); break;
                case 'E': facing = oriented(core::Direction::EAST); break;
                case 'S': facing = oriented(core::Direction::SOUTH); break;
                default:  facing = oriented(core::Direction::NORTH); break;
            }
            BlockState* chest = s_blocks.chests[static_cast<size_t>(trapped)][static_cast<size_t>(type)]
                                               [static_cast<size_t>(facing)];
            generateLootContainer(world, blockPos, chest, kKnownShapeClients,
                                  (!s.empty() && s.back() == 'J') ? kLootHutJunk : kLootBasement);
        } else if (s.rfind("barrel", 0) == 0) {
            if (s.size() < 7) return;
            removeBlock(world, blockPos);
            core::Direction facing;
            switch (s[6]) {
                case 'D': facing = oriented(core::Direction::DOWN); break;
                case 'W': facing = oriented(core::Direction::WEST); break;
                case 'E': facing = oriented(core::Direction::EAST); break;
                case 'N': facing = oriented(core::Direction::NORTH); break;
                case 'S': facing = oriented(core::Direction::SOUTH); break;
                default:  facing = oriented(core::Direction::UP); break;
            }
            generateLootContainer(world, blockPos, s_blocks.barrels[static_cast<size_t>(facing)],
                                  kKnownShapeClients, kLootHutJunk);
        } else if (s.rfind("painting", 0) == 0) {
            if (s.size() < 10) return;
            removeBlock(world, blockPos);
            // The painting entity itself cannot be added by the terrain
            // library; getPaintingOfSize's variant draw is kept.
            const char w = s[9];
            const int paintingWidth = std::isdigit(static_cast<unsigned char>(w)) ? (w - '0') : 1;
            const int paintingHeight = (paintingWidth == 2 || paintingWidth == 4) ? 2 : 1;
            const int count = paintingVariantCount(paintingWidth, paintingHeight);
            if (count > 0) (void)random.nextInt(count);
        }
    }
};

// ============================================================================
// SimpleWellFeature — templates/SimpleWellFeature.java
// ============================================================================
class SimpleWellFeature : public TFTemplateFeature<SwizzleConfig> {
protected:
    std::string getTemplate(XoroshiroRandomSource&) override {
        return "twilightforest:feature/well/simple_well_top";
    }

    int yLevelOffset() const override { return 1; }

    void modifySettings(Chain& chain, XoroshiroRandomSource& random, const SwizzleConfig& config) override {
        config.buildAddProcessors(chain, random);
    }

    void postPlacement(WorldGenLevel& world, XoroshiroRandomSource& random, int rotation, int mirror,
                       const BoundingBox& structureMask, Chain& chain, const core::BlockPos& placementPos,
                       const SwizzleConfig&) override {
        const std::string bottom = "twilightforest:feature/well/simple_well_bottom";
        const FullTemplateData& data = TemplateEngine::get(bottom);
        const core::BlockPos bottomPos = placementPos.below(data.sizeY);
        chain.push_back(smartGrass(world));
        placeTemplate(world, bottom, bottomPos, rotation, mirror, structureMask, chain, random);
        processTemplateMarkers(world, bottom, bottomPos, rotation, mirror, structureMask, random);
    }

    void processMarker(const TemplateEngine::DataMarker& marker, WorldGenLevel& world, int rotation, int mirror,
                       XoroshiroRandomSource& random) override {
        const std::string& s = marker.metadata;
        const core::BlockPos& blockPos = marker.pos;
        if (s.rfind("loot", 0) != 0) return;

        if (random.nextBoolean()) {
            world.setBlock(blockPos, random.nextBoolean() ? s_blocks.cobblestone : s_blocks.mossyCobblestone,
                           kKnownShapeClients);
            return;
        }
        removeBlock(world, blockPos);

        const char c = s.size() > 4 ? s[4] : 'N';
        core::Direction dir;
        switch (c) {
            case 'W': dir = rotateDirection(rotation, mirrorDirection(mirror, core::Direction::WEST)); break;
            case 'E': dir = rotateDirection(rotation, mirrorDirection(mirror, core::Direction::EAST)); break;
            case 'S': dir = rotateDirection(rotation, mirrorDirection(mirror, core::Direction::SOUTH)); break;
            default:  dir = rotateDirection(rotation, mirrorDirection(mirror, core::Direction::NORTH)); break;
        }
        generateLootContainer(world, blockPos, s_blocks.barrels[static_cast<size_t>(dir)], kKnownShapeClients, kLootWell);

        if (random.nextBoolean()) return;

        const core::BlockPos hopperPos = blockPos.relative(dir);
        BlockState* hopper = s_blocks.hoppers[static_cast<size_t>(core::getOpposite(dir))];
        if (hopper != nullptr && world.setBlock(hopperPos, hopper, kKnownShapeClients)) {
            const std::string payload = TemplateEngine::blockEntityPayloadFor("minecraft:hopper", nullptr, std::nullopt);
            if (::world::IChunk* chunk = world.getChunk(hopperPos.getX() >> 4, hopperPos.getZ() >> 4)) {
                if (!payload.empty()) chunk->setBlockEntityNbt(hopperPos, payload);
            }
        }
    }
};

// ============================================================================
// FancyWellFeature — templates/FancyWellFeature.java
// ============================================================================
class FancyWellFeature : public TFTemplateFeature<SwizzleConfig> {
protected:
    std::string getTemplate(XoroshiroRandomSource&) override {
        return "twilightforest:feature/well/fancy_well_top";
    }

    void modifySettings(Chain& chain, XoroshiroRandomSource& random, const SwizzleConfig& config) override {
        config.buildAddProcessors(chain, random);
    }

    void postPlacement(WorldGenLevel& world, XoroshiroRandomSource& random, int rotation, int mirror,
                       const BoundingBox& structureMask, Chain& chain, const core::BlockPos& placementPos,
                       const SwizzleConfig&) override {
        const std::string bottom = "twilightforest:feature/well/fancy_well_bottom";
        const FullTemplateData& data = TemplateEngine::get(bottom);
        const core::BlockPos bottomPos = placementPos.below(data.sizeY);
        chain.push_back(smartGrass(world));
        placeTemplate(world, bottom, bottomPos, rotation, mirror, structureMask, chain, random);
        processTemplateMarkers(world, bottom, bottomPos, rotation, mirror, structureMask, random);
    }

    void processMarker(const TemplateEngine::DataMarker& marker, WorldGenLevel& world, int, int,
                       XoroshiroRandomSource&) override {
        if (marker.metadata.rfind("loot", 0) != 0) return;
        removeBlock(world, marker.pos);
        generateLootContainer(world, marker.pos, s_blocks.barrels[static_cast<size_t>(core::Direction::UP)],
                              kKnownShapeClients, kLootFancyWell);
    }
};

// ============================================================================
// GroveRuinsFeature / StoneCircleFeature — templates/{GroveRuins,StoneCircle}Feature.java
// ============================================================================
class GroveRuinsFeature : public TFTemplateFeature<NoneFeatureConfiguration> {
protected:
    std::string getTemplate(XoroshiroRandomSource& random) override {
        return random.nextBoolean() ? "twilightforest:feature/ruins/grove_pillar"
                                    : "twilightforest:feature/ruins/grove_arch";
    }
    void modifySettings(Chain& chain, XoroshiroRandomSource& random, const NoneFeatureConfiguration&) override {
        chain.push_back(stoneBricksVariants(random));
    }
};

class StoneCircleFeature : public TFTemplateFeature<NoneFeatureConfiguration> {
protected:
    std::string getTemplate(XoroshiroRandomSource&) override {
        return "twilightforest:feature/ruins/stone_circle";
    }
    void modifySettings(Chain& chain, XoroshiroRandomSource& random, const NoneFeatureConfiguration&) override {
        chain.push_back(cobbleVariants(random));
    }
};

// ============================================================================
// GraveyardFeature — templates/GraveyardFeature.java (+ WebTemplateProcessor)
// ============================================================================
class GraveyardFeature : public Feature<NoneFeatureConfiguration> {
public:
    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* world = context.level();
        if (world == nullptr) return false;
        const core::BlockPos& pos = context.origin();
        WorldgenRandom& rand = context.random();

        static const std::string kGraveyard = "twilightforest:feature/graveyard/graveyard";
        static const std::string kTrap = "twilightforest:feature/graveyard/grave_trap";
        // GraveType.VALUES: Full, Upper, Lower
        static const std::string kGraves[3] = {
            "twilightforest:feature/graveyard/grave_full",
            "twilightforest:feature/graveyard/grave_upper",
            "twilightforest:feature/graveyard/grave_lower"};

        const FullTemplateData& base = TemplateEngine::get(kGraveyard);
        const FullTemplateData& fullGrave = TemplateEngine::get(kGraves[0]);
        if (base.palettes.empty() || fullGrave.palettes.empty()) return false;

        const int rotation = rand.nextInt(4);
        const int mirror = rand.nextInt(3 + 1) % 3;

        const core::BlockPos transformedSize = rotatedSize(base, rotation);
        const core::BlockPos transformedGraveSize = rotatedSize(fullGrave, rotation);

        const core::BlockPos shifted = pos.offset(-8, 0, -8);
        const int chunkX = shifted.getX() >> 4;
        const int chunkZ = shifted.getZ() >> 4;
        const core::BlockPos endPos = shifted.offset(transformedSize.getX(), transformedSize.getY(), transformedSize.getZ());
        const int endChunkX = endPos.getX() >> 4;
        const int endChunkZ = endPos.getZ() >> 4;
        const BoundingBox bb((chunkX << 4) + 8, 0, (chunkZ << 4) + 8,
                             (endChunkX << 4) + 15 + 8, 255, (endChunkZ << 4) + 15 + 8);

        core::BlockPos::MutableBlockPos startPos((chunkX << 4) + 8, pos.getY() - 1, (chunkZ << 4) + 8);
        if (!offsetToAverageGroundLevel(*world, startPos, transformedSize)) return false;

        const core::BlockPos placementPos =
            zeroPositionWithTransform(startPos, mirror, rotation, base.sizeX, base.sizeZ).offset(1, -1, 0);
        const core::BlockPos size = transformedSize.offset(-1, 0, -1);
        const core::BlockPos graveSize = transformedGraveSize.offset(-1, 0, -1);

        // WebTemplateProcessor: a non-grass block turns into a cobweb one time
        // in five — and the cobweb's StructureBlockInfo carries the
        // processor's `pos` argument (the template's placement position), so
        // the block leaves its own spot and the cobweb lands on the
        // placement position. Relocations are recorded per placeInWorld.
        struct Relocations { core::BlockPos target{0, 0, 0}; size_t lastIndex = 0; bool any = false; };
        Relocations relocations;
        Chain chain;
        chain.push_back([&rand, &relocations](const BlockContext& c, BlockState* state, BlockState*) -> BlockState* {
            if (isBlock(state, "minecraft:grass_block")) return state;
            if (rand.nextInt(5) == 0) {
                relocations.target = c.structurePos;
                relocations.lastIndex = c.index;
                relocations.any = true;
                return nullptr;
            }
            return state;
        });

        // Place one template with the web processor, then settle the cobweb
        // relocated onto its position: the last processed entry for that
        // position wins, as in Java's in-order placement.
        auto placeWithWebs = [&](const std::string& templateId, const core::BlockPos& at) {
            relocations = Relocations{};
            const FullTemplateData& data = TemplateEngine::get(templateId);
            placeTemplate(*world, templateId, at, rotation, mirror, bb, chain, rand, [&]() {
                if (!relocations.any) return;
                // The template block whose own position is the placement
                // position (local 0,0,0 under any rotation/mirror).
                bool regularLater = false;
                for (size_t i = 0; i < data.blocks.size(); ++i) {
                    const TemplateBlockInfo& info = data.blocks[i];
                    if (info.x == 0 && info.y == 0 && info.z == 0) {
                        regularLater = i > relocations.lastIndex
                            && !isBlock(data.palettes[0][static_cast<size_t>(info.stateIdx)], "minecraft:structure_block");
                        break;
                    }
                }
                const core::BlockPos& target = relocations.target;
                if (!regularLater && bb.isInside(target.getX(), target.getY(), target.getZ())) {
                    world->setBlock(target, s_blocks.cobweb, kKnownShapeAll);
                }
            });
        };

        placeWithWebs(kGraveyard, placementPos);
        std::vector<TemplateEngine::DataMarker> data =
            templateMarkers(kGraveyard, placementPos, rotation, mirror, bb, rand);

        const core::BlockPos start = core::BlockPos(startPos).offset(1, 1, 0);
        const core::BlockPos end = start.offset(size.getX(), 0, size.getZ());

        for (int x = 1; x <= size.getX() - 1; ++x) {
            for (int z = 1; z <= size.getZ() - 1; ++z) {
                if (world->isEmptyBlock(start.offset(x, 0, z)) && rand.nextInt(12) == 0) {
                    world->setBlock(start.offset(x, 0, z), s_blocks.cobweb, kKnownShapeAll);
                }
            }
        }

        const core::BlockPos inner = start.offset(2, 0, 2);
        const core::BlockPos bound = end.offset(-2, 0, -2);
        const core::BlockPos innerSize(bound.getX() - inner.getX(), bound.getY() - inner.getY(), bound.getZ() - inner.getZ());
        const core::BlockPos fixed = inner.offset(
            (rotation == kRotCW180 ? graveSize.getX() : 0)
                + (mirror == kMirrorFrontBack ? transformedGraveSize.getX() - 1 : 0) * (rotation == kRotCW180 ? -1 : 1),
            0,
            (rotation == kRotCCW90 ? graveSize.getZ() : 0)
                + (mirror == kMirrorFrontBack ? transformedGraveSize.getZ() - 1 : 0) * (rotation == kRotCCW90 ? -1 : 1));
        const core::BlockPos fixedSize = innerSize.offset(-graveSize.getX(), 0, -graveSize.getZ());
        const core::BlockPos chestloc = rotatePos(
            core::BlockPos(rand.nextInt(2) - (mirror == kMirrorFrontBack ? 1 : 0), 1, 0), rotation);

        const int stepX = (rotation == kRotCW90 || rotation == kRotCCW90) ? 2 : 5;
        const int stepZ = (rotation == kRotNone || rotation == kRotCW180) ? 2 : 5;
        for (int x = 0; x <= fixedSize.getX(); x += stepX) {
            for (int z = 0; z <= fixedSize.getZ(); z += stepZ) {
                if (x == innerSize.getX() / 2 || z == innerSize.getZ() / 2) continue;
                core::BlockPos placement = fixed.offset(x, -2, z);
                const int graveType = rand.nextInt(3);
                const std::string& grave = kGraves[graveType];
                placeWithWebs(grave, placement);
                for (auto& marker : templateMarkers(grave, placement, rotation, mirror, bb, rand)) {
                    data.push_back(std::move(marker));
                }
                if (graveType == 0) {   // GraveType.Full
                    if (rand.nextBoolean()) {
                        if (rand.nextInt(3) == 0) {
                            placement = placement.offset(rotatePos(core::BlockPos(
                                mirror == kMirrorFrontBack ? 1 : -1, 0, mirror == kMirrorLeftRight ? 1 : -1), rotation));
                            placeWithWebs(kTrap, placement);
                        }
                        // The mod filters the trap at the graveyard's
                        // placement position (a palette draw, no markers).
                        for (auto& marker : templateMarkers(kTrap, placementPos, rotation, mirror, bb, rand)) {
                            data.push_back(std::move(marker));
                        }
                        const core::BlockPos chestPos = placement.offset(chestloc.getX(), chestloc.getY(), chestloc.getZ());
                        // TRAPPED_CHEST[facing=west].rotate(rotation).mirror(mirror)
                        BlockState* chest = mirrorState(rotateState(s_blocks.graveyardChest, rotation), mirror);
                        if (chest != nullptr && world->setBlock(chestPos, chest, kKnownShapeAll)) {
                            if (sameBlock(world->getBlockState(chestPos), chest)) {
                                setLootContainer(*world, chestPos, chest->getIdentifier(), kLootGraveyard,
                                                 tfLootSeed(*world, chestPos));
                            }
                            world->setBlock(chestPos.below(), s_blocks.mossyCobblestone, kUpdateAll);
                        }
                        // The wraith cannot be added by the terrain library;
                        // EventHooks.finalizeMobSpawn -> Mob.finalizeSpawn draws
                        // from level.getRandom(): random.triangle(0, 0.11485)
                        // (two nextDouble) and setLeftHanded(nextFloat() < 0.05).
                        XoroshiroRandomSource& levelRandom = world->getRandom();
                        (void)levelRandom.nextDouble();
                        (void)levelRandom.nextDouble();
                        (void)levelRandom.nextFloat();
                    }
                }
            }
        }

        for (const auto& marker : data) {
            if (marker.metadata != "spawner") continue;
            removeBlock(*world, marker.pos);
            if (rand.nextInt(4) == 0) {
                if (world->setBlock(marker.pos, s_blocks.spawner, kUpdateAll)) {
                    setSpawnerEntity(*world, marker.pos, "twilightforest:rising_zombie");
                }
            }
        }
        return true;
    }

private:
    // GraveyardFeature.offsetToAverageGroundLevel (Math.round, FeatureLogic.isAreaClear)
    static bool offsetToAverageGroundLevel(WorldGenLevel& world, core::BlockPos::MutableBlockPos& startPos,
                                           const core::BlockPos& size) {
        StatsAccumulator heights;
        for (int dx = 0; dx < size.getX(); ++dx) {
            for (int dz = 0; dz < size.getZ(); ++dz) {
                const int x = startPos.getX() + dx;
                const int z = startPos.getZ() + dz;
                int y = world.getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z);
                while (y >= 0) {
                    BlockState* state = world.getBlockState(core::BlockPos(x, y, z));
                    if (isBlockNotOk(state)) return false;
                    if (isBlockOk(state)) break;
                    --y;
                }
                if (y < 0) return false;
                heights.add(static_cast<double>(y));
            }
        }
        if (heights.count == 0) return false;
        if (heights.populationStandardDeviation() > 2.0) return false;

        const int baseY = static_cast<int>(std::floor(heights.mean + 0.5));   // (int) Math.round(mean)
        const int maxY = static_cast<int>(heights.maxValue);
        startPos.set(startPos.getX(), baseY, startPos.getZ());

        // FeatureLogic.isAreaClear: no solid, non-replaceable, non-liquid block
        const core::BlockPos a = startPos.above(maxY - baseY + 1);
        const core::BlockPos b = startPos.offset(size.getX(), size.getY(), size.getZ());
        for (int z = std::min(a.getZ(), b.getZ()); z <= std::max(a.getZ(), b.getZ()); ++z) {
            for (int y = std::min(a.getY(), b.getY()); y <= std::max(a.getY(), b.getY()); ++y) {
                for (int x = std::min(a.getX(), b.getX()); x <= std::max(a.getX(), b.getX()); ++x) {
                    BlockState* state = world.getBlockState(core::BlockPos(x, y, z));
                    if (state != nullptr && !state->canBeReplaced() && state->isSolid() && !state->isFluid()) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
};

// ============================================================================
// Storage
// ============================================================================
bool s_initialized = false;
std::mutex s_bootstrapMutex;
DruidHutFeature s_druidHutFeature;
SimpleWellFeature s_simpleWellFeature;
FancyWellFeature s_fancyWellFeature;
GroveRuinsFeature s_groveRuinsFeature;
StoneCircleFeature s_stoneCircleFeature;
GraveyardFeature s_graveyardFeature;
RandomSelectorFeature s_randomSelectorFeature;
std::vector<std::unique_ptr<PlacedFeature>> s_inlinePlaced;

bool resolveTemplateBlocks() {
    const char* f = "template features";
    auto st = [f](const char* name, const PropertyMap& props) { return resolveState(name, props, f); };
    s_blocks.air = st("minecraft:air", {});
    s_blocks.spawner = st("minecraft:spawner", {});
    s_blocks.cobblestone = st("minecraft:cobblestone", {});
    s_blocks.mossyCobblestone = st("minecraft:mossy_cobblestone", {});
    s_blocks.mossyCobblestoneStairs = st("minecraft:mossy_cobblestone_stairs", {});
    s_blocks.mossyCobblestoneSlab = st("minecraft:mossy_cobblestone_slab", {});
    s_blocks.mossyCobblestoneWall = st("minecraft:mossy_cobblestone_wall", {});
    s_blocks.mossyStoneBricks = st("minecraft:mossy_stone_bricks", {});
    s_blocks.crackedStoneBricks = st("minecraft:cracked_stone_bricks", {});
    s_blocks.mossyStoneBrickStairs = st("minecraft:mossy_stone_brick_stairs", {});
    s_blocks.mossyStoneBrickSlab = st("minecraft:mossy_stone_brick_slab", {});
    s_blocks.mossyStoneBrickWall = st("minecraft:mossy_stone_brick_wall", {});
    s_blocks.podzol = st("minecraft:podzol", {});
    s_blocks.mycelium = st("minecraft:mycelium", {});
    s_blocks.dirtPath = st("minecraft:dirt_path", {});
    s_blocks.coarseDirt = st("minecraft:coarse_dirt", {});
    s_blocks.cobweb = st("minecraft:cobweb", {});
    s_blocks.uberousSoil = isRealTwilight("twilightforest:uberous_soil")
        ? levelgen::twilight_blocks::defaultState("twilightforest:uberous_soil") : nullptr;

    static const char* kTypes[3] = {"single", "left", "right"};
    for (int trapped = 0; trapped < 2; ++trapped) {
        for (int type = 0; type < 3; ++type) {
            for (core::Direction d : kDirections) {
                if (core::getAxis(d) == core::Axis::Y) continue;
                s_blocks.chests[static_cast<size_t>(trapped)][static_cast<size_t>(type)][static_cast<size_t>(d)] =
                    st(trapped ? "minecraft:trapped_chest" : "minecraft:chest",
                       {{"facing", directionName(d)}, {"type", kTypes[type]}, {"waterlogged", "false"}});
            }
        }
    }
    for (core::Direction d : kDirections) {
        s_blocks.barrels[static_cast<size_t>(d)] = st("minecraft:barrel", {{"facing", directionName(d)}, {"open", "false"}});
        if (d != core::Direction::UP) {
            s_blocks.hoppers[static_cast<size_t>(d)] = st("minecraft:hopper", {{"facing", directionName(d)}, {"enabled", "true"}});
        }
    }
    s_blocks.graveyardChest = st("minecraft:trapped_chest", {{"facing", "west"}, {"type", "single"}, {"waterlogged", "false"}});
    return s_blocks.complete();
}

// preprocessing_rules of druid_hut.json / fancy_well.json (eight rules) and
// simple_well.json (the four cobblestone rules): random_block_match 0.5,
// always_true location, the mossy output state.
std::vector<TransfigRule> mossyRules(bool withStoneBricks) {
    const char* f = "template preprocessing rules";
    auto out = [f](const char* name, const PropertyMap& props) { return resolveState(name, props, f); };
    const PropertyMap stairs = {{"facing", "north"}, {"half", "bottom"}, {"shape", "straight"}, {"waterlogged", "false"}};
    const PropertyMap slab = {{"type", "bottom"}, {"waterlogged", "false"}};
    const PropertyMap wall = {{"east", "none"}, {"north", "none"}, {"south", "none"}, {"up", "true"},
                              {"waterlogged", "false"}, {"west", "none"}};
    std::vector<TransfigRule> rules = {
        {"minecraft:cobblestone", 0.5f, out("minecraft:mossy_cobblestone", {})},
        {"minecraft:cobblestone_stairs", 0.5f, out("minecraft:mossy_cobblestone_stairs", stairs)},
        {"minecraft:cobblestone_slab", 0.5f, out("minecraft:mossy_cobblestone_slab", slab)},
        {"minecraft:cobblestone_wall", 0.5f, out("minecraft:mossy_cobblestone_wall", wall)}};
    if (withStoneBricks) {
        rules.push_back({"minecraft:stone_bricks", 0.5f, out("minecraft:mossy_stone_bricks", {})});
        rules.push_back({"minecraft:stone_brick_stairs", 0.5f, out("minecraft:mossy_stone_brick_stairs", stairs)});
        rules.push_back({"minecraft:stone_brick_slab", 0.5f, out("minecraft:mossy_stone_brick_slab", slab)});
        rules.push_back({"minecraft:stone_brick_wall", 0.5f, out("minecraft:mossy_stone_brick_wall", wall)});
    }
    for (const TransfigRule& rule : rules) {
        if (rule.output == nullptr) return {};
    }
    return rules;
}

} // namespace

bool TwilightTemplateFeatures::isInitialized() {
    return s_initialized;
}

void TwilightTemplateFeatures::bootstrap() {
    std::lock_guard<std::mutex> lock(s_bootstrapMutex);
    if (s_initialized) return;
    s_initialized = true;

    if (!resolveTemplateBlocks()) {
        std::fprintf(stderr, "[TwilightTemplateFeatures] template blocks missing - template features not registered\n");
        return;
    }
    buildPalettes();

    using twilight::registerOwned;

    // palette_choices: the four rarity tags, each at outer weight 1
    // (#twilightforest:common / uncommon / rare / treasure, tag order).
    std::vector<std::pair<std::vector<const WoodPalette*>, int>> choices = {
        {{palette("minecraft:spruce"), palette("twilightforest:canopy")}, 1},
        {{palette("minecraft:oak"), palette("twilightforest:darkwood"), palette("twilightforest:twilight_oak")}, 1},
        {{palette("minecraft:birch"), palette("minecraft:jungle"), palette("twilightforest:mangrove")}, 1},
        {{palette("twilightforest:timewood"), palette("twilightforest:transwood"), palette("twilightforest:minewood"),
          palette("twilightforest:sortwood")}, 1}};

    const std::vector<TransfigRule> eightRules = mossyRules(true);
    const std::vector<TransfigRule> fourRules = mossyRules(false);
    if (eightRules.empty() || fourRules.empty()) {
        std::fprintf(stderr, "[TwilightTemplateFeatures] mossy rule blocks missing - template features not registered\n");
        return;
    }

    // druid_hut.json: target_palettes #druid_hut_swizzle_mask (oak, spruce, birch)
    SwizzleConfig druid;
    druid.targets = {palette("minecraft:oak"), palette("minecraft:spruce"), palette("minecraft:birch")};
    druid.paletteChoices = choices;
    druid.preprocessingRules = eightRules;
    registerOwned("twilightforest:druid_hut",
        std::make_unique<ConfiguredFeatureImpl<SwizzleConfig, DruidHutFeature>>(&s_druidHutFeature, druid));

    // simple_well.json / fancy_well.json: #well_swizzle_mask (oak)
    SwizzleConfig simpleWell;
    simpleWell.targets = {palette("minecraft:oak")};
    simpleWell.paletteChoices = choices;
    simpleWell.preprocessingRules = fourRules;
    ConfiguredFeature* simple = registerOwned("twilightforest:simple_well",
        std::make_unique<ConfiguredFeatureImpl<SwizzleConfig, SimpleWellFeature>>(&s_simpleWellFeature, simpleWell));

    SwizzleConfig fancyWell;
    fancyWell.targets = {palette("minecraft:oak")};
    fancyWell.paletteChoices = choices;
    fancyWell.preprocessingRules = eightRules;
    ConfiguredFeature* fancy = registerOwned("twilightforest:fancy_well",
        std::make_unique<ConfiguredFeatureImpl<SwizzleConfig, FancyWellFeature>>(&s_fancyWellFeature, fancyWell));

    // well_placer.json: random_selector fancy_well 0.05, default simple_well
    {
        auto inlinePlaced = [](ConfiguredFeature* feature, const std::string& name) -> PlacedFeature* {
            auto placed = std::make_unique<PlacedFeature>(feature, std::vector<placement::PlacementModifier*>{}, name);
            PlacedFeature* raw = placed.get();
            s_inlinePlaced.push_back(std::move(placed));
            return raw;
        };
        RandomFeatureConfiguration config(
            {WeightedPlacedFeature(inlinePlaced(fancy, "well_placer_fancy"), 0.05f)},
            inlinePlaced(simple, "well_placer_default"));
        registerOwned("twilightforest:well_placer",
            std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
                &s_randomSelectorFeature, config));
    }

    registerOwned("twilightforest:grove_ruins",
        std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, GroveRuinsFeature>>(
            &s_groveRuinsFeature, NoneFeatureConfiguration::INSTANCE));
    registerOwned("twilightforest:stone_circle",
        std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, StoneCircleFeature>>(
            &s_stoneCircleFeature, NoneFeatureConfiguration::INSTANCE));
    registerOwned("twilightforest:graveyard",
        std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, GraveyardFeature>>(
            &s_graveyardFeature, NoneFeatureConfiguration::INSTANCE));
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
