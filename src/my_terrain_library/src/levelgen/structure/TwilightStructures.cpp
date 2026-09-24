#include "levelgen/structure/TwilightStructures.h"

#include "levelgen/structure/TwilightStructureData.h"
#include "levelgen/structure/TwilightLandmarks.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/Heightmap.h"
#include "world/biome/TwilightBiomeSource.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Twilight Forest 4.9 structure dispatcher — see TwilightStructures.h.
// Reference: structures/util/LandmarkStructure.java (findValidGenerationPoint,
// findGenerationPoint, getStructurePieceGenerationStubFunction),
// structures/util/DecorationClearance.java (adjustForTerrain, DecorationConfig).

namespace minecraft {
namespace levelgen {
namespace structure {
namespace TwilightStructures {

namespace {

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Every structure type of the LandmarkStructure family (LandmarkStructure,
// ConquerableStructure, ProgressionStructure, ControlledSpawningStructure).
// Camp, fallen trunk and hollow tree are plain Structures.
bool isLandmarkType(const std::string& type) {
    static const char* const kLandmarks[] = {
        "twilightforest:hollow_hill", "twilightforest:hedge_maze", "twilightforest:naga_courtyard",
        "twilightforest:quest_grove", "twilightforest:lich_tower", "twilightforest:aurora_palace",
        "twilightforest:dark_tower", "twilightforest:final_castle", "twilightforest:giant_house",
        "twilightforest:hydra_lair", "twilightforest:knight_stronghold", "twilightforest:labyrinth",
        "twilightforest:mushroom_tower", "twilightforest:troll_cave", "twilightforest:yeti_cave",
    };
    for (const char* landmark : kLandmarks) {
        if (type == landmark) return true;
    }
    return false;
}

twilight_pieces::LandmarkBuilder landmarkBuilder(const std::string& type) {
    if (type == "twilightforest:hollow_hill") return &twilight_pieces::buildHollowHill;
    if (type == "twilightforest:hedge_maze") return &twilight_pieces::buildHedgeMaze;
    if (type == "twilightforest:naga_courtyard") return &twilight_pieces::buildNagaCourtyard;
    if (type == "twilightforest:quest_grove") return &twilight_pieces::buildQuestGrove;
    if (type == "twilightforest:lich_tower") return &twilight_pieces::buildLichTower;
    return nullptr;
}

// DecorationClearance per structure name, parsed once. Absent entries are
// structures that are not DecorationClearance at all.
struct ClearanceCache {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<DecorationClearance>> byName;
    std::map<std::string, bool> resolved;
};

ClearanceCache& clearanceCache() {
    static ClearanceCache s_cache;
    return s_cache;
}

// DecorationClearance.adjustForTerrain.
int32_t adjustForTerrain(GenerationContext& ctx, bool adjustElevation, int32_t x, int32_t z) {
    const int32_t seaLevel = ctx.generator->getSeaLevel();
    if (!adjustElevation) return seaLevel;
    // ChunkGenerator.getFirstOccupiedHeight = getBaseHeight - 1.
    const int32_t firstOccupied =
        ctx.generator->getBaseHeight(x, z, Heightmap::Types::WORLD_SURFACE_WG, ctx.randomState) - 1;
    return std::clamp(firstOccupied, seaLevel + 1, seaLevel + 7);
}

// Structure.isValidBiome at a block position (the non-TF-source fallback).
bool isValidBiomeAt(GenerationContext& ctx, int32_t x, int32_t y, int32_t z) {
    const world::biome::BiomeKey biome = ctx.biomeSource->getNoiseBiome(x >> 2, y >> 2, z >> 2, *ctx.sampler);
    return ctx.validBiomes->find(biome) != ctx.validBiomes->end();
}

bool generateLandmark(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out,
                      twilight_pieces::LandmarkBuilder builder) {
    const nlohmann::json& config = twilight_data::structure(info.name);
    const bool centerInChunk = config.value("center_in_chunk", true);
    const DecorationClearance* clearance = decorationClearance(info.name);
    const bool adjustElevation = clearance != nullptr && clearance->adjustElevation;

    // findValidGenerationPoint: with the TF biome provider, the key biome at
    // the centre of the landmark's biome-grid tile (quart coordinates:
    // region << 6 is 256 blocks, + 2 quarts = 8 blocks in).
    auto* twilightSource = dynamic_cast<world::biome::TwilightBiomeSource*>(ctx.biomeSource);
    if (twilightSource != nullptr) {
        const int32_t biomeX = (twilight_landmarks::javaRoundFloat(static_cast<float>(ctx.chunkX) / 16.0f) << 6) + 2;
        const int32_t biomeZ = (twilight_landmarks::javaRoundFloat(static_cast<float>(ctx.chunkZ) / 16.0f) << 6) + 2;
        const world::biome::BiomeKey biomeAt = twilightSource->getMainBiome(biomeX, biomeZ);
        if (ctx.validBiomes->find(biomeAt) == ctx.validBiomes->end()) return false;
    }

    // findGenerationPoint.
    const int32_t x = (ctx.chunkX << 4) + (centerInChunk ? 7 : 0);
    const int32_t z = (ctx.chunkZ << 4) + (centerInChunk ? 7 : 0);
    const int32_t y = adjustForTerrain(ctx, adjustElevation, x, z);

    // Structure.findValidGenerationPoint's isValidBiome on the stub, only
    // when the source is not the TF provider (the override replaces it).
    if (twilightSource == nullptr && !isValidBiomeAt(ctx, x, y, z)) return false;

    // RandomSource.create(seed + chunkX * 25117L + chunkZ * 151121L).
    const int64_t seed = static_cast<int64_t>(
        static_cast<uint64_t>(ctx.seed)
        + static_cast<uint64_t>(static_cast<int64_t>(ctx.chunkX) * 25117LL)
        + static_cast<uint64_t>(static_cast<int64_t>(ctx.chunkZ) * 151121LL));
    LegacyRandomSource firstPieceRandom(seed);
    return builder(info, ctx, firstPieceRandom, x, y, z, out);
}

} // namespace

bool isTwilightType(const std::string& type) {
    return startsWith(type, "twilightforest:");
}

bool isImplemented(const StructureInfo& info) {
    if (!isTwilightType(info.type)) return false;
    if (info.type == "twilightforest:hollow_tree") return true;
    return landmarkBuilder(info.type) != nullptr;
}

bool generate(const StructureInfo& info, GenerationContext& ctx, StructureStartData& out) {
    if (info.type == "twilightforest:hollow_tree") {
        return twilight_pieces::buildHollowTree(info, ctx, out);
    }
    twilight_pieces::LandmarkBuilder builder = landmarkBuilder(info.type);
    if (builder == nullptr || !isLandmarkType(info.type)) return false;
    return generateLandmark(info, ctx, out, builder);
}

TwilightTerraformer terraformer(const StructureInfo& info, const StructureStartData& start,
                                const ::world::ChunkPos& chunkPos) {
    if (info.type == "twilightforest:hollow_hill") {
        return twilight_pieces::hollowHillTerraformer(info, start, chunkPos);
    }
    if (info.type == "twilightforest:hedge_maze") {
        return twilight_pieces::hedgeMazeTerraformer(info, start, chunkPos);
    }
    if (info.type == "twilightforest:naga_courtyard") {
        return twilight_pieces::nagaCourtyardTerraformer(info, start, chunkPos);
    }
    if (info.type == "twilightforest:lich_tower") {
        return twilight_pieces::lichTowerTerraformer(info, start, chunkPos);
    }
    return TwilightTerraformer{};
}

const DecorationClearance* decorationClearance(const std::string& structureName) {
    ClearanceCache& cache = clearanceCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.resolved.count(structureName)) {
        auto it = cache.byName.find(structureName);
        return it == cache.byName.end() ? nullptr : it->second.get();
    }
    cache.resolved[structureName] = true;
    if (!isTwilightType(structureName)) return nullptr;

    std::unique_ptr<DecorationClearance> clearance;
    try {
        const nlohmann::json& config = twilight_data::structure(structureName);
        const std::string type = config.value("type", std::string());
        if (config.contains("decoration_clearance")) {
            // DecorationConfig.CODEC (chunk_clearance_radius defaults to 1).
            const nlohmann::json& dc = config["decoration_clearance"];
            clearance = std::make_unique<DecorationClearance>();
            clearance->chunkClearanceRadius = dc.value("chunk_clearance_radius", 1.0f);
            clearance->surfaceDecorations = dc.value("allow_biome_surface_decorations", true);
            clearance->undergroundDecorations = dc.value("allow_biome_underground_decorations", true);
            clearance->vegetation = dc.value("allow_biome_vegetation", true);
            clearance->adjustElevation = dc.value("adjust_structure_elevation", false);
        } else if (isLandmarkType(type)) {
            // LandmarkStructure with an empty decorationConfig: every getter's
            // orElse default.
            clearance = std::make_unique<DecorationClearance>();
        } else if (type == "twilightforest:camp") {
            // CampStructure's hard-coded DecorationClearance.
            clearance = std::make_unique<DecorationClearance>();
            clearance->chunkClearanceRadius = 1.0f;
            clearance->surfaceDecorations = false;
        } else if (type == "twilightforest:fallen_trunk") {
            // FallenTrunkStructure's hard-coded DecorationClearance.
            clearance = std::make_unique<DecorationClearance>();
            clearance->chunkClearanceRadius = 0.0f;
            clearance->surfaceDecorations = false;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[TwilightStructures] %s: %s\n", structureName.c_str(), e.what());
    }
    const DecorationClearance* raw = clearance.get();
    if (clearance) cache.byName.emplace(structureName, std::move(clearance));
    return raw;
}

} // namespace TwilightStructures
} // namespace structure
} // namespace levelgen
} // namespace minecraft
