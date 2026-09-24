#include "data/worldgen/features/ResonantCrystalFormation.h"
#include "math/Mth.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

// The Hush — engine-only dimension. No Java reference; see the header.
//
// The offline mirror used to design the shapes (same algorithm and draw
// order, rendered as isometric PNGs) was
// scratchpad/crystals/formation.py; the numbers below are the ones it
// settled on.

namespace minecraft {
namespace data {
namespace worldgen {
namespace features {

using levelgen::FeaturePlaceContext;
using levelgen::WorldGenLevel;
using levelgen::WorldgenRandom;

bool ResonantCrystalFormationConfiguration::isComplete() const {
    if (!crystal || !air || anchors.empty()) return false;
    for (BlockState* cluster : clusters) {
        if (!cluster) return false;
    }
    if (!hanging && moundRadius > 0 && (!moundAccent || !moundStone || !moundRough)) return false;
    return true;
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// Face indices = the Direction ordinals the cluster table is keyed by.
enum Face : int32_t { FACE_DOWN = 0, FACE_UP = 1, FACE_NORTH = 2, FACE_SOUTH = 3, FACE_WEST = 4, FACE_EAST = 5 };
constexpr int32_t kFaceX[6] = {0, 0, 0, 0, -1, 1};
constexpr int32_t kFaceY[6] = {-1, 1, 0, 0, 0, 0};
constexpr int32_t kFaceZ[6] = {0, 0, -1, 1, 0, 0};
// Side buds pick one of these (the four sides and the top of a step).
constexpr Face kBudFaces[5] = {FACE_EAST, FACE_WEST, FACE_SOUTH, FACE_NORTH, FACE_UP};

/**
 * Base radius per width. Odd widths are centred on a cell, even widths on
 * a grid line; a cell is in the shard when its centre lies within the
 * (tapering) radius of the axis. 0.5 keeps one column, 0.9 a 2x2, 1.5 a
 * 3x3, 1.9 a 4x4 without corners, 2.3 a 5x5 without corners.
 */
struct WidthShape { double radius; bool gridLine; };
constexpr WidthShape kWidths[6] = {
    {0.5, false}, {0.5, false}, {0.9, true}, {1.5, false}, {1.9, true}, {2.3, false}
};

/**
 * Cross-section radius at a fraction of the shard's length: the body
 * narrows from 100 % to 80 % over its first 55 %, then closes to a
 * pyramid point — so a 3x3 base steps down 3x3 -> plus -> one column.
 */
double radiusAt(double baseRadius, double frac) {
    if (frac < 0.55) return baseRadius * (1.0 - 0.2 * frac / 0.55);
    return baseRadius * 0.8 * (1.0 - (frac - 0.55) / 0.45);
}

struct Cell {
    int32_t x, y, z;   // grow space, relative to the origin
};

/** One shard in grow space: base point on the anchor plane, unit axis, length, base radius. */
struct Shard {
    double bx, by, bz;
    double dx, dy, dz;
    double length;
    double radius;
};

/**
 * The formation's cells in grow space: insertion order (placement order,
 * deterministic) plus a membership grid over the reachable box.
 */
class CellSet {
public:
    static constexpr int32_t kReach = ResonantCrystalFormationFeature::MAX_REACH;
    static constexpr int32_t kMinY = -8;
    static constexpr int32_t kMaxY = 28;
    static constexpr int32_t kSide = 2 * kReach + 1;
    static constexpr int32_t kHeight = kMaxY - kMinY + 1;

    CellSet() : m_grid(static_cast<size_t>(kSide * kSide * kHeight), 0) {}

    static bool inBox(int32_t x, int32_t y, int32_t z) {
        return std::abs(x) <= kReach && std::abs(z) <= kReach && y >= kMinY && y <= kMaxY;
    }
    bool contains(int32_t x, int32_t y, int32_t z) const {
        return inBox(x, y, z) && m_grid[index(x, y, z)] != 0;
    }
    void add(int32_t x, int32_t y, int32_t z) {
        if (!inBox(x, y, z)) return;
        uint8_t& slot = m_grid[index(x, y, z)];
        if (slot) return;
        slot = 1;
        m_cells.push_back({x, y, z});
    }
    const std::vector<Cell>& cells() const { return m_cells; }

private:
    static size_t index(int32_t x, int32_t y, int32_t z) {
        return static_cast<size_t>(((y - kMinY) * kSide + (z + kReach)) * kSide + (x + kReach));
    }
    std::vector<uint8_t> m_grid;
    std::vector<Cell> m_cells;
};

/** The axis direction's dominant face: UP for a shard within 45 degrees of vertical. */
Face tipFace(const Shard& shard) {
    const double ax = std::abs(shard.dx);
    const double ay = std::abs(shard.dy);
    const double az = std::abs(shard.dz);
    if (ay >= std::max(ax, az)) return FACE_UP;
    if (ax >= az) return shard.dx > 0 ? FACE_EAST : FACE_WEST;
    return shard.dz > 0 ? FACE_SOUTH : FACE_NORTH;
}

/**
 * Voxelises one shard into `cells` and returns its tip cell (the last cell
 * of the spine).
 *
 * 1. The spine: the axis walked in 1/8-block steps. A step that moves more
 *    than one coordinate is split x, then z, then y, so the spine is face-
 *    connected and every rising cell stands on the one before it (a leaning
 *    shard reads as a staircase, never as diagonally touching cubes). An
 *    upright shard ends on its last rise: trailing sideways steps at the
 *    point would read as a hook under the tip cluster.
 * 2. The body: every cell whose centre lies within radiusAt() of the axis.
 */
Cell voxelise(const Shard& shard, CellSet& cells) {
    const bool upright = tipFace(shard) == FACE_UP;
    const int32_t steps = std::max(1, static_cast<int32_t>(shard.length * 8.0));

    std::vector<Cell> pending;   // sideways steps since the last rise (upright shards)
    Cell cur{};
    Cell last{};
    for (int32_t i = 0; i <= steps; ++i) {
        const double t = shard.length * i / steps;
        const int32_t c[3] = {Mth::floor(shard.bx + shard.dx * t),
                              Mth::floor(shard.by + shard.dy * t),
                              Mth::floor(shard.bz + shard.dz * t)};
        if (i == 0) {
            cur = {c[0], c[1], c[2]};
            cells.add(cur.x, cur.y, cur.z);
            last = cur;
            continue;
        }
        for (int32_t axis : {0, 2, 1}) {
            int32_t& coord = axis == 0 ? cur.x : (axis == 1 ? cur.y : cur.z);
            if (c[axis] == coord) continue;
            coord = c[axis];
            if (upright && axis != 1) {
                pending.push_back(cur);
            } else {
                for (const Cell& p : pending) cells.add(p.x, p.y, p.z);
                pending.clear();
                cells.add(cur.x, cur.y, cur.z);
                last = cur;
            }
        }
    }

    const int32_t reach = static_cast<int32_t>(std::ceil(shard.radius)) + 1;
    const int32_t y0 = Mth::floor(shard.by);
    const int32_t y1 = y0 + static_cast<int32_t>(std::ceil(shard.length));
    for (int32_t y = y0; y <= y1; ++y) {
        // Where the axis crosses this layer's mid-plane.
        const double tLayer = (y + 0.5 - shard.by) / shard.dy;
        const int32_t cx = Mth::floor(shard.bx + shard.dx * tLayer);
        const int32_t cz = Mth::floor(shard.bz + shard.dz * tLayer);
        for (int32_t x = cx - reach; x <= cx + reach; ++x) {
            for (int32_t z = cz - reach; z <= cz + reach; ++z) {
                const double px = x + 0.5 - shard.bx;
                const double py = y + 0.5 - shard.by;
                const double pz = z + 0.5 - shard.bz;
                const double t = px * shard.dx + py * shard.dy + pz * shard.dz;
                if (t < 0.0 || t > shard.length) continue;
                const double qx = px - shard.dx * t;
                const double qy = py - shard.dy * t;
                const double qz = pz - shard.dz * t;
                const double r = radiusAt(shard.radius, t / shard.length);
                if (qx * qx + qy * qy + qz * qz <= r * r) {
                    cells.add(x, y, z);
                }
            }
        }
    }
    return last;
}

int32_t between(WorldgenRandom& random, int32_t lo, int32_t hi) {
    return hi > lo ? lo + random.nextInt(hi - lo + 1) : lo;
}

} // namespace

bool ResonantCrystalFormationFeature::place(
        FeaturePlaceContext<ResonantCrystalFormationConfiguration>& context) {
    const ResonantCrystalFormationConfiguration& config = context.config();
    if (!config.isComplete()) return false;
    WorldGenLevel* level = context.level();
    WorldgenRandom& random = context.random();
    const core::BlockPos origin = context.origin();
    const int32_t sign = config.hanging ? -1 : 1;

    // ---- grow space <-> world ----
    auto toWorld = [&](int32_t x, int32_t y, int32_t z) {
        return core::BlockPos(origin.getX() + x, origin.getY() + sign * y, origin.getZ() + z);
    };
    auto stateAt = [&](int32_t x, int32_t y, int32_t z) -> BlockState* {
        return level->getBlockState(toWorld(x, y, z));
    };
    auto canWrite = [&](const core::BlockPos& pos) {
        return !level->isOutsideBuildHeight(pos) && level->ensureCanWrite(pos);
    };
    // Free for crystal: air or a replaceable plant (hush grass); never water.
    auto isFree = [](BlockState* state) {
        return state != nullptr
            && (state->isAir() || (state->canBeReplaced() && !state->hasAnyFluid()));
    };
    auto isSolid = [](BlockState* state) { return state != nullptr && state->blocksMotion(); };
    auto isAnchor = [&](BlockState* state) {
        if (state == nullptr) return false;
        for (BlockState* anchor : config.anchors) {
            if (state->is(anchor)) return true;
        }
        return false;
    };
    // Footing for the formation's own pieces also includes the mound's calcite.
    auto isFooting = [&](BlockState* state) {
        return isAnchor(state) || (config.moundAccent && state && state->is(config.moundAccent));
    };
    // The first free cell over footing (under it, hanging) in a column,
    // searched from +2 down to -3 in grow space.
    auto footingOffset = [&](int32_t x, int32_t z) -> std::optional<int32_t> {
        for (int32_t y = 2; y >= -3; --y) {
            if (isFree(stateAt(x, y, z)) && isFooting(stateAt(x, y - 1, z))) return y;
        }
        return std::nullopt;
    };
    auto worldFace = [&](int32_t face) -> int32_t {
        if (!config.hanging) return face;
        if (face == FACE_UP) return FACE_DOWN;
        if (face == FACE_DOWN) return FACE_UP;
        return face;
    };

    // ---- 1. The anchor ----
    if (!isFree(stateAt(0, 0, 0)) || !isAnchor(stateAt(0, -1, 0))) return false;
    static constexpr int32_t kNeighbourX[4] = {1, -1, 0, 0};
    static constexpr int32_t kNeighbourZ[4] = {0, 0, 1, -1};
    for (int32_t n = 0; n < 4; ++n) {
        const std::optional<int32_t> y = footingOffset(kNeighbourX[n], kNeighbourZ[n]);
        if (!y || std::abs(*y) > 1) return false;   // a slope, a ledge or a wall
    }

    // ---- 2. The shards ----
    struct Drawn { double length; int32_t width; double lean; };
    auto draw = [&](const CrystalShardSpec& spec, double scale) -> Drawn {
        Drawn d{};
        d.length = (spec.minLength + random.nextFloat() * (spec.maxLength - spec.minLength)) * scale;
        d.width = std::clamp(between(random, spec.minWidth, spec.maxWidth), 1, 5);
        d.lean = (spec.minLeanDegrees + random.nextFloat() * (spec.maxLeanDegrees - spec.minLeanDegrees))
                 * kPi / 180.0;
        return d;
    };
    auto makeShard = [](double baseX, double baseY, double baseZ, const Drawn& d, double azimuth) {
        const WidthShape& shape = kWidths[d.width];
        const double offset = shape.gridLine ? 0.0 : 0.5;
        Shard shard{};
        shard.bx = baseX + offset;
        shard.by = baseY;
        shard.bz = baseZ + offset;
        shard.dx = Mth::sin(d.lean) * Mth::cos(azimuth);
        shard.dy = Mth::cos(d.lean);
        shard.dz = Mth::sin(d.lean) * Mth::sin(azimuth);
        shard.length = std::max(1.0, d.length);
        shard.radius = shape.radius;
        return shard;
    };

    std::vector<Shard> shards;
    const bool landmark = config.landmarkChance > 0.0f && random.nextFloat() < config.landmarkChance;
    const Drawn mainDraw = draw(landmark ? config.landmark : config.main, 1.0);
    const double mainAzimuth = random.nextFloat() * 2.0 * kPi;
    shards.push_back(makeShard(0.0, 0.0, 0.0, mainDraw, mainAzimuth));
    const int32_t mainHalfWidth = mainDraw.width / 2;

    // Satellites fan out evenly around the main shard (with jitter) and lean
    // away from it; a landmark gets one more, 25 % longer.
    const int32_t satellites = between(random, config.minSatellites, config.maxSatellites) + (landmark ? 1 : 0);
    const double fanStart = random.nextFloat() * 2.0 * kPi;
    for (int32_t i = 0; i < satellites; ++i) {
        const double azimuth = fanStart + i * 2.0 * kPi / satellites + (random.nextFloat() - 0.5);
        const int32_t spread = mainHalfWidth + between(random, config.minSatelliteSpread, config.maxSatelliteSpread);
        const Drawn d = draw(config.satellite, landmark ? 1.25 : 1.0);
        const double leanAzimuth = azimuth + (random.nextFloat() - 0.5) * 0.7;
        const int32_t x = static_cast<int32_t>(std::lround(Mth::cos(azimuth) * spread));
        const int32_t z = static_cast<int32_t>(std::lround(Mth::sin(azimuth) * spread));
        const std::optional<int32_t> y = footingOffset(x, z);
        if (!y) continue;   // no footing there (a drop, a wall): the satellite is skipped
        shards.push_back(makeShard(x, *y, z, d, leanAzimuth));
    }

    CellSet cells;
    std::vector<std::pair<Cell, Face>> tips;
    tips.reserve(shards.size());
    for (const Shard& shard : shards) {
        const Cell tip = voxelise(shard, cells);
        tips.emplace_back(tip, tipFace(shard));
    }

    // ---- 3. The mound (floor formations) ----
    // A raised core disc one block over the ground and a dressed ring out to
    // moundRadius, both jittered so the edge is ragged: calcite (more of it
    // at the core, the geode's white), polished hushstone, some rough
    // hushstone. Only anchor blocks are re-dressed, and only where the cell
    // above is free; a hush-grass tuft on a dressed block is cleared.
    if (!config.hanging && config.moundRadius > 0) {
        const int32_t radius = config.moundRadius;
        for (int32_t x = -radius; x <= radius; ++x) {
            for (int32_t z = -radius; z <= radius; ++z) {
                const double dist = std::sqrt(static_cast<double>(x * x + z * z)) + random.nextFloat() * 0.8;
                if (dist > radius + 0.5) continue;
                const std::optional<int32_t> y = footingOffset(x, z);
                if (!y) continue;
                const bool inCore = dist <= config.moundCore + 0.5;
                const float roll = random.nextFloat();
                BlockState* dressing = roll < (inCore ? 0.35f : 0.25f) ? config.moundAccent
                                     : roll < 0.8f ? config.moundStone : config.moundRough;
                const core::BlockPos groundPos = toWorld(x, *y - 1, z);
                const core::BlockPos abovePos = toWorld(x, *y, z);
                if (inCore || random.nextFloat() < 0.75f - 0.5f * static_cast<float>(dist / (radius + 0.5))) {
                    BlockState* ground = level->getBlockState(groundPos);
                    BlockState* above = level->getBlockState(abovePos);
                    if (isAnchor(ground) && isFree(above) && canWrite(groundPos) && canWrite(abovePos)) {
                        if (!above->isAir()) level->setBlock(abovePos, config.air, 2);
                        level->setBlock(groundPos, dressing, 2);
                    }
                }
                if (inCore && *y <= 0 && !cells.contains(x, *y, z)
                    && isFree(level->getBlockState(abovePos)) && canWrite(abovePos)) {
                    level->setBlock(abovePos, random.nextFloat() < 0.3f ? config.moundAccent : config.moundStone, 2);
                }
            }
        }
    }

    // ---- 4. The crystal ----
    std::vector<Cell> placed;
    placed.reserve(cells.cells().size());
    for (const Cell& c : cells.cells()) {
        const core::BlockPos pos = toWorld(c.x, c.y, c.z);
        if (isFree(level->getBlockState(pos)) && canWrite(pos)) {
            level->setBlock(pos, config.crystal, 2);
            placed.push_back(c);
        }
    }
    if (placed.empty()) return false;

    // Fill the underside of low overhangs down to their footing (a leaning
    // shard's first steps, a base over a dip), at most three blocks; higher
    // overhangs hang off the shard body above them.
    for (const Cell& c : placed) {
        if (cells.contains(c.x, c.y - 1, c.z)) continue;
        int32_t gap = 0;
        bool grounded = false;
        for (int32_t k = 1; k <= 3; ++k) {
            BlockState* state = stateAt(c.x, c.y - k, c.z);
            if (isSolid(state)) { grounded = true; break; }
            if (!isFree(state)) break;
            gap = k;
        }
        if (!grounded) continue;
        for (int32_t k = 1; k <= gap; ++k) {
            const core::BlockPos pos = toWorld(c.x, c.y - k, c.z);
            if (canWrite(pos)) level->setBlock(pos, config.crystal, 2);
        }
    }

    // ---- 5. Clusters ----
    // A cluster points along `face` (grow space) and needs a full block
    // behind it, as AmethystClusterBlock.canSurvive does (a solid block that
    // is not itself a cluster: clusters are forceSolidOn but have no sturdy
    // face).
    auto putCluster = [&](int32_t x, int32_t y, int32_t z, int32_t face) {
        if (std::abs(x) > MAX_REACH + 1 || std::abs(z) > MAX_REACH + 1) return false;
        const core::BlockPos pos = toWorld(x, y, z);
        if (!isFree(level->getBlockState(pos)) || !canWrite(pos)) return false;
        BlockState* support = stateAt(x - kFaceX[face], y - kFaceY[face], z - kFaceZ[face]);
        if (!isSolid(support) || support->is(config.clusters[FACE_UP])) return false;
        level->setBlock(pos, config.clusters[static_cast<size_t>(worldFace(face))], 2);
        return true;
    };

    // Every shard ends in a cluster pointing along it.
    for (const auto& [tip, face] : tips) {
        putCluster(tip.x + kFaceX[face], tip.y + kFaceY[face], tip.z + kFaceZ[face], face);
    }

    // Buds on the shards' faces (not the bottom layer, where they would sit
    // in the grass).
    int32_t wanted = between(random, config.minSideBuds, config.maxSideBuds);
    for (int32_t attempt = 0; attempt < wanted * 4 && wanted > 0; ++attempt) {
        const Cell& c = placed[static_cast<size_t>(random.nextInt(static_cast<int32_t>(placed.size())))];
        const Face face = kBudFaces[random.nextInt(5)];
        if (c.y < 1) continue;
        if (putCluster(c.x + kFaceX[face], c.y + kFaceY[face], c.z + kFaceZ[face], face)) --wanted;
    }

    // Buds on the ground (ceiling) around the formation.
    wanted = between(random, config.minGroundBuds, config.maxGroundBuds);
    const int32_t budRadius = std::max(config.moundRadius, 2) + 2;
    for (int32_t attempt = 0; attempt < wanted * 4 && wanted > 0; ++attempt) {
        const int32_t x = random.nextInt(2 * budRadius + 1) - budRadius;
        const int32_t z = random.nextInt(2 * budRadius + 1) - budRadius;
        const std::optional<int32_t> y = footingOffset(x, z);
        if (!y) continue;
        if (putCluster(x, *y, z, FACE_UP)) --wanted;
    }

    // Fallen shards: 2-3 crystal blocks lying on the ground just outside the
    // mound, a cluster on the far end. All-or-nothing: every block needs
    // solid ground under it.
    if (!config.hanging) {
        const int32_t fallen = between(random, config.minFallenShards, config.maxFallenShards);
        for (int32_t n = 0; n < fallen; ++n) {
            const double azimuth = random.nextFloat() * 2.0 * kPi;
            const int32_t dist = config.moundRadius + 1 + random.nextInt(3);
            const Face face = kBudFaces[random.nextInt(4)];
            const int32_t length = 2 + random.nextInt(2);
            const int32_t x0 = static_cast<int32_t>(std::lround(Mth::cos(azimuth) * dist));
            const int32_t z0 = static_cast<int32_t>(std::lround(Mth::sin(azimuth) * dist));
            const std::optional<int32_t> y = footingOffset(x0, z0);
            if (!y) continue;
            bool fits = true;
            for (int32_t k = 0; k < length && fits; ++k) {
                const int32_t x = x0 + kFaceX[face] * k;
                const int32_t z = z0 + kFaceZ[face] * k;
                const core::BlockPos pos = toWorld(x, *y, z);
                fits = std::abs(x) <= MAX_REACH && std::abs(z) <= MAX_REACH
                    && isFree(level->getBlockState(pos)) && canWrite(pos)
                    && isSolid(stateAt(x, *y - 1, z));
            }
            if (!fits) continue;
            for (int32_t k = 0; k < length; ++k) {
                level->setBlock(toWorld(x0 + kFaceX[face] * k, *y, z0 + kFaceZ[face] * k), config.crystal, 2);
            }
            putCluster(x0 + kFaceX[face] * length, *y, z0 + kFaceZ[face] * length, face);
        }
    }
    return true;
}

} // namespace features
} // namespace worldgen
} // namespace data
} // namespace minecraft
