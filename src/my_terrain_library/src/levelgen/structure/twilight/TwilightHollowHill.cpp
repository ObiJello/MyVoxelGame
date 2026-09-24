#include "levelgen/structure/twilight/TwilightHollowHill.h"

#include "levelgen/structure/twilight/TwilightPieceBase.h"
#include "levelgen/structure/TwilightStructures.h"
#include "levelgen/structure/TwilightStructureData.h"
#include "data/worldgen/features/TwilightSpikes.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/WorldgenRandom.h"
#include "math/Mth.h"
#include "core/BlockPos.h"
#include "core/Direction.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Twilight Forest 4.9 hollow hills:
//   world/components/structures/type/HollowHillStructure.java (getFirstPiece,
//       getStructureTerraformer)
//   world/components/structures/HollowHillComponent.java (the one piece)
//   world/components/structures/StructureSpeleothemConfig.java,
//   world/components/speleothem/*.java, util/iterators/RectangleLatticeIterator
//       .java + ZippedIterator.java (the speleothem lattice)
//   world/components/chunkgenerators/HollowHillFunction.java,
//       FocusedDensityFunction.java (the terraformer)

namespace minecraft {
namespace levelgen {
namespace structure {
namespace twilight_pieces {

using world::level::block::Blocks;
using data::worldgen::features::twilight::Stalactite;

namespace {

// Mth.PI / Mth.TWO_PI / Mth.SQRT_OF_TWO (float constants).
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;
const float kSqrtOfTwo = static_cast<float>(std::sqrt(2.0));

void logWarning(const std::string& message) {
    fprintf(stderr, "[TwilightHollowHill] %s\n", message.c_str());
}

std::string lowercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "twilightforest:path" -> "path"; other namespaces are not in the TF data
// root and yield "".
std::string twilightPath(const std::string& id) {
    const std::size_t colon = id.find(':');
    if (colon == std::string::npos) return id;
    if (id.compare(0, colon, "twilightforest") != 0) return std::string();
    return id.substr(colon + 1);
}

// Java Float.toString(float): the shortest decimal that round-trips, plain
// notation for 1e-3 <= |v| < 1e7 (at least one fractional digit), else
// computerized scientific ("1.0E-4").
std::string javaFloatToString(float value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "Infinity" : "-Infinity";
    if (value == 0.0f) return std::signbit(value) ? "-0.0" : "0.0";

    char buffer[64];
    for (int precision = 1; precision <= 9; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*e", precision - 1, static_cast<double>(value));
        if (std::strtof(buffer, nullptr) == value) break;
    }
    std::string text(buffer);
    const bool negative = !text.empty() && text[0] == '-';
    const std::size_t ePos = text.find_first_of("eE");
    std::string digits;
    for (std::size_t i = 0; i < ePos && i < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i]))) digits.push_back(text[i]);
    }
    const int exponent = ePos == std::string::npos ? 0 : std::atoi(text.c_str() + ePos + 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    const float magnitude = std::fabs(value);
    std::string out;
    if (magnitude >= 1e-3f && magnitude < 1e7f) {
        const int integerDigits = exponent + 1;
        if (integerDigits <= 0) {
            out = "0." + std::string(static_cast<std::size_t>(-integerDigits), '0') + digits;
        } else if (static_cast<std::size_t>(integerDigits) >= digits.size()) {
            out = digits + std::string(static_cast<std::size_t>(integerDigits) - digits.size(), '0') + ".0";
        } else {
            out = digits.substr(0, static_cast<std::size_t>(integerDigits)) + "."
                + digits.substr(static_cast<std::size_t>(integerDigits));
        }
    } else {
        out = digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : std::string("0"))
            + "E" + std::to_string(exponent);
    }
    return negative ? "-" + out : out;
}

// RectangleLatticeIterator.getNearestLatticeIndex (Mth.positiveModulo float).
int32_t nearestLatticeIndex(float latticeSpacing, int32_t i) {
    const float fi = static_cast<float>(i);
    const float modulo = std::fmod(std::fmod(fi, latticeSpacing) + latticeSpacing, latticeSpacing);
    const float quotient = (fi - modulo) / latticeSpacing;
    return Mth::floor(static_cast<double>(quotient));
}

// RectangleLatticeIterator.boundedGrid(chunkBounds, 0, xSpacing, zSpacing,
// xOffset, zOffset) — lazily generated positions, marching down z then
// across x. next() does not test hasNext(), as in the mod.
class RectangleLattice {
public:
    RectangleLattice(const BoundingBox& bounds, float xSpacing, float zSpacing, float xOffset, float zOffset)
        : m_xSpacing(xSpacing), m_zSpacing(zSpacing), m_xOffset(xOffset), m_zOffset(zOffset) {
        m_startX = nearestLatticeIndex(xSpacing, static_cast<int32_t>(static_cast<float>(bounds.minX) - xOffset));
        m_startZ = nearestLatticeIndex(zSpacing, static_cast<int32_t>(static_cast<float>(bounds.minZ) - zOffset));
        m_countX = nearestLatticeIndex(xSpacing, static_cast<int32_t>(static_cast<float>(bounds.maxX + 1) - xOffset))
            - m_startX;
        m_countZ = nearestLatticeIndex(zSpacing, static_cast<int32_t>(static_cast<float>(bounds.maxZ + 1) - zOffset))
            - m_startZ;
    }

    bool hasNext() const { return m_latticeX < m_countX; }

    std::pair<int32_t, int32_t> next() {
        const int32_t x = static_cast<int32_t>(m_xOffset + static_cast<float>(m_startX + m_latticeX) * m_xSpacing);
        const int32_t z = static_cast<int32_t>(m_zOffset + static_cast<float>(m_startZ + m_latticeZ) * m_zSpacing);
        if (m_latticeZ + 1 < m_countZ) {
            ++m_latticeZ;
        } else {
            m_latticeZ = 0;
            ++m_latticeX;
        }
        return {x, z};
    }

private:
    float m_xSpacing, m_zSpacing, m_xOffset, m_zOffset;
    int32_t m_startX = 0, m_startZ = 0, m_countX = 0, m_countZ = 0;
    int32_t m_latticeX = 0, m_latticeZ = 0;
};

} // namespace

// ============================================================================
// TriangularLatticeConfig
// ============================================================================

TriangularLatticeConfig TriangularLatticeConfig::fromSpacing(float spacing) {
    return fromOffsets(spacing, Mth::cos(static_cast<double>(kPi / 6.0f)) * spacing,
                       Mth::sin(static_cast<double>(kPi / 6.0f)) * spacing);
}

TriangularLatticeConfig TriangularLatticeConfig::fromOffsets(float spacing, float xOffset, float zOffset) {
    TriangularLatticeConfig config;
    config.spacing = spacing;
    config.xOffset = xOffset;
    config.zOffset = zOffset;
    config.xSpacing = xOffset * 2.0f;
    config.zSpacing = spacing;
    return config;
}

std::vector<std::pair<int32_t, int32_t>> TriangularLatticeConfig::boundedGrid(const BoundingBox& bounds) const {
    // new ZippedIterator<>(boundedGrid(bounds, y, xSpacing, zSpacing, 0, 0),
    //                      boundedGrid(bounds, y, xSpacing, zSpacing, xOffset, zOffset))
    RectangleLattice first(bounds, xSpacing, zSpacing, 0.0f, 0.0f);
    RectangleLattice second(bounds, xSpacing, zSpacing, xOffset, zOffset);
    std::vector<std::pair<int32_t, int32_t>> out;
    bool firstNext = first.hasNext();
    while (first.hasNext() || second.hasNext()) {
        RectangleLattice& current = firstNext ? first : second;
        RectangleLattice& other = firstNext ? second : first;
        // Should the other iterator be switched to? Flip the selector if so.
        if (other.hasNext()) firstNext = !firstNext;
        out.push_back(current.next());
    }
    return out;
}

// ============================================================================
// StructureSpeleothemConfig
// ============================================================================

const Stalactite& StructureSpeleothemConfig::WeightedStalactites::pick(WorldgenRandom& random) const {
    // WeightedList.getRandom(random).orElse(STONE_STALACTITE); an empty or
    // weightless list compiles to BlockSpikeFeature::defaultRandom (no draw).
    if (entries.empty() || totalWeight <= 0) {
        return data::worldgen::features::twilight::stoneStalactite();
    }
    int32_t selection = random.nextInt(totalWeight);
    for (const auto& entry : entries) {
        selection -= entry.second;
        if (selection < 0) return entry.first;
    }
    return data::worldgen::features::twilight::stoneStalactite();
}

bool StructureSpeleothemConfig::shouldDoAStalactite(WorldgenRandom& random) const {
    return m_hasVariety && random.nextFloat() < m_stalactiteChance;
}

bool StructureSpeleothemConfig::shouldDoAStalagmite(WorldgenRandom& random) const {
    return m_hasVariety && random.nextFloat() < m_stalagmiteChance;
}

const Stalactite& StructureSpeleothemConfig::getStalactite(WorldgenRandom& random) const {
    return m_stalactites.pick(random);
}

const Stalactite& StructureSpeleothemConfig::getStalagmite(WorldgenRandom& random) const {
    return m_stalagmites.pick(random);
}

// StalactiteReloadListener + StructureSpeleothemConfig.fromLocation /
// compileStalactites / compileStalagmites, run once per config id.
struct SpeleothemConfigLoader {
    using Weighted = std::vector<std::pair<Stalactite, int32_t>>;

    // Stalactite.CODEC: ores = Either<List<Pair<Block, Integer>>, Block>.
    static bool parseStalactite(const nlohmann::json& json, Stalactite& out) {
        out.sizeVariation = json.value("size_variation", 0.25f);
        out.maxLength = json.value("max_length", 11);
        out.weight = json.value("weight", 1);
        const nlohmann::json& ores = json.at("ores");
        if (ores.is_string()) {
            BlockState* state = twilight_blocks::defaultState(ores.get<std::string>());
            out.singleOre = state != nullptr ? state : Blocks::STONE->defaultBlockState();
        } else if (ores.is_array()) {
            for (const nlohmann::json& entry : ores) {
                BlockState* state = twilight_blocks::defaultState(entry.at("block").get<std::string>());
                out.weightedOres.emplace_back(state, entry.at("weight").get<int32_t>());
            }
        } else {
            return false;
        }
        return true;
    }

    // StalactiteReloadListener.populateList: each id names
    // twilight/stalactites/<path>.json in its namespace.
    static std::vector<Stalactite> loadList(const nlohmann::json& ids, const std::string& type) {
        std::vector<Stalactite> out;
        if (!ids.is_array()) return out;
        for (const nlohmann::json& idJson : ids) {
            const std::string id = idJson.get<std::string>();
            const std::string path = twilightPath(id);
            const std::string file = "twilight/stalactites/" + path + ".json";
            if (path.empty() || !twilight_data::exists(file)) {
                logWarning("Could not find stalactite entry for " + id + " (config " + type + ")");
                continue;
            }
            try {
                Stalactite stalactite;
                if (parseStalactite(twilight_data::file(file), stalactite)) {
                    out.push_back(std::move(stalactite));
                } else {
                    logWarning("Failed to parse stalactite entry " + id + " in config " + type);
                }
            } catch (const std::exception& e) {
                logWarning("Failed to parse stalactite entry " + id + " in config " + type + ": " + e.what());
            }
        }
        return out;
    }

    static StructureSpeleothemConfig::WeightedStalactites compile(const Weighted& unbaked) {
        // compileSpeleothems: WeightedList.of(unbakedRandomList).
        StructureSpeleothemConfig::WeightedStalactites list;
        int64_t total = 0;
        for (const auto& entry : unbaked) {
            if (entry.second < 0) continue;  // Weighted rejects negative weights
            total += entry.second;
            list.entries.push_back(entry);
        }
        list.totalWeight = total > 0 && total <= INT32_MAX ? static_cast<int32_t>(total) : 0;
        if (list.totalWeight <= 0) list.entries.clear();
        return list;
    }

    static StructureSpeleothemConfig::WeightedStalactites compileSimple(const std::vector<Stalactite>& list) {
        Weighted unbaked;
        for (const Stalactite& s : list) unbaked.emplace_back(s, s.weight);
        return compile(unbaked);
    }

    // compileStalactites: interpolate between base and ore stalactites.
    static StructureSpeleothemConfig::WeightedStalactites compileStalactites(
        float oreChance, const std::vector<Stalactite>& stalactites, const std::vector<Stalactite>& oreStalactites) {
        const float interpolation = Mth::clamp(oreChance, 0.0f, 1.0f);

        int32_t stoneWeightSum = 0;
        for (const Stalactite& s : stalactites) stoneWeightSum += s.weight;
        int32_t oreWeightSum = 0;
        for (const Stalactite& s : oreStalactites) oreWeightSum += s.weight;
        const float totalWeight = static_cast<float>(stoneWeightSum + oreWeightSum);

        if (totalWeight <= 0.0f) return StructureSpeleothemConfig::WeightedStalactites{};
        if (stalactites.empty() || stoneWeightSum <= 0) return compileSimple(oreStalactites);
        if (oreStalactites.empty() || oreWeightSum <= 0) return compileSimple(stalactites);

        // Math.ceil(Math.pow(10, Mth.clamp((interpolation + "" + totalWeight)
        // .length() - 4, 2, 6))) — the mod counts digits of precision through
        // the Java string forms of the two floats.
        const int32_t length = static_cast<int32_t>(
            (javaFloatToString(interpolation) + javaFloatToString(totalWeight)).size());
        const int32_t exponent = Mth::clamp(length - 4, 2, 6);
        const double quantizationFactor = std::ceil(std::pow(10.0, static_cast<double>(exponent)));

        const double stoneCounterweight =
            quantizationFactor * static_cast<double>(1.0f - interpolation) / static_cast<double>(stoneWeightSum);
        const double oreCounterweight =
            quantizationFactor * static_cast<double>(interpolation) / static_cast<double>(oreWeightSum);

        if (stoneCounterweight <= 0.0) return compileSimple(oreStalactites);
        if (oreCounterweight <= 0.0) return compileSimple(stalactites);

        // Mth.ceil(s.weight() * counterweight) for each, stones then ores.
        Weighted unbaked;
        for (const Stalactite& s : stalactites) {
            unbaked.emplace_back(s, static_cast<int32_t>(std::ceil(static_cast<double>(s.weight) * stoneCounterweight)));
        }
        for (const Stalactite& s : oreStalactites) {
            unbaked.emplace_back(s, static_cast<int32_t>(std::ceil(static_cast<double>(s.weight) * oreCounterweight)));
        }
        return compile(unbaked);
    }

    // RectangleLatticeIterator.TriangularLatticeConfig.CODEC: verbose, then
    // offset, then spacing-only, then the unit DEFAULT (spacing 3.5).
    static TriangularLatticeConfig parseLattice(const nlohmann::json& settings) {
        if (!settings.contains("lattice") || !settings["lattice"].is_object()) {
            return TriangularLatticeConfig::fromSpacing(3.5f);
        }
        const nlohmann::json& lattice = settings["lattice"];
        const bool hasSpacing = lattice.contains("spacing");
        const bool hasOffsets = lattice.contains("x_offset") && lattice.contains("z_offset");
        if (hasSpacing && hasOffsets && lattice.contains("x_spacing") && lattice.contains("z_spacing")) {
            TriangularLatticeConfig config;
            config.spacing = lattice["spacing"].get<float>();
            config.xOffset = lattice["x_offset"].get<float>();
            config.zOffset = lattice["z_offset"].get<float>();
            config.xSpacing = lattice["x_spacing"].get<float>();
            config.zSpacing = lattice["z_spacing"].get<float>();
            return config;
        }
        if (hasSpacing && hasOffsets) {
            return TriangularLatticeConfig::fromOffsets(lattice["spacing"].get<float>(),
                                                        lattice["x_offset"].get<float>(),
                                                        lattice["z_offset"].get<float>());
        }
        if (hasSpacing) return TriangularLatticeConfig::fromSpacing(lattice["spacing"].get<float>());
        return TriangularLatticeConfig::fromSpacing(3.5f);
    }

    static std::shared_ptr<const StructureSpeleothemConfig> load(const std::string& id) {
        const std::string path = twilightPath(id);
        const std::string settingsFile = "twilight/structure_speleothem_settings/" + path + ".json";
        if (path.empty() || !twilight_data::exists(settingsFile)) {
            throw std::runtime_error("structure speleothem settings " + id + " not found");
        }
        const nlohmann::json& settings = twilight_data::file(settingsFile);

        auto config = std::make_shared<StructureSpeleothemConfig>();
        config->m_lattice = parseLattice(settings);
        config->m_type = lowercase(settings.value("type", path));

        // StalactiteReloadListener.HILL_CONFIGS.get(type): the variety file of
        // that type. A missing variety leaves shouldDoA* false (no draws).
        const std::string varietyFile = "twilight/stalactites/" + config->m_type + ".json";
        if (!twilight_data::exists(varietyFile)) {
            logWarning("no stalactite config of type " + config->m_type + " (" + id + ")");
            return config;
        }
        const nlohmann::json& variety = twilight_data::file(varietyFile);
        if (lowercase(variety.value("type", std::string())) != config->m_type) {
            logWarning(varietyFile + " declares type " + variety.value("type", std::string())
                       + ", expected " + config->m_type);
        }
        config->m_hasVariety = true;
        config->m_stalactiteChance = variety.value("stalactite_chance", 0.0f);
        config->m_stalagmiteChance = variety.value("stalagmite_chance", 0.0f);
        const float oreChance = variety.value("ore_chance", 0.0f);

        const std::vector<Stalactite> base = loadList(variety.value("base_stalactites", nlohmann::json::array()),
                                                      config->m_type);
        const std::vector<Stalactite> ores = loadList(variety.value("ore_stalactites", nlohmann::json::array()),
                                                      config->m_type);
        const std::vector<Stalactite> stalagmites = loadList(variety.value("stalagmites", nlohmann::json::array()),
                                                             config->m_type);

        config->m_stalactites = compileStalactites(oreChance, base, ores);
        config->m_stalagmites = compileSimple(stalagmites);
        return config;
    }
};

std::shared_ptr<const StructureSpeleothemConfig> StructureSpeleothemConfig::get(const std::string& id) {
    static std::mutex s_mutex;
    static std::map<std::string, std::shared_ptr<const StructureSpeleothemConfig>> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_cache.find(id);
    if (it != s_cache.end()) return it->second;
    std::shared_ptr<const StructureSpeleothemConfig> config = SpeleothemConfigLoader::load(id);
    s_cache.emplace(id, config);
    return config;
}

// ============================================================================
// HollowHillComponent
// ============================================================================
namespace {

constexpr float kChestSpawnChance = 0.025f;
constexpr float kSpawnerSpawnChance = 0.025f;
constexpr float kSpecialSpawnChance = kChestSpawnChance + kSpawnerSpawnChance;

// Block.UPDATE_SUPPRESS_DROPS | UPDATE_KNOWN_SHAPE | UPDATE_CLIENTS.
constexpr int kCobbleFlags = 32 | 16 | 2;

class HollowHillComponent final : public TFStructureComponentOld {
public:
    HollowHillComponent(int hillSize, std::shared_ptr<const StructureSpeleothemConfig> speleothems)
        : TFStructureComponentOld(static_cast<int>(core::Direction::SOUTH))
        , m_hillSize(hillSize)
        , m_radius(((hillSize * 2 + 1) * 8) - 6)
        , m_hdiam((hillSize * 2 + 1) * 16)
        , m_speleothems(std::move(speleothems))
        , m_cobblestone(Blocks::COBBLESTONE->defaultBlockState())
        , m_caveAir(Blocks::CAVE_AIR->defaultBlockState())
        , m_stone(Blocks::STONE->defaultBlockState()) {}

    // HollowHillComponent.postProcess.
    void postProcess(WorldGenLevel* level, ChunkGenerator* generator, WorldgenRandom& random,
                     const BoundingBox& chunkBB, const ::world::ChunkPos& chunkPos,
                     const core::BlockPos& referencePos, StructurePieceData& self) override {
        (void)chunkPos;
        (void)referencePos;
        bind(self);

        const BoundingBox box = self.boundingBox;
        const int32_t centerX = box.centerX();
        const int32_t centerZ = box.centerZ();
        const float shortenedRadiusSq = static_cast<float>(m_radius * m_radius) * 0.85f;
        const float drainRadius = static_cast<float>(m_hillSize) * 16.5f;

        drainWater(generator, chunkBB, box, level, m_hillSize * 3 + 2, m_caveAir, centerX, centerZ,
                   static_cast<double>(drainRadius * drainRadius), m_stone);

        // Two rectangle lattices zipped into a triangular lattice, bounded by
        // the writeable chunk box (yLevel 0).
        for (const auto& [latticeX, latticeZ] : m_speleothems->lattice().boundedGrid(chunkBB)) {
            const float distSq = getDistSqFromCenter(centerX, centerZ, latticeX, latticeZ);
            if (distSq > shortenedRadiusSq) continue;
            setFeatures(level, random, chunkBB, latticeX, latticeZ, distSq);
        }
    }

    // HollowHillComponent.drainWater (public static in the mod).
    static void drainWater(ChunkGenerator* generator, const BoundingBox& chunkBox, const BoundingBox& structureBox,
                           WorldGenLevel* level, int maxDepth, BlockState* airState, int xCenter, int zCenter,
                           double radiusSq, BlockState* undergroundBlock) {
        BoundingBox bounds;
        if (!tf_common::intersectionOf(chunkBox, structureBox, bounds)) return;

        const int seaLevel = generator->getSeaLevel();
        const int minY = seaLevel - maxDepth;

        for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
            const int dZ = zCenter - z;
            for (int x = bounds.minX; x <= bounds.maxX; ++x) {
                const int dX = xCenter - x;
                const float distSq = static_cast<float>(dX * dX + dZ * dZ);
                if (static_cast<double>(distSq) >= radiusSq) continue;

                const int maxY = std::min(seaLevel, level->getHeight(Heightmap::Types::OCEAN_FLOOR_WG, x, z) - 1);
                bool crossedFloor = false;
                for (int y = maxY; y >= minY; --y) {
                    const core::BlockPos pos(x, y, z);
                    BlockState* stateAt = level->getBlockState(pos);
                    if (stateAt->hasWaterFluid()) {
                        level->setBlock(pos, airState, 3);
                    } else {
                        crossedFloor = true;
                    }
                    if (crossedFloor && (stateAt->is(Blocks::DIRT) || stateAt->is(Blocks::SAND))) {
                        level->setBlock(pos, undergroundBlock, 3);
                    }
                }
            }
        }
    }

private:
    int m_hillSize;
    int m_radius;
    int m_hdiam;
    std::shared_ptr<const StructureSpeleothemConfig> m_speleothems;
    BlockState* m_cobblestone;
    BlockState* m_caveAir;
    BlockState* m_stone;

    static float getDistSqFromCenter(int centerX, int centerZ, int toX, int toZ) {
        const float x = static_cast<float>(toX - centerX) - 0.5f;
        const float z = static_cast<float>(toZ - centerZ) - 0.5f;
        return x * x + z * z;
    }

    void setFeatures(WorldGenLevel* level, WorldgenRandom& random, const BoundingBox& chunkBB,
                     int x, int z, float distSq) {
        // rand.setSeed(rand.nextLong() ^ pos.asLong()) — the lattice pos at y 0.
        const int64_t next = random.nextLong();
        random.setSeed(next ^ core::BlockPos::asLong(x, 0, z));
        placeCeilingFeature(level, random, x, z, distSq);
        placeFloorFeature(level, random, chunkBB, x, z, distSq);
    }

    void placeFloorFeature(WorldGenLevel* level, WorldgenRandom& random, const BoundingBox& chunkBB,
                           int x, int z, float distSq) {
        const int floorY = getFloorY(distSq);
        const float floatChance = random.nextFloat();

        if (floatChance < kSpecialSpawnChance) {
            // Random offset from the lattice point (not for stalagmites, to
            // avoid burying the chest/spawner).
            const float angle = random.nextFloat() * kTwoPi;
            const int px = tf_common::javaRound(Mth::cos(static_cast<double>(angle)) * kSqrtOfTwo) + x;
            const int pz = tf_common::javaRound(Mth::sin(static_cast<double>(angle)) * kSqrtOfTwo) + z;
            const core::BlockPos pos(px, floorY, pz);

            if (floatChance < kSpawnerSpawnChance) {
                const std::string mob = getMobID(random);
                tf_common::setSpawnerInWorld(level, chunkBB, mob, pos.above());
            } else {
                placeTreasureAtWorldPosition(level, getTreasureType(), false, chunkBB, pos.above());
            }

            level->setBlock(pos.below(), m_cobblestone, kCobbleFlags);
            level->setBlock(pos, m_cobblestone, kCobbleFlags);
        } else if (m_speleothems->shouldDoAStalagmite(random)) {
            const int ceilingY = getCeilingY(distSq);
            // Limit the height of stalagmites, plus a little leeway.
            const int forcedMaxHeight = ceilingY - floorY + 4;
            const Stalactite& stalagmite = m_speleothems->getStalagmite(random);
            data::worldgen::features::twilight::startSpike(*level, core::BlockPos(x, floorY, z), stalagmite,
                                                           random, false, forcedMaxHeight);
        }
    }

    void placeCeilingFeature(WorldGenLevel* level, WorldgenRandom& random, int x, int z, float distSq) {
        if (!m_speleothems->shouldDoAStalactite(random)) return;
        const core::BlockPos ceiling(x, getCeilingY(distSq), z);
        // No max height for stalactites: over-generating them has no defect.
        const Stalactite& stalactite = m_speleothems->getStalactite(random);
        data::worldgen::features::twilight::startSpike(*level, ceiling, stalactite, random, true);
    }

    int getCeilingY(float distSq) const {
        const float height = getCeilingHeight(Mth::sqrt(distSq));
        return worldY(static_cast<int>(std::ceil(static_cast<double>(height))));
    }

    int getFloorY(float distSq) const {
        const float height = getFloorHeight(Mth::sqrt(distSq)) + 0.25f;
        return worldY(Mth::floor(static_cast<double>(height)));
    }

    float getFloorHeight(float dist) const {
        const float angle = dist / static_cast<float>(m_hdiam) * kPi;
        return static_cast<float>(m_hillSize * 2)
            - Mth::cos(static_cast<double>(angle)) * (static_cast<float>(m_hdiam) / 20.0f) + 1.0f;
    }

    float getCeilingHeight(float dist) const {
        const float angle = dist / static_cast<float>(m_hdiam) * kPi;
        return Mth::cos(static_cast<double>(angle)) * (static_cast<float>(m_hdiam) / 4.0f);
    }

    // TFLootTables.SMALL/MEDIUM/LARGE_HOLLOW_HILL.
    std::string getTreasureType() const {
        return m_hillSize == 3 ? "twilightforest:hill_3"
             : (m_hillSize == 2 ? "twilightforest:hill_2" : "twilightforest:hill_1");
    }

    // getMobID(rand) -> getMobID(rand, hillSize).
    std::string getMobID(WorldgenRandom& random) const {
        if (m_hillSize == 1) return getLevel1Mob(random);
        if (m_hillSize == 2) return getLevel2Mob(random);
        if (m_hillSize == 3) return getLevel3Mob(random);
        return "minecraft:spider";
    }

    static std::string getLevel1Mob(WorldgenRandom& random) {
        switch (random.nextInt(10)) {
            case 3: case 4: case 5: return "minecraft:spider";
            case 6: case 7: return "minecraft:zombie";
            case 8: return "minecraft:silverfish";
            case 9: return "twilightforest:redcap";
            default: return "twilightforest:swarm_spider";
        }
    }

    static std::string getLevel2Mob(WorldgenRandom& random) {
        switch (random.nextInt(10)) {
            case 3: case 4: case 5: return "minecraft:zombie";
            case 6: case 7: return "minecraft:skeleton";
            case 8: return "twilightforest:swarm_spider";
            case 9: return "minecraft:cave_spider";
            default: return "twilightforest:redcap";
        }
    }

    static std::string getLevel3Mob(WorldgenRandom& random) {
        switch (random.nextInt(11)) {
            case 0: return "twilightforest:slime_beetle";
            case 1: return "twilightforest:fire_beetle";
            case 2: return "twilightforest:pinch_beetle";
            case 3: case 4: case 5: return "minecraft:skeleton";
            case 6: case 7: case 8: return "minecraft:cave_spider";
            case 9: return "minecraft:creeper";
            default: return "twilightforest:wraith";
        }
    }
};

// HollowHillFunction.compute (block-coordinate FunctionContext).
struct HollowHillFunction {
    float centerX, bottomY, centerZ, radius, heightScale;

    // fromPos(blockPos, radius, heightScale): the block's centre.
    static HollowHillFunction fromPos(int x, int y, int z, float radius, float heightScale) {
        return {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f,
                radius, heightScale};
    }

    double operator()(int blockX, int blockY, int blockZ) const {
        const float dX = static_cast<float>(blockX) - centerX;
        const float dY = static_cast<float>(blockY) - bottomY;
        const float dZ = static_cast<float>(blockZ) - centerZ;
        const float dist = Mth::sqrt(dX * dX + dZ * dZ);
        // Cosine is even: only the radius multiplying it sees a negative radius.
        const float height = Mth::cos(static_cast<double>(dist / radius * kPi)) * radius * 0.3333333334f;
        const float normalizedDist = Mth::clamp(dist / std::fabs(radius), 0.0f, 1.0f);
        if (normalizedDist >= 1.0f) return 0.0;
        return static_cast<double>(Mth::clamp(height * heightScale - dY, -1.0f, 1.0f));
    }
};

// FocusedDensityFunction.compute — a sphere mapped from nearValue (centre)
// to farValue (radius and beyond).
struct FocusedDensityFunction {
    float centerX, bottomY, centerZ, radius, nearValue, farValue;

    static FocusedDensityFunction fromPos(int x, int y, int z, float radius, float nearValue, float farValue) {
        return {static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f,
                radius, nearValue, farValue};
    }

    double operator()(int blockX, int blockY, int blockZ) const {
        const float dX = centerX - static_cast<float>(blockX);
        const float dY = bottomY - static_cast<float>(blockY);
        const float dZ = centerZ - static_cast<float>(blockZ);
        const float dist = Mth::sqrt(dX * dX + dY * dY + dZ * dZ);
        return static_cast<double>(Mth::clampedMap(dist, 0.0f, radius, nearValue, farValue));
    }
};

} // namespace

// ============================================================================
// HollowHillStructure.getFirstPiece (+ the no-op addChildren).
// ============================================================================
bool buildHollowHill(const StructureInfo& info, GenerationContext& ctx, LegacyRandomSource& firstPieceRandom,
                     int32_t x, int32_t y, int32_t z, StructureStartData& out) {
    (void)ctx;
    (void)firstPieceRandom;  // getFirstPiece and addChildren draw nothing

    int size = 1;
    std::shared_ptr<const StructureSpeleothemConfig> speleothems;
    try {
        const nlohmann::json& config = twilight_data::structure(info.name);
        // Codec.intRange(1, 3).fieldOf("hill_size").
        size = config.value("hill_size", 1);
        if (size < 1 || size > 3) {
            logWarning(info.name + ": hill_size " + std::to_string(size) + " outside 1..3");
            return false;
        }
        speleothems = StructureSpeleothemConfig::get(config.value("speleothem_config", std::string()));
    } catch (const std::exception& e) {
        logWarning(info.name + ": " + e.what());
        return false;
    }

    int px, py, pz;
    switch (size) {
        case 1: px = x - 3; py = y - 2; pz = z - 3; break;
        case 2: px = x - 7; py = y - 5; pz = z - 7; break;
        default: px = x - 11; py = y - 5; pz = z - 11; break;
    }

    // HollowHillComponent(TFHill, 0, size, px, py, pz, config).
    const int radius = ((size * 2 + 1) * 8) - 6;
    StructurePieceData piece;
    piece.pieceType = "twilightforest:tfhill";
    piece.boundingBox = tf_common::getComponentToAddBoundingBox(
        px, py, pz, -radius, -(3 + size), -radius, radius * 2, radius / (size == 1 ? 2 : size), radius * 2,
        static_cast<int>(core::Direction::SOUTH), true);
    piece.rotation = tf_common::rotationName(OrientedPieceBehavior::ROT_CW180);
    piece.genDepth = 0;
    out.pieces.push_back(std::move(piece));
    out.behaviors.push_back(std::make_shared<HollowHillComponent>(size, std::move(speleothems)));
    return true;
}

// ============================================================================
// HollowHillStructure.getStructureTerraformer
// ============================================================================
TwilightTerraformer hollowHillTerraformer(const StructureInfo& info, const StructureStartData& start,
                                          const ::world::ChunkPos& chunkPos) {
    (void)chunkPos;
    int hillSize = 1;
    try {
        hillSize = twilight_data::structure(info.name).value("hill_size", 1);
    } catch (const std::exception& e) {
        logWarning(info.name + ": " + e.what());
        return TwilightTerraformer{};
    }

    const float radius = (static_cast<float>(hillSize * 4) + 0.8f) * 8.0f;
    const float radiusInner = radius - 8.0f;

    const BoundingBox& structureBox = start.boundingBox;
    const int width = std::min(structureBox.getXSpan(), structureBox.getZSpan());
    const int yCeilingFocus = structureBox.minY;
    const int centerX = structureBox.centerX();
    const int centerY = structureBox.centerY();
    const int centerZ = structureBox.centerZ();

    // Main mound: 0 above the mound surface, up to 1 under it (clamped).
    const HollowHillFunction hillMound =
        HollowHillFunction::fromPos(centerX, yCeilingFocus + 10, centerZ, radius, 0.7f);
    // Inner ceiling (multiplied by -1 below): negative inside the hill gap.
    const HollowHillFunction innerCeiling =
        HollowHillFunction::fromPos(centerX, yCeilingFocus + 6, centerZ, radiusInner, 0.675f);
    // Upward dome (negative radius): the cave floor.
    const HollowHillFunction innerFloor = HollowHillFunction::fromPos(
        centerX, yCeilingFocus + hillSize + hillSize / 2, centerZ, 2.0f - radiusInner, 1.0f / 10.0f);
    const FocusedDensityFunction interiorMask =
        FocusedDensityFunction::fromPos(centerX, yCeilingFocus, centerZ, radiusInner * 0.52f, -radiusInner, 1.0f);
    const int sphereDrop = static_cast<int>(std::ceil(static_cast<double>(radius) * 0.1));
    const FocusedDensityFunction maskingSphere = FocusedDensityFunction::fromPos(
        centerX, centerY - sphereDrop, centerZ, static_cast<float>(width) * 0.5f + 5.0f,
        static_cast<float>(width) * 0.25f, 0.0f);

    // mul(maskingSphere.clamp(0, 1), mul(8, min(hillMound.clamp(0, 1),
    //     max(interiorMask, max(-1 * innerCeiling, innerFloor)))))
    return [=](int32_t blockX, int32_t blockY, int32_t blockZ) -> double {
        const double mask = Mth::clamp(maskingSphere(blockX, blockY, blockZ), 0.0, 1.0);
        if (mask == 0.0) return 0.0;  // Ap2 MUL: v1 == 0 short-circuits
        const double mound = Mth::clamp(hillMound(blockX, blockY, blockZ), 0.0, 1.0);
        const double interior = std::max(-1.0 * innerCeiling(blockX, blockY, blockZ),
                                         innerFloor(blockX, blockY, blockZ));
        const double interiorMasked = std::max(interiorMask(blockX, blockY, blockZ), interior);
        const double hollowHill = std::min(mound, interiorMasked);
        return mask * (8.0 * hollowHill);
    };
}

} // namespace twilight_pieces
} // namespace structure
} // namespace levelgen
} // namespace minecraft
