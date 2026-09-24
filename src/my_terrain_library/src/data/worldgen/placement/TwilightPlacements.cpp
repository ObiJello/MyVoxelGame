#include "data/worldgen/placement/TwilightPlacements.h"

#include "data/worldgen/features/TwilightFeatures.h"
#include "data/worldgen/features/TwilightFeatureRegistry.h"
#include "data/worldgen/features/TwilightTreeFeatures.h"
#include "data/worldgen/features/TwilightDecorFeatures.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "levelgen/carver/CarverConfiguration.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/StructureStartData.h"
#include "levelgen/structure/TwilightStructures.h"
#include "levelgen/TwilightBlocks.h"
#include "levelgen/VerticalAnchor.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/Heightmap.h"
#include "world/IChunk.h"
#include "world/ChunkPos.h"
#include "world/biome/Biome.h"
#include "core/BlockPos.h"
#include "core/Vec3i.h"
#include "external/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Twilight Forest 4.9 — data/twilightforest/worldgen/placed_feature/**.json,
// built modifier for modifier (see TwilightPlacements.h), plus the mod's
// three placement modifiers: world/components/placements/
// {AvoidLandmarkModifier, ChunkCenterModifier, ChunkBlanketingModifier}.java.

namespace minecraft {
namespace data {
namespace worldgen {
namespace placement {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using blockpredicates::BlockPredicate;

bool s_initialized = false;

// Owned storage: every pointer handed to a PlacedFeature stays valid.
std::vector<std::unique_ptr<PlacementModifier>> s_modifiers;
std::vector<std::unique_ptr<PlacedFeature>> s_placedFeatures;
std::vector<std::shared_ptr<carver::IntProvider>> s_intProviders;
std::vector<std::shared_ptr<BlockPredicate>> s_predicates;
std::map<std::string, PlacedFeature*> s_byId;

fs::path dataRoot() {
    if (const char* env = std::getenv("MC_DATA_ROOT")) return fs::path(env);
    fs::path current = fs::current_path();
    while (!current.empty()) {
        fs::path candidate = current / "data";
        if (fs::exists(candidate) && fs::is_directory(candidate)) return candidate;
        if (current == current.root_path()) break;
        current = current.parent_path();
    }
    throw std::runtime_error("Data root not found (Twilight Forest placed features)");
}

std::string normalizeId(const std::string& id) {
    return id.find(':') == std::string::npos ? "minecraft:" + id : id;
}

PlacementModifier* store(std::unique_ptr<PlacementModifier> modifier) {
    PlacementModifier* raw = modifier.get();
    s_modifiers.push_back(std::move(modifier));
    return raw;
}

Heightmap::Types parseHeightmap(const std::string& name) {
    if (name == "WORLD_SURFACE_WG") return Heightmap::Types::WORLD_SURFACE_WG;
    if (name == "OCEAN_FLOOR_WG") return Heightmap::Types::OCEAN_FLOOR_WG;
    if (name == "WORLD_SURFACE") return Heightmap::Types::WORLD_SURFACE;
    if (name == "OCEAN_FLOOR") return Heightmap::Types::OCEAN_FLOOR;
    if (name == "MOTION_BLOCKING") return Heightmap::Types::MOTION_BLOCKING;
    if (name == "MOTION_BLOCKING_NO_LEAVES") return Heightmap::Types::MOTION_BLOCKING_NO_LEAVES;
    throw std::runtime_error("unknown heightmap " + name);
}

// ---------------------------------------------------------------- values
std::shared_ptr<carver::IntProvider> parseIntProvider(const json& j) {
    if (j.is_number_integer()) {
        auto provider = std::make_shared<carver::ConstantInt>(j.get<int32_t>());
        s_intProviders.push_back(provider);
        return provider;
    }
    const std::string type = normalizeId(j.at("type").get<std::string>());
    std::shared_ptr<carver::IntProvider> provider;
    if (type == "minecraft:constant") {
        provider = std::make_shared<carver::ConstantInt>(j.at("value").get<int32_t>());
    } else if (type == "minecraft:uniform") {
        provider = std::make_shared<carver::UniformInt>(j.at("min_inclusive").get<int32_t>(),
                                                        j.at("max_inclusive").get<int32_t>());
    } else if (type == "minecraft:trapezoid") {
        provider = std::make_shared<TrapezoidInt>(j.at("min").get<int32_t>(), j.at("max").get<int32_t>(),
                                                  j.value("plateau", 0));
    } else if (type == "minecraft:weighted_list") {
        std::vector<carver::WeightedIntEntry> entries;
        for (const json& entry : j.at("distribution")) {
            entries.emplace_back(parseIntProvider(entry.at("data")), entry.value("weight", 1));
        }
        provider = std::make_shared<carver::WeightedListInt>(entries);
    } else {
        throw std::runtime_error("unsupported int provider " + type);
    }
    s_intProviders.push_back(provider);
    return provider;
}

VerticalAnchor parseAnchor(const json& j) {
    if (j.contains("absolute")) return VerticalAnchor::absolute(j["absolute"].get<int32_t>());
    if (j.contains("above_bottom")) return VerticalAnchor::aboveBottom(j["above_bottom"].get<int32_t>());
    if (j.contains("below_top")) return VerticalAnchor::belowTop(j["below_top"].get<int32_t>());
    throw std::runtime_error("unsupported vertical anchor " + j.dump());
}

core::Vec3i parseOffset(const json& j) {
    if (!j.contains("offset")) return core::Vec3i::ZERO();
    const json& o = j["offset"];
    return core::Vec3i(o[0].get<int32_t>(), o[1].get<int32_t>(), o[2].get<int32_t>());
}

// "blocks": a single id or a list; TF ids resolve to the engine's block.
std::vector<std::string> parseBlockList(const json& j) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& raw) {
        const std::string id = normalizeId(raw);
        const std::string resolved = levelgen::twilight_blocks::resolveName(id);
        out.push_back(resolved.empty() ? id : resolved);
    };
    if (j.is_string()) add(j.get<std::string>());
    else for (const json& e : j) add(e.get<std::string>());
    return out;
}

// The mod's tags are read as they are: the data dir carries 26.3's tags,
// #minecraft:substrate_overworld included.
std::string mapTag(const std::string& tag) {
    return tag;
}

std::shared_ptr<BlockPredicate> parsePredicate(const json& j) {
    const std::string type = normalizeId(j.at("type").get<std::string>());
    std::shared_ptr<BlockPredicate> predicate;
    if (type == "minecraft:replaceable") {
        predicate = BlockPredicate::replaceable(parseOffset(j));
    } else if (type == "minecraft:matching_block_tag") {
        predicate = BlockPredicate::matchesTag(parseOffset(j), mapTag(normalizeId(j.at("tag").get<std::string>())));
    } else if (type == "minecraft:matching_blocks") {
        predicate = BlockPredicate::matchesBlocks(parseOffset(j), parseBlockList(j.at("blocks")));
    } else if (type == "minecraft:all_of" || type == "minecraft:any_of") {
        std::vector<std::shared_ptr<BlockPredicate>> children;
        for (const json& child : j.at("predicates")) children.push_back(parsePredicate(child));
        predicate = type == "minecraft:all_of" ? BlockPredicate::allOf(children) : BlockPredicate::anyOf(children);
    } else if (type == "minecraft:not") {
        predicate = BlockPredicate::not_(parsePredicate(j.at("predicate")));
    } else if (type == "minecraft:would_survive") {
        const json& stateJson = j.at("state");
        std::unordered_map<std::string, std::string> properties;
        if (stateJson.contains("Properties")) {
            for (auto it = stateJson["Properties"].begin(); it != stateJson["Properties"].end(); ++it) {
                properties[it.key()] = it.value().get<std::string>();
            }
        }
        const std::string name = normalizeId(stateJson.at("Name").get<std::string>());
        BlockState* state = levelgen::twilight_blocks::state(name, properties);
        if (state == nullptr) throw std::runtime_error("would_survive block " + name + " not registered");
        predicate = BlockPredicate::wouldSurvive(state, parseOffset(j));
    } else if (type == "minecraft:true") {
        predicate = BlockPredicate::alwaysTrue();
    } else {
        throw std::runtime_error("unsupported block predicate " + type);
    }
    s_predicates.push_back(predicate);
    return predicate;
}

// ------------------------------------------------------------- modifiers
PlacementModifier* parseModifier(const json& j) {
    const std::string type = normalizeId(j.at("type").get<std::string>());
    if (type == "minecraft:biome") return &BiomeFilter::biome();
    if (type == "minecraft:in_square") return &InSquarePlacement::spread();
    if (type == "minecraft:heightmap") {
        return store(std::make_unique<HeightmapPlacement>(
            HeightmapPlacement::onHeightmap(parseHeightmap(j.at("heightmap").get<std::string>()))));
    }
    if (type == "minecraft:count") {
        return store(std::make_unique<CountPlacement>(CountPlacement::of(parseIntProvider(j.at("count")).get())));
    }
    if (type == "minecraft:count_on_every_layer") {
        return store(std::make_unique<CountOnEveryLayerPlacement>(
            CountOnEveryLayerPlacement::of(parseIntProvider(j.at("count")).get())));
    }
    if (type == "minecraft:rarity_filter") {
        return store(std::make_unique<RarityFilter>(RarityFilter::onAverageOnceEvery(j.at("chance").get<int32_t>())));
    }
    if (type == "minecraft:surface_water_depth_filter") {
        return store(std::make_unique<SurfaceWaterDepthFilter>(
            SurfaceWaterDepthFilter::forMaxDepth(j.at("max_water_depth").get<int32_t>())));
    }
    if (type == "minecraft:block_predicate_filter") {
        return store(std::make_unique<BlockPredicateFilter>(
            BlockPredicateFilter::forPredicate(parsePredicate(j.at("predicate")))));
    }
    if (type == "minecraft:random_offset") {
        auto xz = parseIntProvider(j.at("xz_spread"));
        auto y = parseIntProvider(j.at("y_spread"));
        return store(std::make_unique<RandomOffsetPlacement>(RandomOffsetPlacement::of(xz.get(), y.get())));
    }
    if (type == "minecraft:height_range") {
        const json& height = j.at("height");
        const std::string heightType = normalizeId(height.at("type").get<std::string>());
        const VerticalAnchor minAnchor = parseAnchor(height.at("min_inclusive"));
        const VerticalAnchor maxAnchor = parseAnchor(height.at("max_inclusive"));
        if (heightType == "minecraft:uniform") {
            return store(std::make_unique<HeightRangePlacement>(HeightRangePlacement::uniform(minAnchor, maxAnchor)));
        }
        if (heightType == "minecraft:trapezoid" && height.value("plateau", 0) == 0) {
            // TrapezoidHeight with no plateau == the library's triangle.
            return store(std::make_unique<HeightRangePlacement>(HeightRangePlacement::triangle(minAnchor, maxAnchor)));
        }
        throw std::runtime_error("unsupported height provider " + height.dump());
    }
    if (type == "minecraft:environment_scan") {
        const std::string direction = j.at("direction_of_search").get<std::string>();
        auto target = parsePredicate(j.at("target_condition"));
        auto allowed = j.contains("allowed_search_condition") ? parsePredicate(j["allowed_search_condition"])
                                                                : BlockPredicate::alwaysTrue();
        return store(std::make_unique<EnvironmentScanPlacement>(EnvironmentScanPlacement::scanningFor(
            direction == "up" ? EnvironmentScanPlacement::Direction::UP : EnvironmentScanPlacement::Direction::DOWN,
            target, allowed, j.at("max_steps").get<int32_t>())));
    }
    if (type == "twilightforest:no_structure") {
        std::unordered_set<std::string> allowed;
        if (j.contains("structures_allowed")) {
            const json& list = j["structures_allowed"];
            if (list.is_string()) {
                allowed.insert(normalizeId(list.get<std::string>()));
            } else {
                for (const json& e : list) allowed.insert(normalizeId(e.get<std::string>()));
            }
        }
        return store(std::make_unique<NoStructurePlacement>(
            j.at("occupies_surface").get<bool>(), j.at("occupies_underground").get<bool>(),
            j.at("occupies_vegetation").get<bool>(), j.at("additional_clearance").get<int32_t>(),
            std::move(allowed)));
    }
    if (type == "twilightforest:chunk_centerer") {
        return store(std::make_unique<ChunkCenterPlacement>());
    }
    if (type == "twilightforest:chunk_blanketing") {
        std::unordered_set<std::string> biomeLock;
        if (j.contains("biome_lock")) {
            const json& lock = j["biome_lock"];
            if (lock.is_string()) {
                const std::string id = lock.get<std::string>();
                if (!id.empty() && id[0] == '#') {
                    throw std::runtime_error("biome tag lock " + id + " not supported");
                }
                biomeLock.insert(normalizeId(id));
            } else {
                for (const json& e : lock) biomeLock.insert(normalizeId(e.get<std::string>()));
            }
        }
        return store(std::make_unique<ChunkBlanketingPlacement>(
            j.at("integrity").get<float>(), parseHeightmap(j.at("heightmap").get<std::string>()),
            std::move(biomeLock)));
    }
    throw std::runtime_error("unsupported placement modifier " + type);
}

// greatestAxalDistance (util/BoundingBoxUtils.java): Chebyshev distance from
// pos to the nearest point of the box.
int32_t greatestAxalDistance(const levelgen::structure::BoundingBox& box, const core::BlockPos& pos) {
    const int32_t cx = std::clamp(pos.getX(), box.minX, box.maxX);
    const int32_t cy = std::clamp(pos.getY(), box.minY, box.maxY);
    const int32_t cz = std::clamp(pos.getZ(), box.minZ, box.maxZ);
    return std::max(std::max(std::abs(cx - pos.getX()), std::abs(cy - pos.getY())), std::abs(cz - pos.getZ()));
}

} // namespace

// ============================================================================
// AvoidLandmarkModifier
// ============================================================================
void NoStructurePlacement::appendPositions(PlacementContext& context, WorldgenRandom& random,
                                           const core::BlockPos& origin, std::vector<core::BlockPos>& out) {
    (void)random;
    ::world::IChunk* chunk = context.getLevel()->getChunk(origin.getX() >> 4, origin.getZ() >> 4);
    if (chunk != nullptr) {
        for (const auto& [structureName, startChunks] : chunk->getAllStructureReferences()) {
            if (structureBlocksPlacement(context, origin, structureName, startChunks)) return;
        }
    }
    out.push_back(origin);
}

bool NoStructurePlacement::structureBlocksPlacement(PlacementContext& context, const core::BlockPos& origin,
                                                    const std::string& structureName,
                                                    const std::vector<int64_t>& startChunks) const {
    // allowedInsideStructure
    if (m_structuresAllowed.count(structureName)) return false;
    // structure instanceof DecorationClearance
    const levelgen::structure::TwilightStructures::DecorationClearance* clearance =
        levelgen::structure::TwilightStructures::decorationClearance(structureName);
    if (clearance == nullptr) return false;
    // clearFromStructureZone
    const bool surfaceClear = !m_occupiesSurface || clearance->surfaceDecorations;
    const bool undergroundClear = !m_occupiesUnderground || clearance->undergroundDecorations;
    const bool vegetationClear = !m_occupiesVegetation || clearance->vegetation;
    if (surfaceClear && undergroundClear && vegetationClear) return false;

    for (int64_t packed : startChunks) {
        const ::world::ChunkPos startPos = ::world::ChunkPos::fromLong(packed);
        ::world::IChunk* startChunk = context.getLevel()->getChunk(startPos.x(), startPos.z());
        if (startChunk == nullptr) continue;
        const levelgen::structure::StructureStartData* start = startChunk->getStartForStructure(structureName);
        if (start == nullptr || !start->isValid()) continue;

        // placementIsBlocked(blockPos, start, blockPos - start.getBoundingBox().getCenter(), radius)
        if (clearance->chunkClearanceRadius <= 0.0f) {
            for (const levelgen::structure::StructurePieceData& piece : start->pieces) {
                if (piece.allowFeatures) continue;
                if (greatestAxalDistance(piece.boundingBox, origin) <= m_additionalClearance) return true;
            }
            continue;
        }
        const int32_t dx = origin.getX() - start->boundingBox.centerX();
        const int32_t dz = origin.getZ() - start->boundingBox.centerZ();
        const float size = clearance->chunkClearanceRadius * 16.0f + static_cast<float>(m_additionalClearance);
        if (static_cast<float>(std::abs(dx)) < size && static_cast<float>(std::abs(dz)) < size) return true;
    }
    return false;
}

// ============================================================================
// ChunkCenterModifier
// ============================================================================
void ChunkCenterPlacement::appendPositions(PlacementContext& context, WorldgenRandom& random,
                                           const core::BlockPos& origin, std::vector<core::BlockPos>& out) {
    (void)context;
    (void)random;
    out.emplace_back((origin.getX() & ~15) + 8, origin.getY(), (origin.getZ() & ~15) + 8);
}

// ============================================================================
// ChunkBlanketingModifier
// ============================================================================
void ChunkBlanketingPlacement::appendPositions(PlacementContext& context, WorldgenRandom& random,
                                               const core::BlockPos& origin, std::vector<core::BlockPos>& out) {
    ::world::IChunk* chunk = context.getLevel()->getChunk(origin.getX() >> 4, origin.getZ() >> 4);
    if (chunk == nullptr) return;
    const ::world::ChunkPos chunkPos = chunk->getPos();
    const int32_t originX = chunkPos.getMinBlockX();
    const int32_t originZ = chunkPos.getMinBlockZ();
    for (int32_t zInChunk = 0; zInChunk < 16; ++zInChunk) {
        for (int32_t xInChunk = 0; xInChunk < 16; ++xInChunk) {
            if (random.nextFloat() > m_integrity) continue;
            const core::BlockPos pos(originX + xInChunk,
                                     chunk->getHeight(static_cast<int>(m_heightmap), xInChunk, zInChunk) + 1,
                                     originZ + zInChunk);
            if (!m_biomeLock.empty()) {
                const world::biome::Biome* biome = context.getBiome(pos);
                if (biome == nullptr || !m_biomeLock.count(biome->getName())) continue;
            }
            out.push_back(pos);
        }
    }
}

// ============================================================================
// Registry
// ============================================================================
bool TwilightPlacements::isInitialized() {
    return s_initialized;
}

PlacedFeature* TwilightPlacements::get(const std::string& id) {
    auto it = s_byId.find(id);
    return it == s_byId.end() ? nullptr : it->second;
}

void TwilightPlacements::bootstrap() {
    if (s_initialized) return;
    s_initialized = true;

    using features::TwilightFeatures;
    using features::TwilightTreeFeatures;
    using features::TwilightDecorFeatures;
    if (!TwilightFeatures::isInitialized()) TwilightFeatures::bootstrap();
    if (!TwilightTreeFeatures::isInitialized()) TwilightTreeFeatures::bootstrap();
    if (!TwilightDecorFeatures::isInitialized()) TwilightDecorFeatures::bootstrap();

    fs::path dir;
    try {
        dir = dataRoot() / "twilightforest" / "worldgen" / "placed_feature";
    } catch (const std::exception& e) {
        fprintf(stderr, "[TwilightPlacements] %s - no Twilight Forest placed features\n", e.what());
        return;
    }
    if (!fs::exists(dir)) {
        fprintf(stderr, "[TwilightPlacements] %s missing - no Twilight Forest placed features\n",
                dir.string().c_str());
        return;
    }

    // Sorted for a deterministic build order (the ids are what matter).
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const fs::path& file : files) {
        std::string relative = fs::relative(file, dir).generic_string();
        relative = relative.substr(0, relative.size() - 5);   // ".json"
        const std::string id = "twilightforest:" + relative;
        try {
            std::ifstream input(file);
            json parsed;
            input >> parsed;
            const std::string featureId = normalizeId(parsed.at("feature").get<std::string>());
            levelgen::ConfiguredFeature* feature = features::twilight::findConfigured(featureId);
            if (feature == nullptr) {
                fprintf(stderr, "[TwilightPlacements] %s: configured feature %s not registered - skipped\n",
                        id.c_str(), featureId.c_str());
                continue;
            }
            std::vector<PlacementModifier*> modifiers;
            for (const json& modifier : parsed.at("placement")) {
                modifiers.push_back(parseModifier(modifier));
            }
            auto placed = std::make_unique<PlacedFeature>(feature, modifiers, id);
            s_byId[id] = placed.get();
            s_placedFeatures.push_back(std::move(placed));
        } catch (const std::exception& e) {
            fprintf(stderr, "[TwilightPlacements] %s: %s - skipped\n", id.c_str(), e.what());
        }
    }
}

} // namespace placement
} // namespace worldgen
} // namespace data
} // namespace minecraft
