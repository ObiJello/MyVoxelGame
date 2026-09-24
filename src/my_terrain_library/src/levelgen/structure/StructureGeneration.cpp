#include <cstdio>
#include <cstdlib>
#include <chrono>
#include "util/TerrainProfiling.h"
#include "levelgen/structure/StructureGeneration.h"

#include "levelgen/structure/Structures.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/structure/TwilightStructures.h"
#include "levelgen/Beardifier.h"
#include "levelgen/density/terrain/Beardifier.h"
#include "levelgen/ChunkGenerator.h"
#include "levelgen/RandomState.h"
#include "world/ChunkPos.h"

#include <algorithm>
#include <optional>
#include <iostream>
#include <utility>
#include <vector>
#include <mutex>
#include <set>

// Reference: net/minecraft/world/level/chunk/ChunkGenerator.java:425-530.

namespace minecraft {
namespace levelgen {
namespace structure {
namespace StructureGeneration {

namespace {

// Log each not-yet-ported structure once - silent skips are the invisible
// parity-hole failure mode; loud-but-once keeps runs readable.
void warnUnimplementedOnce(const std::string& name) {
    static std::mutex s_mutex;
    static std::set<std::string> s_warned;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_warned.insert(name).second) {
        std::cerr << "[structures] NOT YET PORTED, start skipped: " << name << "\n";
    }
}

// Reference: ChunkGenerator.tryGenerateStructure()
bool tryGenerateStructure(const StructureSelectionEntry& selected,
                          ChunkGeneratorStructureState& state,
                          ChunkGenerator* generator,
                          RandomState* randomState,
                          const world::biome::Climate::Sampler& climateSampler,
                          ::world::IChunk* chunk,
                          int32_t chunkX, int32_t chunkZ) {
    const StructureInfo& info = *selected.structure;
    TERRAIN_ZONE_N("Struct.Try");
    TERRAIN_ZONE_TEXT(info.name.c_str(), info.name.size());
    if (!Structures::isImplemented(info)) {
        warnUnimplementedOnce(info.name);
        return false;
    }

    // fetchReferences: existing start's counter, else 0.
    int32_t references = 0;
    if (const auto* existing = chunk->getStartForStructure(info.name)) {
        references = existing->references;
    }

    const auto& validBiomes = BiomeTags::resolve(info.biomesTag);
    GenerationContext context(generator, randomState, state.biomeSource(), &climateSampler,
                              state.getLevelSeed(), chunkX, chunkZ, &validBiomes);
    StructureStartData start;
    struct SlowTryLog {   // OBEY_STRUCT_LOG=1: name every structure start over 30 ms
        const std::string& name; std::chrono::steady_clock::time_point t0; int32_t cx, cz;
        ~SlowTryLog() {
            static const bool on = std::getenv("OBEY_STRUCT_LOG") != nullptr;
            if (!on) return;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 30.0) std::fprintf(stderr, "[StructTry] %s (%d,%d) %.0fms\n", name.c_str(), cx, cz, ms);
        }
    } slowTryLog{info.name, std::chrono::steady_clock::now(), chunkX, chunkZ};
    // An exception escaping a worker task never completes the chunk future -
    // the pipeline HANGS with all workers idle (observed 2026-08-12). Fail
    // loudly and immediately instead.
    try {
        if (Structures::generate(info, context, references, start) && start.isValid()) {
            chunk->setStartForStructure(info.name, start);
            return true;
        }
    } catch (const std::exception& e) {
        std::cerr << "[structures] FATAL during " << info.name << " at chunk ("
                  << chunkX << "," << chunkZ << "): " << e.what() << std::endl;
        std::abort();
    }
    return false;
}

} // namespace

void createStructures(ChunkGeneratorStructureState& state,
                      ChunkGenerator* generator,
                      RandomState* randomState,
                      ::world::IChunk* chunk) {
    { TERRAIN_ZONE_N("Struct.EnsureRings"); state.ensureStructuresGenerated(); }
    // randomState.createClimateSampler(SamplerContext.builder().enableCaches().build()):
    // one caching context for every structure tried from this chunk.
    levelgen::density::SamplerContext samplerContext = levelgen::density::SamplerContext::builder().enableCaches().build();
    const world::biome::Climate::Sampler climateSampler = randomState->createClimateSampler(samplerContext);
    ::world::ChunkPos sourceChunkPos = chunk->getPos();
    int32_t cx = sourceChunkPos.x();
    int32_t cz = sourceChunkPos.z();

    for (const StructureSet* set : state.possibleStructureSets()) {
        // Skip the whole set when any of its structures already has a valid
        // start in this chunk (Java's lambda early-return).
        bool alreadyStarted = false;
        for (const auto& entry : set->structures) {
            const auto* existing = chunk->getStartForStructure(entry.structure->name);
            if (existing != nullptr && existing->isValid()) {
                alreadyStarted = true;
                break;
            }
        }
        if (alreadyStarted) continue;

        if (!set->placement->isStructureChunk(state, cx, cz)) continue;

        if (set->structures.size() == 1) {
            tryGenerateStructure(set->structures[0], state, generator, randomState, climateSampler, chunk, cx, cz);
        } else {
            // Weighted pick with removal-and-retry.
            std::vector<StructureSelectionEntry> options(set->structures);
            LegacyRandomSource random(0);
            random.setLargeFeatureSeed(state.getLevelSeed(), cx, cz);
            int32_t total = 0;
            for (const auto& option : options) total += option.weight;

            while (!options.empty()) {
                int32_t choice = random.nextInt(total);
                size_t index = 0;
                for (const auto& option : options) {
                    choice -= option.weight;
                    if (choice < 0) break;
                    ++index;
                }
                const StructureSelectionEntry selected = options[index];
                if (tryGenerateStructure(selected, state, generator, randomState, climateSampler, chunk, cx, cz)) {
                    // Java's `return` exits the forEach LAMBDA -> continue with
                    // the next structure set, not the whole createStructures.
                    break;
                }
                options.erase(options.begin() + static_cast<std::ptrdiff_t>(index));
                total -= selected.weight;
            }
        }
    }
}

void createReferences(const std::vector<std::vector<::world::IChunk*>>& chunks,
                      ::world::IChunk* chunk) {
    ::world::ChunkPos centerPos = chunk->getPos();
    int32_t centerX = centerPos.x();
    int32_t centerZ = centerPos.z();
    int32_t minBlockX = centerX * 16;
    int32_t minBlockZ = centerZ * 16;

    int gridSize = static_cast<int>(chunks.size());
    int gridRadius = (gridSize - 1) / 2;
    int range = 8;

    for (int32_t sx = centerX - range; sx <= centerX + range; ++sx) {
        for (int32_t sz = centerZ - range; sz <= centerZ + range; ++sz) {
            int gridX = sx - centerX + gridRadius;
            int gridZ = sz - centerZ + gridRadius;
            if (gridZ < 0 || gridZ >= gridSize) continue;
            if (gridX < 0 || gridX >= static_cast<int>(chunks[gridZ].size())) continue;
            ::world::IChunk* source = chunks[gridZ][gridX];
            if (source == nullptr) continue;

            for (const auto& [name, start] : source->getAllStructureStarts()) {
                if (!start.isValid()) continue;
                if (start.boundingBox.intersects(minBlockX, minBlockZ,
                                                 minBlockX + 15, minBlockZ + 15)) {
                    chunk->addReferenceForStructure(name, ::world::ChunkPos(sx, sz).toLong());
                }
            }
        }
    }
}

namespace {

// Reference: TerrainAdjustment.CODEC serialized names.
levelgen::TerrainAdjustment parseTerrainAdjustment(const std::string& name) {
    if (name == "bury") return levelgen::TerrainAdjustment::BURY;
    if (name == "beard_thin") return levelgen::TerrainAdjustment::BEARD_THIN;
    if (name == "beard_box") return levelgen::TerrainAdjustment::BEARD_BOX;
    if (name == "encapsulate") return levelgen::TerrainAdjustment::ENCAPSULATE;
    return levelgen::TerrainAdjustment::NONE;
}

// Twilight Forest: chunkgenerators/CustomTerrainBeardifier.java — the
// vanilla beardifier plus the terraformers of every CustomDensitySource start
// referenced by the chunk (asmhooks/WorldgenHooks.gatherCustomTerrain), each
// ADDED to the vanilla value. min/max stay the vanilla (unbounded) ones.
// A Beardifier plus the Twilight Forest's CustomDensitySource starts through
// the chunk (the mod adds each terraformer's density to the beardifier's).
class TwilightCustomTerrainBeardifier final : public levelgen::density::DensitySampler {
public:
    TwilightCustomTerrainBeardifier(std::shared_ptr<const levelgen::density::Beardifier> beardifier,
                                    std::vector<TwilightTerraformer> customDensities)
        : m_beardifier(std::move(beardifier)), m_customDensities(std::move(customDensities)) {}

    void sampleVolume(levelgen::density::SamplerContext& context, levelgen::density::DensityBuffer& outputBuffer,
                      const levelgen::density::DensityVolume& volume) const override {
        sampleVolumeNaive(context, outputBuffer, volume, *this);
    }

    float sampleValue(levelgen::density::SamplerContext& context, int x, int y, int z) const override {
        double density = m_beardifier->sampleValue(context, x, y, z);
        for (const TwilightTerraformer& custom : m_customDensities) {
            density += custom(x, y, z);
        }
        return static_cast<float>(density);
    }

private:
    std::shared_ptr<const levelgen::density::Beardifier> m_beardifier;
    std::vector<TwilightTerraformer> m_customDensities;
};

// Reference: Beardifier.includeBoundingBox().
void includeBox(bool& have, levelgen::BoundingBox& acc, const levelgen::BoundingBox& add) {
    if (!have) {
        acc = add;
        have = true;
    } else {
        acc = levelgen::BoundingBox(
            std::min(acc.minX, add.minX), std::min(acc.minY, add.minY),
            std::min(acc.minZ, add.minZ), std::max(acc.maxX, add.maxX),
            std::max(acc.maxY, add.maxY), std::max(acc.maxZ, add.maxZ));
    }
}

} // namespace

std::shared_ptr<const levelgen::density::DensitySampler> createBeardifier(
    const std::vector<std::vector<::world::IChunk*>>& chunks,
    ::world::IChunk* chunk) {
    ::world::ChunkPos centerPos = chunk->getPos();
    int32_t centerX = centerPos.x();
    int32_t centerZ = centerPos.z();
    int32_t minBlockX = centerX * 16;
    int32_t minBlockZ = centerZ * 16;

    int gridSize = static_cast<int>(chunks.size());
    int gridRadius = (gridSize - 1) / 2;

    std::vector<levelgen::Rigid> rigids;
    std::vector<levelgen::JigsawJunction> junctions;
    bool haveBox = false;
    levelgen::BoundingBox anyPieceBox;

    for (const auto& [name, refs] : chunk->getAllStructureReferences()) {
        levelgen::TerrainAdjustment adjustment =
            parseTerrainAdjustment(StructureSets::structureByName(name).terrainAdaptation);
        if (adjustment == levelgen::TerrainAdjustment::NONE) continue;

        // Reference: forStructuresInChunk iterates the LongOpenHashSet in
        // hash-table order (FP-sum order for overlapping adapted starts).
        for (int64_t ref : fastutilLongSetOrder(refs)) {
            ::world::ChunkPos refPos = ::world::ChunkPos::fromLong(ref);
            int gridX = refPos.x() - centerX + gridRadius;
            int gridZ = refPos.z() - centerZ + gridRadius;
            if (gridZ < 0 || gridZ >= gridSize) continue;
            if (gridX < 0 || gridX >= static_cast<int>(chunks[gridZ].size())) continue;
            ::world::IChunk* source = chunks[gridZ][gridX];
            if (source == nullptr) continue;

            const StructureStartData* start = source->getStartForStructure(name);
            if (start == nullptr || !start->isValid()) continue;

            for (const StructurePieceData& piece : start->pieces) {
                // Reference: StructurePiece.isCloseToChunk(pos, 12).
                if (!piece.boundingBox.intersects(minBlockX - 12, minBlockZ - 12,
                                                  minBlockX + 15 + 12, minBlockZ + 15 + 12)) {
                    continue;
                }
                const BoundingBox& b = piece.boundingBox;
                levelgen::BoundingBox pieceBox(b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ);
                if (piece.poolElement) {
                    if (piece.rigidProjection) {
                        rigids.emplace_back(pieceBox, adjustment, piece.groundLevelDelta);
                        includeBox(haveBox, anyPieceBox, pieceBox);
                    }
                    for (const auto& j : piece.junctions) {
                        int jx = j[0];
                        int jz = j[2];
                        if (jx > minBlockX - 12 && jz > minBlockZ - 12
                            && jx < minBlockX + 15 + 12 && jz < minBlockZ + 15 + 12) {
                            junctions.emplace_back(jx, j[1], jz, j[3]);
                            includeBox(haveBox, anyPieceBox,
                                       levelgen::BoundingBox(jx, j[1], jz, jx, j[1], jz));
                        }
                    }
                } else {
                    rigids.emplace_back(pieceBox, adjustment, 0);
                    includeBox(haveBox, anyPieceBox, pieceBox);
                }
            }
        }
    }

    // Twilight Forest CustomDensitySource starts through this chunk
    // (StructureManager.startsForStructure(chunkPos, CustomDensitySource)).
    std::vector<TwilightTerraformer> customDensities;
    for (const auto& [name, refs] : chunk->getAllStructureReferences()) {
        const StructureInfo& info = StructureSets::structureByName(name);
        if (!TwilightStructures::isTwilightType(info.type)) continue;
        for (int64_t ref : fastutilLongSetOrder(refs)) {
            ::world::ChunkPos refPos = ::world::ChunkPos::fromLong(ref);
            int gridX = refPos.x() - centerX + gridRadius;
            int gridZ = refPos.z() - centerZ + gridRadius;
            if (gridZ < 0 || gridZ >= gridSize) continue;
            if (gridX < 0 || gridX >= static_cast<int>(chunks[gridZ].size())) continue;
            ::world::IChunk* source = chunks[gridZ][gridX];
            if (source == nullptr) continue;
            const StructureStartData* start = source->getStartForStructure(name);
            if (start == nullptr || !start->isValid()) continue;
            TwilightTerraformer terraformer = TwilightStructures::terraformer(info, *start, centerPos);
            if (terraformer) customDensities.push_back(std::move(terraformer));
        }
    }

    if (!haveBox && customDensities.empty()) {
        return nullptr;  // Java: Beardifier.EMPTY
    }
    std::optional<levelgen::BoundingBox> affected;
    if (haveBox) affected = anyPieceBox.inflatedBy(24);
    auto beardifier = std::make_shared<const levelgen::density::Beardifier>(std::move(rigids), std::move(junctions),
                                                                            affected);
    if (!customDensities.empty()) {
        return std::make_shared<const TwilightCustomTerrainBeardifier>(std::move(beardifier),
                                                                       std::move(customDensities));
    }
    return beardifier;
}

void recordSpawnOverrideAreas(WorldGenLevel* level, ::world::IChunk* chunk) {
    if (level == nullptr || chunk == nullptr) return;
    const auto& referenceMap = chunk->getAllStructureReferences();
    if (referenceMap.empty()) return;

    const ::world::ChunkPos pos = chunk->getPos();
    const int minX = pos.x() * 16;
    const int minZ = pos.z() * 16;
    for (const auto& [name, refs] : referenceMap) {
        if (!StructureSets::structureByName(name).hasSpawnOverrides) continue;
        for (int64_t ref : refs) {
            ::world::ChunkPos refPos = ::world::ChunkPos::fromLong(ref);
            ::world::IChunk* source = level->getChunk(refPos.x(), refPos.z());
            if (source == nullptr) continue;
            const StructureStartData* start = source->getStartForStructure(name);
            if (start == nullptr || !start->isValid()) continue;

            ::world::IChunk::StructureSpawnArea area;
            area.structure = name;
            // Reference: StructureStart.getBoundingBox -> PiecesContainer
            // .calculateBoundingBox = BoundingBox.encapsulatingBoxes(pieces).
            area.startBox = start->pieces.front().boundingBox;
            for (const StructurePieceData& piece : start->pieces) {
                const BoundingBox& b = piece.boundingBox;
                area.startBox.minX = std::min(area.startBox.minX, b.minX);
                area.startBox.minY = std::min(area.startBox.minY, b.minY);
                area.startBox.minZ = std::min(area.startBox.minZ, b.minZ);
                area.startBox.maxX = std::max(area.startBox.maxX, b.maxX);
                area.startBox.maxY = std::max(area.startBox.maxY, b.maxY);
                area.startBox.maxZ = std::max(area.startBox.maxZ, b.maxZ);
                if (!b.intersects(minX, minZ, minX + 15, minZ + 15)) continue;
                ::world::IChunk::StructureSpawnPiece spawnPiece;
                spawnPiece.box = b;
                spawnPiece.pieceType = piece.pieceType;
                // The P-line rotation name, back to the Rotation ordinal.
                if (piece.rotation == "CLOCKWISE_90") spawnPiece.rotation = 1;
                else if (piece.rotation == "CLOCKWISE_180") spawnPiece.rotation = 2;
                else if (piece.rotation == "COUNTERCLOCKWISE_90") spawnPiece.rotation = 3;
                if (piece.poolElement) {
                    // detail = "<element type>:<location>#<ground delta>..." -
                    // the element type id is itself namespaced, so the
                    // location starts after the SECOND colon.
                    const std::string& detail = piece.detail;
                    const size_t first = detail.find(':');
                    const size_t second =
                        first == std::string::npos ? std::string::npos : detail.find(':', first + 1);
                    const size_t hash = detail.find('#');
                    if (second != std::string::npos && hash != std::string::npos && hash > second) {
                        spawnPiece.templateId = detail.substr(second + 1, hash - second - 1);
                        if (spawnPiece.templateId == "-") spawnPiece.templateId.clear();
                    }
                }
                area.pieces.push_back(std::move(spawnPiece));
            }
            chunk->addStructureSpawnArea(std::move(area));
        }
    }
}

} // namespace StructureGeneration
} // namespace structure
} // namespace levelgen
} // namespace minecraft
