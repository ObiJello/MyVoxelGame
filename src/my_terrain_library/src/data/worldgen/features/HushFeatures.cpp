#include "data/worldgen/features/HushFeatures.h"
#include "data/worldgen/features/AquaticFeatures.h"
#include "data/worldgen/features/ResonantCrystalFormation.h"
#include "levelgen/feature/stateproviders/BlockStateProvider.h"
#include "levelgen/feature/configurations/TreeConfiguration.h"
#include "levelgen/feature/trunkplacers/TrunkPlacer.h"
#include "levelgen/feature/foliageplacers/FoliagePlacer.h"
#include "levelgen/feature/featuresize/FeatureSize.h"
#include "levelgen/feature/treedecorators/TreeDecorator.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/structure/templatesystem/RuleTest.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/VerticalAnchor.h"
#include "levelgen/Heightmap.h"
#include "levelgen/placement/PlacementModifiers.h"
#include "world/level/block/state/properties/BlockStateProperties.h"
#include "core/Direction.h"
#include "util/IntProvider.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The Hush — engine-only dimension. No Java reference; each feature names the
// vanilla feature it is modelled on.

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

namespace {

// ---------------------------------------------------------------------------
// Hush-only feature classes. No Java counterpart; each names the vanilla
// feature whose shape it borrows. The ruin writes only into water or air,
// the bridge only into air or hush grass (plus its footings, set into rim
// ground); neither cuts into terrain, and both check ensureCanWrite first.
// The crystal formations live in ResonantCrystalFormation.cpp.
// ---------------------------------------------------------------------------

bool isAirOrWater(BlockState* state) {
    return state != nullptr
        && (state->isAir() || state->getIdentifier() == "minecraft:water");
}

bool isSolidAt(WorldGenLevel* level, const core::BlockPos& pos) {
    BlockState* state = level->getBlockState(pos);
    return state != nullptr && state->blocksMotion();
}

/**
 * SunkenRuinFeature — a broken fragment of hushstone-brick masonry on a
 * flooded floor (the Sunken Choir). The origin comes from the OCEAN_FLOOR
 * heightmap; it must be water, so dry shores stay bare. One of three
 * shapes, each column grounded on its own floor so nothing floats:
 *   0  pillar   2-5 tall, a chiseled capital when 4+ tall survives
 *   1  wall     3-5 long along X or Z, 1-3 tall per column, 15% gaps
 *   2  arch     two 3-tall piers 4 apart and a lintel whose keystone is
 *               gone half the time
 * then 2-4 fallen blocks scattered within 3 blocks on the floor. The
 * ForestRock / BlockBlob idea (a few set-piece blocks, random walk) at the
 * scale of an ocean ruin fragment.
 */
class SunkenRuinFeature : public Feature<NoneFeatureConfiguration> {
public:
    BlockState* bricks = nullptr;
    BlockState* cracked = nullptr;
    BlockState* chiseled = nullptr;
    BlockState* polished = nullptr;

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();
        const core::BlockPos origin = context.origin();
        if (!bricks || !cracked || !chiseled || !polished) return false;
        if (!level->isWaterAt(origin)) return false;
        if (!isSolidAt(level, origin.below())) return false;

        auto masonry = [&]() -> BlockState* {
            return random.nextFloat() < 0.35f ? cracked : bricks;
        };
        auto put = [&](const core::BlockPos& pos, BlockState* state) {
            if (!level->ensureCanWrite(pos)) return;
            if (isAirOrWater(level->getBlockState(pos))) {
                level->setBlock(pos, state, 2);
            }
        };
        // A column from its own floor up to origin.y + top: the part below
        // origin.y fills a dip so the piece stands on the ground.
        auto column = [&](int32_t x, int32_t z, int32_t top, BlockState* cap) {
            const int32_t floorY = level->getHeight(Heightmap::Types::OCEAN_FLOOR, x, z);
            const int32_t baseY = std::min(floorY, origin.getY());
            for (int32_t y = baseY; y <= origin.getY() + top; ++y) {
                put(core::BlockPos(x, y, z), (y == origin.getY() + top && cap) ? cap : masonry());
            }
        };

        const bool alongX = random.nextBoolean();
        const int32_t ax = alongX ? 1 : 0;
        const int32_t az = alongX ? 0 : 1;
        const int32_t ox = origin.getX();
        const int32_t oz = origin.getZ();

        // The floor under the piece's centre is dressed to polished hushstone
        // (the plinth the fragment stood on).
        if (level->ensureCanWrite(origin.below())) {
            level->setBlock(origin.below(), polished, 2);
        }

        switch (random.nextInt(3)) {
            case 0: {
                const int32_t height = 2 + random.nextInt(4);
                column(ox, oz, height - 1, height >= 4 ? chiseled : nullptr);
                break;
            }
            case 1: {
                const int32_t length = 3 + random.nextInt(3);
                for (int32_t i = 0; i < length; ++i) {
                    const int32_t height = 1 + random.nextInt(3);
                    if (random.nextFloat() < 0.15f) continue;
                    const int32_t off = i - length / 2;
                    column(ox + ax * off, oz + az * off, height - 1, nullptr);
                }
                break;
            }
            default: {
                for (int32_t side : {-2, 2}) {
                    column(ox + ax * side, oz + az * side, 2, nullptr);
                }
                const bool keystoneFallen = random.nextBoolean();
                for (int32_t i = -2; i <= 2; ++i) {
                    if (i == 0 && keystoneFallen) continue;
                    put(core::BlockPos(ox + ax * i, origin.getY() + 3, oz + az * i),
                        i == 0 ? chiseled : masonry());
                }
                break;
            }
        }

        const int32_t rubble = 2 + random.nextInt(3);
        for (int32_t n = 0; n < rubble; ++n) {
            const int32_t x = ox + random.nextInt(7) - 3;
            const int32_t z = oz + random.nextInt(7) - 3;
            const core::BlockPos pos(x, level->getHeight(Heightmap::Types::OCEAN_FLOOR, x, z), z);
            if (level->isWaterAt(pos)) {
                put(pos, random.nextBoolean() ? cracked : bricks);
            }
        }
        return true;
    }
};

/**
 * RopeBridgeFeature — a whisperwood suspension bridge across a chasm (the
 * Hollow Deep). The origin comes from the MOTION_BLOCKING heightmap, so it
 * stands on the ground.
 *
 * Finding the gap (per horizontal direction, starting at a random one): walk
 * at most RIM_SEARCH blocks along the ground to a rim, then scan the centre
 * line: the far side must be ground at the same level within MAX_REACH of
 * the origin, the gap MIN_SPAN..MAX_SPAN wide, air (never water) on the deck
 * line with the two cells over it free, and at least MIN_DEPTH blocks of air
 * deep somewhere. Then the whole volume the bridge needs is checked before
 * anything is written — the 3-wide deck and the headroom over it, both
 * hand-rope lines, both towers standing on solid rim ground — so a chasm that
 * is flooded or partly blocked (an overhang, a tree, a jagged wall reaching
 * into the deck line anywhere but at its two ends) is refused and the next
 * direction is tried. MAX_REACH keeps every write inside the one-chunk
 * feature margin: origin local x/z in 0..15, writes from -1 to far + 1 <= 16
 * along the span and +-3 across it.
 *
 * The bridge (j along the span, i across it, deck rows i = -1..1):
 *   deck      a catenary sag, deepest at mid-span (SAG_PER_BLOCK of the span),
 *             in half-blocks so every step is a slab step: a whole level is
 *             a whisperwood-plank centre run between stripped-log edge
 *             boards, a half level is a bottom slab over a top slab (one
 *             block thick) across all three rows. Edge boards are now and
 *             then missing or sunk half a block (never the centre row, never
 *             at a tie).
 *   towers    on both rims: two 4-high whisperwood-log posts (i = +-2) and a
 *             log cross-beam over the deck, an echo lantern hanging under
 *             it; a footing of hushstone bricks set into the rim around the
 *             posts, and a landing of planks between stripped-log boards.
 *   ropes     whisperwood fences at i = +-2 from the post tops down in a
 *             catenary to a rail one block over the deck at mid-span,
 *             stacked where the rope steps down so the line stays unbroken,
 *             with connections set along the span (worldgen runs no shape
 *             updates).
 *   ties      stripped-log cross-ties under the deck (i = -2..2) about every
 *             third column, iron-chain hangers from each tie end up to the
 *             rope, and sometimes a short chain dangling under a tie.
 * Everything is written only into free cells (air or hush grass), except the
 * footing and the landing, which replace rim ground.
 */
class RopeBridgeFeature : public Feature<NoneFeatureConfiguration> {
public:
    BlockState* planks = nullptr;
    BlockState* slabBottom = nullptr;       // whisperwood_slab type=bottom
    BlockState* slabTop = nullptr;          // whisperwood_slab type=top
    BlockState* strippedX = nullptr;        // stripped_whisperwood_log axis=x
    BlockState* strippedZ = nullptr;        // stripped_whisperwood_log axis=z
    BlockState* logX = nullptr;             // whisperwood_log axis=x
    BlockState* logY = nullptr;             // whisperwood_log axis=y
    BlockState* logZ = nullptr;             // whisperwood_log axis=z
    BlockState* chainY = nullptr;           // iron_chain axis=y
    BlockState* lantern = nullptr;          // echo_lantern hanging=true
    BlockState* bricks = nullptr;           // hushstone_bricks
    /** whisperwood_fence by connection mask: bit 0 = the +axis side, bit 1 = the -axis side. */
    std::array<BlockState*, 4> fenceX{};    // east / west
    std::array<BlockState*, 4> fenceZ{};    // south / north
    /** Rim ground the footing and landing may replace (hushstone, polished, sculk loam, hush moss). */
    std::vector<BlockState*> rimGround;

    static constexpr int32_t RIM_SEARCH = 6;
    static constexpr int32_t MIN_SPAN = 3;
    static constexpr int32_t MAX_SPAN = 14;
    static constexpr int32_t MAX_REACH = 15;
    static constexpr int32_t MIN_DEPTH = 5;
    static constexpr int32_t POST_HEIGHT = 4;
    static constexpr double SAG_PER_BLOCK = 0.15;
    static constexpr double CATENARY_K = 1.6;

    bool isComplete() const {
        for (BlockState* s : {planks, slabBottom, slabTop, strippedX, strippedZ, logX, logY, logZ,
                              chainY, lantern, bricks}) {
            if (!s) return false;
        }
        for (size_t m = 0; m < 4; ++m) {
            if (!fenceX[m] || !fenceZ[m]) return false;
        }
        return !rimGround.empty();
    }

    /** The normalised catenary: 0 at mid-span (t = 0), 1 at the rims (t = +-1). */
    static double catenary(double t) {
        return (std::cosh(CATENARY_K * t) - 1.0) / (std::cosh(CATENARY_K) - 1.0);
    }

    bool place(FeaturePlaceContext<NoneFeatureConfiguration>& context) override {
        WorldGenLevel* level = context.level();
        WorldgenRandom& random = context.random();
        const core::BlockPos origin = context.origin();
        if (!isComplete()) return false;
        if (!isSolidAt(level, origin.below())) return false;

        const int32_t groundY = origin.getY() - 1;
        static constexpr std::array<int32_t, 4> kDx = {1, 0, -1, 0};
        static constexpr std::array<int32_t, 4> kDz = {0, 1, 0, -1};
        const int32_t first = random.nextInt(4);

        // Free = air or a replaceable plant, never a fluid.
        auto isFree = [](BlockState* state) {
            return state != nullptr
                && (state->isAir() || (state->canBeReplaced() && !state->hasAnyFluid()));
        };
        auto isRimGround = [&](BlockState* state) {
            if (state == nullptr) return false;
            for (BlockState* ground : rimGround) {
                if (state->is(ground)) return true;
            }
            return false;
        };

        for (int32_t turn = 0; turn < 4; ++turn) {
            const int32_t d = (first + turn) & 3;
            const int32_t dx = kDx[static_cast<size_t>(d)];
            const int32_t dz = kDz[static_cast<size_t>(d)];
            const int32_t px = -dz;
            const int32_t pz = dx;
            auto at = [&](int32_t j, int32_t i, int32_t y) {
                return core::BlockPos(origin.getX() + dx * j + px * i, y, origin.getZ() + dz * j + pz * i);
            };
            auto stateAt = [&](int32_t j, int32_t i, int32_t y) { return level->getBlockState(at(j, i, y)); };

            // 1. Walk along the ground to the rim.
            int32_t edge = -1;
            for (int32_t k = 1; k <= RIM_SEARCH; ++k) {
                if (isSolidAt(level, at(k, 0, groundY + 1))) break;     // ground rises: no rim here
                if (!isSolidAt(level, at(k, 0, groundY))) { edge = k; break; }
            }
            if (edge < 0) continue;

            // 2. Scan the gap along the centre line to the far rim.
            int32_t far = -1;
            int32_t deepest = 0;
            for (int32_t j = edge; j <= MAX_REACH; ++j) {
                BlockState* cell = stateAt(j, 0, groundY);
                if (cell != nullptr && cell->blocksMotion()) {
                    if (!isSolidAt(level, at(j, 0, groundY + 1))) far = j;
                    break;
                }
                if (cell == nullptr || !cell->isAir()
                    || !isFree(stateAt(j, 0, groundY + 1)) || !isFree(stateAt(j, 0, groundY + 2))) {
                    break;
                }
                int32_t depth = 0;
                while (depth < MIN_DEPTH) {
                    BlockState* below = stateAt(j, 0, groundY - depth - 1);
                    if (below == nullptr || !below->isAir()) break;
                    ++depth;
                }
                deepest = std::max(deepest, depth);
            }
            const int32_t span = far - edge;
            if (far < 0 || span < MIN_SPAN || span > MAX_SPAN || deepest < MIN_DEPTH) continue;

            // 3. The shape, per column c = j - (edge - 1) over edge - 1 .. far
            //    (the two landings at the ends).
            const int32_t columns = span + 2;
            auto col = [&](int32_t j) { return static_cast<size_t>(j - (edge - 1)); };
            std::vector<int32_t> halfSteps(static_cast<size_t>(columns), 0);   // deck surface, half-blocks below the rim
            const double sag = span * SAG_PER_BLOCK;
            for (int32_t j = edge; j < far; ++j) {
                const double t = (j - edge + 0.5) / span * 2.0 - 1.0;
                halfSteps[col(j)] = -static_cast<int32_t>(std::lround(2.0 * sag * (1.0 - catenary(t))));
            }
            // Never more than a half-step between neighbours: walkable without jumping.
            for (int32_t j = edge; j < far; ++j) {
                halfSteps[col(j)] = std::max(halfSteps[col(j)], halfSteps[col(j - 1)] - 1);
            }
            for (int32_t j = far - 1; j >= edge; --j) {
                halfSteps[col(j)] = std::max(halfSteps[col(j)], halfSteps[col(j + 1)] - 1);
            }
            auto isHalf = [&](int32_t j) { return (halfSteps[col(j)] % 2) != 0; };
            // The deck's (upper) cell: ceil(halfSteps / 2) below the rim level.
            auto deckY = [&](int32_t j) { return groundY + halfSteps[col(j)] / 2; };
            const int32_t postTop = groundY + POST_HEIGHT;
            std::vector<int32_t> rope(static_cast<size_t>(columns), postTop);
            for (int32_t j = edge; j < far; ++j) {
                const double t = (j - edge + 0.5) / span * 2.0 - 1.0;
                rope[col(j)] = deckY(j) + 1 + static_cast<int32_t>(std::lround(1.0 + 2.0 * catenary(t)));
            }
            // Cross-ties: whole-level columns about every third one.
            std::vector<bool> tie(static_cast<size_t>(columns), false);
            for (int32_t j = edge, last = edge - 2; j < far; ++j) {
                if (isHalf(j)) continue;
                if (j - last >= 3 || (j == far - 1 && j - last >= 2)) {
                    tie[col(j)] = true;
                    last = j;
                }
            }

            // 4. Everything the bridge writes must be inside the region, and
            //    the volume it needs free.
            int32_t lowest = groundY;
            for (int32_t j = edge; j < far; ++j) lowest = std::min(lowest, deckY(j) - 3);
            bool fits = true;
            for (int32_t j : {edge - 2, far + 1}) {
                for (int32_t i : {-3, 3}) {
                    for (int32_t y : {lowest, postTop + 1}) {
                        const core::BlockPos pos = at(j, i, y);
                        fits = fits && !level->isOutsideBuildHeight(pos) && level->ensureCanWrite(pos);
                    }
                }
            }
            for (int32_t j : {edge - 1, far}) {
                for (int32_t i = -2; i <= 2 && fits; ++i) {
                    if ((i == -2 || i == 2) && !isSolidAt(level, at(j, i, groundY))) fits = false;   // tower footing
                    for (int32_t h = 1; h <= POST_HEIGHT && fits; ++h) fits = isFree(stateAt(j, i, groundY + h));
                }
            }
            for (int32_t j = edge; j < far && fits; ++j) {
                const int32_t bottom = deckY(j) - (isHalf(j) ? 1 : 0);
                for (int32_t i = -1; i <= 1 && fits; ++i) {
                    for (int32_t y = bottom; y <= groundY + 3 && fits; ++y) {
                        BlockState* state = stateAt(j, i, y);
                        // A jagged wall may reach into a side row at the ends: it becomes the deck there.
                        if (i != 0 && (j == edge || j == far - 1) && y <= groundY && isRimGround(state)) continue;
                        fits = isFree(state);
                    }
                }
                for (int32_t i : {-2, 2}) {
                    for (int32_t y = deckY(j); y <= rope[col(j)] && fits; ++y) fits = isFree(stateAt(j, i, y));
                }
            }
            if (!fits) continue;

            // 5. Build.
            auto put = [&](int32_t j, int32_t i, int32_t y, BlockState* state) {
                const core::BlockPos pos = at(j, i, y);
                if (isFree(level->getBlockState(pos))) level->setBlock(pos, state, 2);
            };
            BlockState* edgeBoard = dx != 0 ? strippedX : strippedZ;     // along the span
            BlockState* crossLog = dx != 0 ? strippedZ : strippedX;      // across it
            BlockState* beam = dx != 0 ? logZ : logX;

            // Footings (hushstone bricks set into the rim) and landings.
            for (int32_t j : {edge - 2, edge - 1, far, far + 1}) {
                const bool landingColumn = j == edge - 1 || j == far;
                for (int32_t i = -3; i <= 3; ++i) {
                    const core::BlockPos pos = at(j, i, groundY);
                    BlockState* here = level->getBlockState(pos);
                    if (landingColumn && i >= -1 && i <= 1) {
                        if (isRimGround(here) || (here != nullptr && here->isAir())) {
                            level->setBlock(pos, i == 0 ? planks : edgeBoard, 2);
                        }
                    } else if (isRimGround(here)) {
                        level->setBlock(pos, bricks, 2);
                    }
                }
            }

            // Deck.
            for (int32_t j = edge; j < far; ++j) {
                const bool half = isHalf(j);
                const int32_t y = deckY(j);
                for (int32_t i = -1; i <= 1; ++i) {
                    if (i != 0 && !tie[col(j)] && j > edge && j < far - 1) {
                        const float roll = random.nextFloat();
                        if (roll < 0.08f) continue;                            // a missing board
                        if (roll < 0.14f && !half) {                           // a broken one, sunk half a block
                            put(j, i, y, slabBottom);
                            continue;
                        }
                    }
                    if (half) {
                        put(j, i, y, slabBottom);
                        put(j, i, y - 1, slabTop);
                    } else {
                        put(j, i, y, i == 0 ? planks : edgeBoard);
                    }
                }
            }

            // Towers.
            for (int32_t j : {edge - 1, far}) {
                for (int32_t i : {-2, 2}) {
                    for (int32_t h = 1; h <= POST_HEIGHT; ++h) put(j, i, groundY + h, logY);
                }
                for (int32_t i = -1; i <= 1; ++i) put(j, i, postTop, beam);
                put(j, 0, postTop - 1, lantern);
            }

            // Cross-ties, hangers, dangling chains.
            for (int32_t j = edge; j < far; ++j) {
                if (!tie[col(j)]) continue;
                const int32_t tieY = deckY(j) - 1;
                for (int32_t i = -2; i <= 2; ++i) put(j, i, tieY, crossLog);
                for (int32_t i : {-2, 2}) {
                    for (int32_t y = tieY + 1; y < rope[col(j)]; ++y) put(j, i, y, chainY);
                }
                if (random.nextFloat() < 0.4f) {
                    put(j, 0, tieY - 1, chainY);
                    if (random.nextFloat() < 0.5f) put(j, 0, tieY - 2, chainY);
                }
            }

            // Hand-ropes: a fence at each column's rope height, stacked up to
            // the neighbour's height where the rope steps (on the lower
            // column; the post columns are the posts themselves).
            for (int32_t i : {-2, 2}) {
                std::vector<std::pair<int32_t, int32_t>> cells;    // (j, y)
                auto hasRope = [&](int32_t j, int32_t y) {
                    for (const auto& [cj, cy] : cells) {
                        if (cj == j && cy == y) return true;
                    }
                    return false;
                };
                for (int32_t j = edge; j < far; ++j) cells.emplace_back(j, rope[col(j)]);
                for (int32_t j = edge - 1; j < far; ++j) {
                    const int32_t a = rope[col(j)];
                    const int32_t b = rope[col(j + 1)];
                    if (a == b) continue;
                    const int32_t lowJ = a < b ? j : j + 1;
                    if (lowJ == edge - 1 || lowJ == far) continue;
                    for (int32_t y = std::min(a, b) + 1; y <= std::max(a, b); ++y) {
                        if (!hasRope(lowJ, y)) cells.emplace_back(lowJ, y);
                    }
                }
                auto ropeOrPost = [&](int32_t j, int32_t y) {
                    return hasRope(j, y) || ((j == edge - 1 || j == far) && y > groundY && y <= postTop);
                };
                for (const auto& [j, y] : cells) {
                    // Bit 0 = the +x / +z side, bit 1 = the -x / -z side; +j
                    // is +x / +z when the span runs east or south.
                    const bool forward = ropeOrPost(j + 1, y);
                    const bool back = ropeOrPost(j - 1, y);
                    const bool positive = (dx + dz) > 0;
                    const size_t mask = static_cast<size_t>(((positive ? forward : back) ? 1 : 0)
                                                           | ((positive ? back : forward) ? 2 : 0));
                    put(j, i, y, dx != 0 ? fenceX[mask] : fenceZ[mask]);
                }
            }
            return true;
        }
        return false;
    }
};

SunkenRuinFeature s_sunkenRuinFeature;
ResonantCrystalFormationFeature s_crystalFormationFeature;
RopeBridgeFeature s_ropeBridgeFeature;
ForestRockFeature s_forestRockFeature;

} // namespace

// Feature instances
OreFeature HushFeatures::s_oreFeature;
std::shared_ptr<TreeFeature> HushFeatures::s_treeFeature = nullptr;
RandomPatchFeature HushFeatures::s_randomPatchFeature;
SimpleBlockFeature HushFeatures::s_simpleBlockFeature;
BlockColumnFeature HushFeatures::s_blockColumnFeature;
VegetationPatchFeature HushFeatures::s_vegetationPatchFeature;
SimpleRandomSelectorFeature HushFeatures::s_simpleRandomSelectorFeature;
RandomSelectorFeature HushFeatures::s_randomSelectorFeature;
bool HushFeatures::s_initialized = false;

// Configured feature pointers
ConfiguredFeature* HushFeatures::ORE_ECHO = nullptr;
ConfiguredFeature* HushFeatures::WHISPERWOOD = nullptr;
ConfiguredFeature* HushFeatures::PATCH_RESONANCE_BLOOM = nullptr;
ConfiguredFeature* HushFeatures::HUSH_MOSS_VEGETATION = nullptr;
ConfiguredFeature* HushFeatures::HUSH_MOSS_PATCH = nullptr;
ConfiguredFeature* HushFeatures::ORE_RESONITE = nullptr;
ConfiguredFeature* HushFeatures::RESONANT_CLUSTERS = nullptr;
ConfiguredFeature* HushFeatures::RESONANT_CRYSTAL_CLUMP = nullptr;
ConfiguredFeature* HushFeatures::RESONANT_CLUSTER_SURFACE = nullptr;
ConfiguredFeature* HushFeatures::PATCH_HUSH_GRASS = nullptr;
ConfiguredFeature* HushFeatures::WHISPERWOOD_LARGE = nullptr;
ConfiguredFeature* HushFeatures::WHISPERWOOD_FOREST_TREES = nullptr;
ConfiguredFeature* HushFeatures::SUNKEN_RUIN = nullptr;
ConfiguredFeature* HushFeatures::KELP = nullptr;
ConfiguredFeature* HushFeatures::SEA_PICKLE = nullptr;
ConfiguredFeature* HushFeatures::ROPE_BRIDGE = nullptr;
ConfiguredFeature* HushFeatures::RESONANT_STALACTITE = nullptr;
ConfiguredFeature* HushFeatures::HUSHSTONE_BOULDER = nullptr;
ConfiguredFeature* HushFeatures::CRYSTAL_FORMATION_SMALL = nullptr;
ConfiguredFeature* HushFeatures::CRYSTAL_FORMATION_FIELD = nullptr;
ConfiguredFeature* HushFeatures::CRYSTAL_FORMATION_TALL = nullptr;

// Owned storage (unique_ptr / shared_ptr: the raw pointers handed out stay
// valid when the vectors grow)
static std::vector<std::unique_ptr<ConfiguredFeature>> s_features;
static std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
static std::vector<std::unique_ptr<PlacementModifier>> s_placementModifiers;
static std::vector<std::shared_ptr<BlockStateProvider>> s_stateProviders;
static std::vector<std::shared_ptr<blockpredicates::BlockPredicate>> s_blockPredicates;
static std::vector<std::unique_ptr<RandomPatchConfiguration>> s_patchConfigs;
static std::vector<std::unique_ptr<VegetationPatchConfiguration>> s_vegPatchConfigs;
static std::vector<std::unique_ptr<TreeConfiguration>> s_treeConfigs;
static std::vector<std::shared_ptr<carver::IntProvider>> s_carverIntProviders;
static std::vector<std::unique_ptr<SimpleRandomFeatureConfiguration>> s_simpleRandomConfigs;
static std::vector<std::unique_ptr<RandomFeatureConfiguration>> s_randomConfigs;
static std::vector<std::shared_ptr<levelgen::feature::treedecorators::TreeDecorator>> s_treeDecorators;

void HushFeatures::bootstrap() {
    if (s_initialized) return;

    // Every Hush block resolves by identifier. This runs inside the shared
    // BiomeFeatureRegistry::bootstrap() — i.e. for the Overworld too — so a
    // missing block must not throw here: it leaves every Hush feature null
    // (HushPlacements skips them, addFeature warns) and the Hush generates
    // bare terrain. The hard failure for an unregistered Hush block is the
    // default-block check in MyTerrainGenerator, which only the Hush hits.
    const char* const kRequiredBlocks[] = {
        "minecraft:hushstone", "minecraft:polished_hushstone", "minecraft:echo_ore",
        "minecraft:resonant_crystal", "minecraft:resonance_bloom", "minecraft:hush_moss",
        "minecraft:whisperwood_log", "minecraft:lantern_leaves"
    };
    for (const char* name : kRequiredBlocks) {
        if (!minecraft::world::level::block::Blocks::getDefaultState(name)) {
            fprintf(stderr, "[HushFeatures] %s missing from the block registry -"
                            " The Hush will generate without features\n", name);
            s_initialized = true;
            return;
        }
    }
    auto block = [](const char* name) -> BlockState* {
        return minecraft::world::level::block::Blocks::getDefaultState(name);
    };

    // ---- helpers mirroring the NetherFeatures / VegetationFeatures patterns ----
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

    auto createSimpleBlockConfiguredFeature =
        [&](std::shared_ptr<BlockStateProvider> provider) -> ConfiguredFeature* {
        // SimpleBlockConfiguration holds a raw pointer; the shared_ptr is
        // kept here for the process lifetime.
        s_stateProviders.push_back(provider);
        auto feature = std::make_unique<ConfiguredFeatureImpl<SimpleBlockConfiguration, SimpleBlockFeature>>(
            &s_simpleBlockFeature,
            SimpleBlockConfiguration(provider.get(), false)
        );
        ConfiguredFeature* raw = feature.get();
        s_features.push_back(std::move(feature));
        return raw;
    };

    // ONLY_IN_AIR + matchesBlocks(0,-1,0, allowedOn) — FeatureUtils'
    // simplePatchConfiguration predicate (VegetationFeatures.cpp).
    auto onlyInAirOn = [&](const std::vector<std::string>& allowedOnNames)
        -> std::shared_ptr<blockpredicates::BlockPredicate> {
        auto predicate = blockpredicates::BlockPredicate::allOf(
            blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE,
            blockpredicates::BlockPredicate::matchesBlocks(core::Vec3i(0, -1, 0), allowedOnNames)
        );
        s_blockPredicates.push_back(predicate);
        return predicate;
    };

    auto predicateFilter = [&](std::shared_ptr<blockpredicates::BlockPredicate> predicate) -> PlacementModifier* {
        return storeModifier(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(std::move(predicate))));
    };

    // FeatureUtils.simplePatchConfiguration: tries 96, xz 7, y 3.
    auto simplePatchConfiguration =
        [&](std::shared_ptr<BlockStateProvider> provider,
            const std::vector<std::string>& allowedOnNames,
            const std::string& innerName) -> RandomPatchConfiguration* {
        PlacedFeature* inner = createPlacedFeature(
            createSimpleBlockConfiguredFeature(std::move(provider)),
            {predicateFilter(onlyInAirOn(allowedOnNames))},
            innerName
        );
        auto config = std::make_unique<RandomPatchConfiguration>(96, 7, 3, inner);
        RandomPatchConfiguration* raw = config.get();
        s_patchConfigs.push_back(std::move(config));
        return raw;
    };

    auto carverConstantInt = [](int32_t value) -> std::shared_ptr<carver::IntProvider> {
        auto ptr = std::make_shared<carver::ConstantInt>(value);
        s_carverIntProviders.push_back(ptr);
        return ptr;
    };

    // ---- blocks (hushstone, polished hushstone and the tree blocks are
    // referenced by identifier below) ----
    BlockState* echoOre = block("minecraft:echo_ore");
    BlockState* resonantCrystal = block("minecraft:resonant_crystal");
    BlockState* resonanceBloom = block("minecraft:resonance_bloom");
    BlockState* hushMoss = block("minecraft:hush_moss");

    // ORE_ECHO — OreFeatures::createSingleOreConfig(BlockMatchTest(hushstone),
    // echo_ore, 7, 0) reproduced here (that helper is private). A single
    // BlockMatchTest target, NOT createOreConfig: its stone/deepslate tag
    // targets never match hushstone.
    {
        std::shared_ptr<RuleTest> target = std::make_shared<BlockMatchTest>("minecraft:hushstone");
        std::vector<OreConfiguration::TargetBlockState> targets = {
            OreConfiguration::target(target, echoOre)
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<OreConfiguration, OreFeature>>(
            &s_oreFeature,
            OreConfiguration(targets, 7, 0.0f)
        );
        ORE_ECHO = feature.get();
        s_features.push_back(std::move(feature));
    }

    // WHISPERWOOD — TreeFeatures::createStraightBlobTree("whisperwood_log",
    // "lantern_leaves", 5, 2, 1, 2).ignoreVines() reproduced here (private
    // helper): StraightTrunkPlacer(5, 2, 1), BlobFoliagePlacer(2, 0, 3),
    // TwoLayersFeatureSize(1, 0, 1). The default dirt provider stays dirt:
    // sculk loam and hush moss are in #minecraft:dirt, so setDirtAt leaves
    // them alone under the trunk.
    s_treeFeature = std::make_shared<TreeFeature>();
    auto straightBlobWhisperwood = [&](int32_t baseHeight, int32_t heightRandA,
                                       int32_t heightRandB, int32_t foliageRadius,
                                       int32_t foliageHeight) -> ConfiguredFeature* {
        auto trunkProvider = BlockStateProvider::simple("minecraft:whisperwood_log");
        auto foliageProvider = BlockStateProvider::simple("minecraft:lantern_leaves");
        s_stateProviders.push_back(trunkProvider);
        s_stateProviders.push_back(foliageProvider);

        auto trunkPlacer = std::make_shared<StraightTrunkPlacer>(baseHeight, heightRandA, heightRandB);
        auto foliagePlacer = std::make_shared<BlobFoliagePlacer>(
            carverConstantInt(foliageRadius), carverConstantInt(0), foliageHeight);
        auto featureSize = std::make_shared<TwoLayersFeatureSize>(1, 0, 1);

        TreeConfigurationBuilder builder(
            trunkProvider, trunkPlacer, foliageProvider, foliagePlacer, featureSize);
        // Whisperfruit hangs under the lantern leaves (docs/the-hush.md): the
        // mangrove's hanging-propagule decorator shape — AttachedToLeaves-
        // Decorator(p, exclusion xz 1 / y 0, provider, 2 empty below, DOWN) —
        // at a sparser 0.08 and exclusion xz 2, so a tree carries a handful,
        // not a curtain. The age is randomized 0..2 like the propagule's so a
        // fresh forest already has ripe fruit.
        {
            auto fruitProvider = std::make_shared<levelgen::feature::stateproviders::RandomizedIntStateProvider>(
                BlockStateProvider::simple("minecraft:hanging_whisperfruit"), "age", 0, 2);
            s_stateProviders.push_back(fruitProvider);
            auto fruitDecorator = std::make_shared<levelgen::feature::treedecorators::AttachedToLeavesDecorator>(
                0.08f, 2, 0, fruitProvider, 2,
                std::vector<core::Direction>{core::Direction::DOWN});
            s_treeDecorators.push_back(fruitDecorator);
            builder.decorators({fruitDecorator});
        }
        builder.ignoreVines();

        auto config = std::make_unique<TreeConfiguration>(builder.build());
        auto feature = std::make_unique<ConfiguredFeatureImpl<TreeConfiguration, TreeFeature>>(
            s_treeFeature.get(), *config);
        ConfiguredFeature* raw = feature.get();
        s_treeConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
        return raw;
    };
    WHISPERWOOD = straightBlobWhisperwood(5, 2, 1, 2, 3);

    // WHISPERWOOD_LARGE — the forest's tall variant: StraightTrunkPlacer(8, 3, 1),
    // BlobFoliagePlacer(3, 0, 4) (the FANCY-less analogue of a tall birch).
    WHISPERWOOD_LARGE = straightBlobWhisperwood(8, 3, 1, 3, 4);

    // WHISPERWOOD_FOREST_TREES — the TREES_BIRCH_AND_OAK shape
    // (VegetationFeatures.java line 213: RandomSelectorFeature over inline
    // placed trees, default = the common one). WHISPERWOOD_LARGE at 0.25 is
    // the 1 : 3 weighting; the wouldSurvive / water-depth filters live on the
    // outer placement (HushPlacements::treePlacement), so both inline
    // placements carry no modifiers.
    {
        PlacedFeature* large = createPlacedFeature(WHISPERWOOD_LARGE, {}, "whisperwood_large_inline");
        PlacedFeature* normal = createPlacedFeature(WHISPERWOOD, {}, "whisperwood_inline");
        auto config = std::make_unique<RandomFeatureConfiguration>(
            std::vector<WeightedPlacedFeature>{WeightedPlacedFeature(large, 0.25f)},
            normal);
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomFeatureConfiguration, RandomSelectorFeature>>(
            &s_randomSelectorFeature, *config);
        WHISPERWOOD_FOREST_TREES = feature.get();
        s_randomConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_RESONANCE_BLOOM — FeatureUtils.simplePatchConfiguration pattern
    // (VegetationFeatures.cpp:236-256): tries 96, only in air, on sculk loam /
    // hush moss / grass block / dirt.
    {
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature,
            *simplePatchConfiguration(
                simpleProvider(resonanceBloom),
                {"minecraft:sculk_loam", "minecraft:hush_moss",
                 "minecraft:grass_block", "minecraft:dirt"},
                "resonance_bloom_inline")
        );
        PATCH_RESONANCE_BLOOM = feature.get();
        s_features.push_back(std::move(feature));
    }

    // HUSH_MOSS_VEGETATION — the vegetation placed by the moss patch. The
    // vanilla MOSS_VEGETATION is grass / moss carpet / azalea, none of which
    // belong to the Hush palette, so the only candidate is the resonance
    // bloom (SimpleBlockFeature checks canSurvive itself).
    HUSH_MOSS_VEGETATION = createSimpleBlockConfiguredFeature(simpleProvider(resonanceBloom));

    // HUSH_MOSS_PATCH — copy of CaveFeatures MOSS_PATCH (CaveFeatures.java
    // line 113): FLOOR, depth 1, extra-bottom 0, vertical range 5, xz radius
    // UniformInt(4, 7), extra-edge 0.3; replaceable = #hush_moss_replaceable
    // (sculk loam, hushstone), ground = hush moss. The only number that
    // differs is the vegetation chance: MOSS_PATCH's 0.8 draws mostly grass
    // and carpet; with blooms as the sole candidate 0.8 would carpet every
    // patch in light-10 flowers, so it is scaled to the azalea share of
    // MOSS_VEGETATION (11/96 of 0.8 ~ 0.1).
    {
        PlacedFeature* vegetation = createPlacedFeature(
            HUSH_MOSS_VEGETATION, {}, "hush_moss_vegetation_inline");

        auto config = std::make_unique<VegetationPatchConfiguration>(
            "minecraft:hush_moss_replaceable",
            simpleProvider(hushMoss),
            vegetation,
            levelgen::CaveSurface::FLOOR,
            std::make_shared<util::ConstantInt>(1),
            0.0f,
            5,
            0.1f,
            std::make_shared<util::UniformInt>(4, 7),
            0.3f
        );

        auto feature = std::make_unique<ConfiguredFeatureImpl<VegetationPatchConfiguration, VegetationPatchFeature>>(
            &s_vegetationPatchFeature, *config);
        HUSH_MOSS_PATCH = feature.get();
        s_vegPatchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =====================================================================
    // Crystal Caverns / surface expansion. These blocks are registered by
    // the block set that ships with the caverns; each feature resolves its
    // own and a missing one logs and leaves only that feature null
    // (HushPlacements::createPlaced skips a null feature, addFeature warns).
    // =====================================================================
    auto optionalBlock = [&](const char* name, const char* feature) -> BlockState* {
        BlockState* state = block(name);
        if (!state) {
            fprintf(stderr, "[HushFeatures] %s missing from the block registry - %s skipped\n",
                    name, feature);
        }
        return state;
    };

    // ORE_RESONITE — the ORE_ECHO shape (single BlockMatchTest target on
    // hushstone), vein size 6.
    if (BlockState* resoniteOre = optionalBlock("minecraft:resonite_ore", "ORE_RESONITE")) {
        std::shared_ptr<RuleTest> target = std::make_shared<BlockMatchTest>("minecraft:hushstone");
        std::vector<OreConfiguration::TargetBlockState> targets = {
            OreConfiguration::target(target, resoniteOre)
        };
        auto feature = std::make_unique<ConfiguredFeatureImpl<OreConfiguration, OreFeature>>(
            &s_oreFeature,
            OreConfiguration(targets, 6, 0.0f)
        );
        ORE_RESONITE = feature.get();
        s_features.push_back(std::move(feature));
    }

    // RESONANT_CLUSTERS / RESONANT_CLUSTER_SURFACE — resonant_cluster is the
    // amethyst-cluster block class (facing + waterlogged; canSurvive wants a
    // sturdy face on the block behind it, which SimpleBlockFeature checks).
    //
    // RESONANT_CLUSTERS copies the POINTED_DRIPSTONE shape
    // (CaveFeatures.cpp: simple_random_selector over inline placed variants,
    // nextInt(n) drawn before any placement) with six variants, one per
    // facing:
    //   UP    environment_scan(down, solid, through air, 12) + offset +1
    //   DOWN  environment_scan(up,   solid, through air, 12) + offset -1
    //   walls environment_scan(down, ...) + offset uniform(+1, +3), then a
    //         block_predicate_filter: origin in air, solid behind the cluster
    // so floors, ceilings and the lower three blocks of every wall all
    // take clusters. Water cells are skipped (no waterlogged variant; the
    // still pools stay clear).
    if (BlockState* clusterDefault = optionalBlock("minecraft:resonant_cluster", "RESONANT_CLUSTERS")) {
        using minecraft::world::level::block::state::properties::BlockStateProperties;
        BlockStateProperties::initialize();
        auto* facingProperty = BlockStateProperties::FACING;
        if (!facingProperty || !clusterDefault->hasProperty(facingProperty)) {
            fprintf(stderr, "[HushFeatures] minecraft:resonant_cluster has no facing property -"
                            " RESONANT_CLUSTERS skipped\n");
        } else {
            auto clusterFacing = [&](core::Direction dir) -> ConfiguredFeature* {
                BlockState* state = clusterDefault->setValue(*facingProperty, dir);
                return createSimpleBlockConfiguredFeature(simpleProvider(state ? state : clusterDefault));
            };

            auto onlyInAir = blockpredicates::BlockPredicate::ONLY_IN_AIR_PREDICATE;
            auto scan = [&](EnvironmentScanPlacement::Direction dir) -> PlacementModifier* {
                return storeModifier(std::make_unique<EnvironmentScanPlacement>(
                    EnvironmentScanPlacement::scanningFor(
                        dir, blockpredicates::BlockPredicate::solid(), onlyInAir, 12)));
            };
            auto verticalOffset = [&](std::shared_ptr<carver::IntProvider> spread) -> PlacementModifier* {
                s_carverIntProviders.push_back(spread);
                return storeModifier(std::make_unique<RandomOffsetPlacement>(
                    RandomOffsetPlacement::vertical(spread.get())));
            };
            auto solidBehind = [&](core::Direction facing) -> PlacementModifier* {
                core::Direction behind = core::getOpposite(facing);
                core::Vec3i offset(core::getStepX(behind), core::getStepY(behind), core::getStepZ(behind));
                auto predicate = blockpredicates::BlockPredicate::allOf(
                    onlyInAir, blockpredicates::BlockPredicate::solid(offset));
                s_blockPredicates.push_back(predicate);
                return predicateFilter(predicate);
            };

            std::vector<PlacedFeature*> variants;
            variants.push_back(createPlacedFeature(
                clusterFacing(core::Direction::UP),
                {scan(EnvironmentScanPlacement::Direction::DOWN),
                 verticalOffset(carverConstantInt(1))},
                "resonant_cluster_floor_inline"));
            variants.push_back(createPlacedFeature(
                clusterFacing(core::Direction::DOWN),
                {scan(EnvironmentScanPlacement::Direction::UP),
                 verticalOffset(carverConstantInt(-1))},
                "resonant_cluster_ceiling_inline"));
            for (core::Direction wall : {core::Direction::NORTH, core::Direction::SOUTH,
                                         core::Direction::WEST, core::Direction::EAST}) {
                auto rise = std::make_shared<carver::UniformInt>(1, 3);
                variants.push_back(createPlacedFeature(
                    clusterFacing(wall),
                    {scan(EnvironmentScanPlacement::Direction::DOWN),
                     verticalOffset(rise), solidBehind(wall)},
                    std::string("resonant_cluster_wall_") + core::getName(wall) + "_inline"));
            }

            auto selectorConfig = std::make_unique<SimpleRandomFeatureConfiguration>(variants);
            auto selector = std::make_unique<ConfiguredFeatureImpl<SimpleRandomFeatureConfiguration, SimpleRandomSelectorFeature>>(
                &s_simpleRandomSelectorFeature, *selectorConfig);
            RESONANT_CLUSTERS = selector.get();
            s_simpleRandomConfigs.push_back(std::move(selectorConfig));
            s_features.push_back(std::move(selector));

            // RESONANT_CLUSTER_SURFACE — a few up-facing clusters on the
            // barrens' bare stone: RandomPatch(tries 4, xz 2, y 1) of the
            // floor variant, only in air on hushstone / polished hushstone.
            PlacedFeature* surfaceCluster = createPlacedFeature(
                clusterFacing(core::Direction::UP),
                {predicateFilter(onlyInAirOn({"minecraft:hushstone", "minecraft:polished_hushstone"}))},
                "resonant_cluster_surface_inline");
            auto config = std::make_unique<RandomPatchConfiguration>(4, 2, 1, surfaceCluster);
            auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
                &s_randomPatchFeature, *config);
            RESONANT_CLUSTER_SURFACE = feature.get();
            s_patchConfigs.push_back(std::move(config));
            s_features.push_back(std::move(feature));
        }
    }

    // RESONANT_CRYSTAL_CLUMP — a 2-3 block blob of resonant crystal on a
    // cavern floor: RandomPatch(tries 3, xz 1, y 1) of a single crystal, only
    // in air on the Hush stone family. The floor is found by the placement
    // (environment_scan down + offset +1, HushPlacements).
    {
        PlacedFeature* crystal = createPlacedFeature(
            createSimpleBlockConfiguredFeature(simpleProvider(resonantCrystal)),
            {predicateFilter(onlyInAirOn({"minecraft:hushstone", "minecraft:polished_hushstone",
                                          "minecraft:resonite_ore", "minecraft:echo_ore"}))},
            "resonant_crystal_clump_inline");
        auto config = std::make_unique<RandomPatchConfiguration>(3, 1, 1, crystal);
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        RESONANT_CRYSTAL_CLUMP = feature.get();
        s_patchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // PATCH_HUSH_GRASS — the PATCH_GRASS shape (VegetationFeatures.java
    // grassPatch) at half its tries (16 over the same 7-block spread): the
    // Hush's grass is scattered blades, not the plains' carpet. Only in air
    // on sculk loam / hush moss (SimpleBlockFeature also runs the bush's own
    // canSurvive).
    if (BlockState* hushGrass = optionalBlock("minecraft:hush_grass", "PATCH_HUSH_GRASS")) {
        PlacedFeature* inner = createPlacedFeature(
            createSimpleBlockConfiguredFeature(simpleProvider(hushGrass)),
            {predicateFilter(onlyInAirOn({"minecraft:sculk_loam", "minecraft:hush_moss"}))},
            "hush_grass_inline");
        auto config = std::make_unique<RandomPatchConfiguration>(16, 7, 3, inner);
        auto feature = std::make_unique<ConfiguredFeatureImpl<RandomPatchConfiguration, RandomPatchFeature>>(
            &s_randomPatchFeature, *config);
        PATCH_HUSH_GRASS = feature.get();
        s_patchConfigs.push_back(std::move(config));
        s_features.push_back(std::move(feature));
    }

    // =====================================================================
    // Sunken Choir / Hollow Deep / Aurora Steppe. Same rule as above: a
    // missing block logs and leaves only its feature null.
    // =====================================================================
    using minecraft::world::level::block::state::properties::BlockStateProperties;
    BlockStateProperties::initialize();

    // SUNKEN_RUIN — the Hush-only fragment class above.
    {
        BlockState* bricks = optionalBlock("minecraft:hushstone_bricks", "SUNKEN_RUIN");
        BlockState* cracked = optionalBlock("minecraft:cracked_hushstone_bricks", "SUNKEN_RUIN");
        BlockState* chiseled = optionalBlock("minecraft:chiseled_hushstone_bricks", "SUNKEN_RUIN");
        BlockState* polished = block("minecraft:polished_hushstone");
        if (bricks && cracked && chiseled && polished) {
            s_sunkenRuinFeature.bricks = bricks;
            s_sunkenRuinFeature.cracked = cracked;
            s_sunkenRuinFeature.chiseled = chiseled;
            s_sunkenRuinFeature.polished = polished;
            auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, SunkenRuinFeature>>(
                &s_sunkenRuinFeature, NoneFeatureConfiguration());
            SUNKEN_RUIN = feature.get();
            s_features.push_back(std::move(feature));
        }
    }

    // KELP / SEA_PICKLE — the vanilla features (26.3 AquaticFeatures: the
    // kelp block column, one sea pickle of 1-4). The Hush's glow is the
    // atmosphere's, not the block's; its placements set the counts.
    AquaticFeatures::bootstrap();
    KELP = AquaticFeatures::KELP;
    SEA_PICKLE = AquaticFeatures::SEA_PICKLE;

    // ROPE_BRIDGE — the Hush-only suspension bridge class above.
    {
        using minecraft::world::level::block::state::properties::SlabType;
        BlockState* planks = optionalBlock("minecraft:whisperwood_planks", "ROPE_BRIDGE");
        BlockState* slab = optionalBlock("minecraft:whisperwood_slab", "ROPE_BRIDGE");
        BlockState* stripped = optionalBlock("minecraft:stripped_whisperwood_log", "ROPE_BRIDGE");
        BlockState* log = block("minecraft:whisperwood_log");
        BlockState* fence = optionalBlock("minecraft:whisperwood_fence", "ROPE_BRIDGE");
        BlockState* chain = optionalBlock("minecraft:iron_chain", "ROPE_BRIDGE");
        BlockState* lantern = optionalBlock("minecraft:echo_lantern", "ROPE_BRIDGE");
        BlockState* bricks = optionalBlock("minecraft:hushstone_bricks", "ROPE_BRIDGE");
        auto* axis = BlockStateProperties::AXIS;
        auto* slabType = BlockStateProperties::SLAB_TYPE;
        auto* hanging = BlockStateProperties::HANGING;
        auto* north = BlockStateProperties::NORTH;
        auto* east = BlockStateProperties::EAST;
        auto* south = BlockStateProperties::SOUTH;
        auto* west = BlockStateProperties::WEST;
        if (planks && slab && stripped && log && fence && chain && lantern && bricks
            && axis && slabType && hanging && north && east && south && west
            && chain->hasProperty(axis) && log->hasProperty(axis) && stripped->hasProperty(axis)
            && slab->hasProperty(slabType) && lantern->hasProperty(hanging) && fence->hasProperty(east)) {
            RopeBridgeFeature& bridge = s_ropeBridgeFeature;
            bridge.planks = planks;
            bridge.slabBottom = slab->trySetValue(*slabType, SlabType(SlabType::BOTTOM));
            bridge.slabTop = slab->trySetValue(*slabType, SlabType(SlabType::TOP));
            bridge.strippedX = stripped->trySetValue(*axis, core::Axis::X);
            bridge.strippedZ = stripped->trySetValue(*axis, core::Axis::Z);
            bridge.logX = log->trySetValue(*axis, core::Axis::X);
            bridge.logY = log->trySetValue(*axis, core::Axis::Y);
            bridge.logZ = log->trySetValue(*axis, core::Axis::Z);
            bridge.chainY = chain->trySetValue(*axis, core::Axis::Y);
            bridge.lantern = lantern->trySetValue(*hanging, true);
            bridge.bricks = bricks;
            // Unconnected fence first, then one state per connection mask
            // along each axis (bit 0 = +axis side, bit 1 = -axis side).
            BlockState* bare = fence->trySetValue(*north, false)->trySetValue(*east, false)
                                    ->trySetValue(*south, false)->trySetValue(*west, false);
            for (size_t mask = 0; mask < 4; ++mask) {
                bridge.fenceX[mask] = bare->trySetValue(*east, (mask & 1) != 0)->trySetValue(*west, (mask & 2) != 0);
                bridge.fenceZ[mask] = bare->trySetValue(*south, (mask & 1) != 0)->trySetValue(*north, (mask & 2) != 0);
            }
            bridge.rimGround = {block("minecraft:hushstone"), block("minecraft:polished_hushstone"),
                                block("minecraft:sculk_loam"), hushMoss};
            bridge.rimGround.erase(std::remove(bridge.rimGround.begin(), bridge.rimGround.end(), nullptr),
                                   bridge.rimGround.end());
            auto feature = std::make_unique<ConfiguredFeatureImpl<NoneFeatureConfiguration, RopeBridgeFeature>>(
                &s_ropeBridgeFeature, NoneFeatureConfiguration());
            ROPE_BRIDGE = feature.get();
            s_features.push_back(std::move(feature));
        } else {
            fprintf(stderr, "[HushFeatures] a whisperwood bridge block or property is missing - ROPE_BRIDGE skipped\n");
        }
    }

    // =====================================================================
    // Resonant crystal formations (ResonantCrystalFormationFeature): tapered
    // shards fanned out from one anchor, tipped and budded with resonant
    // clusters. Four presets; the shape numbers were tuned against the
    // offline mirror (see ResonantCrystalFormation.cpp). Lengths are blocks
    // along the axis, widths 1..5 across the base, leans degrees from
    // vertical. A missing block logs and leaves all four null.
    // =====================================================================
    {
        BlockState* cluster = optionalBlock("minecraft:resonant_cluster", "CRYSTAL_FORMATION_*");
        BlockState* calcite = optionalBlock("minecraft:calcite", "CRYSTAL_FORMATION_*");
        BlockState* hushstone = block("minecraft:hushstone");
        BlockState* polished = block("minecraft:polished_hushstone");
        BlockState* sculkLoam = optionalBlock("minecraft:sculk_loam", "CRYSTAL_FORMATION_*");
        BlockState* air = block("minecraft:air");
        auto* facingProperty = BlockStateProperties::FACING;

        if (cluster && calcite && sculkLoam && air && facingProperty && cluster->hasProperty(facingProperty)) {
            ResonantCrystalFormationConfiguration base;
            base.crystal = resonantCrystal;
            base.air = air;
            base.moundAccent = calcite;
            base.moundStone = polished;
            base.moundRough = hushstone;
            // Indexed by the Direction ordinal: DOWN, UP, NORTH, SOUTH, WEST, EAST.
            const core::Direction facings[6] = {core::Direction::DOWN, core::Direction::UP,
                                                core::Direction::NORTH, core::Direction::SOUTH,
                                                core::Direction::WEST, core::Direction::EAST};
            for (size_t i = 0; i < base.clusters.size(); ++i) {
                BlockState* facing = cluster->trySetValue(*facingProperty, facings[i]);
                base.clusters[i] = facing ? facing : cluster;
            }
            // The Hush's ground: formations stand only on these.
            base.anchors = {hushstone, polished, sculkLoam, hushMoss};

            auto shardSpec = [](float minLength, float maxLength, int32_t minWidth, int32_t maxWidth,
                                float minLean, float maxLean) {
                CrystalShardSpec spec;
                spec.minLength = minLength;
                spec.maxLength = maxLength;
                spec.minWidth = minWidth;
                spec.maxWidth = maxWidth;
                spec.minLeanDegrees = minLean;
                spec.maxLeanDegrees = maxLean;
                return spec;
            };
            auto createFormation = [&](const ResonantCrystalFormationConfiguration& config) -> ConfiguredFeature* {
                auto feature = std::make_unique<ConfiguredFeatureImpl<ResonantCrystalFormationConfiguration,
                                                                      ResonantCrystalFormationFeature>>(
                    &s_crystalFormationFeature, config);
                ConfiguredFeature* raw = feature.get();
                s_features.push_back(std::move(feature));
                return raw;
            };

            // CRYSTAL_FORMATION_SMALL — the meadows' accent (and the barrens'
            // scatter between the big ones): a 4-7 long, one- or two-wide
            // shard, two or three thin 2-4 satellites leaning 25-50 degrees,
            // a mound of radius 2, a few buds, sometimes a fallen shard.
            {
                ResonantCrystalFormationConfiguration config = base;
                config.main = shardSpec(4.0f, 7.0f, 1, 2, 0.0f, 12.0f);
                config.satellite = shardSpec(2.0f, 4.0f, 1, 1, 25.0f, 50.0f);
                config.minSatellites = 2;
                config.maxSatellites = 3;
                config.minSatelliteSpread = 1;
                config.maxSatelliteSpread = 2;
                config.moundCore = 1;
                config.moundRadius = 2;
                config.minGroundBuds = 2;
                config.maxGroundBuds = 4;
                config.minSideBuds = 1;
                config.maxSideBuds = 2;
                config.minFallenShards = 0;
                config.maxFallenShards = 1;
                CRYSTAL_FORMATION_SMALL = createFormation(config);
            }

            // CRYSTAL_FORMATION_FIELD — the barrens' outcrop: a 6-10 long 2x2
            // or 3x3 shard (one in five a 11-15 long 3x3 / 4x4 landmark),
            // three to six 3-7 satellites leaning 20-50 degrees, a mound of
            // radius 5 with a raised core of 3, one or two fallen shards.
            {
                ResonantCrystalFormationConfiguration config = base;
                config.main = shardSpec(6.0f, 10.0f, 2, 3, 0.0f, 14.0f);
                config.landmarkChance = 0.2f;
                config.landmark = shardSpec(11.0f, 15.0f, 3, 4, 0.0f, 8.0f);
                config.satellite = shardSpec(3.0f, 7.0f, 1, 2, 20.0f, 50.0f);
                config.minSatellites = 3;
                config.maxSatellites = 6;
                config.minSatelliteSpread = 1;
                config.maxSatelliteSpread = 3;
                config.moundCore = 3;
                config.moundRadius = 5;
                config.minGroundBuds = 4;
                config.maxGroundBuds = 8;
                config.minSideBuds = 3;
                config.maxSideBuds = 6;
                config.minFallenShards = 1;
                config.maxFallenShards = 2;
                CRYSTAL_FORMATION_FIELD = createFormation(config);
            }

            // CRYSTAL_FORMATION_TALL — the aurora steppe's and the hollow
            // deep rims' landmark: a 10-15 long 3x3 / 4x4 shard standing
            // nearly straight (a third of them a 15-19 long rounded 5x5),
            // three to five 4-9 satellites up to 3x3 leaning 15-40 degrees.
            {
                ResonantCrystalFormationConfiguration config = base;
                config.main = shardSpec(10.0f, 15.0f, 3, 4, 2.0f, 10.0f);
                config.landmarkChance = 0.35f;
                config.landmark = shardSpec(15.0f, 19.0f, 5, 5, 0.0f, 6.0f);
                config.satellite = shardSpec(4.0f, 9.0f, 1, 3, 15.0f, 40.0f);
                config.minSatellites = 3;
                config.maxSatellites = 5;
                config.minSatelliteSpread = 1;
                config.maxSatelliteSpread = 3;
                config.moundCore = 3;
                config.moundRadius = 5;
                config.minGroundBuds = 4;
                config.maxGroundBuds = 7;
                config.minSideBuds = 4;
                config.maxSideBuds = 7;
                config.minFallenShards = 0;
                config.maxFallenShards = 2;
                CRYSTAL_FORMATION_TALL = createFormation(config);
            }

            // RESONANT_STALACTITE — the hanging variant (the placement finds
            // the ceiling): a 4-8 long 2x2 shard (15 % an 8-11
            // long 3x3), one to three 2-5 satellites leaning 20-45 degrees,
            // clusters on the ceiling around it; no mound, nothing fallen.
            // It may also hang from echo and resonite ore.
            {
                ResonantCrystalFormationConfiguration config = base;
                config.hanging = true;
                config.main = shardSpec(4.0f, 8.0f, 2, 2, 0.0f, 12.0f);
                config.landmarkChance = 0.15f;
                config.landmark = shardSpec(8.0f, 11.0f, 3, 3, 0.0f, 6.0f);
                config.satellite = shardSpec(2.0f, 5.0f, 1, 1, 20.0f, 45.0f);
                config.minSatellites = 1;
                config.maxSatellites = 3;
                config.minSatelliteSpread = 1;
                config.maxSatelliteSpread = 2;
                config.minGroundBuds = 2;
                config.maxGroundBuds = 4;
                config.minSideBuds = 1;
                config.maxSideBuds = 3;
                config.moundAccent = nullptr;   // the ceiling is never dressed
                for (const char* ore : {"minecraft:echo_ore", "minecraft:resonite_ore"}) {
                    if (BlockState* state = block(ore)) config.anchors.push_back(state);
                }
                RESONANT_STALACTITE = createFormation(config);
            }
        } else {
            fprintf(stderr, "[HushFeatures] resonant crystal formations skipped (see above)\n");
        }
    }

    // HUSHSTONE_BOULDER — MiscOverworldFeatures.FOREST_ROCK with polished
    // hushstone for mossy cobblestone (it settles on #dirt, which holds
    // sculk loam and hush moss).
    if (BlockState* polished = block("minecraft:polished_hushstone")) {
        auto feature = std::make_unique<ConfiguredFeatureImpl<BlockStateConfiguration, ForestRockFeature>>(
            &s_forestRockFeature, BlockStateConfiguration(polished));
        HUSHSTONE_BOULDER = feature.get();
        s_features.push_back(std::move(feature));
    }

    s_initialized = true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
