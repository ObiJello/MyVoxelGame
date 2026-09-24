#include "data/worldgen/features/AetherFeatures.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/featuresize/FeatureSize.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/WorldGenLevel.h"
#include "world/biome/Biome.h"
#include "core/BlockPos.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The Aether 1.5.10 — data/aether/worldgen/configured_feature/*.json and the
// Java classes named at each feature. Aether blocks are registered by the
// engine as minecraft:<slug>.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using namespace levelgen;
using namespace levelgen::placement;
using levelgen::feature::TreeFeature;
using levelgen::feature::configurations::TreeConfiguration;
using levelgen::feature::configurations::TreeConfigurationBuilder;
using levelgen::feature::trunkplacers::StraightTrunkPlacer;
using levelgen::feature::foliageplacers::BlobFoliagePlacer;
using levelgen::feature::featuresize::TwoLayersFeatureSize;
using levelgen::feature::stateproviders::BlockStateProvider;
using levelgen::feature::stateproviders::SimpleStateProvider;
using levelgen::structure::templatesystem::BlockMatchTest;
using levelgen::structure::templatesystem::RuleTest;

// ============================================================================
// AercloudFeature.place — world/feature/AercloudFeature.java
// ============================================================================
bool AercloudFeature::place(FeaturePlaceContext<AercloudConfiguration>& context) {
    WorldGenLevel* level = context.level();
    WorldgenRandom& random = context.random();
    const AercloudConfiguration& config = context.config();

    // boolean direction = random.nextBoolean();
    // BlockPos blockPos = origin.offset(-random.nextInt(8), 0,
    //                                   (direction ? 8 : 0) - random.nextInt(8));
    // Java evaluates the offset arguments left to right; C++ argument order
    // is unspecified, so the two draws are sequenced explicitly.
    const bool direction = random.nextBoolean();
    const int32_t startX = -random.nextInt(8);
    const int32_t startZ = (direction ? 8 : 0) - random.nextInt(8);
    core::BlockPos blockPos = context.origin().offset(startX, 0, startZ);
    BlockState* blockState = config.block;   // simple_state_provider: no draw
    if (!blockState) return false;

    for (int32_t amount = 0; amount < config.bounds; ++amount) {
        const int32_t xOffset = random.nextInt(2);
        const int32_t yOffset = random.nextBoolean() ? random.nextInt(3) - 1 : 0;
        const int32_t zOffset = random.nextInt(2);

        if (direction) {
            blockPos = blockPos.offset(xOffset, yOffset, -zOffset);
        } else {
            blockPos = blockPos.offset(xOffset, yOffset, zOffset);
        }

        // The loop bounds call the random on EVERY check, as in Java.
        for (int32_t x = blockPos.getX(); x < blockPos.getX() + random.nextInt(2) + 3; ++x) {
            for (int32_t y = blockPos.getY(); y < blockPos.getY() + random.nextInt(1) + 2; ++y) {
                for (int32_t z = blockPos.getZ(); z < blockPos.getZ() + random.nextInt(2) + 3; ++z) {
                    core::BlockPos newPosition(x, y, z);
                    if (level->isEmptyBlock(newPosition)) {
                        const int32_t manhattan = std::abs(x - blockPos.getX())
                                                + std::abs(y - blockPos.getY())
                                                + std::abs(z - blockPos.getZ());
                        if (manhattan < 4 + random.nextInt(2)) {
                            setBlock(level, newPosition, blockState);
                        }
                    }
                }
            }
        }
    }
    return true;
}

// ============================================================================
// ShelfFeature.place — world/feature/ShelfFeature.java, with
// BlockPlacementUtil.placeDisk / placeProvidedBlock (world/BlockPlacementUtil.java)
// ============================================================================
namespace {

bool isAirAt(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    return state && state->isAir();
}

// BlockPlacementUtil.placeProvidedBlock: only into air, flags 2.
void placeProvidedBlock(WorldGenLevel* level, BlockState* state, const core::BlockPos& pos) {
    if (isAirAt(level, pos)) {
        level->setBlock(pos, state, 2);
    }
}

// BlockPlacementUtil.placeDisk: the centre plus four rotated quarter-disks.
void placeDisk(WorldGenLevel* level, BlockState* state, const core::BlockPos& center, float radius) {
    const float radiusSq = radius * radius;
    placeProvidedBlock(level, state, center);
    for (int32_t z = 0; static_cast<float>(z) < radius; ++z) {
        for (int32_t x = 0; static_cast<float>(x) < radius; ++x) {
            if (static_cast<float>(x * x + z * z) > radiusSq) continue;
            placeProvidedBlock(level, state, center.offset(x, 0, z));
            placeProvidedBlock(level, state, center.offset(-x, 0, -z));
            placeProvidedBlock(level, state, center.offset(-z, 0, x));
            placeProvidedBlock(level, state, center.offset(z, 0, -x));
        }
    }
}

} // namespace

bool ShelfFeature::place(FeaturePlaceContext<ShelfConfiguration>& context) {
    WorldGenLevel* level = context.level();
    const core::BlockPos& pos = context.origin();
    const ShelfConfiguration& config = context.config();
    if (!config.block) return false;

    auto isValid = [&config](BlockState* state) -> bool {
        if (!state) return false;
        const std::string& name = state->getBlockName();
        for (const std::string& valid : config.validBlocks) {
            if (name == valid) return true;
        }
        return false;
    };

    for (int32_t x = pos.getX(); x < pos.getX() + 16; ++x) {
        for (int32_t z = pos.getZ(); z < pos.getZ() + 16; ++z) {
            // y_range.getMinValue() .. y_range.getMaxValue() (exclusive, as Java)
            for (int32_t y = config.yMinInclusive; y < config.yMaxInclusive; ++y) {
                core::BlockPos placementPos(x, y, z);
                if (isAirAt(level, placementPos)
                    && isValid(level->getBlockState(placementPos.above()))
                    && isAirAt(level, placementPos.above(2))) {
                    // config.radius().sample(random): a constant float, no draw.
                    placeDisk(level, config.block, placementPos, config.radius);
                    break;
                }
            }
        }
    }
    return true;
}

// ============================================================================
// GoldenOakTrunkPlacer — world/trunkplacer/GoldenOakTrunkPlacer.java
// ============================================================================
std::vector<levelgen::feature::foliageplacers::FoliageAttachment> GoldenOakTrunkPlacer::placeTrunk(
    levelgen::feature::trunkplacers::LevelReader& level,
    levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<BlockStateProvider> trunkProvider,
    std::shared_ptr<BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);
    for (int i = 0; i < treeHeight; ++i) {
        // Java: i > 4 && random.nextInt(3) > 0 && i < 9 — short-circuit, so
        // the draw happens only for i > 4.
        if (i > 4 && random.nextInt(3) > 0 && i < 9) {
            branch(level, random, trunkSetter, origin.getX(), origin.getY() + i, origin.getZ(),
                   i / 4 - 1, trunkProvider);
        }
        placeLog(level, trunkSetter, random, origin.above(i), trunkProvider);
    }
    std::vector<levelgen::feature::foliageplacers::FoliageAttachment> attachments;
    attachments.emplace_back(origin.above(treeHeight), 0, false);
    return attachments;
}

void GoldenOakTrunkPlacer::branch(
    levelgen::feature::trunkplacers::LevelReader& level,
    WorldgenRandom& random,
    levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
    int x, int y, int z, int slant,
    std::shared_ptr<BlockStateProvider> trunkProvider
) {
    const int directionX = random.nextInt(3) - 1;
    const int directionZ = random.nextInt(3) - 1;
    // The loop bound re-draws every check, as in Java.
    for (int n = 0; n < random.nextInt(2) + 1; ++n) {
        x += directionX;
        y += slant;
        z += directionZ;
        placeLog(level, trunkSetter, random, core::BlockPos(x, y, z), trunkProvider);
    }
}

// ============================================================================
// GoldenOakFoliagePlacer — world/foliageplacer/GoldenOakFoliagePlacer.java
// ============================================================================
void GoldenOakFoliagePlacer::createFoliageImpl(
    levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
    WorldgenRandom& random,
    std::shared_ptr<BlockStateProvider> foliageProvider,
    int treeHeight,
    const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    (void)treeHeight;
    (void)leafRadius;   // Java ignores foliageRadius here: every row has range 4
    for (int i = offset; i >= offset - foliageHeight; --i) {
        placeLeavesRow(foliageSetter, random, foliageProvider, attachment.pos(), 4, i,
                       attachment.doubleTrunk());
    }
}

// ============================================================================
// CrystalTreeTrunkPlacer — world/trunkplacer/CrystalTreeTrunkPlacer.java
// ============================================================================
namespace {

// 1.21.1 Mth.SIN: SIN[i] = (float)Math.sin(i * 2π / 65536). Only the four
// quarter-turn lookups the crystal trunk makes are ever read.
float javaSinTable(int32_t index) {
    return static_cast<float>(std::sin(static_cast<double>(index) * 3.141592653589793 * 2.0 / 65536.0));
}

// 1.21.1 Mth.sin(float) / Mth.cos(float): float multiply, int cast, & 65535.
float javaMthSin(float value) {
    return javaSinTable(static_cast<int32_t>(value * 10430.378f) & 65535);
}

float javaMthCos(float value) {
    return javaSinTable(static_cast<int32_t>(value * 10430.378f + 16384.0f) & 65535);
}

} // namespace

std::vector<levelgen::feature::foliageplacers::FoliageAttachment> CrystalTreeTrunkPlacer::placeTrunk(
    levelgen::feature::trunkplacers::LevelReader& level,
    levelgen::feature::trunkplacers::TrunkSetter trunkSetter,
    WorldgenRandom& random,
    int treeHeight,
    const core::BlockPos& origin,
    std::shared_ptr<BlockStateProvider> trunkProvider,
    std::shared_ptr<BlockStateProvider> dirtProvider,
    bool forceDirt
) {
    setDirtAt(level, trunkSetter, random, origin.below(), dirtProvider, forceDirt);
    StraightTrunkPlacer::placeTrunk(level, trunkSetter, random, treeHeight, origin,
                                    trunkProvider, dirtProvider, forceDirt);
    // Mth.TWO_PI = 6.2831855F; the angle accumulates in float across both rings.
    const float quarterTurn = 0.25f * 6.2831855f;
    float f = 0.0f;
    for (int i = 2; i < 7; i += 3) {
        for (int l = 0; l < 4; ++l) {
            const int j = static_cast<int>(javaMthCos(f));
            const int k = static_cast<int>(javaMthSin(f));
            placeLog(level, trunkSetter, random, origin.offset(j, i, k), trunkProvider);
            f += quarterTurn;
        }
    }
    std::vector<levelgen::feature::foliageplacers::FoliageAttachment> attachments;
    attachments.emplace_back(origin.above(treeHeight), 0, false);
    return attachments;
}

// ============================================================================
// CrystalFoliagePlacer — world/foliageplacer/CrystalFoliagePlacer.java
// ============================================================================
void CrystalFoliagePlacer::createFoliageImpl(
    levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
    WorldgenRandom& random,
    std::shared_ptr<BlockStateProvider> foliageProvider,
    int treeHeight,
    const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    (void)treeHeight; (void)foliageHeight; (void)leafRadius;
    const core::BlockPos& blockPos = attachment.pos();
    const bool doubleTrunk = attachment.doubleTrunk();
    int i = 0;
    for (int l = offset; l >= offset - 5; --l) {
        int j;
        switch (i) {
            case 1:
            case 3:
                j = 1;
                break;
            case 2:
            case 4:
                // placeLeavesDiamond(radius 1): north, south, west, east.
                placeLeavesRow(foliageSetter, random, foliageProvider, blockPos.north(), 1, l, doubleTrunk);
                placeLeavesRow(foliageSetter, random, foliageProvider, blockPos.south(), 1, l, doubleTrunk);
                placeLeavesRow(foliageSetter, random, foliageProvider, blockPos.west(), 1, l, doubleTrunk);
                placeLeavesRow(foliageSetter, random, foliageProvider, blockPos.east(), 1, l, doubleTrunk);
                j = 1;
                break;
            case 5:
                j = 2;
                break;
            default:
                j = 0;
                break;
        }
        placeLeavesRow(foliageSetter, random, foliageProvider, blockPos, j, l, doubleTrunk);
        ++i;
    }
}

// ============================================================================
// HolidayFoliagePlacer — world/foliageplacer/HolidayFoliagePlacer.java
// ============================================================================
void HolidayFoliagePlacer::disk360(
    levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
    WorldgenRandom& random,
    const std::shared_ptr<BlockStateProvider>& provider,
    bool doubleTrunk, const core::BlockPos& pos, int height, int distance, int range
) {
    placeLeavesRow(foliageSetter, random, provider, pos.east(distance), range, height, doubleTrunk);
    placeLeavesRow(foliageSetter, random, provider, pos.south(distance), range, height, doubleTrunk);
    placeLeavesRow(foliageSetter, random, provider, pos.west(distance), range, height, doubleTrunk);
    placeLeavesRow(foliageSetter, random, provider, pos.north(distance), range, height, doubleTrunk);
}

void HolidayFoliagePlacer::createFoliageImpl(
    levelgen::feature::foliageplacers::FoliageSetter& foliageSetter,
    WorldgenRandom& random,
    std::shared_ptr<BlockStateProvider> foliageProvider,
    int treeHeight,
    const levelgen::feature::foliageplacers::FoliageAttachment& attachment,
    int foliageHeight,
    int leafRadius,
    int offset
) {
    (void)treeHeight; (void)foliageHeight; (void)leafRadius;
    const core::BlockPos& p = attachment.pos();
    const bool dt = attachment.doubleTrunk();
    int i = 0;
    for (int l = offset; l >= offset - 7; --l) {
        switch (i) {
            case 1:
                placeLeavesRow(foliageSetter, random, foliageProvider, p, 1, l, dt);
                break;
            case 2:
                placeLeavesRow(foliageSetter, random, foliageProvider, p, 1, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.east().north(), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.west().north(), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.east().south(), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.west().south(), 0, l, dt);
                break;
            case 3:
                disk360(foliageSetter, random, foliageProvider, dt, p, l, 1, 1);
                break;
            case 4:
                placeLeavesRow(foliageSetter, random, foliageProvider, p, 2, l, dt);
                disk360(foliageSetter, random, foliageProvider, dt, p, l, 3, 0);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.east(2).north(2), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.west(2).north(2), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.east(2).south(2), 0, l, dt);
                placeLeavesRow(foliageSetter, random, foliageProvider, p.west(2).south(2), 0, l, dt);
                break;
            case 5:
                disk360(foliageSetter, random, foliageProvider, dt, p, l, 1, 2);
                break;
            case 6:
                placeLeavesRow(foliageSetter, random, foliageProvider, p, 3, l, dt);
                disk360(foliageSetter, random, foliageProvider, dt, p, l, 4, 0);
                break;
            case 7:
                disk360(foliageSetter, random, foliageProvider, dt, p, l, 2, 2);
                break;
            default:   // i == 0: a row of range i
                placeLeavesRow(foliageSetter, random, foliageProvider, p, i, l, dt);
                break;
        }
        ++i;
    }
}

// ============================================================================
// HolidayTreeDecorator — world/treedecorator/HolidayTreeDecorator.java
// ============================================================================
namespace {

bool isAetherDirtTag(BlockState* state) {
    // #aether:aether_dirt = aether_grass_block, enchanted_aether_grass_block,
    // aether_dirt (engine slugs in the minecraft namespace).
    if (!state) return false;
    const std::string& id = state->getIdentifier();
    return id == "minecraft:aether_grass_block" || id == "minecraft:enchanted_aether_grass_block"
        || id == "minecraft:aether_dirt";
}

bool isLeavesTag(BlockState* state) {
    return state && blockpredicates::matchesBlockTagName(state, "minecraft:leaves");
}

} // namespace

void HolidayTreeDecorator::place(levelgen::feature::treedecorators::DecoratorContext& context) {
    const std::vector<core::BlockPos>& logs = context.logs();
    if (logs.empty()) return;
    const int y = logs.front().getY();
    for (const core::BlockPos& logPos : logs) {
        if (logPos.getY() == y) placeCircle(context, logPos);
    }
}

void HolidayTreeDecorator::placeCircle(levelgen::feature::treedecorators::DecoratorContext& context,
                                       const core::BlockPos& pos) {
    placeBlockAt(context, pos, 0.0f);
    const int radius = 10;
    for (int z = 1; z < radius; ++z) {
        for (int x = 0; x < radius; ++x) {
            if (x * x + z * z > radius * radius) continue;
            // (float) Math.sqrt(x² + z²) / Mth.square(radius) — the int square.
            const float distance = static_cast<float>(std::sqrt(static_cast<double>(x * x + z * z)))
                                 / static_cast<float>(radius * radius);
            placeBlockAt(context, pos.offset(x, 0, z), distance);
            placeBlockAt(context, pos.offset(-x, 0, -z), distance);
            placeBlockAt(context, pos.offset(-z, 0, x), distance);
            placeBlockAt(context, pos.offset(z, 0, -x), distance);
        }
    }
}

void HolidayTreeDecorator::placeBlockAt(levelgen::feature::treedecorators::DecoratorContext& context,
                                        const core::BlockPos& pos, float distance) {
    WorldgenRandom& random = context.random();
    for (int i = 9; i >= -4; --i) {
        const core::BlockPos blockPos = pos.above(i);
        if (!context.isAir(blockPos.above())) continue;
        BlockState* state = context.getBlockState(blockPos);
        const bool leaves = isLeavesTag(state);
        const bool ground = isAetherDirtTag(state) || leaves
                         || (state && levelgen::FeatureHelpers::isDirt(state));
        if (ground && context.isAir(blockPos.above(4))) {
            if (distance <= random.nextFloat() / 2.0f * (1.0f - distance)) {
                if (leaves) {
                    if (m_snow) context.setBlock(blockPos.above(), m_snow);
                } else if (BlockState* placed = m_provider->getState(random, blockPos)) {
                    context.setBlock(blockPos.above(), placed);
                }
            }
        }
    }
}

// ============================================================================
// CrystalIslandFeature — world/feature/CrystalIslandFeature.java
// ============================================================================
bool CrystalIslandFeature::place(FeaturePlaceContext<NoneFeatureConfiguration>& context) {
    WorldGenLevel* level = context.level();
    const core::BlockPos pos = context.origin();
    if (!crystalTree || !grass || !holystone) return false;
    if (!crystalTree->place(level, context.chunkGenerator(), context.random(), pos.above())) {
        return false;
    }
    // Direction.values().subList(2, 6) = NORTH, SOUTH, WEST, EAST;
    // getClockWise: N->E, S->W, W->N, E->S.
    static const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
    static const int kClockwise[4][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 3; ++i) {
        BlockState* state = (i == 0) ? grass : holystone;
        const int offset = i;
        setIslandBlock(level, pos.below(offset), state);
        for (int d = 0; d < 4; ++d) {
            const int dx = kDirs[d][0];
            const int dz = kDirs[d][1];
            setIslandBlock(level, pos.offset(dx, -offset, dz), state);
            if (offset != 2) {
                setIslandBlock(level, pos.offset(2 * dx, -offset, 2 * dz), state);
                setIslandBlock(level, pos.offset(dx + kClockwise[d][0], -offset, dz + kClockwise[d][1]), state);
            }
        }
    }
    return true;
}

void CrystalIslandFeature::setIslandBlock(WorldGenLevel* level, const core::BlockPos& pos,
                                          BlockState* state) {
    // BlockLogicUtil.isOutOfBounds: more than one chunk from the region
    // centre — the feature write window, which ensureCanWrite reports.
    if (!level->ensureCanWrite(pos)) return;
    // Java swaps in the surface rule's top material when the test state is in
    // #aether:aether_dirt but is not aether_dirt itself (i.e. for the grass
    // layer) and that material is also aether dirt. The Aether surface rule's
    // top material is aether_grass_block, the state already placed, so the
    // swap is an identity here.
    setBlock(level, pos, state);
}

// ============================================================================
// AetherLakeFeature — world/feature/AetherLakeFeature.java
// ============================================================================
bool AetherLakeFeature::place(FeaturePlaceContext<AetherLakeConfiguration>& context) {
    core::BlockPos blockPos = context.origin();
    WorldGenLevel* level = context.level();
    WorldgenRandom& random = context.random();
    const AetherLakeConfiguration& config = context.config();
    if (blockPos.getY() <= level->getMinY() + 4) return false;
    if (!config.fluid) return false;

    blockPos = blockPos.below(4);
    std::vector<bool> booleans(2048, false);
    const int count = random.nextInt(4) + 4;
    for (int j = 0; j < count; ++j) {
        const double d0 = random.nextDouble() * 6.0 + 3.0;
        const double d1 = random.nextDouble() * 4.0 + 2.0;
        const double d2 = random.nextDouble() * 6.0 + 3.0;
        const double d3 = random.nextDouble() * (16.0 - d0 - 2.0) + 1.0 + d0 / 2.0;
        const double d4 = random.nextDouble() * (8.0 - d1 - 4.0) + 2.0 + d1 / 2.0;
        const double d5 = random.nextDouble() * (16.0 - d2 - 2.0) + 1.0 + d2 / 2.0;
        for (int l = 1; l < 15; ++l) {
            for (int i1 = 1; i1 < 15; ++i1) {
                for (int j1 = 1; j1 < 7; ++j1) {
                    const double d6 = (static_cast<double>(l) - d3) / (d0 / 2.0);
                    const double d7 = (static_cast<double>(j1) - d4) / (d1 / 2.0);
                    const double d8 = (static_cast<double>(i1) - d5) / (d2 / 2.0);
                    if (d6 * d6 + d7 * d7 + d8 * d8 < 1.0) {
                        booleans[(l * 16 + i1) * 8 + j1] = true;
                    }
                }
            }
        }
    }

    // fluid().getState(random, pos) — a simple provider: no draw.
    BlockState* fluidState = config.fluid;
    auto at = [&](int x, int z, int y) { return static_cast<bool>(booleans[(x * 16 + z) * 8 + y]); };

    for (int k1 = 0; k1 < 16; ++k1) {
        for (int k = 0; k < 16; ++k) {
            for (int l2 = 0; l2 < 8; ++l2) {
                const bool flag = !at(k1, k, l2)
                    && ((k1 < 15 && at(k1 + 1, k, l2)) || (k1 > 0 && at(k1 - 1, k, l2))
                        || (k < 15 && at(k1, k + 1, l2)) || (k > 0 && at(k1, k - 1, l2))
                        || (l2 < 7 && at(k1, k, l2 + 1)) || (l2 > 0 && at(k1, k, l2 - 1)));
                if (flag) {
                    BlockState* offsetState = level->getBlockState(blockPos.offset(k1, l2, k));
                    if (!offsetState) continue;
                    if (l2 >= 4 && offsetState->isFluid()) return false;
                    if (l2 < 4 && !offsetState->isSolid() && offsetState != fluidState) return false;
                }
            }
        }
    }

    BlockState* caveAir = world::level::block::Blocks::CAVE_AIR
        ? world::level::block::Blocks::CAVE_AIR->defaultBlockState()
        : world::level::block::Blocks::AIR->defaultBlockState();
    auto canReplaceBlock = [](BlockState* state) {
        return state && !blockpredicates::matchesBlockTagName(state, "minecraft:features_cannot_replace");
    };
    for (int l1 = 0; l1 < 16; ++l1) {
        for (int i2 = 0; i2 < 16; ++i2) {
            for (int i3 = 0; i3 < 8; ++i3) {
                if (!at(l1, i2, i3)) continue;
                const core::BlockPos offsetPos = blockPos.offset(l1, i3, i2);
                if (canReplaceBlock(level->getBlockState(offsetPos))) {
                    const bool flag1 = i3 >= 4;
                    level->setBlock(offsetPos, flag1 ? caveAir : fluidState, 2);
                    if (flag1) {
                        // level.scheduleTick(offsetPos, AIR.getBlock(), 0): no
                        // worldgen output.
                        markAboveForPostProcessing(level, offsetPos);
                    }
                }
            }
        }
    }

    // top().getState(random, pos) — simple provider: no draw.
    BlockState* topState = config.top;
    if (topState && !topState->isAir()) {
        for (int i2 = 0; i2 < 16; ++i2) {
            for (int j3 = 0; j3 < 16; ++j3) {
                for (int j4 = 4; j4 < 8; ++j4) {
                    if (!at(i2, j3, j4)) continue;
                    const core::BlockPos offsetPos = blockPos.offset(i2, j4 - 1, j3);
                    BlockState* state = level->getBlockState(offsetPos);
                    if (state && levelgen::FeatureHelpers::isDirt(state)) {
                        level->setBlock(offsetPos, topState, 2);
                    }
                }
            }
        }
    }

    if (fluidState->getIdentifier() == "minecraft:water") {
        for (int k2 = 0; k2 < 16; ++k2) {
            for (int k3 = 0; k3 < 16; ++k3) {
                const core::BlockPos offsetPos = blockPos.offset(k2, 4, k3);
                const world::biome::Biome* biome = level->getBiome(offsetPos);
                BlockState* state = level->getBlockState(offsetPos);
                const bool waterSource = state && state->getIdentifier() == "minecraft:water";
                if (biome && biome->coldEnoughToSnow(offsetPos) && waterSource && canReplaceBlock(state)) {
                    level->setBlock(offsetPos, world::level::block::Blocks::ICE->defaultBlockState(), 2);
                }
            }
        }
    }
    return true;
}

// ============================================================================
// Registry
// ============================================================================

OreFeature AetherFeatures::s_oreFeature;
std::shared_ptr<TreeFeature> AetherFeatures::s_treeFeature = nullptr;
RandomPatchFeature AetherFeatures::s_randomPatchFeature;
SimpleBlockFeature AetherFeatures::s_simpleBlockFeature;
RandomSelectorFeature AetherFeatures::s_randomSelectorFeature;
AercloudFeature AetherFeatures::s_aercloudFeature;
ShelfFeature AetherFeatures::s_shelfFeature;
CrystalIslandFeature AetherFeatures::s_crystalIslandFeature;
AetherLakeFeature AetherFeatures::s_lakeFeature;
SpringFeature AetherFeatures::s_springFeature;
bool AetherFeatures::s_initialized = false;

ConfiguredFeature* AetherFeatures::SKYROOT_TREE = nullptr;
ConfiguredFeature* AetherFeatures::GOLDEN_OAK_TREE = nullptr;
ConfiguredFeature* AetherFeatures::TREES_SKYROOT_AND_GOLDEN_OAK = nullptr;
ConfiguredFeature* AetherFeatures::COLD_AERCLOUD = nullptr;
ConfiguredFeature* AetherFeatures::BLUE_AERCLOUD = nullptr;
ConfiguredFeature* AetherFeatures::GOLDEN_AERCLOUD = nullptr;
ConfiguredFeature* AetherFeatures::QUICKSOIL_SHELF = nullptr;
ConfiguredFeature* AetherFeatures::AETHER_DIRT_ORE = nullptr;
ConfiguredFeature* AetherFeatures::ICESTONE_ORE = nullptr;
ConfiguredFeature* AetherFeatures::AMBROSIUM_ORE = nullptr;
ConfiguredFeature* AetherFeatures::ZANITE_ORE = nullptr;
ConfiguredFeature* AetherFeatures::GRAVITITE_ORE_BURIED = nullptr;
ConfiguredFeature* AetherFeatures::GRAVITITE_ORE = nullptr;
ConfiguredFeature* AetherFeatures::GRASS_PATCH = nullptr;
ConfiguredFeature* AetherFeatures::TALL_GRASS_PATCH = nullptr;
ConfiguredFeature* AetherFeatures::WHITE_FLOWER_PATCH = nullptr;
ConfiguredFeature* AetherFeatures::PURPLE_FLOWER_PATCH = nullptr;
ConfiguredFeature* AetherFeatures::BERRY_BUSH_PATCH = nullptr;
ConfiguredFeature* AetherFeatures::CRYSTAL_TREE = nullptr;
ConfiguredFeature* AetherFeatures::CRYSTAL_ISLAND = nullptr;
ConfiguredFeature* AetherFeatures::HOLIDAY_TREE = nullptr;
ConfiguredFeature* AetherFeatures::WATER_LAKE = nullptr;
ConfiguredFeature* AetherFeatures::WATER_SPRING = nullptr;

// Owned storage (unique_ptr / shared_ptr: raw pointers handed out stay valid
// when the vectors grow)
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::vector<std::unique_ptr<PlacementModifier>> s_placementModifiers;
static std::vector<std::shared_ptr<BlockStateProvider>> s_stateProviders;
static std::vector<std::shared_ptr<blockpredicates::BlockPredicate>> s_blockPredicates;
static std::vector<std::unique_ptr<RandomPatchConfiguration>> s_patchConfigs;
static std::vector<std::unique_ptr<TreeConfiguration>> s_treeConfigs;
static std::vector<std::shared_ptr<carver::IntProvider>> s_carverIntProviders;
static std::vector<std::unique_ptr<RandomFeatureConfiguration>> s_randomConfigs;

void AetherFeatures::bootstrap() {
    if (s_initialized) return;

    // Runs inside the shared BiomeFeatureRegistry::bootstrap() (every
    // dimension), so a missing Aether block never throws: that feature stays
    // null, AetherPlacements skips it and addFeature warns. The hard failure
    // for an unregistered Aether is the holystone default-block check in
    // MyTerrainGenerator, which only the Aether hits.
    auto block = [](const char* name, const char* feature) -> BlockState* {
        BlockState* state = minecraft::world::level::block::Blocks::getDefaultState(name);
        if (!state) {
            fprintf(stderr, "[AetherFeatures] %s missing from the block registry - %s skipped\n",
                    name, feature);
        }
        return state;
    };

    auto storeModifier = [](std::unique_ptr<PlacementModifier> mod) -> PlacementModifier* {
        PlacementModifier* raw = mod.get();
        s_placementModifiers.push_back(std::move(mod));
        return raw;
    };

    auto createPlacedFeature = [](ConfiguredFeature* feature,
                                  const std::vector<PlacementModifier*>& modifiers,
                                  const std::string& name) -> PlacedFeature* {
        auto placed = std::make_unique<PlacedFeature>(feature, modifiers, name);
        PlacedFeature* raw = placed.get();
        s_placedFeatures.push_back(std::move(placed));
        return raw;
    };

    auto simpleProvider = [](BlockState* state) -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<SimpleStateProvider>(state);
        s_stateProviders.push_back(provider);
        return provider;
    };

    auto predicateFilter = [&](std::shared_ptr<blockpredicates::BlockPredicate> predicate) -> PlacementModifier* {
        s_blockPredicates.push_back(predicate);
        return storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(std::move(predicate))));
    };

    auto carverConstantInt = [](int32_t value) -> std::shared_ptr<carver::IntProvider> {
        auto ptr = std::make_shared<carver::ConstantInt>(value);
        s_carverIntProviders.push_back(ptr);
        return ptr;
    };

    // ---------------------------------------------------------------- ores
    // minecraft:ore with one target, tag_match #aether:holystone_ore_replaceables.
    // That tag is exactly [aether:holystone] (data/aether/tags/block/
    // holystone_ore_replaceables.json) and the engine ships no aether tag
    // namespace, so the target is a BlockMatchTest on holystone.
    auto ore = [&](const char* oreBlock, int32_t size, float discardChance) -> ConfiguredFeature* {
        BlockState* state = block(oreBlock, oreBlock);
        if (!state) return nullptr;
        std::shared_ptr<RuleTest> target = std::make_shared<BlockMatchTest>("minecraft:holystone");
        std::vector<OreConfiguration::TargetBlockState> targets = {
            OreConfiguration::target(target, state)
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<OreConfiguration, OreFeature>>(
            &s_oreFeature, OreConfiguration(targets, size, discardChance));
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };
    AETHER_DIRT_ORE      = ore("minecraft:aether_dirt",   33, 0.0f);   // aether_dirt_ore.json
    ICESTONE_ORE         = ore("minecraft:icestone",      32, 0.0f);   // icestone_ore.json
    AMBROSIUM_ORE        = ore("minecraft:ambrosium_ore", 16, 0.0f);   // ambrosium_ore.json
    ZANITE_ORE           = ore("minecraft:zanite_ore",     5, 0.5f);   // zanite_ore.json
    GRAVITITE_ORE_BURIED = ore("minecraft:gravitite_ore",  3, 0.5f);   // gravitite_ore_buried.json
    GRAVITITE_ORE        = ore("minecraft:gravitite_ore",  4, 0.0f);   // gravitite_ore.json

    // --------------------------------------------------------------- trees
    s_treeFeature = std::make_shared<TreeFeature>();
    auto tree = [&](const char* logName, const char* leavesName,
                    std::shared_ptr<levelgen::feature::trunkplacers::TrunkPlacer> trunkPlacer,
                    std::shared_ptr<levelgen::feature::foliageplacers::FoliagePlacer> foliagePlacer,
                    std::shared_ptr<levelgen::feature::featuresize::FeatureSize> featureSize,
                    const char* featureName) -> ConfiguredFeature* {
        BlockState* log = block(logName, featureName);
        BlockState* leaves = block(leavesName, featureName);
        if (!log || !leaves) return nullptr;
        // Default states: log axis=y, leaves distance=7 persistent=false
        // waterlogged=false — the JSON's states minus the mod-only double_drops.
        TreeConfigurationBuilder builder(
            simpleProvider(log), std::move(trunkPlacer),
            simpleProvider(leaves), std::move(foliagePlacer), std::move(featureSize));
        builder.ignoreVines();   // dirt_provider minecraft:dirt, force_dirt false: the defaults
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        ConfiguredFeature* raw = feature.get();
        s_treeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
        return raw;
    };

    // skyroot_tree.json: straight_trunk_placer(4, 2, 0), blob_foliage_placer
    // (radius 2, offset 0, height 3), two_layers_feature_size(1, 0, 1).
    SKYROOT_TREE = tree("minecraft:skyroot_log", "minecraft:skyroot_leaves",
        std::make_shared<StraightTrunkPlacer>(4, 2, 0),
        std::make_shared<BlobFoliagePlacer>(carverConstantInt(2), carverConstantInt(0), 3),
        std::make_shared<TwoLayersFeatureSize>(1, 0, 1),
        "SKYROOT_TREE");

    // golden_oak_tree.json: aether:golden_oak_trunk_placer (base 10, rand 0, 0),
    // aether:golden_oak_foliage_placer (radius 3, offset 1, trunk_height 7),
    // two_layers_feature_size(0, 0, 0, min_clipped_height 10).
    GOLDEN_OAK_TREE = tree("minecraft:golden_oak_log", "minecraft:golden_oak_leaves",
        std::make_shared<GoldenOakTrunkPlacer>(10, 0, 0),
        std::make_shared<GoldenOakFoliagePlacer>(
            carverConstantInt(3), carverConstantInt(1), carverConstantInt(7)),
        std::make_shared<TwoLayersFeatureSize>(0, 0, 0, std::optional<int>(10)),
        "GOLDEN_OAK_TREE");

    // trees_skyroot_and_golden_oak.json — random_selector: golden oak at
    // chance 0.01, default skyroot; each inline placement carries
    // PlacementUtils.filteredByBlockSurvival(<its sapling>).
    auto survivalFilter = [&](const char* saplingName) -> std::vector<PlacementModifier*> {
        BlockState* sapling = minecraft::world::level::block::Blocks::getDefaultState(saplingName);
        if (!sapling) {
            fprintf(stderr, "[AetherFeatures] %s missing from the block registry -"
                            " its tree is placed without the would_survive filter\n", saplingName);
            return {};
        }
        return {predicateFilter(blockpredicates::BlockPredicate::wouldSurvive(sapling, core::Vec3i::ZERO()))};
    };
    if (SKYROOT_TREE) {
        PlacedFeature* skyroot = createPlacedFeature(
            SKYROOT_TREE, survivalFilter("minecraft:skyroot_sapling"), "skyroot_tree_inline");
        std::vector<WeightedPlacedFeature> weighted;
        if (GOLDEN_OAK_TREE) {
            weighted.emplace_back(createPlacedFeature(
                GOLDEN_OAK_TREE, survivalFilter("minecraft:golden_oak_sapling"),
                "golden_oak_tree_inline"), 0.01f);
        }
        auto config = std::make_unique<RandomFeatureConfiguration>(std::move(weighted), skyroot);
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            &s_randomSelectorFeature, *config);
        TREES_SKYROOT_AND_GOLDEN_OAK = feature.get();
        s_randomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // ----------------------------------------------------------- aerclouds
    // AetherConfiguredFeatureBuilders.aercloud(bounds, state).
    auto aercloud = [&](const char* cloudName, int32_t bounds) -> ConfiguredFeature* {
        BlockState* state = block(cloudName, cloudName);
        if (!state) return nullptr;
        auto feature = std::make_unique<ConfiguredFeatureImpl<AercloudConfiguration, AercloudFeature>>(
            &s_aercloudFeature, AercloudConfiguration(bounds, state));
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };
    COLD_AERCLOUD   = aercloud("minecraft:cold_aercloud",   16);   // cold_aercloud.json
    BLUE_AERCLOUD   = aercloud("minecraft:blue_aercloud",    8);   // blue_aercloud.json
    GOLDEN_AERCLOUD = aercloud("minecraft:golden_aercloud",  4);   // golden_aercloud.json

    // ----------------------------------------------------- quicksoil shelf
    // quicksoil_shelf.json: block quicksoil, radius 3.4641016, y_range
    // uniform(0, 48), valid_blocks aether:aether_grass_block.
    if (BlockState* quicksoil = block("minecraft:quicksoil", "QUICKSOIL_SHELF")) {
        auto feature = std::make_unique<ConfiguredFeatureImpl<ShelfConfiguration, ShelfFeature>>(
            &s_shelfFeature,
            ShelfConfiguration(quicksoil, 3.4641016f, 0, 48,
                               {std::string("minecraft:aether_grass_block")}));
        QUICKSOIL_SHELF = feature.get();
        s_features.push_back(std::move(feature));
    }

    // ------------------------------------------------------------- patches
    // random_patch / flower (Feature.FLOWER is a RandomPatchFeature in
    // 1.21.1): an inline simple_block behind
    // block_predicate_filter(matching_blocks minecraft:air) — ONLY_IN_AIR.
    // SimpleBlockFeature runs the placed block's own canSurvive. The flower
    // and berry-bush patches name a one-entry weighted_state_provider, which
    // still draws nextInt(1) per placement, so they get a WeightedStateProvider
    // rather than a simple one.
    auto patch = [&](const char* blockName, int32_t tries, bool weighted,
                     const char* featureName) -> ConfiguredFeature* {
        BlockState* state = block(blockName, featureName);
        if (!state) return nullptr;
        std::shared_ptr<BlockStateProvider> provider;
        if (weighted) {
            provider = std::make_shared<levelgen::feature::stateproviders::WeightedStateProvider>(
                std::vector<levelgen::feature::stateproviders::WeightedStateEntry>{
                    levelgen::feature::stateproviders::WeightedStateEntry(state, 1)});
            s_stateProviders.push_back(provider);
        } else {
            provider = simpleProvider(state);
        }
        auto simple = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature, SimpleBlockConfiguration(provider.get(), false));
        ConfiguredFeature* simpleRaw = simple.get();
        s_features.push_back(std::move(simple));
        PlacedFeature* inner = createPlacedFeature(
            simpleRaw,
            {predicateFilter(blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE)},
            std::string(featureName) + "_inline");
        auto config = std::make_unique<RandomPatchConfiguration>(tries, 7, 3, inner);
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        ConfiguredFeature* raw = feature.get();
        s_patchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
        return raw;
    };
    GRASS_PATCH         = patch("minecraft:short_grass",   32, false, "GRASS_PATCH");          // grass_patch.json
    TALL_GRASS_PATCH    = patch("minecraft:tall_grass",    96, false, "TALL_GRASS_PATCH");     // tall_grass_patch.json (half=lower)
    WHITE_FLOWER_PATCH  = patch("minecraft:white_flower",  64, true,  "WHITE_FLOWER_PATCH");   // white_flower_patch.json
    PURPLE_FLOWER_PATCH = patch("minecraft:purple_flower", 64, true,  "PURPLE_FLOWER_PATCH");  // purple_flower_patch.json
    BERRY_BUSH_PATCH    = patch("minecraft:berry_bush",    32, true,  "BERRY_BUSH_PATCH");     // berry_bush_patch.json

    // ------------------------------------------------- pass-two features
    // These four must never be null: their FeatureSorter slots seed every
    // later feature in their steps. A block the registry lacks falls back to
    // a named stand-in (logged once) — the shape and the RNG stream stay the
    // mod's, only the look differs until the block lands.
    auto blockOr = [](const char* name, const char* standIn, const char* feature) -> BlockState* {
        BlockState* state = minecraft::world::level::block::Blocks::getDefaultState(name);
        if (state) return state;
        state = minecraft::world::level::block::Blocks::getDefaultState(standIn);
        fprintf(stderr, "[AetherFeatures] %s missing from the block registry - %s uses stand-in %s\n",
                name, feature, standIn);
        if (!state) state = minecraft::world::level::block::Blocks::AIR->defaultBlockState();
        return state;
    };
    auto weightedProvider = [&](BlockState* a, int wa, BlockState* b, int wb)
        -> std::shared_ptr<BlockStateProvider> {
        auto provider = std::make_shared<levelgen::feature::stateproviders::WeightedStateProvider>(
            std::vector<levelgen::feature::stateproviders::WeightedStateEntry>{
                levelgen::feature::stateproviders::WeightedStateEntry(a, wa),
                levelgen::feature::stateproviders::WeightedStateEntry(b, wb)});
        s_stateProviders.push_back(provider);
        return provider;
    };

    // crystal_tree.json — skyroot log trunk (axis y), crystal_leaves 4 :
    // crystal_fruit_leaves 1, aether:crystal_tree_trunk_placer (7, 0, 0),
    // aether:crystal_foliage_placer (radius 0, offset 0, trunk_height 6),
    // two_layers_feature_size (1, 0, 1), ignore_vines, dirt minecraft:dirt.
    {
        BlockState* log = blockOr("minecraft:skyroot_log", "minecraft:oak_log", "CRYSTAL_TREE");
        BlockState* leaves = blockOr("minecraft:crystal_leaves", "minecraft:skyroot_leaves", "CRYSTAL_TREE");
        BlockState* fruit = blockOr("minecraft:crystal_fruit_leaves", "minecraft:skyroot_leaves", "CRYSTAL_TREE");
        TreeConfigurationBuilder builder(
            simpleProvider(log), std::make_shared<CrystalTreeTrunkPlacer>(7, 0, 0),
            weightedProvider(leaves, 4, fruit, 1),
            std::make_shared<CrystalFoliagePlacer>(carverConstantInt(0), carverConstantInt(0), carverConstantInt(6)),
            std::make_shared<TwoLayersFeatureSize>(1, 0, 1));
        builder.ignoreVines();
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        CRYSTAL_TREE = feature.get();
        s_treeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // crystal_island.json — aether:crystal_island, no config. The tree is
    // PlacementUtils.inlinePlaced(CRYSTAL_TREE): no modifiers.
    {
        s_crystalIslandFeature.crystalTree = createPlacedFeature(CRYSTAL_TREE, {}, "crystal_tree_inline");
        s_crystalIslandFeature.grass = blockOr("minecraft:aether_grass_block", "minecraft:grass_block", "CRYSTAL_ISLAND");
        s_crystalIslandFeature.holystone = blockOr("minecraft:holystone", "minecraft:stone", "CRYSTAL_ISLAND");
        auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, CrystalIslandFeature>>(
            &s_crystalIslandFeature, NoneFeatureConfiguration::INSTANCE);
        CRYSTAL_ISLAND = feature.get();
        s_features.push_back(std::move(feature));
    }

    // holiday_tree.json — skyroot log trunk, straight_trunk_placer (9, 0, 0),
    // holiday leaves 4 : decorated holiday leaves 1, aether:holiday_foliage_placer
    // (radius 0, offset 0, trunk_height 8), two_layers (1, 0, 1), ignore_vines,
    // decorator aether:holiday_tree_decorator (snow layers=1 10 : present 1).
    {
        BlockState* log = blockOr("minecraft:skyroot_log", "minecraft:oak_log", "HOLIDAY_TREE");
        BlockState* leaves = blockOr("minecraft:holiday_leaves", "minecraft:skyroot_leaves", "HOLIDAY_TREE");
        BlockState* decorated = blockOr("minecraft:decorated_holiday_leaves", "minecraft:skyroot_leaves", "HOLIDAY_TREE");
        BlockState* snow = blockOr("minecraft:snow", "minecraft:air", "HOLIDAY_TREE");
        BlockState* present = blockOr("minecraft:present", "minecraft:snow", "HOLIDAY_TREE");
        auto decorator = std::make_shared<HolidayTreeDecorator>(weightedProvider(snow, 10, present, 1), snow);
        TreeConfigurationBuilder builder(
            simpleProvider(log), std::make_shared<StraightTrunkPlacer>(9, 0, 0),
            weightedProvider(leaves, 4, decorated, 1),
            std::make_shared<HolidayFoliagePlacer>(carverConstantInt(0), carverConstantInt(0), carverConstantInt(8)),
            std::make_shared<TwoLayersFeatureSize>(1, 0, 1));
        builder.ignoreVines();
        builder.decorators({decorator});
        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        HOLIDAY_TREE = feature.get();
        s_treeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // water_lake.json — aether:lake, fluid water (level 0), top aether grass.
    {
        BlockState* water = minecraft::world::level::block::Blocks::WATER->defaultBlockState();
        BlockState* top = blockOr("minecraft:aether_grass_block", "minecraft:grass_block", "WATER_LAKE");
        auto feature = std::make_unique<ConfiguredFeatureImpl<AetherLakeConfiguration, AetherLakeFeature>>(
            &s_lakeFeature, AetherLakeConfiguration(water, top));
        WATER_LAKE = feature.get();
        s_features.push_back(std::move(feature));
    }

    // water_spring.json — spring_feature: falling water, requires_block_below,
    // rock_count 4, hole_count 1, valid_blocks holystone + aether_dirt.
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<SpringConfiguration, SpringFeature>>(
            &s_springFeature,
            SpringConfiguration("minecraft:water", true, 4, 1,
                                {"minecraft:holystone", "minecraft:aether_dirt"}));
        WATER_SPRING = feature.get();
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
