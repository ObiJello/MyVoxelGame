#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenTweaks.h"
#include "util/TerrainProfiling.h"
#include "world/biome/FixedBiomeSource.h"
#include "levelgen/WorldGenLevel.h"
#include <mutex>
#include <set>
#include <string>
#include "levelgen/FeatureSorter.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/SurfaceSystem.h"
#include "core/SectionPos.h"
#include "levelgen/SurfaceRules.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/RandomState.h"
#include "levelgen/Blender.h"
#include "levelgen/Beardifier.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Aquifer.h"
#include "levelgen/carver/CaveWorldCarver.h"
#include "levelgen/carver/NetherWorldCarver.h"
#include "levelgen/carver/CanyonWorldCarver.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/ConfiguredWorldCarver.h"
#include "world/ProtoChunk.h"
#include "world/LevelChunkSection.h"
#include "world/biome/Biome.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biomes.h"
#include "random/RandomSupport.h"
#include "random/LegacyRandomSource.h"
#include "math/Mth.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/StructurePieceBehavior.h"

// Reference: net/minecraft/world/level/chunk/ChunkGenerator.java

namespace minecraft {
namespace levelgen {

//=============================================================================
// ChunkGenerator
//=============================================================================

// Feature logging control - static variables
static bool s_featureLoggingEnabled = false;
static std::ostream* s_featureLogStream = &std::cerr;
static int s_logLevel = 0;  // 0=off, 1=basic, 2=detailed (positions), 3=verbose (random state)

void ChunkGenerator::setFeatureLoggingEnabled(bool enabled, int level) {
    s_featureLoggingEnabled = enabled;
    s_logLevel = level;
}

void ChunkGenerator::setFeatureLogStream(std::ostream* stream) {
    s_featureLogStream = stream ? stream : &std::cerr;
}

void ChunkGenerator::applyBiomeDecoration(
    WorldGenLevel* level,
    ::world::IChunk* chunk,
    const std::vector<StepFeatureData>& featuresPerStep
) {
    // Reference: ChunkGenerator.java lines 269-375

    ::world::ChunkPos centerPos = chunk->getPos();

    // Get origin at the corner of the chunk at min section Y
    // Reference: SectionPos.of(centerPos, level.getMinSectionY()).origin()
    // origin = (chunkX * 16, minSectionY * 16, chunkZ * 16)
    int32_t minSectionY = level->getMinY() >> 4;
    core::BlockPos origin(
        centerPos.getMinBlockX(),
        minSectionY << 4,  // sectionY * 16
        centerPos.getMinBlockZ()
    );

    // Get seed from level
    int64_t seed = level->getSeed();

    // Create random source with unique seed
    // Reference: new WorldgenRandom(new XoroshiroRandomSource(RandomSupport.generateUniqueSeed()))
    XoroshiroRandomSource randomSource{RandomSupport::generateUniqueSeed()};
    WorldgenRandom random{randomSource};

    // Set decoration seed based on chunk position
    // Reference: random.setDecorationSeed(level.getSeed(), origin.getX(), origin.getZ())
    int64_t decorationSeed = random.setDecorationSeed(seed, origin.getX(), origin.getZ());

    // Block trace: log decoration seed and chunk info
    if (feature::BlockChangeTrace::enabled && feature::BlockChangeTrace::stream) {
        *feature::BlockChangeTrace::stream << "# CHUNK (" << centerPos.x() << ", " << centerPos.z() << ")\n";
        *feature::BlockChangeTrace::stream << "# Origin: " << origin.getX() << ", " << origin.getY() << ", " << origin.getZ() << "\n";
        *feature::BlockChangeTrace::stream << "# DecorationSeed: " << decorationSeed << "\n";
        *feature::BlockChangeTrace::stream << "# WorldSeed: " << seed << "\n\n";
    }

    // Feature logging - chunk header
    if (s_featureLoggingEnabled && s_featureLogStream) {
        *s_featureLogStream << "\n# CHUNK (" << centerPos.x() << ", " << centerPos.z() << ")\n";
        *s_featureLogStream << "# Origin: " << origin.getX() << ", " << origin.getY() << ", " << origin.getZ() << "\n";
        *s_featureLogStream << "# WorldSeed: " << seed << "\n";
        *s_featureLogStream << "# DecorationSeed: " << decorationSeed << "\n\n";
    }

    // Collect biomes from 3x3 chunk area.
    // Reference: ChunkPos.rangeClosed(sectionPos.chunk(), 1).forEach(...)
    // Java iterates each section's stored biome palette values, then retains only
    // biomes present in biomeSource.possibleBiomes().
    std::set<const world::biome::Biome*> possibleBiomes;
    std::set<const world::biome::Biome*> candidateBiomes;
    world::biome::BiomeSource* biomeSource = nullptr;
    if (auto* noiseBasedGenerator = dynamic_cast<NoiseBasedChunkGenerator*>(this)) {
        biomeSource = noiseBasedGenerator->getBiomeSource();
    }
    const auto& sourcePossibleBiomes =
        biomeSource ? biomeSource->possibleBiomes() : std::set<world::biome::BiomeKey>{};
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            ::world::IChunk* neighborChunk = level->getChunk(centerPos.x() + dx, centerPos.z() + dz);
            if (neighborChunk) {
                for (int32_t sectionIndex = 0; sectionIndex < neighborChunk->getSectionsCount(); ++sectionIndex) {
                    const world::LevelChunkSection& section = neighborChunk->getSection(sectionIndex);
                    // Dedupe by pointer FIRST: the name lookup ran once per
                    // biome entry of every section of the 3x3 (measured
                    // 2026-08-30 as most of the string work in decoration).
                    for (const world::biome::Biome* biome : section.getBiomes()) {
                        if (biome == nullptr) {
                            continue;
                        }
                        candidateBiomes.insert(biome);
                    }
                }
            }
        }
    }

    // Get number of generation steps
    for (const world::biome::Biome* biome : candidateBiomes) {
        if (biomeSource &&
            sourcePossibleBiomes.find(biome->getName()) == sourcePossibleBiomes.end()) {
            continue;
        }
        possibleBiomes.insert(biome);
    }
    int32_t featureStepCount = static_cast<int32_t>(featuresPerStep.size());
    int32_t generationSteps = std::max(GenerationStep::DECORATION_COUNT, featureStepCount);

    // Structures grouped by GenerationStep ordinal, registry (alphabetical id)
    // order within each step. Reference: structuresRegistry.stream().collect(
    // Collectors.groupingBy(s -> s.step().ordinal())) - groupingBy preserves
    // encounter order.
    static const std::vector<std::vector<const structure::StructureInfo*>>& s_structuresByStep =
        []() -> const std::vector<std::vector<const structure::StructureInfo*>>& {
            static std::vector<std::vector<const structure::StructureInfo*>> byStep(
                static_cast<size_t>(GenerationStep::DECORATION_COUNT));
            for (const structure::StructureInfo* info : structure::StructureSets::allStructures()) {
                for (int32_t ord = 0; ord < GenerationStep::DECORATION_COUNT; ++ord) {
                    if (GenerationStep::getName(static_cast<GenerationStep::Decoration>(ord))
                        == info->step) {
                        byStep[static_cast<size_t>(ord)].push_back(info);
                        break;
                    }
                }
            }
            return byStep;
        }();

    // Writable area for structure placement.
    // Reference: getWritableArea(chunk) - y in [minY+1, maxY].
    structure::BoundingBox writableArea(
        centerPos.getMinBlockX(), level->getMinY() + 1, centerPos.getMinBlockZ(),
        centerPos.getMinBlockX() + 15, level->getMaxY(), centerPos.getMinBlockZ() + 15);

    // Iterate through all generation steps
    // Reference: for(int stepIndex = 0; stepIndex < generationSteps; ++stepIndex)
    for (int32_t stepIndex = 0; stepIndex < generationSteps; ++stepIndex) {
        // Structure pass BEFORE features within each step.
        // Reference: ChunkGenerator.java:299-322 - index counter restarts per
        // step; setFeatureSeed per STRUCTURE (even when it has no starts here).
        if (m_generateStructures && stepIndex < GenerationStep::DECORATION_COUNT) {
            // Structure pieces are shared by every chunk the structure spans
            // and their placement mutates piece state, so when decoration runs
            // on several pool threads (ChunkStatusTasks::generateFeatures) the
            // structure part must still be serialised — this is the one lock
            // vanilla's single worldgen lane provided implicitly. Ordinary
            // features (trees, ores, patches) below stay parallel.
            static std::mutex s_structurePlacementMutex;
            std::lock_guard<std::mutex> structureLock(s_structurePlacementMutex);
            int32_t index = 0;
            for (const structure::StructureInfo* info :
                 s_structuresByStep[static_cast<size_t>(stepIndex)]) {
                random.setFeatureSeed(decorationSeed, index, stepIndex);
                // Reference: structureManager.startsForStructure(sectionPos,
                // structure) - center chunk's refs -> starts in ref chunks,
                // iterated in Java's LongOpenHashSet order (starts share the
                // per-structure random; order is draw-stream-load-bearing).
                for (int64_t ref : structure::fastutilLongSetOrder(
                         chunk->getReferencesForStructure(info->name))) {
                    ::world::ChunkPos refPos = ::world::ChunkPos::fromLong(ref);
                    ::world::IChunk* refChunk = level->getChunk(refPos.x(), refPos.z());
                    if (refChunk == nullptr) continue;
                    structure::StructureStartData* start =
                        refChunk->getMutableStartForStructure(info->name);
                    if (start == nullptr || !start->isValid()) continue;
                    // Reference: StructureStart.placeInChunk().
                    const structure::BoundingBox& firstBox = start->pieces[0].boundingBox;
                    core::BlockPos referencePos(firstBox.centerX(), firstBox.minY,
                                                firstBox.centerZ());
                    // Worker-task exceptions never complete the chunk future
                    // (pipeline hang with idle workers), so the exception must
                    // not be allowed to escape this loop. It is caught and the
                    // structure SKIPPED rather than aborting the process.
                    //
                    // This used to std::abort(). That is the right call for a
                    // complete implementation — Java crashes here too — but
                    // this is a port, and the data is vanilla's: the first time
                    // a player walks into content using a processor, predicate
                    // or piece type that is not implemented yet, an abort takes
                    // the whole game down with them. A bastion that fails to
                    // place is a missing bastion; an abort is a lost session.
                    //
                    // Catching HERE rather than not catching at all is what
                    // keeps the original concern satisfied: the loop finishes
                    // normally, so the chunk future still completes and the
                    // pipeline never stalls.
                    try {
                        for (size_t pieceIndex = 0; pieceIndex < start->pieces.size(); ++pieceIndex) {
                            structure::StructurePieceData& piece = start->pieces[pieceIndex];
                            if (!piece.boundingBox.intersects(writableArea)) continue;
                            if (pieceIndex < start->behaviors.size() && start->behaviors[pieceIndex]) {
                                start->behaviors[pieceIndex]->postProcess(
                                    level, this, random, writableArea, centerPos,
                                    referencePos, piece);
                            }
                        }
                        // Reference: this.structure.afterPlace(...) - null
                        // means Java's default no-op.
                        if (start->afterPlace) {
                            start->afterPlace(level, this, random, writableArea,
                                              centerPos, *start);
                        }
                    } catch (const std::exception& e) {
                        // Once per (structure, reason), not once per chunk —
                        // an unimplemented feature repeats for every chunk the
                        // structure touches, and a wall of identical lines
                        // buries whatever else went wrong that session.
                        static std::mutex s_reportedMutex;
                        static std::set<std::string> s_reported;
                        const std::string key = info->name + '|' + e.what();
                        bool first = false;
                        {
                            std::lock_guard<std::mutex> lock(s_reportedMutex);
                            first = s_reported.insert(key).second;
                        }
                        if (first) {
                            std::cerr << "[structures] SKIPPED " << info->name
                                      << " near chunk (" << centerPos.x() << ", "
                                      << centerPos.z() << "): " << e.what()
                                      << " -- further occurrences suppressed"
                                      << std::endl;
                        }
                    }
                }
                ++index;
            }
        }

        if (stepIndex >= featureStepCount) {
            continue;
        }

        // World Properties: per-step feature toggle (non-vanilla; the
        // structure pass above already ran, mirroring the separate
        // structures option).
        if (stepIndex < static_cast<int32_t>(WorldGenTweaks::get().featureStepEnabled.size())
            && !WorldGenTweaks::get().featureStepEnabled[static_cast<size_t>(stepIndex)]) {
            continue;
        }

        const StepFeatureData& stepFeatureData = featuresPerStep[stepIndex];
        if (stepFeatureData.features.empty()) continue;

        // Collect possible feature indices for this step from biomes in 3x3 area
        // Reference: IntSet possibleFeaturesThisStep = new IntArraySet();
        std::set<int> possibleFeatureIndices;

        for (const world::biome::Biome* biome : possibleBiomes) {
            if (!biome) continue;
            // Reference: generationSettingsGetter.apply(biome).features() -
            // per-generator override (flat worlds) falls back to the registry.
            static const std::vector<const placement::PlacedFeature*> s_noFeatures;
            const auto* adjusted = featuresForBiomeOverride(biome->getName());
            const auto& featuresInBiomeThisStep = adjusted
                ? (stepIndex < static_cast<int32_t>(adjusted->size())
                       ? (*adjusted)[stepIndex] : s_noFeatures)
                : data::worldgen::BiomeFeatureRegistry::getFeaturesForStep(biome->getName(), stepIndex);
            for (const placement::PlacedFeature* feature : featuresInBiomeThisStep) {
                int idx = stepFeatureData.getIndex(const_cast<placement::PlacedFeature*>(feature));
                if (idx >= 0) {
                    possibleFeatureIndices.insert(idx);
                }
            }
        }

        // Sort indices and place features
        // Reference: int[] indexArray = possibleFeaturesThisStep.toIntArray(); Arrays.sort(indexArray);
        std::vector<int> sortedIndices(possibleFeatureIndices.begin(), possibleFeatureIndices.end());
        std::sort(sortedIndices.begin(), sortedIndices.end());

        // Log step header if enabled
        if (s_featureLoggingEnabled && s_featureLogStream && s_logLevel >= 1) {
            *s_featureLogStream << "# ===== STEP " << stepIndex << " (" << sortedIndices.size() << " features) =====\n";
        }

        // Place each feature using global index for seeding
        for (int globalIndex : sortedIndices) {
            if (globalIndex < 0 || globalIndex >= static_cast<int>(stepFeatureData.features.size())) {
                continue;
            }

            placement::PlacedFeature* feature = stepFeatureData.features[globalIndex];
            if (!feature) continue;

            // Set current step/index for logging and block-change tracing
            placement::PlacedFeature::setCurrentStepIndex(stepIndex, globalIndex);
            feature::BlockChangeTrace::currentStep = stepIndex;
            feature::BlockChangeTrace::currentIndex = globalIndex;
            feature::BlockChangeTrace::currentFeatureName = feature->getDebugName();

            // Set feature seed using GLOBAL index (critical for parity)
            // Reference: random.setFeatureSeed(decorationSeed, globalIndexOfFeature, stepIndex);
            random.setFeatureSeed(decorationSeed, globalIndex, stepIndex);

            // Block trace: log seed state after setFeatureSeed
            if (feature::BlockChangeTrace::enabled && feature::BlockChangeTrace::stream) {
                uint64_t seedLo, seedHi;
                random.getSeedState(seedLo, seedHi);
                *feature::BlockChangeTrace::stream << "FEATURE STEP=" << stepIndex << " IDX=" << globalIndex
                    << " " << feature->getDebugName()
                    << " seed_lo=" << seedLo << " seed_hi=" << seedHi
                    << " gauss_cached=" << random.hasNextGaussian() << "\n";
            }

            // Log feature info with random state if enabled
            if (s_featureLoggingEnabled && s_featureLogStream && s_logLevel >= 2) {
                const std::string featureName = feature->getDebugName();
                *s_featureLogStream << "FEATURE STEP=" << stepIndex << " IDX=" << globalIndex
                                    << " " << (featureName.empty() ? "(unnamed)" : featureName) << "\n";

                // Log random state for verbose mode
                if (s_logLevel >= 3) {
                    uint64_t seedLo, seedHi;
                    random.getSeedState(seedLo, seedHi);
                    *s_featureLogStream << "  SEED_LO=" << static_cast<int64_t>(seedLo)
                                        << " SEED_HI=" << static_cast<int64_t>(seedHi) << "\n";
                }
            }

            // Place the feature with biome check
            // Reference: feature.placeWithBiomeCheck(level, this, random, origin);
            feature->placeWithBiomeCheck(level, this, random, origin);
        }
    }

    // Log completion
    if (s_featureLoggingEnabled && s_featureLogStream && s_logLevel >= 1) {
        *s_featureLogStream << "# END CHUNK (" << centerPos.x() << ", " << centerPos.z() << ")\n\n";
    }
}

bool ChunkGenerator::hasFeatureInBiome(const std::string& biomeKey,
                                       const placement::PlacedFeature* feature) const {
    // Reference: getBiomeGenerationSettings(biome).hasFeature(feature) - the
    // registry holds the vanilla per-biome lists; flat worlds override this.
    return data::worldgen::BiomeFeatureRegistry::hasFeature(biomeKey, feature);
}

void ChunkGenerator::getWritableArea(
    const ::world::IChunk* chunk,
    int32_t& minX, int32_t& minY, int32_t& minZ,
    int32_t& maxX, int32_t& maxY, int32_t& maxZ
) {
    // Reference: ChunkGenerator.java lines 377-385
    ::world::ChunkPos chunkPos = chunk->getPos();
    minX = chunkPos.getMinBlockX();
    minZ = chunkPos.getMinBlockZ();
    minY = chunk->getMinBuildHeight() + 1;
    maxX = minX + 15;
    maxZ = minZ + 15;
    maxY = chunk->getMaxBuildHeight();
}

//=============================================================================
// NoiseBasedChunkGenerator
//=============================================================================

const std::vector<StepFeatureData>* NoiseBasedChunkGenerator::customFeaturesPerStep() {
    // Reference: ChunkGenerator.featuresPerStep - built from THIS generator's
    // biomeSource.possibleBiomes(). Only single-biome (FixedBiomeSource)
    // worlds need a per-generator build; dimension-wide sources keep the
    // parity-proven static builds in the callers (nullptr).
    auto* fixed = dynamic_cast<world::biome::FixedBiomeSource*>(m_biomeSource);
    if (!fixed) return nullptr;
    if (!m_singleBiomeFeaturesBuilt) {
        std::vector<std::string> keys{fixed->biome()};
        m_singleBiomeFeaturesPerStep = FeatureSorter::buildFeaturesPerStep<std::string>(
            keys,
            [this](const std::string& biomeKey) -> std::vector<std::vector<placement::PlacedFeature*>> {
                const auto* adjusted = featuresForBiomeOverride(biomeKey);
                const auto& features = adjusted
                    ? *adjusted
                    : data::worldgen::BiomeFeatureRegistry::getFeaturesForBiome(biomeKey);
                std::vector<std::vector<placement::PlacedFeature*>> result;
                result.reserve(features.size());
                for (const auto& stepFeatures : features) {
                    std::vector<placement::PlacedFeature*> step;
                    step.reserve(stepFeatures.size());
                    for (const auto* f : stepFeatures) {
                        step.push_back(const_cast<placement::PlacedFeature*>(f));
                    }
                    result.push_back(std::move(step));
                }
                return result;
            },
            true);
        m_singleBiomeFeaturesBuilt = true;
    }
    return &m_singleBiomeFeaturesPerStep;
}

NoiseBasedChunkGenerator::NoiseBasedChunkGenerator(
    NoiseGeneratorSettings* settings,
    SurfaceSystem* surfaceSystem,
    RuleSource* surfaceRules,
    BlockState* defaultBlock,
    BlockState* airBlock,
    FluidPicker* fluidPicker,
    Beardifier* beardifier
)
    : m_seaLevel(settings ? settings->seaLevel() : 63)
    , m_minY(settings ? settings->noiseSettings().minY() : -64)
    , m_height(settings ? settings->noiseSettings().height() : 384)
    , m_cellWidth(settings ? settings->noiseSettings().getCellWidth() : 4)
    , m_cellHeight(settings ? settings->noiseSettings().getCellHeight() : 8)
    , m_surfaceSystem(surfaceSystem)
    , m_surfaceRules(surfaceRules)
    , m_defaultBlock(defaultBlock)
    , m_airBlock(airBlock)
    , m_fluidPicker(fluidPicker)
    , m_beardifier(beardifier)
    , m_settings(settings)
    , m_biomeSource(nullptr)
{}

NoiseBasedChunkGenerator::NoiseBasedChunkGenerator(
    int32_t seaLevel,
    int32_t minY,
    int32_t height,
    int32_t cellWidth,
    int32_t cellHeight,
    SurfaceSystem* surfaceSystem,
    RuleSource* surfaceRules,
    BlockState* defaultBlock,
    BlockState* airBlock,
    FluidPicker* fluidPicker,
    Beardifier* beardifier
)
    : m_seaLevel(seaLevel)
    , m_minY(minY)
    , m_height(height)
    , m_cellWidth(cellWidth)
    , m_cellHeight(cellHeight)
    , m_surfaceSystem(surfaceSystem)
    , m_surfaceRules(surfaceRules)
    , m_defaultBlock(defaultBlock)
    , m_airBlock(airBlock)
    , m_fluidPicker(fluidPicker)
    , m_beardifier(beardifier)
    , m_settings(nullptr)
    , m_biomeSource(nullptr)
{}

namespace {

// Reference: NoiseBasedChunkGenerator.createNoiseChunk passes
// Beardifier.forStructuresInChunk(structureManager, chunk.getPos()). The C++
// per-chunk Beardifier is built by the chunk-status tasks (where the
// dependency grid lives) and stored on the ProtoChunk; chunks without one
// (structures off, or no adapted structure nearby) fall back to EMPTY.
Beardifier* beardifierForChunk(::world::IChunk* chunk, Beardifier* generatorFallback) {
    if (auto* proto = dynamic_cast<minecraft::world::ProtoChunk*>(chunk)) {
        if (proto->structureBeardifier() != nullptr) {
            return proto->structureBeardifier();
        }
    }
    return generatorFallback != nullptr ? generatorFallback : Beardifier::EMPTY();
}

} // namespace

NoiseBasedChunkGenerator::NoiseBasedChunkGenerator(
    int32_t seaLevel,
    int32_t minY,
    int32_t height,
    SurfaceSystem* surfaceSystem,
    RuleSource* surfaceRules
)
    : m_seaLevel(seaLevel)
    , m_minY(minY)
    , m_height(height)
    , m_cellWidth(4)
    , m_cellHeight(8)
    , m_surfaceSystem(surfaceSystem)
    , m_surfaceRules(surfaceRules)
    , m_defaultBlock(nullptr)
    , m_airBlock(nullptr)
    , m_fluidPicker(nullptr)
    , m_beardifier(nullptr)
    , m_settings(nullptr)
    , m_biomeSource(nullptr)
{}

void NoiseBasedChunkGenerator::fillFromNoise(
    RandomState* randomState,
    Blender* blender,
    ::world::IChunk* chunk
) {
    // Reference: NoiseBasedChunkGenerator.java fillFromNoise() lines 233-261

    // Calculate cell dimensions
    // Reference: lines 234-237
    int32_t cellMinY = Mth::floorDiv(m_minY, m_cellHeight);
    int32_t cellCountY = Mth::floorDiv(m_height, m_cellHeight);
    if (cellCountY <= 0) {
        return;
    }

    // Call internal fill method
    doFill(blender, randomState, chunk, cellMinY, cellCountY);
}

void NoiseBasedChunkGenerator::doFill(
    Blender* blender,
    RandomState* randomState,
    ::world::IChunk* chunk,
    int32_t cellMinY,
    int32_t cellCountY
) {
    // Reference: NoiseBasedChunkGenerator.java doFill() lines 263-337

    // Cast to ProtoChunk to access sections
    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (!protoChunk) {
        return;  // Not a ProtoChunk, can't fill
    }

    // Get or create NoiseChunk - cached on ProtoChunk for reuse across stages
    // Reference: NoiseBasedChunkGenerator.java doFill() line 264
    // Java: protoChunk.getOrCreateNoiseChunk((chunk) -> this.createNoiseChunk(...))
    // Note: Always use Blender::empty() for cached NoiseChunk - the blender parameter
    // is only used for world upgrades (blending old chunks), not new generation
    NoiseChunk* noiseChunk;
    {
        noiseChunk = protoChunk->getOrCreateNoiseChunk([this, randomState](::world::IChunk* c) {
            NoiseGeneratorSettings defaultSettings;
            const NoiseGeneratorSettings& settingsRef = m_settings ? *m_settings : defaultSettings;
            return NoiseChunk::forChunk(
                c,
                *randomState,
                beardifierForChunk(c, m_beardifier),
                settingsRef,
                m_fluidPicker,
                Blender::empty()
            );
        });
    }

    // Get heightmaps
    // Reference: lines 265-266
    Heightmap& oceanFloor = protoChunk->getOrCreateHeightmap(Heightmap::Types::OCEAN_FLOOR_WG);
    Heightmap& worldSurface = protoChunk->getOrCreateHeightmap(Heightmap::Types::WORLD_SURFACE_WG);

    // Get chunk position
    // Reference: lines 267-269
    ::world::ChunkPos chunkPos = chunk->getPos();
    int32_t chunkStartBlockX = chunkPos.getMinBlockX();
    int32_t chunkStartBlockZ = chunkPos.getMinBlockZ();

    // Get aquifer for fluid scheduling
    // Reference: line 270
    Aquifer* aquifer = noiseChunk->aquifer();

    // Initialize interpolation
    // Reference: line 271
    {
        noiseChunk->initializeForFirstCellX();
    }

    // Cell dimensions
    // Reference: lines 273-276
    int32_t cellWidth = m_cellWidth;
    int32_t cellHeight = m_cellHeight;
    int32_t cellCountX = 16 / cellWidth;  // Usually 4
    int32_t cellCountZ = 16 / cellWidth;  // Usually 4

    // Profile the entire main loop

    // OUTER LOOP: X cells (0 to cellCountX-1, ascending)
    // Reference: lines 278-332
    for (int32_t cellXIndex = 0; cellXIndex < cellCountX; ++cellXIndex) {
        noiseChunk->advanceCellX(cellXIndex);

        // MIDDLE LOOP: Z cells (0 to cellCountZ-1, ascending)
        // Reference: lines 281-330
        for (int32_t cellZIndex = 0; cellZIndex < cellCountZ; ++cellZIndex) {
            int32_t lastSectionIndex = protoChunk->getSectionsCount() - 1;
            world::LevelChunkSection* section = &protoChunk->getSection(lastSectionIndex);

            // INNER LOOP: Y cells (DESCENDING: cellCountY-1 to 0)
            // Reference: lines 285-329
            for (int32_t cellYIndex = cellCountY - 1; cellYIndex >= 0; --cellYIndex) {
                noiseChunk->selectCellYZ(cellYIndex, cellZIndex);

                // Within-cell Y (DESCENDING: cellHeight-1 to 0)
                // Reference: lines 288-328
                for (int32_t yInCell = cellHeight - 1; yInCell >= 0; --yInCell) {
                    int32_t posY = (cellMinY + cellYIndex) * cellHeight + yInCell;
                    int32_t yInSection = posY & 15;
                    int32_t sectionIndex = protoChunk->getSectionIndex(posY);

                    // Switch section if needed
                    // Reference: lines 292-295
                    if (lastSectionIndex != sectionIndex) {
                        lastSectionIndex = sectionIndex;
                        section = &protoChunk->getSection(sectionIndex);
                    }

                    // Update interpolation for Y
                    // Reference: lines 297-298
                    double factorY = static_cast<double>(yInCell) / static_cast<double>(cellHeight);
                    noiseChunk->updateForY(posY, factorY);

                    // Within-cell X (0 to cellWidth-1, ascending)
                    // Reference: lines 300-327
                    for (int32_t xInCell = 0; xInCell < cellWidth; ++xInCell) {
                        int32_t posX = chunkStartBlockX + cellXIndex * cellWidth + xInCell;
                        int32_t xInSection = posX & 15;

                        // Update interpolation for X
                        // Reference: lines 303-304
                        double factorX = static_cast<double>(xInCell) / static_cast<double>(cellWidth);
                        noiseChunk->updateForX(posX, factorX);

                        // Within-cell Z (0 to cellWidth-1, ascending)
                        // Reference: lines 306-326
                        for (int32_t zInCell = 0; zInCell < cellWidth; ++zInCell) {
                            int32_t posZ = chunkStartBlockZ + cellZIndex * cellWidth + zInCell;
                            int32_t zInSection = posZ & 15;

                            // Update interpolation for Z
                            // Reference: lines 309-310
                            double factorZ = static_cast<double>(zInCell) / static_cast<double>(cellWidth);
                            noiseChunk->updateForZ(posZ, factorZ);

                            // Get interpolated block state
                            // Reference: lines 311-314
                            BlockState* state = noiseChunk->getInterpolatedState();

                            // If state is null, use defaultBlock (stone)
                            // Reference: Java lines 312-314
                            // Java: if (state == null) { state = settings.defaultBlock(); }
                            if (state == nullptr) {
                                state = m_defaultBlock;
                            }

                            // Skip air blocks
                            // Reference: lines 317-324
                            // Java: if (state != AIR && !SharedConstants.debugVoidTerrain(chunkPos)) { ... }
                            if (state != nullptr && !state->isAir()) {
                                // Set block in section
                                section->setBlockState(xInSection, yInSection, zInSection, state, false);

                                // Update heightmaps
                                // Reference: lines 319-320
                                oceanFloor.update(xInSection, posY, zInSection, state);
                                worldSurface.update(xInSection, posY, zInSection, state);

                                // Mark fluids for post-processing
                                // Reference: lines 321-324
                                if (aquifer && aquifer->shouldScheduleFluidUpdate() && state->isFluid()) {
                                    core::BlockPos blockPos(posX, posY, posZ);
                                    chunk->markPosForPostprocessing(blockPos);
                                }
                            }
                        }
                    }
                }
            }
        }

        // CRITICAL: Swap slices after each X column
        // Reference: line 332
        noiseChunk->swapSlices();
    }

    // Stop interpolation
    // Reference: line 335
    noiseChunk->stopInterpolation();

    // Clean up
    // TODO: Fix NoiseChunk destructor crash - skip deletion for now
    // delete noiseChunk;
}

void NoiseBasedChunkGenerator::applyCarvers(
    int64_t seed,
    RandomState* randomState,
    std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
    ::world::IChunk* chunk,
    GenerationStep::Decoration step
) {
    // World Properties: caves/canyons toggle (non-vanilla; default true).
    if (!WorldGenTweaks::get().carversEnabled) return;

    // Reference: NoiseBasedChunkGenerator.java applyCarvers() lines 198-231

    // Cast to ProtoChunk to access carving mask
    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (!protoChunk) {
        return;
    }

    // Get chunk position
    ::world::ChunkPos chunkPos = chunk->getPos();

    // Get or create carving mask - Reference: line 207
    carver::CarvingMask& mask = protoChunk->getOrCreateCarvingMask();

    // Get or create NoiseChunk for aquifer - cached on ProtoChunk
    // Reference: NoiseBasedChunkGenerator.java applyCarvers() line 204-205
    NoiseChunk* noiseChunk = protoChunk->getOrCreateNoiseChunk([this, randomState](::world::IChunk* c) {
        NoiseGeneratorSettings defaultSettings;
        const NoiseGeneratorSettings& settingsRef = m_settings ? *m_settings : defaultSettings;
        return NoiseChunk::forChunk(
            c,
            *randomState,
            beardifierForChunk(c, m_beardifier),
            settingsRef,
            m_fluidPicker,
            Blender::empty()
        );
    });
    Aquifer* aquifer = noiseChunk->aquifer();

    // Create carving context with proper arguments
    // Note: surfaceRule is needed for topMaterial() when carvers carve grass_block
    // CRITICAL: Java WorldGenerationContext = (max(level minY, generator minY),
    // min(level height, generator getGenDepth = noiseSettings height)).
    // In the nether the dimension is 256 tall but noise settings height is 128 —
    // this clamps the carve ceiling (belowTop anchors, maxY-7) to y<=120.
    int32_t ctxMinY = std::max(chunk->getMinBuildHeight(), m_minY);
    int32_t ctxHeight = std::min(chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(), m_height);
    carver::CarvingContext carvingContext(
        ctxMinY,
        ctxHeight,
        noiseChunk,
        randomState,
        m_surfaceRules
    );

    // Create WorldgenRandom with LegacyRandomSource for carver seeding - Reference: line 201
    // CRITICAL: Java uses LegacyRandomSource for carving, NOT XoroshiroRandomSource
    LegacyRandomSource legacyRandom(RandomSupport::generateUniqueSeed());

    // Range of chunks to check for carver starts - Reference: line 202
    constexpr int32_t CARVER_RANGE = 8;

    // Iterate over nearby chunks that could affect this chunk - Reference: lines 209-228
    for (int32_t dx = -CARVER_RANGE; dx <= CARVER_RANGE; ++dx) {
        for (int32_t dz = -CARVER_RANGE; dz <= CARVER_RANGE; ++dz) {
            ::world::ChunkPos sourcePos(chunkPos.x() + dx, chunkPos.z() + dz);

            // Get biome at source chunk's center position - Reference: lines 213-214
            // CRITICAL: Must query biome at each source chunk position to get its specific carvers
            core::BlockPos centerPos(sourcePos.getMinBlockX() + 8, 0, sourcePos.getMinBlockZ() + 8);
            world::biome::BiomeHolder sourceBiome = biomeGetter(centerPos);

            // Get configured carvers from biome's generation settings
            // Reference: line 214 - biome.value().getGenerationSettings().getCarvers(step)
            // Reference: Carvers.java - CAVE, CAVE_EXTRA_UNDERGROUND, CANYON
            const world::biome::BiomeGenerationSettings* genSettings = nullptr;
            bool isNetherDimension = m_settings != nullptr
                && m_settings->defaultBlock() != nullptr
                && m_settings->defaultBlock()->getIdentifier() == "minecraft:netherrack";
            if (const char* dbg = getenv("CARVER_DEBUG")) {
                (void)dbg;
                static std::once_flag dbgOnce;
                std::call_once(dbgOnce, [&] {
                    fprintf(stderr, "[carver-debug] settings=%p defaultBlock=%s isNether=%d sourceBiome=%d\n",
                            (void*)m_settings,
                            (m_settings && m_settings->defaultBlock()) ? m_settings->defaultBlock()->getIdentifier().c_str() : "<null>",
                            (int)isNetherDimension, (int)(bool)sourceBiome);
                });
            }
            if (sourceBiome && isNetherDimension) {
                // All 5 nether biomes carry exactly minecraft:nether_cave.
                // Reference: Carvers.java NETHER_CAVE - probability 0.2,
                // UniformHeight(absolute(0), belowTop(1)), yScale const 0.5,
                // lavaLevel aboveBottom(10), #nether_carver_replaceables,
                // h/v mult const 1.0, floorLevel const -0.7.
                static world::biome::BiomeGenerationSettings netherSettings;
                static std::once_flag netherCarverInitOnce;
                std::call_once(netherCarverInitOnce, [&] {
                    // BlockTags.NETHER_CARVER_REPLACEABLES resolved from
                    // data/minecraft/tags/block/nether_carver_replaceables.json
                    static std::set<std::string> netherReplaceable = {
                        // #base_stone_overworld
                        "minecraft:stone", "minecraft:granite", "minecraft:diorite",
                        "minecraft:andesite", "minecraft:tuff", "minecraft:deepslate",
                        // #base_stone_nether
                        "minecraft:netherrack", "minecraft:basalt", "minecraft:blackstone",
                        // #dirt
                        "minecraft:dirt", "minecraft:grass_block", "minecraft:podzol",
                        "minecraft:coarse_dirt", "minecraft:mycelium", "minecraft:rooted_dirt",
                        "minecraft:moss_block", "minecraft:pale_moss_block", "minecraft:mud",
                        "minecraft:muddy_mangrove_roots",
                        // #nylium
                        "minecraft:crimson_nylium", "minecraft:warped_nylium",
                        // #wart_blocks
                        "minecraft:nether_wart_block", "minecraft:warped_wart_block",
                        // direct entries
                        "minecraft:soul_sand", "minecraft:soul_soil"
                    };
                    static carver::UniformHeight netherCaveHeight(
                        VerticalAnchor::absolute(0),
                        VerticalAnchor::belowTop(1)
                    );
                    static carver::ConstantFloat netherYScale(0.5f);
                    static carver::ConstantFloat netherHorizontalMult(1.0f);
                    static carver::ConstantFloat netherVerticalMult(1.0f);
                    static carver::ConstantFloat netherFloorLevel(-0.7f);
                    static carver::CaveCarverConfiguration netherCaveConfig(
                        0.2f,
                        &netherCaveHeight,
                        &netherYScale,
                        VerticalAnchor::aboveBottom(10),
                        carver::CarverDebugSettings(),
                        netherReplaceable,
                        &netherHorizontalMult,
                        &netherVerticalMult,
                        &netherFloorLevel
                    );
                    static carver::NetherWorldCarver netherCarver;
                    static carver::ConfiguredCaveCarver configuredNetherCarver(
                        &netherCarver, netherCaveConfig);
                    netherSettings.addCarver(&configuredNetherCarver);
                });
                genSettings = &netherSettings;
            } else if (sourceBiome) {
                // Configure all 3 default overworld carvers to match Java exactly.
                // call_once: the old non-atomic bool guard raced when worker
                // threads reached CARVERS for two chunks simultaneously.
                static world::biome::BiomeGenerationSettings defaultSettings;
                static std::once_flag carverInitOnce;
                std::call_once(carverInitOnce, [&] {
                    // Replaceable blocks from BlockTags.OVERWORLD_CARVER_REPLACEABLES
                    // Extracted from Minecraft 26.1-snapshot-1 data/minecraft/tags/block/overworld_carver_replaceables.json
                    static std::set<std::string> replaceable = {
                        // #base_stone_overworld
                        "minecraft:stone", "minecraft:granite", "minecraft:diorite", "minecraft:andesite",
                        "minecraft:tuff", "minecraft:deepslate",
                        // #dirt
                        "minecraft:dirt", "minecraft:grass_block", "minecraft:podzol", "minecraft:coarse_dirt",
                        "minecraft:mycelium", "minecraft:rooted_dirt", "minecraft:moss_block", "minecraft:pale_moss_block",
                        "minecraft:mud", "minecraft:muddy_mangrove_roots",
                        // #sand
                        "minecraft:sand", "minecraft:red_sand", "minecraft:suspicious_sand",
                        // #terracotta
                        "minecraft:terracotta", "minecraft:white_terracotta", "minecraft:orange_terracotta",
                        "minecraft:magenta_terracotta", "minecraft:light_blue_terracotta", "minecraft:yellow_terracotta",
                        "minecraft:lime_terracotta", "minecraft:pink_terracotta", "minecraft:gray_terracotta",
                        "minecraft:light_gray_terracotta", "minecraft:cyan_terracotta", "minecraft:purple_terracotta",
                        "minecraft:blue_terracotta", "minecraft:brown_terracotta", "minecraft:green_terracotta",
                        "minecraft:red_terracotta", "minecraft:black_terracotta",
                        // #iron_ores
                        "minecraft:iron_ore", "minecraft:deepslate_iron_ore",
                        // #copper_ores
                        "minecraft:copper_ore", "minecraft:deepslate_copper_ore",
                        // #snow
                        "minecraft:snow", "minecraft:snow_block", "minecraft:powder_snow",
                        // Direct entries
                        "minecraft:water", "minecraft:gravel", "minecraft:suspicious_gravel",
                        "minecraft:sandstone", "minecraft:red_sandstone", "minecraft:calcite",
                        "minecraft:packed_ice", "minecraft:raw_iron_block", "minecraft:raw_copper_block"
                    };

                    // ========================================
                    // CAVE carver (carverIndex=0)
                    // Reference: Carvers.java line 33
                    // ========================================
                    static carver::UniformHeight caveHeight(
                        VerticalAnchor::aboveBottom(8),
                        VerticalAnchor::absolute(180)
                    );
                    static carver::UniformFloat caveYScale(0.1f, 0.9f);
                    static carver::UniformFloat caveHorizontalMult(0.7f, 1.4f);
                    static carver::UniformFloat caveVerticalMult(0.8f, 1.3f);
                    static carver::UniformFloat caveFloorLevel(-1.0f, -0.4f);

                    static carver::CaveCarverConfiguration caveConfig(
                        0.15f,                      // probability
                        &caveHeight,                // y height provider
                        &caveYScale,                // y scale
                        VerticalAnchor::aboveBottom(8), // lava level
                        carver::CarverDebugSettings(),
                        replaceable,
                        &caveHorizontalMult,
                        &caveVerticalMult,
                        &caveFloorLevel
                    );

                    static carver::CaveWorldCarver caveCarver;
                    static carver::ConfiguredCaveCarver configuredCaveCarver(&caveCarver, caveConfig);
                    defaultSettings.addCarver(&configuredCaveCarver);

                    // ========================================
                    // CAVE_EXTRA_UNDERGROUND carver (carverIndex=1)
                    // Reference: Carvers.java line 34
                    // ========================================
                    static carver::UniformHeight caveExtraHeight(
                        VerticalAnchor::aboveBottom(8),
                        VerticalAnchor::absolute(47)
                    );
                    static carver::UniformFloat caveExtraYScale(0.1f, 0.9f);
                    static carver::UniformFloat caveExtraHorizontalMult(0.7f, 1.4f);
                    static carver::UniformFloat caveExtraVerticalMult(0.8f, 1.3f);
                    static carver::UniformFloat caveExtraFloorLevel(-1.0f, -0.4f);

                    static carver::CaveCarverConfiguration caveExtraConfig(
                        0.07f,                      // probability (lower than main caves)
                        &caveExtraHeight,           // y height provider (lower max)
                        &caveExtraYScale,           // y scale
                        VerticalAnchor::aboveBottom(8), // lava level
                        carver::CarverDebugSettings(),
                        replaceable,
                        &caveExtraHorizontalMult,
                        &caveExtraVerticalMult,
                        &caveExtraFloorLevel
                    );

                    static carver::CaveWorldCarver caveExtraCarver;
                    static carver::ConfiguredCaveCarver configuredCaveExtraCarver(&caveExtraCarver, caveExtraConfig);
                    defaultSettings.addCarver(&configuredCaveExtraCarver);

                    // ========================================
                    // CANYON carver (carverIndex=2)
                    // Reference: Carvers.java line 35
                    // ========================================
                    static carver::UniformHeight canyonHeight(
                        VerticalAnchor::absolute(10),
                        VerticalAnchor::absolute(67)
                    );
                    static carver::ConstantFloat canyonYScale(3.0f);
                    static carver::UniformFloat canyonVerticalRotation(-0.125f, 0.125f);
                    static carver::UniformFloat canyonDistanceFactor(0.75f, 1.0f);
                    static carver::TrapezoidFloat canyonThickness(0.0f, 6.0f, 2.0f);
                    static carver::UniformFloat canyonHorizontalRadiusFactor(0.75f, 1.0f);

                    static carver::CanyonShapeConfiguration canyonShape(
                        &canyonDistanceFactor,
                        &canyonThickness,
                        3,                          // widthSmoothness
                        &canyonHorizontalRadiusFactor,
                        1.0f,                       // verticalRadiusDefaultFactor
                        0.0f                        // verticalRadiusCenterFactor
                    );

                    static carver::CanyonCarverConfiguration canyonConfig(
                        0.01f,                      // probability (rare)
                        &canyonHeight,              // y height provider
                        &canyonYScale,              // y scale
                        VerticalAnchor::aboveBottom(8), // lava level
                        carver::CarverDebugSettings(),
                        replaceable,
                        &canyonVerticalRotation,
                        canyonShape
                    );

                    static carver::CanyonWorldCarver canyonCarver;
                    static carver::ConfiguredCanyonCarver configuredCanyonCarver(&canyonCarver, canyonConfig);
                    defaultSettings.addCarver(&configuredCanyonCarver);
                });
                genSettings = &defaultSettings;
            }

            if (!genSettings) {
                continue;
            }

            // Get carvers for this biome - Reference: line 214
            const auto& carvers = genSettings->getCarvers();

            // Apply each carver - Reference: lines 217-226
            for (size_t carverIndex = 0; carverIndex < carvers.size(); ++carverIndex) {
                carver::ConfiguredCarverBase* configuredCarver = carvers[carverIndex];
                if (!configuredCarver) {
                    continue;
                }

                // Set seed for this carver at this source position - Reference: line 219
                // Java: random.setLargeFeatureSeed(SEED + (long)carverIndex, sourcePos.x, sourcePos.z);
                int64_t carverSeed = seed + static_cast<int64_t>(carverIndex);
                legacyRandom.setLargeFeatureSeed(carverSeed, sourcePos.x(), sourcePos.z());

                // Check if this chunk should start carving - Reference: line 220
                if (configuredCarver->isStartChunk(legacyRandom)) {
                    // Carve - Reference: line 222
                    configuredCarver->carve(
                        carvingContext,
                        chunk,
                        [&biomeGetter](const core::BlockPos& pos) -> void* {
                            return const_cast<world::biome::Biome*>(biomeGetter(pos));
                        },
                        legacyRandom,
                        aquifer,
                        sourcePos,
                        mask
                    );
                }
            }
        }
    }

    // Don't delete noiseChunk - it's cached on ProtoChunk and will be cleaned up there
}

void NoiseBasedChunkGenerator::buildSurface(
    RandomState* randomState,
    std::function<world::biome::BiomeHolder(const core::BlockPos&)> biomeGetter,
    ::world::IChunk* chunk
) {
    // Reference: NoiseBasedChunkGenerator.java buildSurface() lines 189-210

    if (!m_surfaceSystem || !m_surfaceRules) {
        return;
    }

    // Create WorldGenerationContext with world bounds
    // Reference: NoiseBasedChunkGenerator.java line 192
    WorldGenerationContext generationContext(m_minY, m_height);

    // Get or create NoiseChunk - cached on ProtoChunk for reuse
    // Reference: NoiseBasedChunkGenerator.java lines 195-202
    // Java: protoChunk.getOrCreateNoiseChunk((chunk) -> this.createNoiseChunk(...))
    NoiseChunk* noiseChunk;
    {
        ::world::ProtoChunk* protoChunk = dynamic_cast<::world::ProtoChunk*>(chunk);
        if (protoChunk) {
            // Use cached NoiseChunk if available
            noiseChunk = protoChunk->getOrCreateNoiseChunk([this, randomState](::world::IChunk* c) {
                NoiseGeneratorSettings defaultSettings;
                const NoiseGeneratorSettings& settingsRef = m_settings ? *m_settings : defaultSettings;
                return NoiseChunk::forChunk(
                    c,
                    *randomState,
                    beardifierForChunk(c, m_beardifier),
                    settingsRef,
                    m_fluidPicker,
                    Blender::empty()
                );
            });
        } else {
            // Fallback for non-ProtoChunk
            NoiseGeneratorSettings defaultSettings;
            const NoiseGeneratorSettings& settingsRef = m_settings ? *m_settings : defaultSettings;
            noiseChunk = NoiseChunk::forChunk(
                chunk,
                *randomState,
                beardifierForChunk(chunk, m_beardifier),
                settingsRef,
                m_fluidPicker,
                Blender::empty()
            );
        }
    }

    // Build surface using SurfaceSystem
    // Reference: NoiseBasedChunkGenerator.java lines 204-209
    // The biome getter is used for frozen ocean extension and eroded badlands extension
    {
        m_surfaceSystem->buildSurface(
            randomState,
            [&biomeGetter](const ::minecraft::core::BlockPos& pos) -> void* {
                // Convert BiomeHolder (const Biome*) to void* for SurfaceSystem
                return const_cast<void*>(static_cast<const void*>(biomeGetter(pos)));
            },
            false,  // useLegacyRandom
            generationContext,
            chunk,
            noiseChunk,
            m_surfaceRules
        );
    }

    // Don't delete noiseChunk - it's cached on ProtoChunk and will be cleaned up there
}

int32_t NoiseBasedChunkGenerator::getBaseHeight(
    int32_t x,
    int32_t z,
    Heightmap::Types heightmapType,
    RandomState* randomState
) const {
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32)
                       ^ static_cast<uint64_t>(static_cast<uint32_t>(z))
                       ^ (static_cast<uint64_t>(static_cast<int>(heightmapType)) << 60);
    {
        std::lock_guard<std::mutex> lock(m_baseHeightMutex);
        auto it = m_baseHeightCache.find(key);
        if (it != m_baseHeightCache.end()) return it->second;
    }
    const int32_t h = computeBaseHeight(x, z, heightmapType, randomState);
    std::lock_guard<std::mutex> lock(m_baseHeightMutex);
    if (m_baseHeightCache.size() > 262144) m_baseHeightCache.clear();
    m_baseHeightCache.emplace(key, h);
    return h;
}

int32_t NoiseBasedChunkGenerator::computeBaseHeight(
    int32_t x,
    int32_t z,
    Heightmap::Types heightmapType,
    RandomState* randomState
) const {
    // Reference: NoiseBasedChunkGenerator.java getBaseHeight() lines 107-109
    // Reference: iterateNoiseColumn() lines 126-181

    if (!m_settings || !randomState) {
        return m_minY;
    }

    // Get heightmap predicate
    TERRAIN_ZONE_N("NBCG.BaseHeight");
    Heightmap::OpaquePredicate isOpaque = Heightmap::getOpaquePredicate(heightmapType);

    // Calculate cell dimensions
    int32_t cellHeight = m_cellHeight;
    int32_t cellWidth = m_cellWidth;
    int32_t cellMinY = Mth::floorDiv(m_minY, cellHeight);
    int32_t cellCountY = Mth::floorDiv(m_height, cellHeight);

    if (cellCountY <= 0) {
        return m_minY;
    }

    // Calculate noise chunk position - Reference: lines 144-151
    int32_t noiseChunkX = Mth::floorDiv(x, cellWidth);
    int32_t noiseChunkZ = Mth::floorDiv(z, cellWidth);
    int32_t xInCell = Mth::floorMod(x, cellWidth);
    int32_t zInCell = Mth::floorMod(z, cellWidth);
    int32_t firstBlockX = noiseChunkX * cellWidth;
    int32_t firstBlockZ = noiseChunkZ * cellWidth;
    double factorX = static_cast<double>(xInCell) / static_cast<double>(cellWidth);
    double factorZ = static_cast<double>(zInCell) / static_cast<double>(cellWidth);

    // Create NoiseChunk for single column - Reference: line 152
    NoiseChunk* noiseChunk = new NoiseChunk(
        1,  // cellCountXZ = 1 for single column
        *randomState,
        firstBlockX,
        firstBlockZ,
        m_settings->noiseSettings(),
        m_beardifier ? m_beardifier : Beardifier::EMPTY(),
        *m_settings,
        m_fluidPicker,
        Blender::empty()
    );

    // Initialize interpolation - Reference: lines 153-154
    noiseChunk->initializeForFirstCellX();
    noiseChunk->advanceCellX(0);

    int32_t result = m_minY;  // Default if nothing found

    // Iterate Y cells from top to bottom - Reference: lines 156-177
    for (int32_t cellYIndex = cellCountY - 1; cellYIndex >= 0; --cellYIndex) {
        noiseChunk->selectCellYZ(cellYIndex, 0);

        // Iterate within cell from top to bottom
        for (int32_t yInCell = cellHeight - 1; yInCell >= 0; --yInCell) {
            int32_t posY = (cellMinY + cellYIndex) * cellHeight + yInCell;
            double factorY = static_cast<double>(yInCell) / static_cast<double>(cellHeight);

            noiseChunk->updateForY(posY, factorY);
            noiseChunk->updateForX(x, factorX);
            noiseChunk->updateForZ(z, factorZ);

            // Get interpolated state - Reference: lines 165-166
            BlockState* baseState = noiseChunk->getInterpolatedState();
            BlockState* state = (baseState == nullptr) ? m_defaultBlock : baseState;

            // Check if this block matches the heightmap predicate - Reference: lines 172-175
            if (state != nullptr && isOpaque(state)) {
                noiseChunk->stopInterpolation();
                delete noiseChunk;
                return posY + 1;
            }
        }
    }

    noiseChunk->stopInterpolation();
    delete noiseChunk;
    return m_minY;
}

void NoiseBasedChunkGenerator::getBaseColumn(
    int32_t x,
    int32_t z,
    RandomState* randomState,
    std::vector<BlockState*>& outColumn
) const {
    // Reference: NoiseBasedChunkGenerator.java getBaseColumn() lines 111-115
    // Reference: iterateNoiseColumn() lines 126-181

    outColumn.clear();
    outColumn.resize(m_height, nullptr);

    if (!m_settings || !randomState) {
        return;
    }

    // Calculate cell dimensions
    int32_t cellHeight = m_cellHeight;
    int32_t cellWidth = m_cellWidth;
    int32_t cellMinY = Mth::floorDiv(m_minY, cellHeight);
    int32_t cellCountY = Mth::floorDiv(m_height, cellHeight);

    if (cellCountY <= 0) {
        return;
    }

    // Calculate noise chunk position - Reference: lines 144-151
    int32_t noiseChunkX = Mth::floorDiv(x, cellWidth);
    int32_t noiseChunkZ = Mth::floorDiv(z, cellWidth);
    int32_t xInCell = Mth::floorMod(x, cellWidth);
    int32_t zInCell = Mth::floorMod(z, cellWidth);
    int32_t firstBlockX = noiseChunkX * cellWidth;
    int32_t firstBlockZ = noiseChunkZ * cellWidth;
    double factorX = static_cast<double>(xInCell) / static_cast<double>(cellWidth);
    double factorZ = static_cast<double>(zInCell) / static_cast<double>(cellWidth);

    // Create NoiseChunk for single column - Reference: line 152
    NoiseChunk* noiseChunk = new NoiseChunk(
        1,  // cellCountXZ = 1 for single column
        *randomState,
        firstBlockX,
        firstBlockZ,
        m_settings->noiseSettings(),
        m_beardifier ? m_beardifier : Beardifier::EMPTY(),
        *m_settings,
        m_fluidPicker,
        Blender::empty()
    );

    // Initialize interpolation - Reference: lines 153-154
    noiseChunk->initializeForFirstCellX();
    noiseChunk->advanceCellX(0);

    // Iterate Y cells from top to bottom - Reference: lines 156-177
    for (int32_t cellYIndex = cellCountY - 1; cellYIndex >= 0; --cellYIndex) {
        noiseChunk->selectCellYZ(cellYIndex, 0);

        // Iterate within cell from top to bottom
        for (int32_t yInCell = cellHeight - 1; yInCell >= 0; --yInCell) {
            int32_t posY = (cellMinY + cellYIndex) * cellHeight + yInCell;
            double factorY = static_cast<double>(yInCell) / static_cast<double>(cellHeight);

            noiseChunk->updateForY(posY, factorY);
            noiseChunk->updateForX(x, factorX);
            noiseChunk->updateForZ(z, factorZ);

            // Get interpolated state - Reference: lines 165-166
            BlockState* baseState = noiseChunk->getInterpolatedState();
            BlockState* state = (baseState == nullptr) ? m_defaultBlock : baseState;

            // Store in column array - Reference: lines 167-170
            int32_t yIndex = cellYIndex * cellHeight + yInCell;
            if (yIndex >= 0 && yIndex < m_height) {
                outColumn[yIndex] = state;
            }
        }
    }

    noiseChunk->stopInterpolation();
    delete noiseChunk;
}

void NoiseBasedChunkGenerator::createBiomes(
    RandomState* randomState,
    Blender* blender,
    ::world::IChunk* chunk
) {
    // Reference: NoiseBasedChunkGenerator.java createBiomes() lines 78-83
    // Reference: doCreateBiomes() lines 85-89
    //
    // This populates the chunk's biome data by sampling from the BiomeSource.
    // Biomes are stored at quart resolution (4x4x4 blocks per biome).
    //
    // CRITICAL: Must use cachedClimateSampler from NoiseChunk, NOT the regular
    // RandomState sampler. The cached sampler uses wrapped/interpolated density
    // functions that produce different climate values at biome boundaries.

    if (!chunk || !randomState || !m_biomeSource || !m_settings) {
        return;
    }

    // Try to cast to ProtoChunk early to use cached NoiseChunk
    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (!protoChunk) {
        return;  // Can't cache NoiseChunk without ProtoChunk
    }

    // Get or create NoiseChunk - cached on ProtoChunk for reuse across stages
    // Reference: NoiseBasedChunkGenerator.java doCreateBiomes() line 86:
    //   NoiseChunk noiseChunk = protoChunk.getOrCreateNoiseChunk((chunk) -> this.createNoiseChunk(...))
    // Note: Always use Blender::empty() for cached NoiseChunk
    // Split out on purpose (game-local Tracy patch): BIOMES is the first stage
    // to touch the NoiseChunk, so it pays to BUILD it (~2 MB — density tree,
    // arena, caches). Every later stage reuses it. Without this zone that
    // construction is charged to Gen.Biomes and makes biome assignment look
    // more expensive than terrain noise, which is backwards from Minecraft.
    NoiseChunk* noiseChunk;
    {
        TERRAIN_ZONE_N("Biomes.NoiseChunkCreate");
        noiseChunk = protoChunk->getOrCreateNoiseChunk([this, randomState](::world::IChunk* c) {
            return NoiseChunk::forChunk(
                c,
                *randomState,
                beardifierForChunk(c, m_beardifier),
                *m_settings,
                m_fluidPicker,
                Blender::empty()
            );
        });
    }

    if (!noiseChunk) {
        return;
    }

    // Get cached climate sampler from NoiseChunk
    // Reference: NoiseBasedChunkGenerator.java doCreateBiomes() line 88:
    //   protoChunk.fillBiomesFromNoise(biomeResolver, noiseChunk.cachedClimateSampler(randomState.router(), settings.spawnTarget()))
    NoiseRouter* router = randomState->router();
    if (!router) {
        return;  // Don't delete noiseChunk - it's cached
    }

    // Get spawnTarget from settings
    // Note: For overworld, this is typically empty. Convert from pointer vector to value vector.
    std::vector<world::biome::Climate::ParameterPoint> spawnTarget;
    for (ClimateParameterPoint* ptr : m_settings->spawnTarget()) {
        if (ptr) {
            spawnTarget.push_back(*reinterpret_cast<world::biome::Climate::ParameterPoint*>(ptr));
        }
    }

    // Use the cached climate sampler - this is critical for parity!
    world::biome::Climate::Sampler cachedSampler = [&]() {
        return noiseChunk->cachedClimateSampler(*router, spawnTarget);
    }();

    // Call fillBiomesFromNoise on the ProtoChunk
    // Reference: ChunkAccess.java fillBiomesFromNoise() lines 432-444
    // NOTE: Java uses ThreadLocal for lastResult and NEVER resets it.
    // The RTree cache persists across queries for spatial locality benefits.
    // We removed the resetLastResult() call to match Java's behavior.
    {
        // The actual biome assignment: one climate sample + RTree lookup per
        // quart cell. Compare against Biomes.NoiseChunkCreate above to see
        // which half of Gen.Biomes is worth attacking.
        TERRAIN_ZONE_N("Biomes.Fill");
        protoChunk->fillBiomesFromNoise(m_biomeSource, cachedSampler);
    }

    // Don't delete noiseChunk - it's cached on ProtoChunk and will be cleaned up there
}

} // namespace levelgen
} // namespace minecraft
