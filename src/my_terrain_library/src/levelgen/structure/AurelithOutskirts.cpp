// Engine extension: companion structures and Aurelith's outskirts. The design
// is in include/levelgen/structure/AurelithOutskirts.h; the data (the layout
// of every slot, the templates) is written by tools/gen_aurelith_outskirts.py.
#include "levelgen/structure/AurelithOutskirts.h"

#include "levelgen/structure/ChunkGeneratorStructureState.h"
#include "levelgen/structure/JigsawTemplates.h"
#include "levelgen/structure/PieceBehaviors.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/structure/TemplatePool.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "world/level/block/state/BlockState.h"
#include "external/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace minecraft {
namespace levelgen {
namespace structure {

namespace {

using nlohmann::json;

int32_t floorDiv(int32_t x, int32_t y) {
    int32_t r = x / y;
    if ((x ^ y) < 0 && r * y != x) --r;
    return r;
}

// MC Rotation.rotate(BlockPos) with pivot ZERO (StructureTemplate.transform
// for a jigsaw piece): NONE, CLOCKWISE_90, CLOCKWISE_180, COUNTERCLOCKWISE_90.
std::pair<int32_t, int32_t> rotateXZ(int32_t x, int32_t z, int rotation) {
    switch (rotation & 3) {
        case 1:  return {-z, x};
        case 2:  return {-x, -z};
        case 3:  return {z, -x};
        default: return {x, z};
    }
}

int rotationOrdinal(const std::string& name) {
    if (name == "CLOCKWISE_90") return 1;
    if (name == "CLOCKWISE_180") return 2;
    if (name == "COUNTERCLOCKWISE_90") return 3;
    return 0;
}

const char* rotationName(int rotation) {
    static const char* names[4] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180", "COUNTERCLOCKWISE_90"};
    return names[rotation & 3];
}

// splitmix64: every roll here is a hash of (seed, position, salt), so a
// chunk decorates identically however the generator's threads interleave.
uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double unitHash(uint64_t seed, int64_t a, int64_t b, uint64_t salt) {
    const uint64_t h = mix64(seed ^ mix64(static_cast<uint64_t>(a) * 0x9E3779B97F4A7C15ull
                                          ^ static_cast<uint64_t>(b) * 0xC2B2AE3D27D4EB4Full
                                          ^ salt * 0xD1B54A32D192ED03ull));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// ── The layout (the structure JSON's engine fields) ──────────────────────

struct RoadSpec {
    double ax = 0, az = 0, bx = 0, bz = 0;   // the whole road, design frame
    double t0 = 0, t1 = 0;                   // the stretch this slot places
    double half = 3;                         // half width past the apron
    double apronLength = 0, apronHalf = 3;   // a wider start (the gate forecourt)
    double keepA = 0.9, keepB = 0.2;         // paving kept at a / at b
    double wobble = 0;                       // lateral meander, blocks
    uint32_t seed = 0;
    double lampEvery = 0;                    // lamp-post spacing (0 = none)
    double lampUntil = 0;                    // ... up to this t
};

struct PieceSpec {
    std::string templateId;
    int32_t originX = 0, originZ = 0;        // design position of the template's local origin
    int rotation = 0;                        // design rotation (0 = as authored)
    int yOffset = 0;                         // added to the sampled floor
    double chance = 1.0;                     // kept per city with this probability
    bool dry = false;                        // refused over water
};

struct SlotSpec {
    int32_t x = 0, z = 0;                    // design block offset from the anchor origin
    std::vector<RoadSpec> roads;
    std::vector<PieceSpec> pieces;
};

struct Layout {
    std::string anchorSet;
    std::string anchorStructure;
    int32_t originX = 0, originY = 0, originZ = 0;
    std::string processors;
    std::vector<SlotSpec> slots;
};

std::shared_ptr<const Layout> layoutFor(const StructureInfo& info) {
    static std::mutex s_mutex;
    static std::map<const StructureInfo*, std::shared_ptr<const Layout>> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(&info);
    if (it != s_cache.end()) return it->second;

    const json root = json::parse(info.modJson);
    auto layout = std::make_shared<Layout>();
    layout->anchorSet = root.at("anchor_set").get<std::string>();
    layout->anchorStructure = root.at("anchor_structure").get<std::string>();
    const auto& origin = root.at("anchor_origin");
    layout->originX = origin.at(0).get<int32_t>();
    layout->originY = origin.at(1).get<int32_t>();
    layout->originZ = origin.at(2).get<int32_t>();
    layout->processors = root.value("processors", std::string("minecraft:empty"));
    for (const auto& s : root.at("slots")) {
        SlotSpec slot;
        slot.x = s.at("at").at(0).get<int32_t>();
        slot.z = s.at("at").at(1).get<int32_t>();
        if (s.contains("roads")) {
            for (const auto& r : s["roads"]) {
                RoadSpec road;
                road.ax = r.at("a").at(0).get<double>();
                road.az = r.at("a").at(1).get<double>();
                road.bx = r.at("b").at(0).get<double>();
                road.bz = r.at("b").at(1).get<double>();
                road.t0 = r.at("t").at(0).get<double>();
                road.t1 = r.at("t").at(1).get<double>();
                road.half = r.value("half", 3.0);
                if (r.contains("apron")) {
                    road.apronLength = r["apron"].at(0).get<double>();
                    road.apronHalf = r["apron"].at(1).get<double>();
                }
                road.keepA = r.at("keep").at(0).get<double>();
                road.keepB = r.at("keep").at(1).get<double>();
                road.wobble = r.value("wobble", 0.0);
                road.seed = r.value("seed", 0u);
                if (r.contains("lamps")) {
                    road.lampEvery = r["lamps"].at(0).get<double>();
                    road.lampUntil = r["lamps"].at(1).get<double>();
                }
                slot.roads.push_back(road);
            }
        }
        if (s.contains("pieces")) {
            for (const auto& p : s["pieces"]) {
                PieceSpec piece;
                piece.templateId = p.at("template").get<std::string>();
                piece.originX = p.at("origin").at(0).get<int32_t>();
                piece.originZ = p.at("origin").at(1).get<int32_t>();
                piece.rotation = p.value("rotation", 0) & 3;
                piece.yOffset = p.value("y_offset", 0);
                piece.chance = p.value("chance", 1.0);
                piece.dry = p.value("dry", false);
                slot.pieces.push_back(std::move(piece));
            }
        }
        layout->slots.push_back(std::move(slot));
    }
    s_cache.emplace(&info, layout);
    return layout;
}

const RandomSpreadStructurePlacement& anchorPlacement(const std::string& setName) {
    const StructureSet& set = StructureSets::byName(setName);
    const auto* spread = dynamic_cast<const RandomSpreadStructurePlacement*>(set.placement.get());
    if (spread == nullptr) {
        throw std::runtime_error("companion anchor set " + setName + " is not a random_spread placement");
    }
    // Structures::generate has no ChunkGeneratorStructureState to re-run the
    // anchor's frequency / exclusion rules against, so a companion only
    // anchors on a set whose potential chunks ARE its structure chunks.
    if (spread->frequency() < 1.0f || spread->exclusionZone().has_value()) {
        throw std::runtime_error("companion anchor set " + setName +
                                 " must have frequency 1 and no exclusion zone");
    }
    return *spread;
}

// Every potential anchor start within `reach` chunks of (cx, cz).
std::vector<std::pair<int32_t, int32_t>> candidateAnchors(const RandomSpreadStructurePlacement& anchor,
                                                          int64_t seed, int32_t cx, int32_t cz,
                                                          int32_t reach) {
    const int32_t spacing = anchor.effectiveSpacing();
    std::vector<std::pair<int32_t, int32_t>> out;
    for (int32_t gx = floorDiv(cx - reach, spacing); gx <= floorDiv(cx + reach, spacing); ++gx) {
        for (int32_t gz = floorDiv(cz - reach, spacing); gz <= floorDiv(cz + reach, spacing); ++gz) {
            const auto a = anchor.getPotentialStructureChunk(seed, gx * spacing, gz * spacing);
            if (std::abs(a.first - cx) > reach || std::abs(a.second - cz) > reach) continue;
            if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
        }
    }
    return out;
}

// ── The anchor's own generation, cached ───────────────────────────────────

struct AnchorResult {
    bool valid = false;
    int rotation = 0;
    int32_t originY = 0;       // the start piece's bounding-box minY
};

AnchorResult anchorAt(const Layout& layout, GenerationContext& ctx, int32_t ax, int32_t az) {
    static std::mutex s_mutex;
    static std::map<std::tuple<int64_t, int32_t, int32_t>, AnchorResult> s_cache;
    const auto key = std::make_tuple(ctx.seed, ax, az);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_cache.find(key);
        if (it != s_cache.end()) return it->second;
    }
    AnchorResult result;
    const StructureInfo& info = StructureSets::structureByName(layout.anchorStructure);
    const auto& biomes = BiomeTags::resolve(info.biomesTag);
    GenerationContext anchorCtx(ctx.generator, ctx.randomState, ctx.biomeSource, ctx.sampler,
                                ctx.seed, ax, az, &biomes);
    StructureStartData start;
    if (Structures::generate(info, anchorCtx, 0, start) && start.isValid()) {
        result.valid = true;
        result.rotation = rotationOrdinal(start.pieces.front().rotation);
        result.originY = start.pieces.front().boundingBox.minY;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    // A world holds a handful of cities in any session; the cap only keeps
    // a pathological search (a /locate sweep) from growing it for ever.
    if (s_cache.size() > 4096) s_cache.clear();
    s_cache.emplace(key, result);
    return result;
}

// ── The road piece ────────────────────────────────────────────────────────

// Where the road may lay its stones: natural ground only. Never water or
// lava (a road that meets a lake drowns), never another structure's blocks
// (a lighthouse, a ruin placed earlier in the step).
bool isNaturalGround(const BlockState* state) {
    static const std::unordered_set<std::string> kGround = {
        "minecraft:sculk_loam", "minecraft:hush_moss", "minecraft:hushstone",
        "minecraft:sculk", "minecraft:calcite", "minecraft:dirt", "minecraft:grass_block",
        "minecraft:coarse_dirt", "minecraft:rooted_dirt", "minecraft:podzol",
        "minecraft:stone", "minecraft:deepslate", "minecraft:tuff", "minecraft:gravel",
        "minecraft:sand", "minecraft:red_sand", "minecraft:moss_block", "minecraft:mud",
        "minecraft:clay", "minecraft:snow_block",
    };
    return state != nullptr && kGround.count(state->getIdentifier()) != 0;
}

struct RoadBlocks {
    BlockState* tiles;
    BlockState* bricks;
    BlockState* cracked;
    BlockState* polished;
    BlockState* moss;
    BlockState* sculk;
    BlockState* loam;
    BlockState* slab;
    BlockState* wall;
    BlockState* lantern;
    BlockState* air;

    static const RoadBlocks& get() {
        static const RoadBlocks blocks = [] {
            auto spec = [](const char* s) { return TemplateEngine::parseBlockStateSpec(s); };
            RoadBlocks b{};
            b.tiles    = spec("minecraft:choirstone_tiles");
            b.bricks   = spec("minecraft:choirstone_bricks");
            b.cracked  = spec("minecraft:cracked_choirstone_bricks");
            b.polished = spec("minecraft:polished_choirstone");
            b.moss     = spec("minecraft:hush_moss");
            b.sculk    = spec("minecraft:sculk");
            b.loam     = spec("minecraft:sculk_loam");
            b.slab     = spec("minecraft:choirstone_brick_slab[type=bottom,waterlogged=false]");
            b.wall     = spec("minecraft:choirstone_brick_wall[east=none,north=none,south=none,up=true,"
                              "waterlogged=false,west=none]");
            b.lantern  = spec("minecraft:echo_lantern[hanging=false,waterlogged=false]");
            b.air      = spec("minecraft:air");
            return b;
        }();
        return blocks;
    }
};

/**
 * One stretch of a ruined road. A pure function of the world seed, the road
 * and the column: the road's line runs from `a` to `b` in the city's design
 * frame; `t` is the distance along it, `u` the offset across (positive to the
 * traveller's right leaving the city). The centre line meanders by
 * `wobble` * sin(t / 37 + phase) once past the first 60..140 blocks
 * (tools/gen_aurelith_outskirts.py mirrors the formula to place the
 * mile-markers beside it).
 */
class AurelithRoadBehavior : public StructurePieceBehavior {
public:
    AurelithRoadBehavior(RoadSpec spec, int32_t originX, int32_t originZ, int rotation)
        : m_spec(spec), m_originX(originX), m_originZ(originZ), m_rotation(rotation) {
        const double dx = m_spec.bx - m_spec.ax, dz = m_spec.bz - m_spec.az;
        m_length = std::max(1.0, std::hypot(dx, dz));
        m_dirX = dx / m_length;
        m_dirZ = dz / m_length;
        m_phase = static_cast<double>(m_spec.seed % 628u) / 100.0;
    }

    double centre(double t) const {
        if (m_spec.wobble <= 0.0) return 0.0;
        return m_spec.wobble * smoothstep(60.0, 140.0, t) * std::sin(t / 37.0 + m_phase);
    }
    double halfWidth(double t) const {
        if (t >= m_spec.apronLength) return m_spec.half;
        // The apron narrows smoothly into the road over its last 8 blocks.
        const double k = smoothstep(m_spec.apronLength - 8.0, m_spec.apronLength, t);
        return m_spec.apronHalf + (m_spec.half - m_spec.apronHalf) * k;
    }
    double maxHalf() const { return std::max(m_spec.half, m_spec.apronHalf) + m_spec.wobble + 2.0; }

    void postProcess(WorldGenLevel* level, ChunkGenerator* /*generator*/, WorldgenRandom& /*random*/,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& /*chunkPos*/,
                     const core::BlockPos& /*referencePos*/, StructurePieceData& self) override {
        const RoadBlocks& blocks = RoadBlocks::get();
        const uint64_t seed = static_cast<uint64_t>(level->getSeed()) ^ (static_cast<uint64_t>(m_spec.seed) << 17);
        const BoundingBox& box = self.boundingBox;
        const int32_t x0 = std::max(box.minX, chunkBB.minX), x1 = std::min(box.maxX, chunkBB.maxX);
        const int32_t z0 = std::max(box.minZ, chunkBB.minZ), z1 = std::min(box.maxZ, chunkBB.maxZ);
        auto put = [&](int32_t x, int32_t y, int32_t z, BlockState* state) {
            if (y < chunkBB.minY || y > chunkBB.maxY) return;
            level->setBlock(core::BlockPos(x, y, z), state, 2);
        };
        for (int32_t x = x0; x <= x1; ++x) {
            for (int32_t z = z0; z <= z1; ++z) {
                // World column -> design frame -> road frame.
                const auto d = rotateXZ(x - m_originX, z - m_originZ, (4 - m_rotation) & 3);
                const double px = d.first - m_spec.ax, pz = d.second - m_spec.az;
                const double t = px * m_dirX + pz * m_dirZ;
                if (t < m_spec.t0 || t >= m_spec.t1 || t < 0.0 || t > m_length) continue;
                // Right-hand normal of the direction (x east, z south).
                const double u = -px * m_dirZ + pz * m_dirX;
                const double du = u - centre(t);
                const double w = halfWidth(t);
                if (std::abs(du) > w + 1.5) continue;

                const int32_t top = level->getHeight(Heightmap::Types::WORLD_SURFACE_WG, x, z) - 1;
                const core::BlockPos topPos(x, top, z);
                if (!isNaturalGround(level->getBlockState(topPos))) continue;

                const double f = t / m_length;
                // Kept paving thins along the road, and the last eighth of it
                // gives out altogether: the road fades into the Hush.
                const double keep = (m_spec.keepA + (m_spec.keepB - m_spec.keepA) * std::pow(f, 1.2))
                                  * (1.0 - 0.85 * smoothstep(0.86, 1.0, f));
                const double h1 = unitHash(seed, x, z, 1);
                const double h2 = unitHash(seed, x, z, 2);
                const double h3 = unitHash(seed, x, z, 3);

                // Lamp posts, alternating sides, every lampEvery near the city.
                if (m_spec.lampEvery > 0.0 && t < m_spec.lampUntil) {
                    const double k = std::round((t - m_spec.lampEvery * 0.5) / m_spec.lampEvery);
                    const double tk = m_spec.lampEvery * 0.5 + k * m_spec.lampEvery;
                    const double side = (static_cast<int64_t>(k) & 1) ? 1.0 : -1.0;
                    const double postU = centre(tk) + side * (halfWidth(tk) + 1.0);
                    if (std::abs(t - tk) < 0.5 && std::abs(u - postU) < 0.5) {
                        // Stump, broken shaft, or (now and then, near the
                        // gates) the whole post with its lantern still lit.
                        const double hp = unitHash(seed, x, z, 7);
                        const int height = hp < 0.15 ? 0 : hp < 0.45 ? 1 : hp < 0.75 ? 2 : 3;
                        put(x, top, z, blocks.polished);
                        for (int i = 1; i <= height; ++i) put(x, top + i, z, blocks.wall);
                        if (height == 3 && unitHash(seed, x, z, 8) < 0.6 * (1.0 - f)) {
                            put(x, top + 4, z, blocks.lantern);
                        }
                        continue;
                    }
                }

                if (std::abs(du) > w) {
                    // The verge: kerb stones, and rubble thrown off the road.
                    if (h1 < 0.22 * keep) put(x, top, z, h2 < 0.5 ? blocks.polished : blocks.cracked);
                    else if (h1 < 0.22 * keep + 0.03) put(x, top + 1, z, h2 < 0.6 ? blocks.slab : blocks.cracked);
                    continue;
                }
                if (h1 > keep) {
                    // Lost paving: the ground shows; a loose stone now and then.
                    if (h2 < 0.04) put(x, top + 1, z, blocks.slab);
                    continue;
                }
                BlockState* stone = std::abs(du) >= w - 0.5 ? blocks.polished
                                  : std::abs(du) <= 0.75     ? blocks.tiles
                                                             : blocks.bricks;
                if (h2 < 0.08 + 0.22 * f) stone = blocks.cracked;
                else if (h2 > 0.93 - 0.20 * f) stone = blocks.moss;          // grown over
                if (h3 < 0.012) stone = blocks.sculk;
                // Sunk: the stone lies one lower in a shallow pothole.
                if (h3 > 0.985 - 0.05 * f && top - 1 >= chunkBB.minY) {
                    put(x, top, z, blocks.air);
                    put(x, top - 1, z, stone);
                    continue;
                }
                put(x, top, z, stone);
                // A shard of the parapet lying on the road.
                if (h3 > 0.40 && h3 < 0.40 + 0.015) put(x, top + 1, z, blocks.slab);
            }
        }
    }

private:
    RoadSpec m_spec;
    int32_t m_originX, m_originZ;     // world position of the design origin (the Heart)
    int m_rotation;
    double m_length = 1.0, m_dirX = 0.0, m_dirZ = -1.0, m_phase = 0.0;
};

} // namespace

// ── AnchoredStructurePlacement ────────────────────────────────────────────

AnchoredStructurePlacement::AnchoredStructurePlacement(int32_t salt, std::string anchorSet,
                                                       int32_t originX, int32_t originZ,
                                                       std::vector<std::pair<int32_t, int32_t>> slots)
    : StructurePlacement(0, 0, 0, FrequencyReductionMethod::DEFAULT, 1.0f, salt, std::nullopt),
      m_anchorSet(std::move(anchorSet)), m_originX(originX), m_originZ(originZ),
      m_slots(std::move(slots)) {
    for (const auto& [sx, sz] : m_slots) {
        for (int r = 0; r < 4; ++r) {
            const auto c = slotChunk(0, 0, r, m_originX, m_originZ, sx, sz);
            m_reachChunks = std::max({m_reachChunks, std::abs(c.first), std::abs(c.second)});
        }
    }
}

std::pair<int32_t, int32_t> AnchoredStructurePlacement::slotChunk(int32_t ax, int32_t az, int rotation,
                                                                  int32_t originX, int32_t originZ,
                                                                  int32_t slotX, int32_t slotZ) {
    const auto w = rotateXZ(slotX + originX, slotZ + originZ, rotation);
    return {ax + floorDiv(w.first, 16), az + floorDiv(w.second, 16)};
}

bool AnchoredStructurePlacement::isPlacementChunk(const ChunkGeneratorStructureState& state,
                                                  int32_t sourceX, int32_t sourceZ) const {
    const RandomSpreadStructurePlacement& anchor = anchorPlacement(m_anchorSet);
    for (const auto& a : candidateAnchors(anchor, state.getLevelSeed(), sourceX, sourceZ, m_reachChunks)) {
        for (int r = 0; r < 4; ++r) {
            for (const auto& [sx, sz] : m_slots) {
                const auto c = slotChunk(a.first, a.second, r, m_originX, m_originZ, sx, sz);
                if (c.first == sourceX && c.second == sourceZ) return true;
            }
        }
    }
    return false;
}

// ── The structure ─────────────────────────────────────────────────────────

namespace AurelithOutskirts {

bool isOutskirtsType(const std::string& type) {
    return type == "obeycraft:aurelith_outskirts";
}

bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out) {
    const std::shared_ptr<const Layout> layout = layoutFor(info);
    const RandomSpreadStructurePlacement& anchor = anchorPlacement(layout->anchorSet);

    int32_t reach = 0;
    for (const SlotSpec& s : layout->slots) {
        for (int r = 0; r < 4; ++r) {
            const auto c = AnchoredStructurePlacement::slotChunk(0, 0, r, layout->originX, layout->originZ, s.x, s.z);
            reach = std::max({reach, std::abs(c.first), std::abs(c.second)});
        }
    }

    for (const auto& a : candidateAnchors(anchor, ctx.seed, ctx.chunkX, ctx.chunkZ, reach)) {
        // Which slots could start in this chunk under any rotation — only
        // then is the anchor's (costly, cached) generation worth running.
        bool any = false;
        for (int r = 0; r < 4 && !any; ++r) {
            for (const SlotSpec& s : layout->slots) {
                const auto c = AnchoredStructurePlacement::slotChunk(a.first, a.second, r, layout->originX,
                                                                     layout->originZ, s.x, s.z);
                if (c.first == ctx.chunkX && c.second == ctx.chunkZ) { any = true; break; }
            }
        }
        if (!any) continue;

        const AnchorResult city = anchorAt(*layout, ctx, a.first, a.second);
        if (!city.valid) continue;
        const int rotation = city.rotation;
        // The anchor's reference point (the Heart) in the world.
        const auto ho = rotateXZ(layout->originX, layout->originZ, rotation);
        const int32_t originX = a.first * 16 + ho.first;
        const int32_t originZ = a.second * 16 + ho.second;
        const uint64_t citySeed = mix64(static_cast<uint64_t>(ctx.seed) ^ mix64(
            static_cast<uint64_t>(static_cast<uint32_t>(a.first)) << 32 ^ static_cast<uint32_t>(a.second)));

        size_t slotIndex = 0;
        for (const SlotSpec& s : layout->slots) {
            ++slotIndex;
            const auto c = AnchoredStructurePlacement::slotChunk(a.first, a.second, rotation, layout->originX,
                                                                 layout->originZ, s.x, s.z);
            if (c.first != ctx.chunkX || c.second != ctx.chunkZ) continue;

            // Roads: code pieces over the stretch's design rectangle.
            for (const RoadSpec& road : s.roads) {
                auto behavior = std::make_shared<AurelithRoadBehavior>(road, originX, originZ, rotation);
                const double len = std::max(1.0, std::hypot(road.bx - road.ax, road.bz - road.az));
                const double dirX = (road.bx - road.ax) / len, dirZ = (road.bz - road.az) / len;
                const double across = behavior->maxHalf();
                int32_t minX = INT32_MAX, minZ = INT32_MAX, maxX = INT32_MIN, maxZ = INT32_MIN;
                int32_t minY = INT32_MAX, maxY = INT32_MIN;
                for (const double t : {road.t0, road.t1}) {
                    for (const double u : {-across, across}) {
                        const double dx = road.ax + dirX * t - dirZ * u;
                        const double dz = road.az + dirZ * t + dirX * u;
                        const auto w = rotateXZ(static_cast<int32_t>(std::floor(dx)),
                                                static_cast<int32_t>(std::floor(dz)), rotation);
                        minX = std::min(minX, originX + w.first - 1);
                        maxX = std::max(maxX, originX + w.first + 1);
                        minZ = std::min(minZ, originZ + w.second - 1);
                        maxZ = std::max(maxZ, originZ + w.second + 1);
                    }
                }
                // The road drapes over the ground: its box spans the surface
                // heights sampled along the stretch (the box only decides
                // which chunks run the piece; the stones go on the real
                // surface).
                for (double t = road.t0; t <= road.t1 + 0.5; t += 8.0) {
                    const auto w = rotateXZ(static_cast<int32_t>(std::floor(road.ax + dirX * t)),
                                            static_cast<int32_t>(std::floor(road.az + dirZ * t)), rotation);
                    const int32_t h = ctx.generator->getBaseHeight(originX + w.first, originZ + w.second,
                                                                   Heightmap::Types::WORLD_SURFACE_WG,
                                                                   ctx.randomState);
                    minY = std::min(minY, h - 6);
                    maxY = std::max(maxY, h + 6);
                }
                StructurePieceData piece;
                piece.pieceType = "obeycraft:aurelith_road";
                piece.boundingBox = BoundingBox(minX, minY, minZ, maxX, maxY, maxZ);
                piece.rotation = rotationName(rotation);
                piece.detail = "road#" + std::to_string(road.seed) + "@" + std::to_string(static_cast<int>(road.t0));
                // A pool-element piece that is NOT rigid: the Beardifier
                // leaves the ground under it alone (the road follows it).
                piece.poolElement = true;
                piece.rigidProjection = false;
                piece.groundLevelDelta = 0;
                out.pieces.push_back(std::move(piece));
                out.behaviors.push_back(std::move(behavior));
            }

            // Templates: rigid, on the median surface of their footprint.
            size_t pieceIndex = 0;
            for (const PieceSpec& p : s.pieces) {
                ++pieceIndex;
                if (p.chance < 1.0 && unitHash(citySeed, static_cast<int64_t>(slotIndex),
                                               static_cast<int64_t>(pieceIndex), 11) >= p.chance) {
                    continue;
                }
                const int worldRotation = (rotation + p.rotation) & 3;
                const auto o = rotateXZ(p.originX, p.originZ, rotation);
                const int32_t ox = originX + o.first, oz = originZ + o.second;
                BoundingBox box = JigsawTemplates::elementBoundingBox(p.templateId, ox, 0, oz, worldRotation);
                const int32_t cx = (box.minX + box.maxX) / 2, cz = (box.minZ + box.maxZ) / 2;
                if (p.dry) {
                    const int32_t surface = ctx.generator->getBaseHeight(cx, cz, Heightmap::Types::WORLD_SURFACE_WG,
                                                                         ctx.randomState);
                    const int32_t floor = ctx.generator->getBaseHeight(cx, cz, Heightmap::Types::OCEAN_FLOOR_WG,
                                                                       ctx.randomState);
                    if (floor < surface) continue;   // standing water over the site
                }
                std::array<int32_t, 5> heights{};
                const std::array<std::pair<int32_t, int32_t>, 5> probes = {{
                    {cx, cz}, {box.minX + 1, box.minZ + 1}, {box.maxX - 1, box.minZ + 1},
                    {box.minX + 1, box.maxZ - 1}, {box.maxX - 1, box.maxZ - 1}}};
                for (size_t i = 0; i < probes.size(); ++i) {
                    heights[i] = ctx.generator->getBaseHeight(probes[i].first, probes[i].second,
                                                              Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState);
                }
                std::sort(heights.begin(), heights.end());
                // Template convention (as the city's): local y 0 is the
                // footing, y 1 the floor laid in the old surface, so the
                // origin sits two under the first free block.
                const int32_t oy = heights[2] - 2 + p.yOffset;
                box.move(0, oy, 0);

                PoolElement element;
                element.kind = PoolElementKind::SINGLE;
                element.location = p.templateId;
                element.processors = layout->processors;
                element.projection = "rigid";
                StructurePieceData piece;
                piece.pieceType = "minecraft:jigsaw";
                piece.boundingBox = box;
                piece.rotation = rotationName(worldRotation);
                piece.detail = "minecraft:single_pool_element:" + p.templateId + "#1";
                piece.poolElement = true;
                piece.rigidProjection = true;
                piece.groundLevelDelta = 1;
                out.pieces.push_back(std::move(piece));
                out.behaviors.push_back(PieceBehaviors::jigsawPiece(
                    element, core::BlockPos(ox, oy, oz), worldRotation, /*keepLiquids=*/true));
            }
        }
        if (!out.pieces.empty()) return true;
    }
    return false;
}

} // namespace AurelithOutskirts

} // namespace structure
} // namespace levelgen
} // namespace minecraft
