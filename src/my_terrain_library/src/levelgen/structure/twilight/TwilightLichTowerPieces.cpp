#include "levelgen/structure/twilight/TwilightLichTower.h"

#include "levelgen/structure/twilight/TwilightTemplatePieces.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/TemplateEngine.h"
#include "levelgen/structure/TwilightStructureData.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/feature/Feature.h"
#include "data/worldgen/features/TreeFeatures.h"
#include "math/Mth.h"
#include "nbt/AllTags.h"
#include "world/IChunk.h"
#include "world/level/block/Block.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "external/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — the postProcess half of lichtowerrevamp/*.java:
// LichTowerUtil.addDefaultProcessors + the per-piece processors (TrimProcessor,
// SpawnerProcessor, SoftReplaceProcessor, VerticalDecayProcessor,
// WoodMultiPaletteSwizzle), every piece's postProcess additions and
// handleDataMarker, LichYardBox / LichYardLights / LichYardGrave placement.
//
// Entities the mod adds during placement (paintings, the magic painting, the
// leashed zombies, the death-tome lectern mimic) are not placed: the engine's
// worldgen has no entity output. Their RNG draws are not consumed either
// (the painting draws depend on the painting registry and on the hang
// checks), so decoration rolls that follow them in the same piece pass may
// differ from the mod; the layout never does.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace lich_tower {

namespace {

namespace tt = twilight_template;
using core::BlockPos;
using core::Direction;
using world::level::block::Blocks;

void logOnce(const std::string& key, const std::string& message) {
    static std::mutex s_mutex;
    static std::set<std::string> s_logged;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_logged.insert(key).second) {
        std::cerr << "[TwilightLichTower] " << message << std::endl;
    }
}

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

BlockState* vanilla(const char* id) { return Blocks::getDefaultState(id); }

BlockState* twilight(const char* name) { return twilight_blocks::defaultState(name); }

BlockState* twilightWith(const char* name, const std::unordered_map<std::string, std::string>& props) {
    return twilight_blocks::state(name, props);
}

bool isId(const BlockState* state, const char* id) {
    return state != nullptr && state->getBlock() != nullptr && state->getBlock()->getIdentifier() == id;
}

bool inside(const BoundingBox& box, const BlockPos& pos) {
    return box.isInside(pos.getX(), pos.getY(), pos.getZ());
}

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
    if (parts.size() == 1) return parts;
    while (!parts.empty() && parts.back().empty()) parts.pop_back();
    return parts;
}

bool isNumeric(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

int nextIntBetweenInclusive(WorldgenRandom& random, int min, int max) {
    return random.nextInt(max - min + 1) + min;
}

int floorMod(int a, int b) {
    const int r = a % b;
    return r < 0 ? r + b : r;
}

// Math.round(float).
int roundFloat(float value) { return static_cast<int>(std::floor(value + 0.5f)); }

// Identifier.bySeparator(label, '.') / Identifier.parse(label) as a block.
BlockState* blockFromLabel(const std::string& label) {
    std::string id = label;
    const size_t dot = label.find('.');
    if (dot != std::string::npos) {
        id = label.substr(0, dot) + ":" + label.substr(dot + 1);
    } else if (label.find(':') == std::string::npos) {
        id = "minecraft:" + label;
    }
    if (startsWith(id, "twilightforest:")) return twilight_blocks::defaultState(id);
    BlockState* state = Blocks::getDefaultState(id);
    if (state == nullptr) state = twilight_blocks::defaultState(id);
    if (state == nullptr) logOnce("label:" + id, "lich tower marker block " + id + " is not registered");
    return state;
}

// ----------------------------------------------------------------------------
// Block families the pieces test.
// ----------------------------------------------------------------------------

// #twilightforest:banisters as placed in the engine: the resolved ids of the
// mod's banisters (their stand-ins while unported).
bool isBanister(const BlockState* state) {
    static const std::unordered_set<std::string> kIds = [] {
        static const char* const kNames[] = {
            "oak_banister", "spruce_banister", "birch_banister", "jungle_banister", "acacia_banister",
            "dark_oak_banister", "crimson_banister", "warped_banister", "vangrove_banister",
            "bamboo_banister", "cherry_banister", "pale_oak_banister", "twilight_oak_banister",
            "canopy_banister", "mangrove_banister", "dark_banister", "time_banister",
            "transformation_banister", "mining_banister", "sorting_banister"};
        std::unordered_set<std::string> ids;
        for (const char* name : kNames) {
            const std::string resolved = twilight_blocks::resolveName(std::string("twilightforest:") + name);
            if (!resolved.empty()) ids.insert(resolved);
        }
        return ids;
    }();
    return state != nullptr && state->getBlock() != nullptr && kIds.count(state->getBlock()->getIdentifier()) != 0;
}

// ----------------------------------------------------------------------------
// Processors
// ----------------------------------------------------------------------------

// LichTowerUtil.addDefaultProcessors after JigsawReplacement and the
// STRUCTURE_BLOCK ignore (both applied by the engine's placement flags).
void addDefaultProcessors(std::vector<tt::Processor>& chain, WorldGenLevel* level) {
    chain.push_back(tt::stoneBricksVariants());
    chain.push_back(tt::cobbleVariants());
    chain.push_back(tt::infestBlocks());
    chain.push_back(tt::updateMarking({"minecraft:birch_fence", "minecraft:polished_andesite_stairs",
                                       "minecraft:stone_brick_wall", "minecraft:mossy_stone_brick_wall",
                                       "minecraft:cobblestone_wall", "minecraft:mossy_cobblestone_wall",
                                       "twilightforest:wrought_iron_fence", "twilightforest:canopy_fence",
                                       "twilightforest:twisted_stone_pillar"},
                                      level));
}

// LichTowerBase.TrimProcessor: keep trim stairs off stone bricks the wings
// already placed.
tt::Processor trimProcessor(WorldGenLevel* level) {
    return [level](const BlockPos& pos, BlockState* state, const BlockPos&, BlockState*, const BlockPos&) -> BlockState* {
        if (isId(state, "minecraft:polished_andesite_stairs") && level != nullptr) {
            BlockState* at = level->getBlockState(pos);
            if (at != nullptr && blockpredicates::matchesBlockTagName(at, "minecraft:stone_bricks")) return nullptr;
        }
        return state;
    };
}

// WoodMultiPaletteSwizzle([twilight_oak -> canopy, canopy -> twilight_oak])
// over the data/twilightforest/twilight/wood_palettes palettes.
struct WoodPalette {
    std::map<std::string, std::string> shapes;   // shape -> block name
};

const WoodPalette& woodPalette(const std::string& name) {
    static std::mutex s_mutex;
    static std::map<std::string, WoodPalette> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(name);
    if (it != s_cache.end()) return it->second;
    WoodPalette palette;
    const nlohmann::json& json = twilight_data::file("twilight/wood_palettes/" + name + ".json");
    for (auto entry = json.begin(); entry != json.end(); ++entry) {
        if (entry.value().is_string()) palette.shapes[entry.key()] = entry.value().get<std::string>();
    }
    return s_cache.emplace(name, std::move(palette)).first->second;
}

tt::Processor woodSwizzle() {
    // Each pair (from, to): a block of `from` becomes the same shape of `to`
    // (planks -> default state, the rest keep their properties).
    struct Swap { tt::Block* block; std::string shape; std::string target; };
    std::vector<std::vector<Swap>> pairs;
    const std::pair<const char*, const char*> kPairs[2] = {{"twilight_oak", "canopy"}, {"canopy", "twilight_oak"}};
    for (const auto& [from, to] : kPairs) {
        const WoodPalette& source = woodPalette(from);
        const WoodPalette& target = woodPalette(to);
        std::vector<Swap> swaps;
        for (const auto& [shape, name] : source.shapes) {
            tt::Block* block = tt::realTwilightBlock(name);
            auto t = target.shapes.find(shape);
            if (block != nullptr && t != target.shapes.end()) swaps.push_back({block, shape, t->second});
        }
        pairs.push_back(std::move(swaps));
    }
    return [pairs](const BlockPos&, BlockState* state, const BlockPos&, BlockState*, const BlockPos&) -> BlockState* {
        if (state == nullptr) return state;
        for (const auto& swaps : pairs) {
            for (const Swap& swap : swaps) {
                if (state->getBlock() != swap.block) continue;
                if (swap.shape == "planks") return twilight_blocks::defaultState(swap.target);
                return tt::transferAllStateKeys(state, swap.target);
            }
        }
        return state;
    };
}

// LichTowerUtil.stairDecayProcessors[decayLevel] (VerticalDecayProcessor over
// the twilight oak / canopy slabs and banisters).
tt::Processor stairDecay(int decayLevel) {
    static const float kChances[8] = {0.025f, 0.05f, 0.075f, 0.1f, 0.125f, 0.15f, 0.175f, 0.2f};
    // Java clamps with min(decayLevel, length), which would index past the
    // array at level 8; tower depths never reach it.
    const int index = std::min(decayLevel, 7);
    return tt::verticalDecay({"twilightforest:twilight_oak_slab", "twilightforest:canopy_slab",
                              "twilightforest:twilight_oak_banister", "twilightforest:canopy_banister"},
                             kChances[index]);
}

// ----------------------------------------------------------------------------
// SpawnerProcessor — writes the spawner block entity after placement (the
// processor only rewrites the template nbt, which TemplateEngine processors
// cannot touch). Raw palette names decide, so the lich boss spawner stand-in
// (also minecraft:spawner) stays unconfigured.
// ----------------------------------------------------------------------------
struct SpawnerEntity {
    const char* id;          // engine entity id
    int weight;
    float width;             // EntityType width (rescale)
};

struct SpawnerConfig {
    int range = 0;           // 0 = keep the template's
    float entityWidthMax = 0.0f;
    std::vector<SpawnerEntity> entities;
};

const SpawnerConfig& roomSpawners() {
    // LichTowerUtil.roomSpawners: compile(2, 0.8f, {spider 1, cave_spider 1,
    // swarm_spider 1, hedge_spider 1, skeleton 4, zombie 4}).
    static const SpawnerConfig kConfig{2, 0.8f, {
        {"minecraft:spider", 1, 1.4f}, {"minecraft:cave_spider", 1, 0.7f},
        {"minecraft:swarm_spider", 1, 0.8f}, {"minecraft:hedge_spider", 1, 1.4f},
        {"minecraft:skeleton", 4, 0.6f}, {"minecraft:zombie", 4, 0.6f}}};
    return kConfig;
}

const SpawnerConfig& centralSpawners() {
    // LichTowerUtil.centralSpawners: compile(4, {skeleton 2, zombie 1,
    // swarm_spider 1}).
    static const SpawnerConfig kConfig{4, 0.0f, {
        {"minecraft:skeleton", 2, 0.6f}, {"minecraft:zombie", 1, 0.6f}, {"minecraft:swarm_spider", 1, 0.8f}}};
    return kConfig;
}

std::string spawnDataSnbt(const std::string& entityId, float scale) {
    // SpawnData(entity, CustomSpawnRules(block [0,7], sky [0,15] = default,
    // omitted), no equipment); keys in canonical order.
    std::string entity = "{";
    if (scale != 1.0f) {
        // SpawnerProcessor's rescale to fit entity_width_max (Java appends to
        // the attributes list it expects to exist).
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(scale));
        entity += "attributes:[{base:" + std::string(buffer) + "f,id:\"generic.scale\"}],";
    }
    entity += "id:\"" + entityId + "\"}";
    return "{custom_spawn_rules:{block_light_limit:[0,7]},entity:" + entity + "}";
}

std::string spawnerPayload(const nbt::CompoundTag* templateNbt, int range, bool rangeOverride,
                           const std::string& spawnData, int delay) {
    auto shortOf = [templateNbt](const char* key, int16_t fallback) -> int {
        return templateNbt != nullptr ? templateNbt->getShortOr(key, fallback) : fallback;
    };
    const int spawnRange = rangeOverride ? range : shortOf("SpawnRange", 4);
    return "{Delay:" + std::to_string(delay) + "s,MaxNearbyEntities:" + std::to_string(shortOf("MaxNearbyEntities", 6))
         + "s,MaxSpawnDelay:" + std::to_string(shortOf("MaxSpawnDelay", 800))
         + "s,MinSpawnDelay:" + std::to_string(shortOf("MinSpawnDelay", 200))
         + "s,RequiredPlayerRange:" + std::to_string(shortOf("RequiredPlayerRange", 16))
         + "s,SpawnCount:" + std::to_string(shortOf("SpawnCount", 4))
         + "s,SpawnData:" + spawnData + ",SpawnPotentials:[],SpawnRange:" + std::to_string(spawnRange)
         + "s,components:{},id:\"minecraft:mob_spawner\"}";
}

void applySpawnerProcessor(const SpawnerConfig& config, WorldGenLevel* level, const std::string& templateId,
                           const BlockPos& position, const TemplatePlaceSettings& settings,
                           const BoundingBox& chunkBB) {
    int totalWeight = 0;
    for (const SpawnerEntity& e : config.entities) totalWeight += e.weight;
    for (const char* name : {"minecraft:spawner", "twilightforest:sinister_spawner"}) {
        for (const tt::FilteredBlock& info : tt::filterBlocks(templateId, position, settings, name, true, &chunkBB)) {
            if (info.nbt == nullptr) continue;
            BlockState* placed = level->getBlockState(info.pos);
            if (!isId(placed, "minecraft:spawner")) continue;
            // Delay = round(MinSpawnDelay * startDelayFactor 0.25).
            const int minDelay = info.nbt->getShortOr("MinSpawnDelay", 0);
            const int delay = roundFloat(static_cast<float>(minDelay) * 0.25f);
            // SpawnData is always rerolled (getList("SpawnData") of a compound
            // is empty): positional random at the block.
            LegacyRandomSource random(Mth::getSeed(info.pos.getX(), info.pos.getY(), info.pos.getZ()));
            int selection = random.nextInt(totalWeight);
            const SpawnerEntity* chosen = &config.entities.front();
            for (const SpawnerEntity& e : config.entities) {
                selection -= e.weight;
                if (selection < 0) {
                    chosen = &e;
                    break;
                }
            }
            float scale = 1.0f;
            if (config.entityWidthMax > 0.0f && chosen->width != 0.0f && !(chosen->width < config.entityWidthMax)) {
                scale = config.entityWidthMax / chosen->width;
            }
            tt::setBlockEntity(level, info.pos,
                               spawnerPayload(info.nbt, config.range, config.range > 0,
                                              spawnDataSnbt(chosen->id, scale), delay));
        }
    }
}

// ----------------------------------------------------------------------------
// org.joml.SimplexNoise.noise(float, float, float) (Gustavson's float
// simplex; JOML 1.10.8 as shipped with 26.x).
// ----------------------------------------------------------------------------
namespace simplex {

const int8_t kGrad3[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                              {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};

const uint8_t kP[256] = {
    151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142,
    8, 99, 37, 240, 21, 10, 23, 190, 6, 148, 247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117,
    35, 11, 32, 57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175, 74, 165, 71,
    134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41,
    55, 46, 245, 40, 244, 102, 143, 54, 65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89,
    18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64, 52, 217, 226,
    250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17, 182,
    189, 28, 42, 223, 183, 170, 213, 119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43,
    172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104, 218, 246, 97,
    228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107,
    49, 192, 214, 31, 181, 199, 106, 157, 184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138,
    236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180};

int perm(int i) { return kP[i & 255]; }
int permMod12(int i) { return perm(i) % 12; }

int fastfloor(float x) {
    const int xi = static_cast<int>(x);
    return x < static_cast<float>(xi) ? xi - 1 : xi;
}

float dot(const int8_t* g, float x, float y, float z) {
    return static_cast<float>(g[0]) * x + static_cast<float>(g[1]) * y + static_cast<float>(g[2]) * z;
}

float noise(float xin, float yin, float zin) {
    const float F3 = 0.33333334f;
    const float G3 = 0.16666667f;
    const float s = (xin + yin + zin) * F3;
    const int i = fastfloor(xin + s);
    const int j = fastfloor(yin + s);
    const int k = fastfloor(zin + s);
    const float t = static_cast<float>(i + j + k) * G3;
    const float X0 = static_cast<float>(i) - t;
    const float Y0 = static_cast<float>(j) - t;
    const float Z0 = static_cast<float>(k) - t;
    const float x0 = xin - X0;
    const float y0 = yin - Y0;
    const float z0 = zin - Z0;
    int i1, j1, k1, i2, j2, k2;
    if (x0 >= y0) {
        if (y0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0) { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0) { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }
    const float x1 = x0 - static_cast<float>(i1) + G3;
    const float y1 = y0 - static_cast<float>(j1) + G3;
    const float z1 = z0 - static_cast<float>(k1) + G3;
    const float x2 = x0 - static_cast<float>(i2) + F3;
    const float y2 = y0 - static_cast<float>(j2) + F3;
    const float z2 = z0 - static_cast<float>(k2) + F3;
    const float x3 = x0 - 1.0f + 0.5f;
    const float y3 = y0 - 1.0f + 0.5f;
    const float z3 = z0 - 1.0f + 0.5f;
    const int ii = i & 255;
    const int jj = j & 255;
    const int kk = k & 255;
    const int gi0 = permMod12(ii + perm(jj + perm(kk)));
    const int gi1 = permMod12(ii + i1 + perm(jj + j1 + perm(kk + k1)));
    const int gi2 = permMod12(ii + i2 + perm(jj + j2 + perm(kk + k2)));
    const int gi3 = permMod12(ii + 1 + perm(jj + 1 + perm(kk + 1)));
    float n0, n1, n2, n3;
    float t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
    if (t0 < 0.0f) n0 = 0.0f; else { t0 *= t0; n0 = t0 * t0 * dot(kGrad3[gi0], x0, y0, z0); }
    float t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
    if (t1 < 0.0f) n1 = 0.0f; else { t1 *= t1; n1 = t1 * t1 * dot(kGrad3[gi1], x1, y1, z1); }
    float t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
    if (t2 < 0.0f) n2 = 0.0f; else { t2 *= t2; n2 = t2 * t2 * dot(kGrad3[gi2], x2, y2, z2); }
    float t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
    if (t3 < 0.0f) n3 = 0.0f; else { t3 *= t3; n3 = t3 * t3 * dot(kGrad3[gi3], x3, y3, z3); }
    return 32.0f * (n0 + n1 + n2 + n3);
}

} // namespace simplex

// ============================================================================
// The template pieces.
// ============================================================================
class LichTemplateBehavior final : public tt::JigsawPieceBehavior {
public:
    LichTemplateBehavior(const LichPiece& piece, ProcessorFactory processors, bool keepLiquids,
                         const SpawnerConfig* spawners)
        : JigsawPieceBehavior(piece.jigsaw, std::move(processors), keepLiquids), m_piece(piece),
          m_spawners(spawners) {
        m_piece.shelfPositions.clear();
    }

    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const BlockPos& referencePos, StructurePieceData& self) override {
        (void)chunkPos;
        // Pre-placement steps.
        if (m_piece.kind == LichKind::MagicGallery) removeGalleryBanisters(level, chunkBB);
        if (m_piece.kind == LichKind::YardGrave) fillUnderGrave(level, chunkBB);

        // TwilightTemplateStructurePiece.customPostProcess (+ spawners).
        const TemplatePlaceSettings settings = makeSettings(level);
        self.boundingBox = tt::templateBoundingBox(m_config.templateId, settings, m_config.templatePosition);
        // (customPostProcess resets the box to the template's, dropping
        // LichTowerBase's +30 exactly as the mod does after generation.)
        if (TemplateEngine::placeInWorld(level, m_config.templateId, m_config.templatePosition, referencePos,
                                         settings, random, chunkBB)) {
            if (m_spawners != nullptr) {
                applySpawnerProcessor(*m_spawners, level, m_config.templateId, m_config.templatePosition,
                                      settings, chunkBB);
            }
            for (const TemplateEngine::DataMarker& marker :
                 TemplateEngine::dataMarkers(m_config.templateId, m_config.templatePosition, settings, chunkBB)) {
                handleDataMarker(marker.metadata, marker.pos, level, random, chunkBB, generator,
                                 settings.rotation);
            }
        }

        // Post-placement steps.
        switch (m_piece.kind) {
            case LichKind::WingBridge: bridgeBanisters(level, chunkBB); break;
            case LichKind::WingRoom: roomExtras(level, chunkBB); break;
            case LichKind::FoyerDecor: foyerDecoration(level, random, chunkBB); break;
            case LichKind::BossRoom: bossRoomCandles(level, random, chunkBB); break;
            case LichKind::PerimeterFence: escapeLadder(level, chunkBB); break;
            default: break;
        }
    }

protected:
    void handleDataMarker(const std::string& label, const BlockPos& pos, WorldGenLevel* level,
                          WorldgenRandom& random, const BoundingBox& chunkBB, ChunkGenerator* generator,
                          int rotation) override {
        (void)rotation;
        switch (m_piece.kind) {
            case LichKind::Foyer: foyerMarker(label, pos, level, random); break;
            case LichKind::Base: baseMarker(label, pos, level, random); break;
            case LichKind::WingRoom: roomMarker(label, pos, level, random); break;
            case LichKind::RoomDecor: decorMarker(label, pos, level, random, generator); break;
            case LichKind::MagicGallery:
                // removeBlock, then the magic painting / paintings (entities).
                tt::removeBlock(level, pos);
                break;
            case LichKind::Segment:
            case LichKind::BossRoom:
                // LichBossRoom.placePainting: painting entities only.
                break;
            default:
                break;
        }
        (void)chunkBB;
    }

private:
    int pieceRotation() const { return m_config.rotation; }

    const tt::JigsawRecord& source() const { return m_piece.jigsaw.sourceJigsaw(); }

    // ------------------------------------------------------------------
    // LichTowerFoyer.handleDataMarker — the vestibule chest.
    // ------------------------------------------------------------------
    static int dataRotationOf(const std::vector<std::string>& directionSplit) {
        if (directionSplit.size() == 1) return tt::ROT_CW180;
        const std::optional<Direction> dir = tt::directionFromName(directionSplit[1]);
        return tt::relativeRotation(Direction::NORTH, dir.value_or(Direction::SOUTH));
    }

    void foyerMarker(const std::string& label, const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random) {
        const std::vector<std::string> directionSplit = javaSplit(label, '@');
        if (directionSplit.empty()) return;
        const int dataRotation = dataRotationOf(directionSplit);
        if (!m_piece.putChest) return;
        const std::string toMatch = m_piece.chestSide ? "chest_a" : "chest_b";
        if (toMatch != directionSplit[0]) return;
        tt::removeBlock(level, pos);
        const int stateRotation = tt::rotated(pieceRotation(), dataRotation);
        level->setBlock(pos, tt::rotateBlockState(vanilla("minecraft:chest"), stateRotation), 2);
        const int64_t seed = random.nextLong();
        tt::setBlockEntity(level, pos, tt::lootContainerPayload("minecraft:chest",
                                                                "twilightforest:chests/tower_room", seed));
        if (BlockState* planks = twilight("twilightforest:canopy_planks")) level->setBlock(pos.below(), planks, 2);
    }

    // ------------------------------------------------------------------
    // LichTowerBase.handleDataMarker — "candle:N".
    // ------------------------------------------------------------------
    void baseMarker(const std::string& label, const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random) {
        const std::vector<std::string> split = javaSplit(label, ':');
        if (split.size() != 2 || split[0] != "candle" || !isNumeric(split[1])) return;
        tt::removeBlock(level, pos);
        const bool majorCandle = std::stoi(split[1]) == 2;
        if (!majorCandle && random.nextInt(3) != 0) return;
        const int candleCount = majorCandle ? 3 : 1 + random.nextInt(2);
        BlockState* candle = tt::withProperty(tt::withProperty(vanilla("minecraft:candle"), "lit", "true"),
                                              "candles", std::to_string(candleCount));
        level->setBlock(pos, candle, 3);
    }

    // ------------------------------------------------------------------
    // LichTowerRoomDecor.handleDataMarker.
    // ------------------------------------------------------------------
    static BlockState* randomPlant(WorldgenRandom& random) {
        // TFStructureHelper.randomPlant(nextInt(6)).
        switch (random.nextInt(6)) {
            case 1: return vanilla("minecraft:potted_spruce_sapling");
            case 2: return vanilla("minecraft:potted_birch_sapling");
            case 3: return vanilla("minecraft:potted_jungle_sapling");
            case 4: return vanilla("minecraft:potted_red_mushroom");
            case 5: return vanilla("minecraft:potted_brown_mushroom");
            default: return vanilla("minecraft:potted_oak_sapling");
        }
    }

    void decorMarker(const std::string& label, const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random,
                     ChunkGenerator* generator) {
        level->setBlock(pos, Blocks::AIR->defaultBlockState(), 2);
        if (label == "sapling") {
            level->setBlock(pos, randomPlant(random), 2);
        } else if (label == "tree") {
            static std::once_flag s_trees;
            std::call_once(s_trees, [] { ::minecraft::data::worldgen::features::TreeFeatures::bootstrap(); });
            using ::minecraft::data::worldgen::features::TreeFeatures;
            // TFStructureHelper.randomTree: OAK, SPRUCE, BIRCH,
            // JUNGLE_TREE_NO_VINE (the engine registers the vined jungle tree
            // only; it stands in).
            ConfiguredFeature* tree = nullptr;
            switch (random.nextInt(4)) {
                case 1: tree = TreeFeatures::SPRUCE; break;
                case 2: tree = TreeFeatures::BIRCH; break;
                case 3: tree = TreeFeatures::JUNGLE_TREE; break;
                default: tree = TreeFeatures::OAK; break;
            }
            if (tree == nullptr || !tree->place(level, generator, random, pos)) {
                level->setBlock(pos, randomPlant(random), 2);
            }
        }
    }

    // ------------------------------------------------------------------
    // LichTowerWingRoom.handleDataMarker / handleDataParams.
    // ------------------------------------------------------------------
    bool canHangBlock(const BlockPos& pos) const {
        const int dX = pos.getX() - m_piece.jigsaw.boundingBox.minX;
        const int dZ = pos.getZ() - m_piece.jigsaw.boundingBox.minZ;
        const auto& allowed = m_piece.allowedCeilingPlacements;
        return dX >= 0 && dX < static_cast<int>(allowed.size()) && allowed[static_cast<size_t>(dX)] == dZ;
    }

    static int parseRange(const std::string& label, WorldgenRandom& random, int defaultMin, int defaultMax) {
        const std::vector<std::string> params = javaSplit(label, '-');
        if (params.size() == 1 && isNumeric(params[0])) return std::stoi(params[0]);
        if (params.size() == 2 && isNumeric(params[0]) && isNumeric(params[1])) {
            return nextIntBetweenInclusive(random, std::stoi(params[0]), std::stoi(params[1]));
        }
        return nextIntBetweenInclusive(random, defaultMin, defaultMax);
    }

    static int candleRanged(const std::string& label, WorldgenRandom& random) {
        return parseRange(label, random, 1, 3);
    }

    static int headRotation(const std::string& label, WorldgenRandom& random) {
        const std::vector<std::string> params = javaSplit(label, '+');
        if (params.size() == 1 && isNumeric(params[0])) return std::stoi(params[0]);
        if (params.size() == 2 && isNumeric(params[0]) && isNumeric(params[1])) {
            const int src = std::stoi(params[0]);
            const int extra = std::stoi(params[1]);
            return floorMod(nextIntBetweenInclusive(random, src, src + extra), 16);
        }
        return nextIntBetweenInclusive(random, 0, 15);
    }

    std::optional<BlockPos> danglingBlock(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random,
                                          BlockState* binding, const std::string& parameters) const {
        const std::vector<std::string> ropeChance = javaSplit(parameters, '%');
        if (ropeChance.empty() || !canHangBlock(pos)) return std::nullopt;
        const int ropeLength = parseRange(ropeChance.back(), random, 1, 2);
        BlockPos result = pos;
        if (ropeLength > 0) {
            if (binding != nullptr) {
                for (int dy = 0; dy < ropeLength; ++dy) level->setBlock(pos.below(dy), binding, 2);
            }
            result = pos.below(ropeLength);
        }
        return result;
    }

    void roomMarker(const std::string& label, const BlockPos& markerPos, WorldGenLevel* level,
                    WorldgenRandom& random) {
        BlockPos pos = markerPos;
        const std::vector<std::string> modifiedLabel = javaSplit(label, '>');
        const std::string variety = modifiedLabel.size() == 2 ? modifiedLabel[1] : label;
        if (modifiedLabel.size() == 2) {
            if (startsWith(modifiedLabel[0], "rope")) {
                auto next = danglingBlock(pos, level, random, twilight("twilightforest:rope"),
                                          modifiedLabel[0].substr(4));
                if (!next) return;
                pos = *next;
            }
            if (startsWith(modifiedLabel[0], "chain")) {
                auto next = danglingBlock(pos, level, random, vanilla("minecraft:iron_chain"),
                                          modifiedLabel[0].substr(5));
                if (!next) return;
                pos = *next;
            } else if (modifiedLabel[0] == "pedestal") {
                if (BlockState* pillar = twilight("twilightforest:twisted_stone_pillar")) level->setBlock(pos, pillar, 2);
                pos = pos.above();
            } else if (modifiedLabel[0] == "below") {
                // The 9x9 winding_ways room.
                pos = pos.below();
            }
        }
        const std::vector<std::string> directionSplit = javaSplit(variety, '@');
        if (directionSplit.empty()) return;
        const int dataRotation = dataRotationOf(directionSplit);
        const std::vector<std::string> permutationSplit = javaSplit(directionSplit[0], '|');
        if (permutationSplit.empty()) return;
        const std::string chosenLabel =
            permutationSplit[static_cast<size_t>(random.nextInt(static_cast<int>(permutationSplit.size())))];
        const std::vector<std::string> parameters = javaSplit(chosenLabel, ':');
        if (parameters.empty()) return;
        tt::removeBlock(level, pos);
        handleDataParams(pos, level, random, parameters, dataRotation);
    }

    void setIfPresent(WorldGenLevel* level, const BlockPos& pos, BlockState* state, int flags = 2) {
        if (state != nullptr) level->setBlock(pos, state, flags);
    }

    void handleDataParams(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random,
                          const std::vector<std::string>& p, int dataRotation) {
        const std::string& kind = p[0];
        const int stateRotation = tt::rotated(pieceRotation(), dataRotation);
        if (kind == "air" || kind == "empty") return;   // block already replaced
        if (kind == "bookshelf") { setIfPresent(level, pos, vanilla("minecraft:bookshelf")); return; }
        if (kind == "canopy_shelf" || kind == "canopy_bookshelf") {
            setIfPresent(level, pos, twilight("twilightforest:canopy_bookshelf"));
            return;
        }
        if (kind == "stone_brick_slab") { setIfPresent(level, pos, vanilla("minecraft:stone_brick_slab")); return; }
        if (kind == "lava") { setIfPresent(level, pos, vanilla("minecraft:lava")); return; }
        if (kind == "water") { setIfPresent(level, pos, vanilla("minecraft:water")); return; }
        if (kind == "firefly_jar") { setIfPresent(level, pos, twilight("twilightforest:firefly_jar")); return; }
        if (kind == "terrorcotta_arcs") { setIfPresent(level, pos, twilight("twilightforest:terrorcotta_arcs")); return; }
        if (kind == "mason_jar") { putMasonJar(pos, level, random, p); return; }
        if (kind == "canopy_slab") { setIfPresent(level, pos, twilight("twilightforest:canopy_slab")); return; }
        if (kind == "canopy_stairs") { setIfPresent(level, pos, twilight("twilightforest:canopy_stairs")); return; }
        if (kind == "creeper_head") { putHead(pos, level, random, p, "minecraft:creeper_head", dataRotation); return; }
        if (kind == "skeleton_skull") { putHead(pos, level, random, p, "minecraft:skeleton_skull", dataRotation); return; }
        if (kind == "wither_skull") { putHead(pos, level, random, p, "minecraft:wither_skeleton_skull", dataRotation); return; }
        if (kind == "zombie_head") { putHead(pos, level, random, p, "minecraft:zombie_head", dataRotation); return; }
        if (kind == "creeper_candle") { putHeadCandles(pos, level, random, p, "twilightforest:creeper_skull_candle", dataRotation); return; }
        if (kind == "skeleton_candle") { putHeadCandles(pos, level, random, p, "twilightforest:skeleton_skull_candle", dataRotation); return; }
        if (kind == "wither_candle") { putHeadCandles(pos, level, random, p, "twilightforest:wither_skeleton_skull_candle", dataRotation); return; }
        if (kind == "zombie_candle") { putHeadCandles(pos, level, random, p, "twilightforest:zombie_skull_candle", dataRotation); return; }
        if (kind == "spawner") { putSpawner(pos, level, random, p, false); return; }
        if (kind == "sinister_spawner") { putSpawner(pos, level, random, p, true); return; }
        if (kind == "brewing_stand") { putBrewingStand(pos, level, random); return; }
        if (kind == "lectern") { putTrappableLectern(pos, level, dataRotation, random.nextBoolean()); return; }
        if (kind == "chiseled_canopy_shelf") { putTrappableBookshelf(pos, level, random, dataRotation); return; }
        if (kind == "chest") { putChest(pos, level, random, p, dataRotation, "minecraft:chest"); return; }
        if (kind == "trapped_chest") { putChest(pos, level, random, p, dataRotation, "minecraft:trapped_chest"); return; }
        if (kind == "candle" || kind == "candles") { putCandles(p, random, level, pos, "minecraft:candle"); return; }
        static const char* const kColors[] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink",
                                              "gray", "light_gray", "cyan", "purple", "blue", "brown", "green",
                                              "red", "black"};
        for (const char* color : kColors) {
            if (kind == std::string(color) + "_candle") {
                putCandles(p, random, level, pos, ("minecraft:" + kind).c_str());
                return;
            }
        }
        if (kind == "water_cauldron") { putWaterCauldron(p, random, level, pos); return; }
        if (kind == "zombie_trap") { putZombieTrap(random, level, pos); return; }
        if (kind == "wrought_iron_post") {
            setIfPresent(level, pos, twilightWith("twilightforest:wrought_iron_fence", {{"post", "post"}}));
            markPostprocessing(level, pos);
            return;
        }
        if (kind == "empty_lectern") {
            setIfPresent(level, pos, tt::rotateBlockState(vanilla("minecraft:lectern"), stateRotation));
            setLecternPayload(level, pos, false);
            return;
        }
        if (kind == "candled_lectern") {
            if (random.nextInt(4) != 0) {
                putCandles(p, random, level, pos.above(), "minecraft:candle");
            } else {
                putHeadCandles(pos.above(), level, random, p, "twilightforest:skeleton_skull_candle", dataRotation);
            }
            BlockState* lectern = tt::rotateBlockState(vanilla("minecraft:lectern"), stateRotation);
            setIfPresent(level, pos, lectern);
            setLecternPayload(level, pos, false);
            return;
        }
        // Default: the label is a block id ("ns.path" or a vanilla path).
        BlockState* state = blockFromLabel(kind);
        if (state != nullptr && !state->isAir()) {
            level->setBlock(pos, tt::rotateBlockState(state, stateRotation), 2);
        }
    }

    static void markPostprocessing(WorldGenLevel* level, const BlockPos& pos) {
        if (::world::IChunk* chunk = level->getChunk(pos.getX() >> 4, pos.getZ() >> 4)) {
            chunk->markPosForPostprocessing(pos);
        }
    }

    void canopySlabAbove(WorldGenLevel* level, const BlockPos& pos) {
        // A canopy bookshelf above a container becomes a top canopy slab.
        BlockState* above = level->getBlockState(pos.above());
        BlockState* shelf = twilight("twilightforest:canopy_bookshelf");
        if (above != nullptr && shelf != nullptr && above->getBlock() == shelf->getBlock()) {
            setIfPresent(level, pos.above(), twilightWith("twilightforest:canopy_slab", {{"type", "top"}}));
        }
    }

    void putMasonJar(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random,
                     const std::vector<std::string>& p) {
        setIfPresent(level, pos, twilight("twilightforest:mason_jar"));
        if (p.size() >= 2) {
            // MasonJarBlockEntity.fillFromLootTable(table, random.nextLong())
            // and the item rotation draw. The jar's contents are not written:
            // the engine has no mason jar block entity.
            (void)random.nextLong();
            if (p.size() == 3) (void)headRotation(p[2], random);
        }
        canopySlabAbove(level, pos);
    }

    void putBrewingStand(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random) {
        BlockState* stand = vanilla("minecraft:brewing_stand");
        stand = tt::withProperty(tt::withProperty(tt::withProperty(stand, "has_bottle_0", "true"),
                                                  "has_bottle_1", "true"), "has_bottle_2", "true");
        setIfPresent(level, pos, stand);
        const bool splash = random.nextInt(4) == 0;
        const char* potion;
        switch (random.nextInt(7)) {
            case 6: potion = "minecraft:strong_healing"; break;
            case 4: case 5: potion = "minecraft:regeneration"; break;
            case 1: case 2: case 3: potion = "minecraft:healing"; break;
            default: potion = "minecraft:water"; break;
        }
        const int blaze = nextIntBetweenInclusive(random, 1, 5);
        const int fuel = nextIntBetweenInclusive(random, 10, 20);
        const std::string item = std::string("components:{\"minecraft:potion_contents\":{potion:\"") + potion
                               + "\"}},count:1,id:\"" + (splash ? "minecraft:splash_potion" : "minecraft:potion") + "\"";
        std::string items = "[";
        for (int slot = 0; slot < 3; ++slot) {
            items += "{Slot:" + std::to_string(slot) + "b," + item + "},";
        }
        items += "{Slot:4b,count:" + std::to_string(blaze) + ",id:\"minecraft:blaze_powder\"}]";
        tt::setBlockEntity(level, pos, "{BrewTime:0s,Fuel:" + std::to_string(fuel) + "b,Items:" + items
                                           + ",components:{},id:\"minecraft:brewing_stand\"}");
    }

    void setLecternPayload(WorldGenLevel* level, const BlockPos& pos, bool withBook) {
        if (withBook) {
            tt::setBlockEntity(level, pos, "{Book:{count:1,id:\"minecraft:writable_book\"},Page:0,components:{},id:\"minecraft:lectern\"}");
        } else {
            tt::setBlockEntity(level, pos, "{components:{},id:\"minecraft:lectern\"}");
        }
    }

    void putTrappableLectern(const BlockPos& pos, WorldGenLevel* level, int dataRotation, bool putMimic) {
        const int stateRotation = tt::rotated(pieceRotation(), dataRotation);
        BlockState* lectern = tt::withProperty(vanilla("minecraft:lectern"), "has_book", putMimic ? "false" : "true");
        setIfPresent(level, pos, tt::rotateBlockState(lectern, stateRotation));
        // The mimic is a Death Tome entity (not in the engine); otherwise the
        // lectern holds a writable book.
        setLecternPayload(level, pos, !putMimic);
    }

    void putTrappableBookshelf(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random, int dataRotation) {
        const bool isHostile = random.nextInt(12) == 0;
        const int stateRotation = tt::rotated(pieceRotation(), dataRotation);
        std::unordered_map<std::string, std::string> props = {{"spawner", isHostile ? "true" : "false"}};
        std::vector<int> filledSlots;
        for (int index = 0; index < 6; ++index) {
            if (random.nextInt(3) != 0) {
                filledSlots.push_back(index);
                props["slot_" + std::to_string(index) + "_occupied"] = "true";
            }
        }
        BlockState* shelf = twilightWith("twilightforest:chiseled_canopy_bookshelf", props);
        setIfPresent(level, pos, tt::rotateBlockState(shelf, stateRotation));
        std::string items = "[";
        bool first = true;
        for (int index : filledSlots) {
            // Spawner shelves never hold enchanted books; otherwise 1/16 is
            // EnchantmentHelper.enchantItem(level 1..40) - its enchantment
            // draws need the enchantment registry, so the book stays plain.
            if (!isHostile && random.nextInt(16) == 0) {
                (void)nextIntBetweenInclusive(random, 1, 40);
                logOnce("enchanted-shelf-book", "lich tower chiseled shelf enchanted books are placed as plain books");
            }
            if (!first) items += ",";
            items += "{Slot:" + std::to_string(index) + "b,count:1,id:\"minecraft:book\"}";
            first = false;
        }
        items += "]";
        if (shelf != nullptr && shelf->getBlock()->getIdentifier() == "minecraft:chiseled_bookshelf") {
            tt::setBlockEntity(level, pos, "{Items:" + items + ",components:{},id:\"minecraft:chiseled_bookshelf\",last_interacted_slot:-1}");
        }
        // A hostile shelf's Death Tome spawner (setEntityId draws nothing: its
        // potentials are empty) has no engine counterpart.
    }

    void putSkullPayload(WorldGenLevel* level, const BlockPos& pos) {
        BlockState* placed = level->getBlockState(pos);
        if (placed == nullptr || placed->getBlock() == nullptr) return;
        const std::string& id = placed->getBlock()->getIdentifier();
        if (id.find("_skull") != std::string::npos || id.find("_head") != std::string::npos) {
            tt::setBlockEntity(level, pos, "{components:{},id:\"minecraft:skull\"}");
        }
    }

    void putHead(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random, const std::vector<std::string>& p,
                 const char* headBlock, int dataRotation) {
        const int rotation = p.size() >= 2 ? headRotation(p[1], random) : nextIntBetweenInclusive(random, 0, 15);
        const int stateRotation = tt::rotated(pieceRotation(), tt::rotated(dataRotation, tt::ROT_CW180));
        BlockState* head = tt::withProperty(vanilla(headBlock), "rotation", std::to_string(rotation));
        setIfPresent(level, pos, tt::rotateBlockState(head, stateRotation));
        putSkullPayload(level, pos);
    }

    void putHeadCandles(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random,
                        const std::vector<std::string>& p, const char* candledHead, int dataRotation) {
        const int amount = std::min(4, p.size() >= 2 ? candleRanged(p[1], random) : nextIntBetweenInclusive(random, 1, 3));
        if (amount <= 0) return;
        const int rotation = p.size() >= 3 ? headRotation(p[2], random) : nextIntBetweenInclusive(random, 0, 15);
        const int stateRotation = tt::rotated(pieceRotation(), tt::rotated(dataRotation, tt::ROT_CW180));
        BlockState* head = twilightWith(candledHead, {{"lighting", "normal"},
                                                      {"candles", std::to_string(amount)},
                                                      {"rotation", std::to_string(rotation)}});
        setIfPresent(level, pos, tt::rotateBlockState(head, stateRotation));
        putSkullPayload(level, pos);
    }

    void putCandles(const std::vector<std::string>& p, WorldgenRandom& random, WorldGenLevel* level,
                    const BlockPos& pos, const char* candle) {
        const int amount = std::min(4, p.size() == 2 ? candleRanged(p[1], random) : nextIntBetweenInclusive(random, 1, 3));
        if (amount <= 0) return;
        BlockState* candles = tt::withProperty(tt::withProperty(vanilla(candle), "lit", "true"),
                                               "candles", std::to_string(amount));
        setIfPresent(level, pos, candles);
    }

    void putWaterCauldron(const std::vector<std::string>& p, WorldgenRandom& random, WorldGenLevel* level,
                          const BlockPos& pos) {
        const int amount = std::min(3, p.size() == 2 ? parseRange(p[1], random, 1, 3) : nextIntBetweenInclusive(random, 1, 3));
        BlockState* cauldron = amount <= 0
            ? vanilla("minecraft:cauldron")
            : tt::withProperty(vanilla("minecraft:water_cauldron"), "level", std::to_string(amount));
        setIfPresent(level, pos, cauldron);
    }

    void putZombieTrap(WorldgenRandom& random, WorldGenLevel* level, const BlockPos& pos) {
        BlockState* above = level->getBlockState(pos.above());
        const char* post = (above != nullptr && above->isAir()) ? "capped" : "post";
        setIfPresent(level, pos, twilightWith("twilightforest:wrought_iron_fence", {{"post", post}}));
        markPostprocessing(level, pos);
        // getRandomDirectionInsideChunk: Plane.HORIZONTAL.shuffledCopy, drop
        // the directions leaving the chunk, pick one. The leashed zombie
        // itself is an entity (not placed).
        std::vector<Direction> directions = {Direction::NORTH, Direction::EAST, Direction::SOUTH, Direction::WEST};
        for (size_t i = directions.size(); i > 1; --i) {
            const size_t swapTo = static_cast<size_t>(random.nextInt(static_cast<int>(i)));
            std::swap(directions[i - 1], directions[swapTo]);
        }
        const int xInChunk = pos.getX() & 15;
        const int zInChunk = pos.getZ() & 15;
        auto drop = [&directions](Direction d) {
            directions.erase(std::remove(directions.begin(), directions.end(), d), directions.end());
        };
        if (xInChunk == 0) drop(Direction::WEST);
        if (zInChunk == 0) drop(Direction::NORTH);
        if (xInChunk == 15) drop(Direction::EAST);
        if (zInChunk == 15) drop(Direction::SOUTH);
        if (!directions.empty()) (void)random.nextInt(static_cast<int>(directions.size()));
    }

    void putChest(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random, const std::vector<std::string>& p,
                  int dataRotation, const char* chestBlock) {
        const int stateRotation = tt::rotated(pieceRotation(), dataRotation);
        setIfPresent(level, pos, tt::rotateBlockState(vanilla(chestBlock), stateRotation));
        const std::string beId = chestBlock;
        if (p.size() == 2) {
            std::string lootTable;
            if (p[1] == "room") lootTable = "twilightforest:chests/tower_room";
            else if (p[1] == "library") lootTable = "twilightforest:chests/tower_library";
            else if (p[1] == "potion") lootTable = "twilightforest:chests/tower_potion";
            else if (p[1] == "enchanting") lootTable = "twilightforest:chests/tower_enchanting";
            else {
                const size_t dot = p[1].find('.');
                lootTable = dot != std::string::npos ? p[1].substr(0, dot) + ":" + p[1].substr(dot + 1)
                                                     : "minecraft:" + p[1];
            }
            const int64_t seed = random.nextLong();
            tt::setBlockEntity(level, pos, tt::lootContainerPayload(beId, lootTable, seed));
        } else {
            tt::setBlockEntity(level, pos, "{Items:[],components:{},id:\"" + beId + "\"}");
        }
        canopySlabAbove(level, pos);
    }

    // putSpawner / putSinisterSpawner: configureBaseSpawner.
    static const char* defaultRandomMob(WorldgenRandom& random) {
        switch (random.nextInt(10)) {
            case 7: case 8: case 9: return "minecraft:skeleton";
            case 6: return "minecraft:spider";
            case 5: return "minecraft:cave_spider";
            case 4: return "minecraft:hedge_spider";
            case 3: return "minecraft:swarm_spider";
            default: return "minecraft:zombie";
        }
    }

    static std::string mobFromLabel(const std::string& label) {
        if (label == "hedge_spider") return "minecraft:hedge_spider";
        if (label == "swarm_spider") return "minecraft:swarm_spider";
        // EntityType.byString(label).orElse(ZOMBIE): vanilla ids the lich
        // tower templates name.
        static const std::set<std::string> kVanilla = {"zombie", "skeleton", "spider", "cave_spider", "creeper",
                                                       "witch", "enderman", "husk", "stray", "silverfish"};
        std::string id = label;
        if (startsWith(id, "minecraft:")) id = id.substr(10);
        if (kVanilla.count(id) != 0) return "minecraft:" + id;
        return "minecraft:zombie";
    }

    void putSpawner(const BlockPos& pos, WorldGenLevel* level, WorldgenRandom& random, const std::vector<std::string>& p,
                    bool sinister) {
        setIfPresent(level, pos, sinister ? twilight("twilightforest:sinister_spawner") : vanilla("minecraft:spawner"));
        // pickRandomMob.
        std::string mob;
        if (p.size() >= 2) {
            const std::vector<std::string> monsters = javaSplit(p[1], ',');
            if (monsters.empty()) {
                mob = defaultRandomMob(random);
            } else {
                mob = mobFromLabel(monsters[static_cast<size_t>(random.nextInt(static_cast<int>(monsters.size())))]);
            }
        } else {
            mob = defaultRandomMob(random);
        }
        int spawnRange = 4;
        if (sinister) {
            if (p.size() >= 3 && isNumeric(p[2])) spawnRange = std::clamp(std::stoi(p[2]), 1, 16);
            // entityScanRange (p[3] or spawnRange) is a sinister-spawner field
            // the vanilla spawner payload cannot carry.
        } else if (p.size() == 3 && isNumeric(p[2])) {
            spawnRange = std::clamp(std::stoi(p[2]), 1, 16);
        }
        BlockState* placed = level->getBlockState(pos);
        if (isId(placed, "minecraft:spawner")) {
            tt::setBlockEntity(level, pos, "{Delay:20s,MaxNearbyEntities:6s,MaxSpawnDelay:800s,MinSpawnDelay:200s,"
                                           "RequiredPlayerRange:16s,SpawnCount:4s,SpawnData:"
                                           + spawnDataSnbt(mob, 1.0f) + ",SpawnPotentials:[],SpawnRange:"
                                           + std::to_string(spawnRange) + "s,components:{},id:\"minecraft:mob_spawner\"}");
        }
    }

    // ------------------------------------------------------------------
    // Post-placement additions.
    // ------------------------------------------------------------------
    void removeIfBanister(WorldGenLevel* level, const BlockPos& pos, const BoundingBox& chunkBB) {
        if (inside(chunkBB, pos) && isBanister(level->getBlockState(pos))) tt::removeBlock(level, pos);
    }

    // LichTowerWingBridge.postProcess.
    void bridgeBanisters(WorldGenLevel* level, const BoundingBox& chunkBB) {
        if (!m_piece.fromCentral) return;
        const BlockPos sourcePos = m_piece.jigsaw.sourcePosition();
        const Direction front = source().orientation.front;
        const BlockPos leftPos = sourcePos.relative(tt::clockWise(front));
        const BlockPos rightPos = sourcePos.relative(tt::counterClockWise(front));
        removeIfBanister(level, leftPos, chunkBB);
        removeIfBanister(level, leftPos.above(), chunkBB);
        removeIfBanister(level, rightPos, chunkBB);
        removeIfBanister(level, rightPos.below(), chunkBB);
    }

    // LichTowerMagicGallery.removeBanisters (before placement).
    void removeGalleryBanisters(WorldGenLevel* level, const BoundingBox& chunkBB) {
        const BlockPos sourcePos = m_piece.jigsaw.sourcePosition();
        const Direction front = source().orientation.front;
        const BlockPos leftPos = sourcePos.relative(tt::clockWise(front));
        const Direction ccw = tt::counterClockWise(front);
        // A 2-wide entrance shifts the right side.
        const int spanAlong = tt::span(m_piece.jigsaw.boundingBox, core::getAxis(ccw));
        const int evenShift = (spanAlong + 1) % 2;
        const BlockPos rightPos = sourcePos.relative(ccw, 1 + evenShift);
        removeIfBanister(level, leftPos, chunkBB);
        removeIfBanister(level, leftPos.above(), chunkBB);
        removeIfBanister(level, rightPos, chunkBB);
        removeIfBanister(level, rightPos.below(), chunkBB);
        if (evenShift == 1) removeIfBanister(level, sourcePos.relative(ccw, 1).below(), chunkBB);
    }

    // LichTowerWingRoom.postProcess: ladders and ground corners.
    void roomExtras(WorldGenLevel* level, const BoundingBox& chunkBB) {
        const BoundingBox& box = m_piece.jigsaw.boundingBox;
        const BlockPos templatePos = m_piece.jigsaw.templatePosition();
        BlockState* ladder = vanilla("minecraft:ladder");
        if (source().orientation.front == Direction::DOWN) {
            const BlockPos placeAt = templatePos.offset(source().pos);
            if (inside(chunkBB, placeAt)) {
                BlockState* ladderBlock = tt::withProperty(ladder, "facing", core::getName(source().orientation.top));
                level->setBlock(placeAt.below(), ladderBlock, 2);
                level->setBlock(placeAt, ladderBlock, 2);
                level->setBlock(placeAt.above(), ladderBlock, 2);
                for (int dy = 2; dy <= 5; ++dy) level->setBlock(placeAt.above(dy), Blocks::AIR->defaultBlockState(), 2);
            }
        }
        if (m_piece.ladderIndex >= 0) {
            const tt::JigsawRecord& ladderJigsaw =
                m_piece.jigsaw.spareJigsaws()[static_cast<size_t>(m_piece.ladderIndex)];
            const BlockPos startPos = templatePos.offset(ladderJigsaw.pos);
            if (inside(chunkBB, startPos)) {
                const BlockPos endPos = startPos.above(box.getYSpan() - ladderJigsaw.pos.getY() - 1);
                BlockState* ladderBlock = tt::withProperty(ladder, "facing", core::getName(ladderJigsaw.orientation.top));
                for (int y = startPos.getY(); y <= endPos.getY() - 1; ++y) {
                    level->setBlock(BlockPos(startPos.getX(), y, startPos.getZ()), ladderBlock, 2);
                }
                level->setBlock(endPos, vanilla("minecraft:cobblestone"), 2);
            }
        }
        if (m_piece.generateGround) {
            fillCorner(level, BlockPos(box.minX, box.minY, box.minZ), chunkBB);
            fillCorner(level, BlockPos(box.maxX, box.minY, box.minZ), chunkBB);
            fillCorner(level, BlockPos(box.maxX, box.minY, box.maxZ), chunkBB);
            fillCorner(level, BlockPos(box.minX, box.minY, box.maxZ), chunkBB);
        }
    }

    static void fillCorner(WorldGenLevel* level, const BlockPos& pos, const BoundingBox& chunkBB) {
        if (inside(chunkBB, pos)) {
            level->setBlock(pos, vanilla("minecraft:stone_bricks"), 3);
            level->setBlock(pos.above(), vanilla("minecraft:stone_bricks"), 3);
        }
    }

    // LichTowerFoyerDecor.postProcess.
    void foyerDecoration(WorldGenLevel* level, WorldgenRandom& random, const BoundingBox& chunkBB) {
        const BlockPos placePos = m_piece.jigsaw.sourcePosition();
        if (!inside(chunkBB, placePos)) return;
        const int rotation = pieceRotation();
        random.setSeed(static_cast<int64_t>(static_cast<uint64_t>(placePos.asLong())
                                            + static_cast<uint64_t>(static_cast<int64_t>(placePos.getY()))));
        const int noChest = isId(level->getBlockState(placePos.above()), "minecraft:chiseled_stone_bricks") ? -1 : 0;
        switch (random.nextInt(5) + noChest) {
            case 4: {
                const int chestRotation = tt::rotated(rotation, tt::ROT_CW180);
                level->setBlock(placePos, tt::rotateBlockState(vanilla("minecraft:chest"), chestRotation), 3);
                const int64_t seed = random.nextLong();
                tt::setBlockEntity(level, placePos, tt::lootContainerPayload("minecraft:chest",
                                                                             "twilightforest:chests/tower_foyer", seed));
                BlockState* gap = tt::withProperty(vanilla("minecraft:stone_brick_stairs"), "half", "top");
                level->setBlock(placePos.above(), tt::rotateBlockState(gap, chestRotation), 3);
                break;
            }
            case 3: {
                // Candelabra: rotated, dim, all three candles (its candle data
                // lives in a block entity the engine does not model).
                std::unordered_map<std::string, std::string> props = {
                    {"lighting", "dim"}, {"has_candle_1", "true"}, {"has_candle_2", "true"}, {"has_candle_3", "true"}};
                BlockState* candelabra = tt::rotateBlockState(twilightWith("twilightforest:candelabra", {}), rotation);
                if (candelabra != nullptr) {
                    for (const auto& [key, value] : props) candelabra = tt::withProperty(candelabra, key, value);
                    level->setBlock(placePos, candelabra, 3);
                }
                break;
            }
            default: {
                BlockState* decor = nullptr;
                switch (random.nextInt(5)) {
                    case 3: {
                        const int candles = nextIntBetweenInclusive(random, 1, 3);
                        const int rot = nextIntBetweenInclusive(random, 7, 9);
                        decor = twilightWith("twilightforest:skeleton_skull_candle",
                                             {{"lighting", "normal"}, {"candles", std::to_string(candles)},
                                              {"rotation", std::to_string(rot)}});
                        break;
                    }
                    case 1:
                    case 2: {
                        const int rot = nextIntBetweenInclusive(random, 7, 9);
                        decor = tt::withProperty(vanilla("minecraft:skeleton_skull"), "rotation", std::to_string(rot));
                        break;
                    }
                    default: {
                        const int candles = nextIntBetweenInclusive(random, 1, 3);
                        decor = tt::withProperty(tt::withProperty(vanilla("minecraft:candle"), "candles",
                                                                  std::to_string(candles)), "lit", "true");
                        break;
                    }
                }
                if (decor != nullptr) {
                    level->setBlock(placePos, tt::rotateBlockState(decor, rotation), 3);
                    putSkullPayload(level, placePos);
                }
                break;
            }
        }
        const BlockPos behind = placePos.relative(core::getOpposite(source().orientation.front));
        markPostprocessing(level, behind);
    }

    // LichBossRoom.postProcess: twenty candles around the arena.
    void bossRoomCandles(WorldGenLevel* level, WorldgenRandom& random, const BoundingBox& chunkBB) {
        const BlockPos center = m_piece.jigsaw.sourcePosition().above(9);
        random.setSeed(center.asLong());
        BlockState* candle = tt::withProperty(vanilla("minecraft:candle"), "lit", "true");
        float angle = 0.0f;
        for (int i = 0; i < 20; ++i) {
            angle += random.nextFloat();   // walks around all four sides
            const float range = Mth::clampedLerp(random.nextFloat(), 7.0f, 11.0f);
            const int x = roundFloat(Mth::cos(static_cast<double>(angle)) * range);
            const int z = roundFloat(Mth::sin(static_cast<double>(angle)) * range);
            const int y = random.nextInt(3);
            const BlockPos placeAt = center.offset(x, y, z);
            if (inside(chunkBB, placeAt)) {
                BlockState* at = level->getBlockState(placeAt);
                BlockState* below = level->getBlockState(placeAt.below());
                if (at != nullptr && at->isAir() && below != nullptr && below->isAir()) {
                    level->setBlock(placeAt, candle, 2);
                }
            }
        }
    }

    // LichPerimeterFence.generateEscapeLadder (generateBoundZombie places an
    // entity: not ported).
    void escapeLadder(WorldGenLevel* level, const BoundingBox& chunkBB) {
        const Direction ladderDirection = core::getOpposite(source().orientation.top);
        const BlockPos ladderColumnPos = m_piece.jigsaw.sourcePosition().relative(ladderDirection);
        if (!inside(chunkBB, ladderColumnPos) || m_piece.jigsaw.spareJigsaws().size() != 1
            || tt::isVertical(ladderDirection)) {
            return;
        }
        BlockState* ladder = tt::withProperty(vanilla("minecraft:ladder"), "facing", core::getName(ladderDirection));
        for (int dy = 1; dy <= 4; ++dy) level->setBlock(ladderColumnPos.above(dy), ladder, 3);
    }

    // LichYardGrave.postProcess: dirt under the grave down to the ocean
    // floor, before the template.
    void fillUnderGrave(WorldGenLevel* level, const BoundingBox& chunkBB) {
        const Direction front = source().orientation.front;
        const BoundingBox fillUnder = tt::safeRetract(tt::inflatedBy(m_piece.jigsaw.boundingBox, -1), front, -1);
        const std::optional<BoundingBox> applicable = tt::intersection(fillUnder, chunkBB);
        if (!applicable) return;
        const int yUnder = applicable->minY - 1;
        BlockState* dirt = vanilla("minecraft:dirt");
        for (int x = applicable->minX; x <= applicable->maxX; ++x) {
            for (int z = applicable->minZ; z <= applicable->maxZ; ++z) {
                const int bottom = level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG, x, z);
                for (int y = yUnder; y >= bottom; --y) level->setBlock(BlockPos(x, y, z), dirt, 3);
            }
        }
    }

    LichPiece m_piece;
    const SpawnerConfig* m_spawners;
};

// ============================================================================
// LichYardBox.postProcess — paths / motley dirt from simplex noise.
// ============================================================================
class YardBoxBehavior final : public StructurePieceBehavior {
public:
    explicit YardBoxBehavior(const LichPiece& piece)
        : m_box(piece.plainBox), m_feather(piece.edgeFeatheringRange), m_direction(piece.direction),
          m_dirtMotley(piece.doDirtMotley), m_scale(piece.scale), m_offset(piece.offset) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos&, const BlockPos&, StructurePieceData&) override {
        const std::optional<BoundingBox> boxIntersection = tt::intersection(m_box, chunkBB);
        if (!boxIntersection || m_scale == 0.0f) return;
        const BoundingBox fenceBounds = generateFence()
            ? tt::safeRetract(m_box, core::getOpposite(m_direction), 4) : m_box;
        for (int z = boxIntersection->minZ; z <= boxIntersection->maxZ; ++z) {
            for (int x = boxIntersection->minX; x <= boxIntersection->maxX; ++x) {
                processPos(level, random, x, z, fenceBounds);
            }
        }
    }

private:
    bool generateFence() const { return !m_dirtMotley; }

    BlockState* pickDirt(int x, int y, int z, WorldgenRandom& random) const {
        const float scale = m_scale * 2.5f;
        const float randF = random.nextFloat();
        const float noise = randF < 0.25f
            ? (randF * 4.0f)
            : simplex::noise(static_cast<float>(x) * scale, static_cast<float>(y) * scale + 1024.0f,
                             static_cast<float>(z) * scale) * 0.5f + 0.5f;
        if (noise > 0.6f) return vanilla("minecraft:coarse_dirt");
        if (noise > 0.4f) return vanilla("minecraft:dirt");
        return vanilla("minecraft:rooted_dirt");
    }

    void processPos(WorldGenLevel* level, WorldgenRandom& random, int x, int z, const BoundingBox& fenceBounds) const {
        const int y = level->getHeight(Heightmap::Types::MOTION_BLOCKING_NO_LEAVES, x, z) - 1;
        const BlockPos placeAt(x, y, z);
        // #minecraft:substrate_overworld (#dirt, #mud, #moss_blocks,
        // #grass_blocks): the engine's #dirt carries all of them.
        BlockState* ground = level->getBlockState(placeAt);
        if (ground == nullptr || !blockpredicates::matchesBlockTagName(ground, "minecraft:substrate_overworld")) return;
        const int xBorderDist = std::min(x - m_box.minX, m_box.maxX - x);
        const int zBorderDist = std::min(z - m_box.minZ, m_box.maxZ - z);
        const float borderDist = static_cast<float>(std::min(xBorderDist, zBorderDist)) + m_offset;
        const float featherLevel = borderDist > m_feather ? 1.0f : std::clamp(borderDist / m_feather, 0.0f, 1.0f);
        const float noise = simplex::noise(static_cast<float>(x) * m_scale, static_cast<float>(y) * m_scale,
                                           static_cast<float>(z) * m_scale) * 0.5f - 0.5f;
        const float featheredNoise = noise + featherLevel;
        if (featheredNoise < 0.0f) {
            if (generateFence() && fenceBounds.intersects(x, z, x, z)) {
                const float fenceNoise = simplex::noise(static_cast<float>(x) * 0.15f,
                                                        static_cast<float>(y) * 0.15f - 1024.0f,
                                                        static_cast<float>(z) * 0.15f) * 0.5f;
                if (std::abs(fenceNoise) > 0.15f) {
                    const int noiseRounded = roundFloat(fenceNoise + 0.5f);
                    const bool onEdge = core::getAxis(m_direction) == core::Axis::Z
                        ? (x == m_box.minX + noiseRounded || x == m_box.maxX - noiseRounded)
                        : (z == m_box.minZ + noiseRounded || z == m_box.maxZ - noiseRounded);
                    if (onEdge) {
                        const BlockPos fenceAt = placeAt.above();
                        level->setBlock(fenceAt, vanilla("minecraft:spruce_fence"), 3);
                        if (::world::IChunk* chunk = level->getChunk(fenceAt.getX() >> 4, fenceAt.getZ() >> 4)) {
                            chunk->markPosForPostprocessing(fenceAt);
                        }
                    }
                }
            }
            return;
        }
        BlockState* state = m_dirtMotley ? pickDirt(x, y, z, random) : vanilla("minecraft:dirt_path");
        level->setBlock(placeAt, state, 3);
        // Remove the plants on top (a rare dead bush on the motley dirt).
        if (m_dirtMotley && random.nextFloat() < 0.0125f) {
            level->setBlock(placeAt.above(), vanilla("minecraft:dead_bush"), 3);
        } else {
            level->setBlock(placeAt.above(), Blocks::AIR->defaultBlockState(), 3);
        }
        level->setBlock(placeAt.above(2), Blocks::AIR->defaultBlockState(), 3);
    }

    BoundingBox m_box;
    float m_feather;
    Direction m_direction;
    bool m_dirtMotley;
    float m_scale;
    float m_offset;
};

// ============================================================================
// LichYardLights.postProcess — fence-post candles along the first path.
// ============================================================================
class YardLightsBehavior final : public StructurePieceBehavior {
public:
    explicit YardLightsBehavior(const LichPiece& piece) : m_box(piece.plainBox), m_axis(piece.placeAxis) {}

    void postProcess(WorldGenLevel* level, ChunkGenerator*, WorldgenRandom& random, const BoundingBox& chunkBB,
                     const ::world::ChunkPos&, const BlockPos&, StructurePieceData&) override {
        const std::optional<BoundingBox> boxIntersection = tt::intersection(m_box, tt::inflatedBy(chunkBB, -1));
        if (!boxIntersection) return;
        BlockState* fence = twilight("twilightforest:wrought_iron_fence");
        BlockState* candle = tt::withProperty(vanilla("minecraft:candle"), "lit", "true");
        auto isPlainAir = [level](const BlockPos& p) {
            BlockState* s = level->getBlockState(p);
            return s != nullptr && s->getBlock() == Blocks::AIR;   // not cave air
        };
        for (int z = boxIntersection->minZ; z <= boxIntersection->maxZ; ++z) {
            for (int x = boxIntersection->minX; x <= boxIntersection->maxX; ++x) {
                const int y = level->getHeight(Heightmap::Types::WORLD_SURFACE_WG, x, z);
                const BlockPos placeAt(x, y, z);
                if (isPlainAir(placeAt) && random.nextFloat() <= 0.125f && isPlainAir(placeAt.north())
                    && isPlainAir(placeAt.south()) && isPlainAir(placeAt.east()) && isPlainAir(placeAt.west())) {
                    const bool nearEdge = m_axis == core::Axis::Z
                        ? std::min(x - m_box.minX, m_box.maxX - x) < 3
                        : std::min(z - m_box.minZ, m_box.maxZ - z) < 3;
                    if (nearEdge) {
                        if (fence != nullptr) level->setBlock(placeAt, fence, 3);
                        level->setBlock(placeAt.above(), candle, 3);
                    }
                }
            }
        }
    }

private:
    BoundingBox m_box;
    core::Axis m_axis;
};

// UtilityPiece.postProcess: nothing.
class UtilityBehavior final : public StructurePieceBehavior {
public:
    void postProcess(WorldGenLevel*, ChunkGenerator*, WorldgenRandom&, const BoundingBox&,
                     const ::world::ChunkPos&, const BlockPos&, StructurePieceData&) override {}
};

} // namespace

std::shared_ptr<StructurePieceBehavior> makeBehavior(const LichPiece& piece) {
    using Factory = tt::TemplatePieceBehavior::ProcessorFactory;
    switch (piece.kind) {
        case LichKind::YardBox: return std::make_shared<YardBoxBehavior>(piece);
        case LichKind::YardLights: return std::make_shared<YardLightsBehavior>(piece);
        case LichKind::Utility: return std::make_shared<UtilityBehavior>();
        default: break;
    }
    Factory processors;
    bool keepLiquids = true;
    const SpawnerConfig* spawners = nullptr;
    switch (piece.kind) {
        case LichKind::Base:
            processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) {
                chain.push_back(trimProcessor(level));
                addDefaultProcessors(chain, level);
            };
            break;
        case LichKind::Segment: {
            // LichTowerSegment.stairDecay: level ceil((depth - 3) * 0.5).
            const int decayLevel = static_cast<int>(std::ceil((piece.genDepth() - 3) * 0.5));
            processors = [decayLevel](std::vector<tt::Processor>& chain, WorldGenLevel* level) {
                addDefaultProcessors(chain, level);
                if (decayLevel >= 0) chain.push_back(stairDecay(decayLevel));
            };
            break;
        }
        case LichKind::SpawnerBridge: {
            spawners = &centralSpawners();
            const bool inverted = piece.invertedPalette;
            processors = [inverted](std::vector<tt::Processor>& chain, WorldGenLevel* level) {
                addDefaultProcessors(chain, level);
                if (inverted) chain.push_back(woodSwizzle());
            };
            break;
        }
        case LichKind::WingRoof:
        case LichKind::WingBeard:
            processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) {
                chain.push_back(tt::softReplace(level));
                addDefaultProcessors(chain, level);
            };
            break;
        case LichKind::WingRoom:
            keepLiquids = false;   // LiquidSettings.IGNORE_WATERLOGGING
            spawners = &roomSpawners();
            processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) { addDefaultProcessors(chain, level); };
            break;
        case LichKind::RoomDecor:
        case LichKind::MagicGallery:
            spawners = &roomSpawners();
            processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) { addDefaultProcessors(chain, level); };
            break;
        case LichKind::PerimeterFence:
        case LichKind::YardGrave:
            // JigsawReplacementProcessor + STRUCTURE_BLOCK only.
            break;
        default:
            processors = [](std::vector<tt::Processor>& chain, WorldGenLevel* level) { addDefaultProcessors(chain, level); };
            break;
    }
    return std::make_shared<LichTemplateBehavior>(piece, std::move(processors), keepLiquids, spawners);
}

} // namespace lich_tower
} // namespace structure
} // namespace levelgen
} // namespace minecraft
