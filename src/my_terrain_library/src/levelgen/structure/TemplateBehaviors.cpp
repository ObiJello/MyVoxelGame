#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/structure/OrientedPieceBehavior.h"

#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include "math/Mth.h"
#include "random/LegacyRandomSource.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/level/block/blocks/VineBlock.h"
#include <cstring>
#include "levelgen/WorldgenRandom.h"
#include "levelgen/Heightmap.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <algorithm>

// Reference: TemplateStructurePiece.postProcess subclasses (B6): igloo now,
// shipwreck/ocean ruin/ruined portal to follow.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace PieceBehaviors {

namespace {

using world::level::block::Blocks;

// Template bbox with the piece's settings (mirror NONE for igloo) - corners
// transformed around the pivot, min/max'd, moved to position. Matches the
// gate-verified templateBoundingBox in TemplateLayouts.cpp.
BoundingBox templateBox(const std::string& templateId,
                        const TemplatePlaceSettings& settings,
                        const core::BlockPos& position) {
    const FullTemplateData& data = TemplateEngine::get(templateId);
    if (data.palettes.empty()) {
        return BoundingBox(position.getX() - 1, position.getY() - 1, position.getZ() - 1,
                           position.getX(), position.getY(), position.getZ());
    }
    core::BlockPos c1 = TemplateEngine::calculateRelativePosition(
        settings, core::BlockPos(0, 0, 0));
    core::BlockPos c2 = TemplateEngine::calculateRelativePosition(
        settings, core::BlockPos(data.sizeX - 1, data.sizeY - 1, data.sizeZ - 1));
    BoundingBox box(std::min(c1.getX(), c2.getX()), std::min(c1.getY(), c2.getY()),
                    std::min(c1.getZ(), c2.getZ()), std::max(c1.getX(), c2.getX()),
                    std::max(c1.getY(), c2.getY()), std::max(c1.getZ(), c2.getZ()));
    box.move(position.getX(), position.getY(), position.getZ());
    return box;
}

// Reference: IglooPieces.IglooPiece.
class IglooBehavior final : public StructurePieceBehavior {
public:
    IglooBehavior(std::string templateId, int rotation, core::BlockPos pivot,
                  core::BlockPos offset, core::BlockPos templatePosition)
        : m_templateId(std::move(templateId)), m_rotation(rotation), m_pivot(pivot),
          m_offset(offset), m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = 0;
        settings.rotationPivot = m_pivot;
        settings.ignoreAir = false;               // STRUCTURE_BLOCK ignore only
        settings.keepLiquids = false;             // LiquidSettings.IGNORE_WATERLOGGING

        // Reference: IglooPiece.postProcess - entrance height probe, then a
        // TEMPORARY templatePosition shift (restored after; the recomputed
        // piece bbox keeps the shift, exactly like Java's field write).
        core::BlockPos entrance = TemplateEngine::calculateRelativePosition(
            settings, core::BlockPos(3 - m_offset.getX(), 0, -m_offset.getZ()));
        entrance = entrance.offset(m_templatePosition.getX(), m_templatePosition.getY(),
                                   m_templatePosition.getZ());
        int height = level->getHeight(Heightmap::Types::WORLD_SURFACE_WG,
                                      entrance.getX(), entrance.getZ());
        core::BlockPos oldPosition = m_templatePosition;
        m_templatePosition = m_templatePosition.offset(0, height - 90 - 1, 0);

        // Reference: TemplateStructurePiece.postProcess.
        self.boundingBox = templateBox(m_templateId, settings, m_templatePosition);
        if (TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                         referencePos, settings, random, chunkBB)) {
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(m_templateId, m_templatePosition, settings, chunkBB)) {
                handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB);
            }
        }

        if (m_templateId == "minecraft:igloo/top") {
            // Reference: the trapdoor concealment check.
            core::BlockPos trapdoor = TemplateEngine::calculateRelativePosition(
                settings, core::BlockPos(3, 0, 5));
            trapdoor = trapdoor.offset(m_templatePosition.getX(), m_templatePosition.getY(),
                                       m_templatePosition.getZ());
            BlockState* below = level->getBlockState(trapdoor.below());
            if (!below->isAir() && !below->is(Blocks::getBlock("minecraft:ladder"))) {
                level->setBlock(trapdoor, Blocks::getDefaultState("minecraft:snow_block"), 3);
            }
        }
        m_templatePosition = oldPosition;
    }

private:
    std::string m_templateId;
    int m_rotation;
    core::BlockPos m_pivot;
    core::BlockPos m_offset;
    core::BlockPos m_templatePosition;

    // Reference: IglooPiece.handleDataMarker - "chest" marker becomes air and
    // the chest below draws its loot seed.
    void handleDataMarker(const std::string& markerId, const core::BlockPos& pos,
                          WorldGenLevel* level, WorldgenRandom& random,
                          const BoundingBox& chunkBB) {
        (void)chunkBB;
        if (markerId == "chest") {
            level->setBlock(pos, Blocks::AIR->defaultBlockState(), 3);
            // Java: ChestBlockEntity below -> setLootTable(IGLOO_CHEST,
            // random.nextLong()) - the BE exists (template chest), so the
            // draw always happens.
            int64_t seed = random.nextLong();
            core::BlockPos below = pos.below();
            if (auto* chunk = level->getChunk(below.getX() >> 4, below.getZ() >> 4)) {
                chunk->setBlockEntityNbt(below,
                    "{LootTable:\"minecraft:chests/igloo_chest\",LootTableSeed:"
                    + std::to_string(seed)
                    + "l,components:{},id:\"minecraft:chest\"}");
            }
        }
    }
};

// Reference: ShipwreckPieces.ShipwreckPiece - re-anchors to the heightmap
// mean (ocean) or the beached formula, then places the template.
class ShipwreckBehavior final : public StructurePieceBehavior {
public:
    ShipwreckBehavior(std::string templateId, int rotation, bool isBeached,
                      core::BlockPos templatePosition)
        : m_templateId(std::move(templateId)), m_rotation(rotation),
          m_isBeached(isBeached), m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        const FullTemplateData& data = TemplateEngine::get(m_templateId);
        // Reference: isTooBigToFitInWorldGenRegion - unreachable for vanilla
        // templates (layout throws for it too).
        Heightmap::Types heightmapType = m_isBeached ? Heightmap::Types::WORLD_SURFACE_WG
                                                     : Heightmap::Types::OCEAN_FLOOR_WG;
        int minY = level->getMaxY() + 1;
        int mean = 0;
        int baseSize = data.sizeX * data.sizeZ;
        if (baseSize == 0) {
            mean = level->getHeight(heightmapType, m_templatePosition.getX(),
                                    m_templatePosition.getZ());
        } else {
            // Reference: BlockPos.betweenClosed over the RAW footprint - sum
            // and min are order-independent.
            for (int x = m_templatePosition.getX();
                 x <= m_templatePosition.getX() + data.sizeX - 1; ++x) {
                for (int z = m_templatePosition.getZ();
                     z <= m_templatePosition.getZ() + data.sizeZ - 1; ++z) {
                    int height = level->getHeight(heightmapType, x, z);
                    mean += height;
                    minY = std::min(minY, height);
                }
            }
            mean /= baseSize;
        }
        int newHeight = m_isBeached
            ? minY - data.sizeY / 2 - random.nextInt(3)  // calculateBeachedPosition
            : mean;
        // Reference: adjustPositionHeight - PERSISTENT mutation.
        m_templatePosition = core::BlockPos(m_templatePosition.getX(), newHeight,
                                            m_templatePosition.getZ());

        // Reference: TemplateStructurePiece.postProcess with the shipwreck
        // settings (rotation, pivot (4,0,15), STRUCTURE_AND_AIR ignore).
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = 0;
        settings.rotationPivot = core::BlockPos(4, 0, 15);
        settings.ignoreAir = true;
        settings.keepLiquids = true;
        self.boundingBox = templateBox(m_templateId, settings, m_templatePosition);
        if (TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                         referencePos, settings, random, chunkBB)) {
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(m_templateId, m_templatePosition, settings, chunkBB)) {
                // Reference: ShipwreckPiece.handleDataMarker - map_chest/
                // treasure_chest/supply_chest set loot on the chest BELOW the
                // marker; the BE exists, so the nextLong draw always happens.
                const char* lootTable = nullptr;
                if (marker.metadata == "map_chest") {
                    lootTable = "minecraft:chests/shipwreck_map";
                } else if (marker.metadata == "treasure_chest") {
                    lootTable = "minecraft:chests/shipwreck_treasure";
                } else if (marker.metadata == "supply_chest") {
                    lootTable = "minecraft:chests/shipwreck_supply";
                }
                if (lootTable != nullptr) {
                    int64_t seed = random.nextLong();
                    core::BlockPos below = marker.pos.below();
                    if (auto* chunk = level->getChunk(below.getX() >> 4,
                                                      below.getZ() >> 4)) {
                        chunk->setBlockEntityNbt(below,
                            std::string("{LootTable:\"") + lootTable
                            + "\",LootTableSeed:" + std::to_string(seed)
                            + "l,components:{},id:\"minecraft:chest\"}");
                    }
                }
            }
        }
    }

private:
    std::string m_templateId;
    int m_rotation;
    bool m_isBeached;
    core::BlockPos m_templatePosition;
};

// Reference: OceanRuinPieces.OceanRuinPiece.
class OceanRuinBehavior final : public StructurePieceBehavior {
public:
    OceanRuinBehavior(std::string templateId, int rotation, float integrity, bool warm,
                      bool isLarge, core::BlockPos templatePosition)
        : m_templateId(std::move(templateId)), m_rotation(rotation),
          m_integrity(integrity), m_warm(warm), m_isLarge(isLarge),
          m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        const FullTemplateData& data = TemplateEngine::get(m_templateId);
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = 0;
        settings.rotationPivot = core::BlockPos(0, 0, 0);
        settings.ignoreAir = true;   // STRUCTURE_AND_AIR
        settings.keepLiquids = true;
        // Reference: BlockRotProcessor(integrity) - positional per-block
        // random; keep iff nextFloat <= integrity (draw even at 1.0).
        float integrity = m_integrity;
        settings.processors.push_back(
            [integrity](const core::BlockPos& world, BlockState* state,
                        const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                LegacyRandomSource rotRandom(
                    Mth::getSeed(world.getX(), world.getY(), world.getZ()));
                return rotRandom.nextFloat() <= integrity ? state : nullptr;
            });
        // Reference: archyRuleProcessor - capped suspicious replacement with
        // the AppendLoot BE modifier (B8).
        {
            TemplatePlaceSettings::CappedReplace capped;
            capped.fromBlock = m_warm ? "minecraft:sand" : "minecraft:gravel";
            capped.toBlock = m_warm ? "minecraft:suspicious_sand"
                                    : "minecraft:suspicious_gravel";
            capped.limit = 5;
            capped.lootTable = m_warm ? "minecraft:archaeology/ocean_ruin_warm"
                                      : "minecraft:archaeology/ocean_ruin_cold";
            settings.cappedReplaces.push_back(capped);
        }

        // Reference: OceanRuinPiece.postProcess - anchor to OCEAN_FLOOR_WG,
        // then the sunken-floor probe (both PERSISTENT mutations).
        int height = level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG,
                                      m_templatePosition.getX(), m_templatePosition.getZ());
        m_templatePosition = core::BlockPos(m_templatePosition.getX(), height,
                                            m_templatePosition.getZ());
        core::BlockPos corner = TemplateEngine::calculateRelativePosition(
            settings, core::BlockPos(data.sizeX - 1, 0, data.sizeZ - 1));
        corner = corner.offset(m_templatePosition.getX(), m_templatePosition.getY(),
                               m_templatePosition.getZ());
        m_templatePosition = core::BlockPos(
            m_templatePosition.getX(),
            probeHeight(m_templatePosition, level, corner),
            m_templatePosition.getZ());

        self.boundingBox = templateBox(m_templateId, settings, m_templatePosition);
        if (TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                         referencePos, settings, random, chunkBB)) {
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(m_templateId, m_templatePosition, settings, chunkBB)) {
                handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB,
                                 generator->getSeaLevel());
            }
        }
    }

private:
    std::string m_templateId;
    int m_rotation;
    float m_integrity;
    bool m_warm;
    bool m_isLarge;
    core::BlockPos m_templatePosition;

    static bool hasWaterFluid(BlockState* state) {
        const std::string& id = state->getBlock()->getIdentifier();
        if (id == "minecraft:water") return true;
        auto* wl = world::level::block::state::properties::BlockStateProperties::WATERLOGGED;
        if (state->hasProperty(wl) && state->getValue(*wl)) return true;
        return id == "minecraft:kelp" || id == "minecraft:kelp_plant"
            || id == "minecraft:seagrass" || id == "minecraft:tall_seagrass"
            || id == "minecraft:bubble_column";
    }

    static bool isIceTag(BlockState* state) {
        const std::string& id = state->getBlock()->getIdentifier();
        return id == "minecraft:ice" || id == "minecraft:packed_ice"
            || id == "minecraft:blue_ice" || id == "minecraft:frosted_ice";
    }

    // Reference: OceanRuinPiece.getHeight - sunken-floor probe.
    int probeHeight(const core::BlockPos& pos, WorldGenLevel* level,
                    const core::BlockPos& corner) const {
        int newY = pos.getY();
        int minY = 512;
        int topY = newY - 1;
        int area = 0;
        int x0 = std::min(pos.getX(), corner.getX());
        int x1 = std::max(pos.getX(), corner.getX());
        int z0 = std::min(pos.getZ(), corner.getZ());
        int z1 = std::max(pos.getZ(), corner.getZ());
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                int floorY = pos.getY() - 1;
                BlockState* state = level->getBlockState(core::BlockPos(x, floorY, z));
                while ((state->isAir() || hasWaterFluid(state) || isIceTag(state))
                       && floorY > level->getMinY() + 1) {
                    --floorY;
                    state = level->getBlockState(core::BlockPos(x, floorY, z));
                }
                minY = std::min(minY, floorY);
                if (floorY < topY - 2) ++area;
            }
        }
        int width = std::abs(pos.getX() - corner.getX());
        if (topY - minY > 2 && area > width - 2) {
            newY = minY + 1;
        }
        return newY;
    }

    // Reference: OceanRuinPiece.handleDataMarker. "chest" places a chest with
    // fluid-derived waterlogging + the loot draw. "drowned" spawns the entity
    // (succeeds in the headless server; entity itself is out of scope) and
    // then REPLACES the marker block: AIR above sea level, WATER at/below -
    // the block write is dump-visible (big ruins bury markers in stone).
    void handleDataMarker(const std::string& markerId, const core::BlockPos& pos,
                          WorldGenLevel* level, WorldgenRandom& random,
                          const BoundingBox& chunkBB, int seaLevel) {
        (void)chunkBB;
        if (markerId == "chest") {
            bool waterlogged = hasWaterFluid(level->getBlockState(pos));
            BlockState* chest = Blocks::CHEST->defaultBlockState()->setValue(
                *world::level::block::state::properties::BlockStateProperties::WATERLOGGED,
                waterlogged);
            level->setBlock(pos, chest, 2);
            int64_t seed = random.nextLong();  // ChestBlockEntity.setLootTable
            const char* lootTable = m_isLarge
                ? "minecraft:chests/underwater_ruin_big"
                : "minecraft:chests/underwater_ruin_small";
            if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->setBlockEntityNbt(pos,
                    std::string("{LootTable:\"") + lootTable + "\",LootTableSeed:"
                    + std::to_string(seed)
                    + "l,components:{},id:\"minecraft:chest\"}");
            }
        } else if (markerId == "drowned") {
            // finalizeSpawn uses the LEVEL random, not this stream - no draws.
            if (pos.getY() > seaLevel) {
                level->setBlock(pos, Blocks::AIR->defaultBlockState(), 2);
            } else {
                level->setBlock(pos, Blocks::WATER->defaultBlockState(), 2);
            }
        }
    }
};

// Reference: RuinedPortalPiece - the processor-heavy family.
class RuinedPortalBehavior final : public StructurePieceBehavior {
public:
    struct Properties {
        bool cold = false;
        float mossiness = 0.2f;
        bool airPocket = false;
        bool overgrown = false;
        bool vines = false;
        bool replaceWithBlackstone = false;
    };

    RuinedPortalBehavior(std::string templateId, int rotation, bool mirrorFrontBack,
                         core::BlockPos pivot, core::BlockPos templatePosition,
                         std::string placement, Properties properties)
        : m_templateId(std::move(templateId)), m_rotation(rotation),
          m_mirrorFrontBack(mirrorFrontBack), m_pivot(pivot),
          m_templatePosition(templatePosition), m_placement(std::move(placement)),
          m_properties(properties) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        TemplatePlaceSettings settings = makeSettings(level);
        BoundingBox box = templateBox(m_templateId, settings, m_templatePosition);
        // Reference: single-chunk placement - only the chunk owning the bbox
        // CENTER places, with the chunk box ENLARGED to cover the bbox.
        core::BlockPos center(box.centerX(), box.centerY(), box.centerZ());
        if (!chunkBB.isInside(center.getX(), center.getY(), center.getZ())) return;
        BoundingBox enlarged = chunkBB;
        enlarged.minX = std::min(enlarged.minX, box.minX);
        enlarged.minY = std::min(enlarged.minY, box.minY);
        enlarged.minZ = std::min(enlarged.minZ, box.minZ);
        enlarged.maxX = std::max(enlarged.maxX, box.maxX);
        enlarged.maxY = std::max(enlarged.maxY, box.maxY);
        enlarged.maxZ = std::max(enlarged.maxZ, box.maxZ);

        self.boundingBox = box;
        m_lastBox = box;
        if (TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                         referencePos, settings, random, enlarged)) {
            // handleDataMarker is empty for portals; jigsaw blocks become
            // their final_state.
            TemplateEngine::applyJigsawFinalStates(level, m_templateId,
                                                   m_templatePosition, settings);
        }
        spreadNetherrack(random, level);
        addNetherrackDripColumnsBelowPortal(random, level);
        if (m_properties.vines || m_properties.overgrown) {
            // Reference: BlockPos.betweenClosedStream(bbox) - x fastest,
            // then y, then z.
            for (int z = m_lastBox.minZ; z <= m_lastBox.maxZ; ++z) {
                for (int y = m_lastBox.minY; y <= m_lastBox.maxY; ++y) {
                    for (int x = m_lastBox.minX; x <= m_lastBox.maxX; ++x) {
                        core::BlockPos pos(x, y, z);
                        if (m_properties.vines) maybeAddVines(random, level, pos);
                        if (m_properties.overgrown) maybeAddLeavesAbove(random, level, pos);
                    }
                }
            }
        }
    }

private:
    std::string m_templateId;
    int m_rotation;
    bool m_mirrorFrontBack;
    core::BlockPos m_pivot;
    core::BlockPos m_templatePosition;
    std::string m_placement;
    Properties m_properties;
    BoundingBox m_lastBox;

    static bool isFullBlockProxy(BlockState* state) {
        if (state->isAir() || !state->isSolid()) return false;
        const std::string& id = state->getBlock()->getIdentifier();
        auto ends = [&](const char* s) {
            size_t n = std::strlen(s);
            return id.size() > n && id.compare(id.size() - n, n, s) == 0;
        };
        return !(ends("_stairs") || ends("_slab") || ends("_fence") || ends("_wall")
                 || ends("_pane") || ends("_bed") || ends("_door") || ends("_trapdoor")
                 || ends("_carpet") || id == "minecraft:iron_bars"
                 || id == "minecraft:iron_chain" || id == "minecraft:chest"
                 || id == "minecraft:lava" || id == "minecraft:water");
    }

    TemplatePlaceSettings makeSettings(WorldGenLevel* level) const {
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = m_mirrorFrontBack ? 2 : 0;  // FRONT_BACK : NONE
        settings.rotationPivot = m_pivot;
        settings.ignoreAir = !m_properties.airPocket;  // air pocket keeps air
        settings.keepLiquids = true;
        bool cold = m_properties.cold;
        bool oceanFloor = m_placement == "on_ocean_floor";
        float mossiness = m_properties.mossiness;
        // Reference: RuleProcessor(rules) - ONE positional random per block,
        // rules first-match in order.
        settings.processors.push_back(
            [cold, oceanFloor](const core::BlockPos& world, BlockState* state,
                               const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                LegacyRandomSource ruleRandom(
                    Mth::getSeed(world.getX(), world.getY(), world.getZ()));
                // Rule 1: RandomBlockMatch(gold_block, 0.3) -> air.
                if (state->is(Blocks::getBlock("minecraft:gold_block"))) {
                    if (ruleRandom.nextFloat() < 0.3f) {
                        return Blocks::AIR->defaultBlockState();
                    }
                    return state;  // matched block, failed roll: later rules
                                   // cannot match a gold block anyway
                }
                // Rule 2: the lava rule.
                if (state->is(Blocks::getBlock("minecraft:lava"))) {
                    if (oceanFloor) {
                        return Blocks::getDefaultState("minecraft:magma_block");
                    }
                    if (cold) {
                        return Blocks::getDefaultState("minecraft:netherrack");
                    }
                    if (ruleRandom.nextFloat() < 0.2f) {
                        return Blocks::getDefaultState("minecraft:magma_block");
                    }
                    return state;
                }
                // Rule 3 (!cold): RandomBlockMatch(netherrack, 0.07) -> magma.
                if (!cold && state->is(Blocks::getBlock("minecraft:netherrack"))) {
                    if (ruleRandom.nextFloat() < 0.07f) {
                        return Blocks::getDefaultState("minecraft:magma_block");
                    }
                    return state;
                }
                return state;
            });
        // Reference: BlockAgeProcessor(mossiness) - fresh positional random.
        settings.processors.push_back(
            [mossiness](const core::BlockPos& world, BlockState* state,
                        const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                return blockAge(world, state, mossiness);
            });
        // Reference: ProtectedBlockProcessor(#features_cannot_replace) - drop
        // when the WORLD block is protected.
        settings.processors.push_back(
            [level](const core::BlockPos& world, BlockState* state,
                    const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                BlockState* existing = level->getBlockState(world);
                if (::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                        existing, "minecraft:features_cannot_replace")) {
                    return nullptr;
                }
                return state;
            });
        // Reference: LavaSubmergedBlockProcessor - lava swallows partials.
        settings.processors.push_back(
            [level](const core::BlockPos& world, BlockState* state,
                    const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                BlockState* existing = level->getBlockState(world);
                if (existing->is(Blocks::getBlock("minecraft:lava"))
                    && !isFullBlockProxy(state)) {
                    return Blocks::getDefaultState("minecraft:lava");
                }
                return state;
            });
        // Reference: BlackstoneReplaceProcessor (nether portals only, LAST in
        // the chain). Pure block->block map, no RNG; copies stairs
        // FACING/HALF and slab TYPE onto the replacement's default state.
        if (m_properties.replaceWithBlackstone) {
            settings.processors.push_back(
                [](const core::BlockPos&, BlockState* state,
                   const core::BlockPos&, BlockState*,
                    const core::BlockPos&) -> BlockState* {
                    return blackstoneReplace(state);
                });
        }
        return settings;
    }

    // Reference: BlackstoneReplaceProcessor.processBlock.
    static BlockState* blackstoneReplace(BlockState* state) {
        if (state == nullptr) return state;
        static const std::unordered_map<std::string, const char*> s_replacements = {
            {"minecraft:cobblestone", "minecraft:blackstone"},
            {"minecraft:mossy_cobblestone", "minecraft:blackstone"},
            {"minecraft:stone", "minecraft:polished_blackstone"},
            {"minecraft:stone_bricks", "minecraft:polished_blackstone_bricks"},
            {"minecraft:mossy_stone_bricks", "minecraft:polished_blackstone_bricks"},
            {"minecraft:cobblestone_stairs", "minecraft:blackstone_stairs"},
            {"minecraft:mossy_cobblestone_stairs", "minecraft:blackstone_stairs"},
            {"minecraft:stone_stairs", "minecraft:polished_blackstone_stairs"},
            {"minecraft:stone_brick_stairs", "minecraft:polished_blackstone_brick_stairs"},
            {"minecraft:mossy_stone_brick_stairs", "minecraft:polished_blackstone_brick_stairs"},
            {"minecraft:cobblestone_slab", "minecraft:blackstone_slab"},
            {"minecraft:mossy_cobblestone_slab", "minecraft:blackstone_slab"},
            {"minecraft:smooth_stone_slab", "minecraft:polished_blackstone_slab"},
            {"minecraft:stone_slab", "minecraft:polished_blackstone_slab"},
            {"minecraft:stone_brick_slab", "minecraft:polished_blackstone_brick_slab"},
            {"minecraft:mossy_stone_brick_slab", "minecraft:polished_blackstone_brick_slab"},
            {"minecraft:stone_brick_wall", "minecraft:polished_blackstone_brick_wall"},
            {"minecraft:mossy_stone_brick_wall", "minecraft:polished_blackstone_brick_wall"},
            {"minecraft:cobblestone_wall", "minecraft:blackstone_wall"},
            {"minecraft:mossy_cobblestone_wall", "minecraft:blackstone_wall"},
            {"minecraft:chiseled_stone_bricks", "minecraft:chiseled_polished_blackstone"},
            {"minecraft:cracked_stone_bricks", "minecraft:cracked_polished_blackstone_bricks"},
            {"minecraft:iron_bars", "minecraft:iron_chain"},
        };
        auto it = s_replacements.find(state->getIdentifier());
        if (it == s_replacements.end()) {
            return state;
        }
        BlockState* newState = Blocks::getDefaultState(it->second);
        if (newState == nullptr) return state;
        using world::level::block::state::properties::BlockStateProperties;
        if (state->hasProperty(BlockStateProperties::HORIZONTAL_FACING) &&
            newState->hasProperty(BlockStateProperties::HORIZONTAL_FACING)) {
            newState = newState->setValue(*BlockStateProperties::HORIZONTAL_FACING,
                state->getValue(*BlockStateProperties::HORIZONTAL_FACING));
        }
        if (state->hasProperty(BlockStateProperties::HALF) &&
            newState->hasProperty(BlockStateProperties::HALF)) {
            newState = newState->setValue(*BlockStateProperties::HALF,
                state->getValue(*BlockStateProperties::HALF));
        }
        if (state->hasProperty(BlockStateProperties::SLAB_TYPE) &&
            newState->hasProperty(BlockStateProperties::SLAB_TYPE)) {
            newState = newState->setValue(*BlockStateProperties::SLAB_TYPE,
                state->getValue(*BlockStateProperties::SLAB_TYPE));
        }
        return newState;
    }

    // Reference: BlockAgeProcessor.processBlock (exact draw order).
    static BlockState* blockAge(const core::BlockPos& world, BlockState* state,
                                float mossiness) {
        namespace props = world::level::block::state::properties;
        LegacyRandomSource ageRandom(
            Mth::getSeed(world.getX(), world.getY(), world.getZ()));
        const std::string& id = state->getBlock()->getIdentifier();
        auto ends = [&](const char* s) {
            size_t n = std::strlen(s);
            return id.size() > n && id.compare(id.size() - n, n, s) == 0;
        };
        auto randomFacingStairs = [&](const char* stairs) {
            static const core::Direction kHorizontal[4] = {
                core::Direction::NORTH, core::Direction::EAST,
                core::Direction::SOUTH, core::Direction::WEST};
            core::Direction facing =
                kHorizontal[ageRandom.nextInt(4)];  // Plane.HORIZONTAL.getRandomDirection
            props::Half half(ageRandom.nextInt(2) == 0 ? props::Half::TOP
                                                       : props::Half::BOTTOM);
            return Blocks::getDefaultState(stairs)
                ->setValue(*world::level::block::state::properties::BlockStateProperties::HORIZONTAL_FACING, facing)
                ->setValue(*world::level::block::state::properties::BlockStateProperties::HALF, half);
        };
        // Reference: Block.withPropertiesOf - copy every shared property from
        // the source state; unshared properties keep the target's defaults.
        auto withPropertiesOf = [](const char* target, BlockState* source) {
            BlockState* result = Blocks::getDefaultState(target);
            auto sourceProps = source->getProperties();
            auto defaults = result->getProperties();
            for (BlockState* candidate :
                 result->getBlock()->getStateDefinition().getPossibleStates()) {
                bool ok = true;
                for (const auto& [key, value] : candidate->getProperties()) {
                    auto it = sourceProps.find(key);
                    const std::string& wantValue =
                        it != sourceProps.end() ? it->second : defaults[key];
                    if (value != wantValue) {
                        ok = false;
                        break;
                    }
                }
                if (ok) return candidate;
            }
            return result;
        };
        auto randomBlock2 = [&](BlockState* a, BlockState* b) {
            return ageRandom.nextInt(2) == 0 ? a : b;
        };

        BlockState* newState = nullptr;
        if (id == "minecraft:stone_bricks" || id == "minecraft:stone"
            || id == "minecraft:chiseled_stone_bricks") {
            // maybeReplaceFullStoneBlock.
            if (ageRandom.nextFloat() < 0.5f) {
                BlockState* nonMossyA = Blocks::getDefaultState("minecraft:cracked_stone_bricks");
                BlockState* nonMossyB = randomFacingStairs("minecraft:stone_brick_stairs");
                BlockState* mossyA = Blocks::getDefaultState("minecraft:mossy_stone_bricks");
                BlockState* mossyB = randomFacingStairs("minecraft:mossy_stone_brick_stairs");
                newState = ageRandom.nextFloat() < mossiness ? randomBlock2(mossyA, mossyB)
                                                             : randomBlock2(nonMossyA, nonMossyB);
            }
        } else if (ends("_stairs")) {
            // maybeReplaceStairs (BlockTags.STAIRS membership approximated by
            // the suffix; template stairs are all in the tag).
            if (ageRandom.nextFloat() < 0.5f) {
                BlockState* mossyA =
                    withPropertiesOf("minecraft:mossy_stone_brick_stairs", state);
                BlockState* mossyB =
                    Blocks::getDefaultState("minecraft:mossy_stone_brick_slab");
                BlockState* nonMossyA = Blocks::getDefaultState("minecraft:stone_slab");
                BlockState* nonMossyB = Blocks::getDefaultState("minecraft:stone_brick_slab");
                newState = ageRandom.nextFloat() < mossiness ? randomBlock2(mossyA, mossyB)
                                                             : randomBlock2(nonMossyA, nonMossyB);
            }
        } else if (ends("_slab")) {
            if (ageRandom.nextFloat() < mossiness) {
                newState = withPropertiesOf("minecraft:mossy_stone_brick_slab", state);
            }
        } else if (ends("_wall")) {
            if (ageRandom.nextFloat() < mossiness) {
                newState = withPropertiesOf("minecraft:mossy_stone_brick_wall", state);
            }
        } else if (id == "minecraft:obsidian") {
            if (ageRandom.nextFloat() < 0.15f) {
                newState = Blocks::getDefaultState("minecraft:crying_obsidian");
            }
        }
        return newState != nullptr ? newState : state;
    }

    // Reference: RuinedPortalPiece.getSurfaceY.
    int surfaceY(WorldGenLevel* level, int x, int z) const {
        Heightmap::Types type = m_placement == "on_ocean_floor"
            ? Heightmap::Types::OCEAN_FLOOR_WG : Heightmap::Types::WORLD_SURFACE_WG;
        return level->getHeight(type, x, z) - 1;
    }

    bool canBlockBeReplacedByNetherrackOrMagma(WorldGenLevel* level,
                                               const core::BlockPos& pos) const {
        BlockState* state = level->getBlockState(pos);
        return !state->isAir() && !state->is(Blocks::getBlock("minecraft:obsidian"))
            && !::minecraft::levelgen::blockpredicates::matchesBlockTagName(
                   state, "minecraft:features_cannot_replace")
            && (m_placement == "in_nether"
                || !state->is(Blocks::getBlock("minecraft:lava")));
    }

    void placeNetherrackOrMagma(WorldgenRandom& random, WorldGenLevel* level,
                                const core::BlockPos& pos) const {
        if (!m_properties.cold && random.nextFloat() < 0.07f) {
            level->setBlock(pos, Blocks::getDefaultState("minecraft:magma_block"), 3);
        } else {
            level->setBlock(pos, Blocks::getDefaultState("minecraft:netherrack"), 3);
        }
    }

    void addNetherrackDripColumn(WorldgenRandom& random, WorldGenLevel* level,
                                 const core::BlockPos& start) const {
        core::BlockPos pos = start;
        placeNetherrackOrMagma(random, level, pos);
        int remaining = 8;
        while (remaining > 0 && random.nextFloat() < 0.5f) {
            pos = pos.below();
            --remaining;
            placeNetherrackOrMagma(random, level, pos);
        }
    }

    void addNetherrackDripColumnsBelowPortal(WorldgenRandom& random,
                                             WorldGenLevel* level) const {
        for (int x = m_lastBox.minX + 1; x < m_lastBox.maxX; ++x) {
            for (int z = m_lastBox.minZ + 1; z < m_lastBox.maxZ; ++z) {
                core::BlockPos pos(x, m_lastBox.minY, z);
                if (level->getBlockState(pos)->is(Blocks::getBlock("minecraft:netherrack"))) {
                    addNetherrackDripColumn(random, level, pos.below());
                }
            }
        }
    }

    void spreadNetherrack(WorldgenRandom& random, WorldGenLevel* level) const {
        bool followGroundSurface =
            m_placement == "on_land_surface" || m_placement == "on_ocean_floor";
        int centerX = m_lastBox.centerX();
        int centerZ = m_lastBox.centerZ();
        static const float kProbability[14] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                               0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
        const int maxDistance = 14;
        int averageWidth = (m_lastBox.getXSpan() + m_lastBox.getZSpan()) / 2;
        int distanceAdjustment = random.nextInt(std::max(1, 8 - averageWidth / 2));
        for (int x = centerX - maxDistance; x <= centerX + maxDistance; ++x) {
            for (int z = centerZ - maxDistance; z <= centerZ + maxDistance; ++z) {
                int distance = std::abs(x - centerX) + std::abs(z - centerZ);
                int adjusted = std::max(0, distance + distanceAdjustment);
                if (adjusted >= maxDistance) continue;
                float probability = kProbability[adjusted];
                if (random.nextDouble() < static_cast<double>(probability)) {
                    int surface = surfaceY(level, x, z);
                    int y = followGroundSurface ? surface
                                                : std::min(m_lastBox.minY, surface);
                    core::BlockPos pos(x, y, z);
                    if (std::abs(y - m_lastBox.minY) <= 3
                        && canBlockBeReplacedByNetherrackOrMagma(level, pos)) {
                        placeNetherrackOrMagma(random, level, pos);
                        if (m_properties.overgrown) {
                            maybeAddLeavesAbove(random, level, pos);
                        }
                        addNetherrackDripColumn(random, level, pos.below());
                    }
                }
            }
        }
    }

    void maybeAddVines(WorldgenRandom& random, WorldGenLevel* level,
                       const core::BlockPos& pos) const {
        BlockState* state = level->getBlockState(pos);
        if (state->isAir() || state->is(Blocks::getBlock("minecraft:vine"))) return;
        static const core::Direction kHorizontal[4] = {
            core::Direction::NORTH, core::Direction::EAST,
            core::Direction::SOUTH, core::Direction::WEST};
        core::Direction direction = kHorizontal[random.nextInt(4)];
        core::BlockPos neighborPos = pos.offset(core::getStepX(direction), 0,
                                                core::getStepZ(direction));
        BlockState* neighborState = level->getBlockState(neighborPos);
        if (!neighborState->isAir()) return;
        // Reference: Block.isFaceFull(collision shape, direction).
        if (!state->isFaceSturdy(*level, pos, direction)) return;
        using VB = world::level::block::VineBlock;
        core::Direction attach = core::getOpposite(direction);
        world::level::block::state::properties::BooleanProperty* prop =
            attach == core::Direction::NORTH ? VB::NORTH
            : attach == core::Direction::SOUTH ? VB::SOUTH
            : attach == core::Direction::WEST ? VB::WEST : VB::EAST;
        level->setBlock(neighborPos,
                        Blocks::getDefaultState("minecraft:vine")->setValue(*prop, true), 3);
    }

    void maybeAddLeavesAbove(WorldgenRandom& random, WorldGenLevel* level,
                             const core::BlockPos& pos) const {
        if (random.nextFloat() < 0.5f
            && level->getBlockState(pos)->is(Blocks::getBlock("minecraft:netherrack"))
            && level->getBlockState(pos.above())->isAir()) {
            BlockState* leaves = Blocks::getDefaultState("minecraft:jungle_leaves");
            auto* persistent =
                world::level::block::state::properties::BlockStateProperties::PERSISTENT;
            if (persistent != nullptr && leaves->hasProperty(persistent)) {
                leaves = leaves->setValue(*persistent, true);
            }
            level->setBlock(pos.above(), leaves, 3);
        }
    }
};

// Reference: WoodlandMansionPieces.WoodlandMansionPiece - template piece with
// rotation + mirror, pivot ZERO, STRUCTURE_BLOCK ignore, default waterlogging.
class MansionBehavior final : public StructurePieceBehavior {
public:
    MansionBehavior(std::string templateName, int rotation, int mirror,
                    core::BlockPos templatePosition)
        : m_templateName(std::move(templateName)), m_rotation(rotation),
          m_mirror(mirror), m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = m_mirror;
        settings.rotationPivot = core::BlockPos(0, 0, 0);
        settings.ignoreAir = false;   // BlockIgnoreProcessor.STRUCTURE_BLOCK only
        settings.keepLiquids = true;  // default LiquidSettings.APPLY_WATERLOGGING

        std::string templateId = "minecraft:woodland_mansion/" + m_templateName;
        self.boundingBox = templateBox(templateId, settings, m_templatePosition);
        if (TemplateEngine::placeInWorld(level, templateId, m_templatePosition,
                                         referencePos, settings, random, chunkBB)) {
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(templateId, m_templatePosition, settings, chunkBB)) {
                handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB);
            }
        }
    }

private:
    std::string m_templateName;
    int m_rotation;
    int m_mirror;
    core::BlockPos m_templatePosition;

    // Reference: Rotation.rotate(Direction).
    static core::Direction rotateDir(core::Direction dir, int rotation) {
        using core::Direction;
        for (int i = 0; i < (rotation & 3); ++i) {
            switch (dir) {
                case Direction::NORTH: dir = Direction::EAST; break;
                case Direction::EAST: dir = Direction::SOUTH; break;
                case Direction::SOUTH: dir = Direction::WEST; break;
                case Direction::WEST: dir = Direction::NORTH; break;
                default: break;
            }
        }
        return dir;
    }

    // Reference: WoodlandMansionPiece.handleDataMarker. The chest facing uses
    // placeSettings.getRotation() ONLY (mirror not applied - vanilla quirk);
    // Mage/Warrior/allay markers create entities - creation fails in the
    // reference harness (ocean-ruin drowned precedent): no writes, no draws
    // from the passed random.
    void handleDataMarker(const std::string& markerId, const core::BlockPos& pos,
                          WorldGenLevel* level, WorldgenRandom& random,
                          const BoundingBox& chunkBB) {
        if (markerId.rfind("Chest", 0) != 0) return;
        auto* facingProp =
            world::level::block::state::properties::BlockStateProperties::HORIZONTAL_FACING;
        BlockState* chestState = Blocks::CHEST->defaultBlockState();
        if (markerId == "ChestWest") {
            chestState = chestState->setValue(*facingProp,
                                              rotateDir(core::Direction::WEST, m_rotation));
        } else if (markerId == "ChestEast") {
            chestState = chestState->setValue(*facingProp,
                                              rotateDir(core::Direction::EAST, m_rotation));
        } else if (markerId == "ChestSouth") {
            chestState = chestState->setValue(*facingProp,
                                              rotateDir(core::Direction::SOUTH, m_rotation));
        } else if (markerId == "ChestNorth") {
            chestState = chestState->setValue(*facingProp,
                                              rotateDir(core::Direction::NORTH, m_rotation));
        }
        // Reference: StructurePiece.createChest(pos, state) - explicit state,
        // no reorient; loot-seed nextLong when actually placed.
        if (chunkBB.isInside(pos.getX(), pos.getY(), pos.getZ())
            && !level->getBlockState(pos)->is(Blocks::CHEST)) {
            level->setBlock(pos, chestState, 2);
            int64_t seed = random.nextLong();
            if (auto* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
                chunk->setBlockEntityNbt(pos,
                    "{LootTable:\"minecraft:chests/woodland_mansion\","
                    "LootTableSeed:" + std::to_string(seed)
                    + "l,components:{},id:\"minecraft:chest\"}");
            }
        }
    }
};

} // namespace

std::shared_ptr<StructurePieceBehavior> mansionPiece(const std::string& templateName,
                                                     int rotation, int mirror,
                                                     const core::BlockPos& templatePosition) {
    return std::make_shared<MansionBehavior>(templateName, rotation, mirror, templatePosition);
}

std::function<void(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const BoundingBox&,
                   const ::world::ChunkPos&, StructureStartData&)>
mansionAfterPlace() {
    // Reference: WoodlandMansionStructure.afterPlace - cobblestone columns
    // from (pieces-bbox minY - 1) down to the first solid, for every chunk
    // column whose bbox-floor block is non-air and inside some piece. No RNG.
    return [](WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
              const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
              StructureStartData& start) {
        (void)generator; (void)random; (void)chunkPos;
        if (start.pieces.empty()) return;
        int minY = level->getMinY();
        BoundingBox total = start.pieces.front().boundingBox;
        for (const auto& piece : start.pieces) {
            total.minX = std::min(total.minX, piece.boundingBox.minX);
            total.minY = std::min(total.minY, piece.boundingBox.minY);
            total.minZ = std::min(total.minZ, piece.boundingBox.minZ);
            total.maxX = std::max(total.maxX, piece.boundingBox.maxX);
            total.maxY = std::max(total.maxY, piece.boundingBox.maxY);
            total.maxZ = std::max(total.maxZ, piece.boundingBox.maxZ);
        }
        int yStart = total.minY;
        BlockState* cobblestone = Blocks::getDefaultState("minecraft:cobblestone");
        for (int x = chunkBB.minX; x <= chunkBB.maxX; ++x) {
            for (int z = chunkBB.minZ; z <= chunkBB.maxZ; ++z) {
                core::BlockPos pos(x, yStart, z);
                if (level->isEmptyBlock(pos) || !total.isInside(x, yStart, z)) continue;
                bool insidePiece = false;
                for (const auto& piece : start.pieces) {
                    if (piece.boundingBox.isInside(x, yStart, z)) {
                        insidePiece = true;
                        break;
                    }
                }
                if (!insidePiece) continue;
                for (int y = yStart - 1; y > minY; --y) {
                    core::BlockPos below(x, y, z);
                    if (!level->isEmptyBlock(below)
                        && !level->getBlockState(below)->isFluid()) {
                        break;
                    }
                    level->setBlock(below, cobblestone, 2);
                }
            }
        }
    };
}

std::shared_ptr<StructurePieceBehavior> ruinedPortal(
    const std::string& templateId, int rotation, bool mirrorFrontBack,
    const core::BlockPos& pivot, const core::BlockPos& templatePosition,
    const std::string& placement, bool cold, float mossiness, bool airPocket,
    bool overgrown, bool vines, bool replaceWithBlackstone) {
    RuinedPortalBehavior::Properties properties;
    properties.cold = cold;
    properties.mossiness = mossiness;
    properties.airPocket = airPocket;
    properties.overgrown = overgrown;
    properties.vines = vines;
    properties.replaceWithBlackstone = replaceWithBlackstone;
    return std::make_shared<RuinedPortalBehavior>(templateId, rotation, mirrorFrontBack,
                                                  pivot, templatePosition, placement,
                                                  properties);
}

std::shared_ptr<StructurePieceBehavior> oceanRuin(const std::string& templateId, int rotation,
                                                  float integrity, bool warm, bool isLarge,
                                                  const core::BlockPos& templatePosition) {
    return std::make_shared<OceanRuinBehavior>(templateId, rotation, integrity, warm,
                                               isLarge, templatePosition);
}

std::shared_ptr<StructurePieceBehavior> shipwreck(const std::string& templateId, int rotation,
                                                  bool isBeached,
                                                  const core::BlockPos& templatePosition) {
    return std::make_shared<ShipwreckBehavior>(templateId, rotation, isBeached,
                                               templatePosition);
}

std::shared_ptr<StructurePieceBehavior> igloo(const std::string& templateId, int rotation,
                                              const core::BlockPos& pivot,
                                              const core::BlockPos& offset,
                                              const core::BlockPos& templatePosition) {
    return std::make_shared<IglooBehavior>(templateId, rotation, pivot, offset,
                                           templatePosition);
}

namespace {

// Reference: NetherFossilPieces.NetherFossilPiece.
class NetherFossilBehavior final : public StructurePieceBehavior {
public:
    NetherFossilBehavior(std::string templateId, int rotation,
                         core::BlockPos templatePosition)
        : m_templateId(std::move(templateId)), m_rotation(rotation),
          m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        // makeSettings: rotation, Mirror.NONE, BlockIgnoreProcessor
        // .STRUCTURE_AND_AIR; LiquidSettings default (apply waterlogging).
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = 0;
        settings.rotationPivot = core::BlockPos(0, 0, 0);
        settings.ignoreAir = true;
        settings.keepLiquids = true;

        // Reference: NetherFossilPiece.postProcess - chunkBB ENCAPSULATES the
        // fossil bbox before super.postProcess (the whole fossil places from
        // every intersecting chunk's pass; writes stay within region radius).
        BoundingBox fossilBB = templateBox(m_templateId, settings, m_templatePosition);
        BoundingBox enlarged = chunkBB;
        enlarged.minX = std::min(enlarged.minX, fossilBB.minX);
        enlarged.minY = std::min(enlarged.minY, fossilBB.minY);
        enlarged.minZ = std::min(enlarged.minZ, fossilBB.minZ);
        enlarged.maxX = std::max(enlarged.maxX, fossilBB.maxX);
        enlarged.maxY = std::max(enlarged.maxY, fossilBB.maxY);
        enlarged.maxZ = std::max(enlarged.maxZ, fossilBB.maxZ);

        self.boundingBox = fossilBB;
        TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                     referencePos, settings, random, enlarged);
        // handleDataMarker is a no-op for fossils.

        // Reference: placeDriedGhast - fresh positional legacy random at the
        // fossil bbox CENTER; gated by the ORIGINAL chunkBB.
        LegacyRandomSource base(level->getSeed());
        int32_t cx = fossilBB.centerX();
        int32_t cy = fossilBB.centerY();
        int32_t cz = fossilBB.centerZ();
        LegacyRandomSource positional = base.forkPositional().at(cx, cy, cz);
        if (positional.nextFloat() < 0.5f) {
            int x = fossilBB.minX + positional.nextInt(fossilBB.getXSpan());
            int y = fossilBB.minY;
            int z = fossilBB.minZ + positional.nextInt(fossilBB.getZSpan());
            core::BlockPos randomPos(x, y, z);
            BlockState* existing = level->getBlockState(randomPos);
            if (existing != nullptr && existing->isAir() &&
                chunkBB.isInside(x, y, z)) {
                // DRIED_GHAST.defaultBlockState().rotate(Rotation.getRandom)
                int rot = positional.nextInt(4);
                BlockState* ghast = Blocks::getDefaultState("minecraft:dried_ghast");
                ghast = state_transforms::rotateState(ghast, rot);
                level->setBlock(randomPos, ghast, 2);
            }
        }
    }

private:
    std::string m_templateId;
    int m_rotation;
    core::BlockPos m_templatePosition;
};

// Reference: EndCityPieces.EndCityPiece.
class EndCityBehavior final : public StructurePieceBehavior {
public:
    EndCityBehavior(std::string templateId, int rotation, bool overwrite,
                    core::BlockPos templatePosition)
        : m_templateId(std::move(templateId)), m_rotation(rotation),
          m_overwrite(overwrite), m_templatePosition(templatePosition) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator,
                     WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos,
                     StructurePieceData& self) override {
        (void)generator;
        (void)chunkPos;
        // makeSettings: setIgnoreEntities(true), processor = overwrite ?
        // STRUCTURE_BLOCK : STRUCTURE_AND_AIR, rotation, no pivot.
        TemplatePlaceSettings settings;
        settings.rotation = m_rotation;
        settings.mirror = 0;
        settings.rotationPivot = core::BlockPos(0, 0, 0);
        settings.ignoreAir = !m_overwrite;
        settings.keepLiquids = true;

        self.boundingBox = templateBox(m_templateId, settings, m_templatePosition);
        if (TemplateEngine::placeInWorld(level, m_templateId, m_templatePosition,
                                         referencePos, settings, random, chunkBB)) {
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(m_templateId, m_templatePosition,
                                             settings, chunkBB)) {
                handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB);
            }
        }
    }

private:
    // Reference: EndCityPiece.handleDataMarker.
    void handleDataMarker(const std::string& markerId, const core::BlockPos& pos,
                          WorldGenLevel* level, WorldgenRandom& random,
                          const BoundingBox& chunkBB) {
        if (markerId.rfind("Chest", 0) == 0) {
            core::BlockPos chestPos = pos.below();
            if (chunkBB.isInside(chestPos.getX(), chestPos.getY(), chestPos.getZ())) {
                // RandomizableContainer.setBlockEntityLootTable: the chest
                // block came from the template; here only the BE payload +
                // the nextLong seed draw.
                int64_t seed = random.nextLong();
                if (auto* chunk = level->getChunk(chestPos.getX() >> 4,
                                                  chestPos.getZ() >> 4)) {
                    chunk->setBlockEntityNbt(chestPos,
                        std::string("{LootTable:\"minecraft:chests/end_city_treasure\","
                                    "LootTableSeed:") + std::to_string(seed)
                        + "l,components:{},id:\"minecraft:chest\"}");
                }
            }
        }
        // Sentry (shulker) and Elytra (item frame) are entities: no worldgen
        // RNG draws from this stream, out of scope.
    }

    std::string m_templateId;
    int m_rotation;
    bool m_overwrite;
    core::BlockPos m_templatePosition;
};

}  // namespace

std::shared_ptr<StructurePieceBehavior> netherFossil(const std::string& templateId,
                                                     int rotation,
                                                     const core::BlockPos& templatePosition) {
    return std::make_shared<NetherFossilBehavior>(templateId, rotation, templatePosition);
}

std::shared_ptr<StructurePieceBehavior> endCity(const std::string& templateId, int rotation,
                                                bool overwrite,
                                                const core::BlockPos& templatePosition) {
    return std::make_shared<EndCityBehavior>(templateId, rotation, overwrite,
                                             templatePosition);
}

} // namespace PieceBehaviors
} // namespace structure
} // namespace levelgen
} // namespace minecraft
