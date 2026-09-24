#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenTweaks.h"
#include "util/TerrainProfiling.h"
#include "world/biome/FixedBiomeSource.h"
#include "levelgen/WorldGenLevel.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include "levelgen/FeatureSorter.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "core/QuartPos.h"
#include "core/SectionPos.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "data/worldgen/BiomeFeatureRegistry.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "levelgen/RandomState.h"
#include "levelgen/Blender.h"
#include "levelgen/density/terrain/NoiseChunk.h"
#include "levelgen/density/terrain/NoiseSpawnFinder.h"
#include "levelgen/material/MaterialRules.h"
#include "levelgen/material/MaterialSystem.h"
#include "levelgen/carver/CaveWorldCarver.h"
#include "levelgen/carver/NetherWorldCarver.h"
#include "levelgen/carver/CanyonWorldCarver.h"
#include "levelgen/carver/CarvingContext.h"
#include "levelgen/carver/ConfiguredWorldCarver.h"
#include "levelgen/carver/TwilightCavesCarver.h"
#include "levelgen/blockpredicates/BlockPredicate.h"
#include "world/ProtoChunk.h"
#include "world/LevelChunkSection.h"
#include "world/biome/Biome.h"
#include "world/biome/MultiNoiseBiomeSource.h"
#include "world/biome/Biomes.h"
#include "world/biome/BiomeManager.h"
#include "random/RandomSupport.h"
#include "random/LegacyRandomSource.h"
#include "math/Mth.h"
#include "levelgen/structure/StructureSet.h"
#include "levelgen/structure/StructurePieceBehavior.h"
#include "levelgen/structure/TwilightLandmarks.h"

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

    // WorldGenerationContext.of(level) reads the generator's sea level.
    level->setSeaLevel(getSeaLevel());

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
    //
    // The Aether's structures ("aether:*") sort BEFORE every minecraft:*
    // structure, so counting them in a step would shift the per-structure
    // feature-seed index of every vanilla structure in that step. They are
    // only in the registry the Aether dimension decorates with (the mod's
    // world); every other dimension keeps the vanilla registry and its
    // vanilla seeds.
    using StructuresByStep = std::vector<std::vector<const structure::StructureInfo*>>;
    //
    // The engine's own structures ("obeycraft:*", the companion structures of
    // AurelithOutskirts.h) are appended AFTER every other structure of their
    // step, whatever their name sorts to: no vanilla or mod structure's index
    // (and so its feature seed) moves because one of them exists.
    auto buildStructuresByStep = [](bool includeAether) -> StructuresByStep {
        StructuresByStep byStep(static_cast<size_t>(GenerationStep::DECORATION_COUNT));
        auto file = [&byStep](const structure::StructureInfo* info) {
            for (int32_t ord = 0; ord < GenerationStep::DECORATION_COUNT; ++ord) {
                if (GenerationStep::getName(static_cast<GenerationStep::Decoration>(ord))
                    == info->step) {
                    byStep[static_cast<size_t>(ord)].push_back(info);
                    break;
                }
            }
        };
        // Engine structures (the obeycraft: namespace, and the Hush and
        // Aurelith structures filed under minecraft:) go after everything
        // the real registries hold: filed in alphabetical place they would
        // shift every later vanilla (or Twilight) structure's step index,
        // and with it the setFeatureSeed(decorationSeed, index, step) random
        // its pieces place with.
        std::vector<const structure::StructureInfo*> engine;
        for (const structure::StructureInfo* info : structure::StructureSets::allStructures()) {
            if (!includeAether && info->name.rfind("aether:", 0) == 0) continue;
            const bool engineOwned = info->name.rfind("obeycraft:", 0) == 0
                || (info->name.rfind("minecraft:", 0) == 0
                    && !structure::StructureSets::isVanillaStructure(info->name));
            if (engineOwned) { engine.push_back(info); continue; }
            file(info);
        }
        for (const structure::StructureInfo* info : engine) file(info);
        return byStep;
    };
    static const StructuresByStep s_structuresByStepVanilla = buildStructuresByStep(false);
    static const StructuresByStep s_structuresByStepAether = buildStructuresByStep(true);
    bool isAetherDecoration = false;
    if (auto* noiseGenerator = dynamic_cast<NoiseBasedChunkGenerator*>(this)) {
        const NoiseGeneratorSettings* settings = noiseGenerator->getSettings();
        isAetherDecoration = settings != nullptr && settings->defaultBlock() != nullptr
            && settings->defaultBlock()->getIdentifier() == "minecraft:holystone";
    }
    const StructuresByStep& s_structuresByStep =
        isAetherDecoration ? s_structuresByStepAether : s_structuresByStepVanilla;

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

NoiseBasedChunkGenerator::NoiseBasedChunkGenerator(std::shared_ptr<const NoiseGeneratorSettings> settings)
    : m_settings(std::move(settings)) {}

NoiseBasedChunkGenerator::~NoiseBasedChunkGenerator() = default;

int32_t NoiseBasedChunkGenerator::getSeaLevel() const { return m_settings->seaLevel(); }
int32_t NoiseBasedChunkGenerator::getMinY() const { return m_settings->noiseSettings().minY; }
int32_t NoiseBasedChunkGenerator::getGenDepth() const { return m_settings->noiseSettings().height; }

namespace {

using density::DensityVolume;

// The three vanilla Overworld carvers (CAVE, CAVE_EXTRA_UNDERGROUND, CANYON)
// configured exactly as Carvers.java lines 33-35, added to `settings` with
// the given replaceable-block set. Called once per dimension that carves
// Overworld-style (the Overworld with #overworld_carver_replaceables, The
// Hush with #hush_carver_replaceables); the configurations are allocated
// for the process lifetime, as the file-static originals were, because the
// BiomeGenerationSettings keeps raw pointers to them.
void addOverworldStyleCarvers(world::biome::BiomeGenerationSettings& settings,
                              const std::set<std::string>& replaceable) {
    // ========================================
    // CAVE carver (carverIndex=0)
    // Reference: Carvers.java line 33
    // ========================================
    auto* caveHeight = new carver::UniformHeight(
        VerticalAnchor::aboveBottom(8),
        VerticalAnchor::absolute(180)
    );
    auto* caveYScale = new carver::UniformFloat(0.1f, 0.9f);
    auto* caveHorizontalMult = new carver::UniformFloat(0.7f, 1.4f);
    auto* caveVerticalMult = new carver::UniformFloat(0.8f, 1.3f);
    auto* caveFloorLevel = new carver::UniformFloat(-1.0f, -0.4f);

    auto* caveConfig = new carver::CaveCarverConfiguration(
        0.15f,                      // probability
        caveHeight,                 // y height provider
        caveYScale,                 // y scale
        VerticalAnchor::aboveBottom(8), // lava level
        carver::CarverDebugSettings(),
        replaceable,
        caveHorizontalMult,
        caveVerticalMult,
        caveFloorLevel
    );

    auto* caveCarver = new carver::CaveWorldCarver();
    auto* configuredCaveCarver = new carver::ConfiguredCaveCarver(caveCarver, *caveConfig);
    settings.addCarver(configuredCaveCarver);

    // ========================================
    // CAVE_EXTRA_UNDERGROUND carver (carverIndex=1)
    // Reference: Carvers.java line 34
    // ========================================
    auto* caveExtraHeight = new carver::UniformHeight(
        VerticalAnchor::aboveBottom(8),
        VerticalAnchor::absolute(47)
    );
    auto* caveExtraYScale = new carver::UniformFloat(0.1f, 0.9f);
    auto* caveExtraHorizontalMult = new carver::UniformFloat(0.7f, 1.4f);
    auto* caveExtraVerticalMult = new carver::UniformFloat(0.8f, 1.3f);
    auto* caveExtraFloorLevel = new carver::UniformFloat(-1.0f, -0.4f);

    auto* caveExtraConfig = new carver::CaveCarverConfiguration(
        0.07f,                      // probability (lower than main caves)
        caveExtraHeight,            // y height provider (lower max)
        caveExtraYScale,            // y scale
        VerticalAnchor::aboveBottom(8), // lava level
        carver::CarverDebugSettings(),
        replaceable,
        caveExtraHorizontalMult,
        caveExtraVerticalMult,
        caveExtraFloorLevel
    );

    auto* caveExtraCarver = new carver::CaveWorldCarver();
    auto* configuredCaveExtraCarver = new carver::ConfiguredCaveCarver(caveExtraCarver, *caveExtraConfig);
    settings.addCarver(configuredCaveExtraCarver);

    // ========================================
    // CANYON carver (carverIndex=2)
    // Reference: Carvers.java line 35
    // ========================================
    auto* canyonHeight = new carver::UniformHeight(
        VerticalAnchor::absolute(10),
        VerticalAnchor::absolute(67)
    );
    auto* canyonYScale = new carver::ConstantFloat(3.0f);
    auto* canyonVerticalRotation = new carver::UniformFloat(-0.125f, 0.125f);
    auto* canyonDistanceFactor = new carver::UniformFloat(0.75f, 1.0f);
    auto* canyonThickness = new carver::TrapezoidFloat(0.0f, 6.0f, 2.0f);
    auto* canyonHorizontalRadiusFactor = new carver::UniformFloat(0.75f, 1.0f);

    auto* canyonShape = new carver::CanyonShapeConfiguration(
        canyonDistanceFactor,
        canyonThickness,
        3,                          // widthSmoothness
        canyonHorizontalRadiusFactor,
        1.0f,                       // verticalRadiusDefaultFactor
        0.0f                        // verticalRadiusCenterFactor
    );

    auto* canyonConfig = new carver::CanyonCarverConfiguration(
        0.01f,                      // probability (rare)
        canyonHeight,               // y height provider
        canyonYScale,               // y scale
        VerticalAnchor::aboveBottom(8), // lava level
        carver::CarverDebugSettings(),
        replaceable,
        canyonVerticalRotation,
        *canyonShape
    );

    auto* canyonCarver = new carver::CanyonWorldCarver();
    auto* configuredCanyonCarver = new carver::ConfiguredCanyonCarver(canyonCarver, *canyonConfig);
    settings.addCarver(configuredCanyonCarver);
}


// NoiseBasedChunkGenerator.createFluidPicker: lava below min(-54, sea level),
// the settings' fluid below the sea level.
density::Aquifer::FluidPicker createFluidPicker(const NoiseGeneratorSettings& settings) {
    using minecraft::world::level::block::Blocks;
    const density::Aquifer::FluidStatus lavaStatus{-54, Blocks::LAVA->defaultBlockState()};
    const int seaLevel = settings.seaLevel();
    const density::Aquifer::FluidStatus seaStatus{seaLevel, settings.defaultFluid()};
    return [lavaStatus, seaStatus, seaLevel](int, int y, int) {
        return y < std::min(-54, seaLevel) ? lavaStatus : seaStatus;
    };
}

// The generator's noise range inside the chunk's (NoiseSettings.
// clampToHeightAccessor).
density::NoiseSettings clampedNoiseSettings(const NoiseGeneratorSettings& settings, int levelMinY, int levelHeight) {
    return settings.noiseSettings().clampToHeightAccessor(levelMinY, levelMinY + levelHeight - 1);
}

// BiomeManager.NoiseBiomeSource over a BiomeResolver (withDifferentSource).
class ResolverBiomeSource final : public world::biome::BiomeManager::NoiseBiomeSource {
public:
    explicit ResolverBiomeSource(world::biome::BiomeSource::BiomeResolver resolver) : m_resolver(std::move(resolver)) {}
    world::biome::BiomeHolder getNoiseBiome(int32_t quartX, int32_t quartY, int32_t quartZ) const override {
        return world::biome::Biomes::get(m_resolver(quartX, quartY, quartZ));
    }

private:
    world::biome::BiomeSource::BiomeResolver m_resolver;
};

} // namespace

std::unique_ptr<density::NoiseChunk> NoiseBasedChunkGenerator::createNoiseChunk(
    ::world::IChunk* chunk, RandomState& randomState,
    std::shared_ptr<const density::DensitySampler> beardifier) const {
    const density::NoiseSettings noiseSettings =
        clampedNoiseSettings(*m_settings, chunk->getMinBuildHeight(),
                             chunk->getMaxBuildHeight() - chunk->getMinBuildHeight());
    // NoiseBasedChunkGenerator.chunkVolume.
    const ::world::ChunkPos pos = chunk->getPos();
    const DensityVolume volume(16, noiseSettings.height, 16, pos.getMinBlockX(), noiseSettings.minY,
                               pos.getMinBlockZ());
    return std::make_unique<density::NoiseChunk>(randomState.density(), std::move(beardifier), m_settings->terrain(),
                                                 createFluidPicker(*m_settings), volume);
}

void NoiseBasedChunkGenerator::buildTerrain(RandomState* randomState, const TerrainContext& context,
                                            ::world::IChunk* chunk) {
    // Reference: NoiseBasedChunkGenerator.buildTerrain (26.3).
    const density::NoiseSettings noiseSettings =
        clampedNoiseSettings(*m_settings, chunk->getMinBuildHeight(),
                             chunk->getMaxBuildHeight() - chunk->getMinBuildHeight());
    if (noiseSettings.height <= 0 || randomState == nullptr) return;
    std::unique_ptr<density::NoiseChunk> noiseChunk = createNoiseChunk(chunk, *randomState, context.beardifier);
    {
        TERRAIN_ZONE_N("Gen.Noise");
        doFill(*noiseChunk, chunk);
    }
    {
        TERRAIN_ZONE_N("Gen.Surface");
        buildSurface(*randomState, context, chunk, *noiseChunk);
    }
    {
        TERRAIN_ZONE_N("Gen.Carvers");
        generateCarvers(*randomState, context, chunk, *noiseChunk);
    }
}

void NoiseBasedChunkGenerator::doFill(density::NoiseChunk& noiseChunk, ::world::IChunk* chunk) const {
    // Reference: NoiseBasedChunkGenerator.doFill (26.3).
    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (protoChunk == nullptr) return;
    using minecraft::world::level::block::Blocks;
    BlockState* const air = Blocks::AIR->defaultBlockState();
    BlockState* const defaultBlock = m_settings->defaultBlock();
    Heightmap& oceanFloor = protoChunk->getOrCreateHeightmap(Heightmap::Types::OCEAN_FLOOR_WG);
    Heightmap& worldSurface = protoChunk->getOrCreateHeightmap(Heightmap::Types::WORLD_SURFACE_WG);
    density::Aquifer& aquifer = noiseChunk.aquifer();
    const DensityVolume& volume = noiseChunk.volume();
    density::ScopedBuffer densityBuffer =
        noiseChunk.cachingSamplers().get(m_settings->noiseRouter().finalDensity).sampleVolume(volume);

    for (int z = 0; z < volume.sizeZ; ++z) {
        const int blockZ = volume.blockZ(z);
        for (int x = 0; x < volume.sizeX; ++x) {
            const int blockX = volume.blockX(x);
            for (int y = volume.sizeY - 1; y >= 0; --y) {
                const int blockY = volume.blockY(y);
                world::LevelChunkSection& section = protoChunk->getSection(protoChunk->getSectionIndex(blockY));
                const float density = densityBuffer->get(volume.indexUnchecked(x, y, z));
                BlockState* state = aquifer.computeSubstance(blockX, blockY, blockZ, static_cast<double>(density));
                if (state == nullptr) state = defaultBlock;
                if (state != air) {
                    section.setBlockState(x, blockY & 15, z, state, false);
                    oceanFloor.update(x, blockY, z, state);
                    worldSurface.update(x, blockY, z, state);
                    if (aquifer.shouldScheduleFluidUpdate() && state->isFluid()) {
                        chunk->markPosForPostprocessing(core::BlockPos(blockX, blockY, blockZ));
                    }
                }
            }
        }
    }
}

const material::MaterialRule* NoiseBasedChunkGenerator::materialRule() {
    std::call_once(m_materialRuleOnce, [this] {
        material::registerModMaterialRules();
        m_materialRule = material::MaterialRuleRegistry::get().rule(m_settings->materialRule());
    });
    return m_materialRule.get();
}

void NoiseBasedChunkGenerator::buildSurface(RandomState& randomState, const TerrainContext& context,
                                            ::world::IChunk* chunk, density::NoiseChunk& noiseChunk) {
    // Reference: NoiseBasedChunkGenerator.buildSurface (26.3).
    const material::MaterialRule* rule = materialRule();
    if (rule != nullptr && randomState.surfaceSystem() != nullptr) {
        const WorldGenerationContext generationContext(
            std::max(chunk->getMinBuildHeight(), getMinY()),
            std::min(chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(), getGenDepth()));
        randomState.surfaceSystem()->buildSurface(randomState.density(), context.biomeGetter, generationContext,
                                                  chunk, noiseChunk, *rule,
                                                  m_biomeSource != nullptr ? &m_biomeSource->possibleBiomes()
                                                                           : nullptr);
    }
    if (m_settings->isTwilightForest()) {
        twilightChunkBlanketing(context, chunk);
    }
}

void NoiseBasedChunkGenerator::twilightChunkBlanketing(const TerrainContext& context, ::world::IChunk* chunk) {
    const auto& biomeGetter = context.biomeGetter;
    // The Twilight Forest's dark-forest canopy: twilight/chunk_blanket_
    // processors/dark_forest_canopy.json, a CanopyBlanketProcessor (biome
    // mask dark_forest + dark_forest_center, block hardened_dark_leaves,
    // height 14, avoid_structures [dark_tower]). ChunkBlanketProcessors.
    // chunkBlanketing runs the processors in registry order (dark_forest_
    // canopy before snowy_forest_glacier) for chunks whose section biomes
    // include one of the processor's biomes; addDarkForestCanopy then
    // thickens a hardened-leaf roof from how many of the 3x3 quart
    // neighbours of each of the 5x5 quarts around the chunk are dark forest,
    // interpolated per column, 14 blocks above the surface, holed out within
    // 24 blocks of the landmark centre when a dark tower runs through the
    // chunk. The provider is simple_state_provider, so its Xoroshiro random
    // (seed, Mth.getSeed(chunkOrigin)) is never drawn.
    {
        using minecraft::world::level::block::Blocks;
        static BlockState* const s_hardenedDarkLeaves = Blocks::getDefaultState("minecraft:hardened_dark_leaves");
        constexpr int32_t CANOPY_HEIGHT = 14;
        auto isCanopyBiome = [](const std::string& name) {
            return name == "twilightforest:dark_forest" || name == "twilightforest:dark_forest_center";
        };
        bool chunkHasCanopyBiome = false;
        if (s_hardenedDarkLeaves != nullptr) {
            // The biomes stored in the chunk's sections (every quart).
            const ::world::ChunkPos chunkPos = chunk->getPos();
            const int32_t quartMinX = chunkPos.getMinBlockX() >> 2;
            const int32_t quartMinZ = chunkPos.getMinBlockZ() >> 2;
            const int32_t quartMinY = chunk->getMinY() >> 2;
            const int32_t quartCountY = (chunk->getMaxY() - chunk->getMinY() + 1) >> 2;
            for (int32_t qy = 0; qy < quartCountY && !chunkHasCanopyBiome; ++qy) {
                for (int32_t qx = 0; qx < 4 && !chunkHasCanopyBiome; ++qx) {
                    for (int32_t qz = 0; qz < 4; ++qz) {
                        world::biome::BiomeHolder biome = chunk->getBiome(core::BlockPos(
                            (quartMinX + qx) << 2, (quartMinY + qy) << 2, (quartMinZ + qz) << 2));
                        if (biome && isCanopyBiome(biome->getName())) {
                            chunkHasCanopyBiome = true;
                            break;
                        }
                    }
                }
            }
        }
        if (chunkHasCanopyBiome) {
            const ::world::ChunkPos chunkPos = chunk->getPos();
            const int32_t originX = chunkPos.getMinBlockX();
            const int32_t originZ = chunkPos.getMinBlockZ();

            int32_t thicks[5 * 5] = {};
            bool biomeFound = false;
            for (int32_t dZ = 0; dZ < 5; ++dZ) {
                for (int32_t dX = 0; dX < 5; ++dX) {
                    for (int32_t bx = -1; bx <= 1; ++bx) {
                        for (int32_t bz = -1; bz <= 1; ++bz) {
                            world::biome::BiomeHolder biomeAt = biomeGetter(core::BlockPos(
                                originX + ((dX + bx) << 2), 0, originZ + ((dZ + bz) << 2)));
                            if (biomeAt && isCanopyBiome(biomeAt->getName())) {
                                thicks[dX + dZ * 5]++;
                                biomeFound = true;
                            }
                        }
                    }
                }
            }

            if (biomeFound) {
                // chunk.getAllReferences() meets avoid_structures ([dark_tower]).
                const auto& references = chunk->getAllStructureReferences();
                auto darkTower = references.find("twilightforest:dark_tower");
                const bool clearingForStructureNearby =
                    darkTower != references.end() && !darkTower->second.empty();
                int32_t hx = 0;
                int32_t hz = 0;
                if (clearingForStructureNearby) {
                    const core::BlockPos nearestCenter = structure::twilight_landmarks::getNearestCenterXZ(
                        chunkPos.x(), chunkPos.z(), CANOPY_HEIGHT);
                    hx = nearestCenter.getX() - originX;
                    hz = nearestCenter.getZ() - originZ;
                }

                for (int32_t dZ = 0; dZ < 16; ++dZ) {
                    for (int32_t dX = 0; dX < 16; ++dX) {
                        const int32_t qx = dX >> 2;
                        const int32_t qz = dZ >> 2;

                        const int32_t topOccupiedBlock =
                            chunk->getHeight(static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), dX, dZ);
                        const core::BlockPos surfacePos(originX + dX, topOccupiedBlock, originZ + dZ);
                        BlockState* surfaceState = chunk->getBlockState(surfacePos);
                        // chunk.getFluidState(surfacePos).is(FluidTags.WATER)
                        if (surfaceState != nullptr && surfaceState->hasWaterFluid()) continue;

                        const float xweight = static_cast<float>(dX % 4) * 0.25f + 0.125f;
                        const float zweight = static_cast<float>(dZ % 4) * 0.25f + 0.125f;

                        float thickness = static_cast<float>(thicks[qx + qz * 5]) * (1.0f - xweight) * (1.0f - zweight)
                            + static_cast<float>(thicks[qx + 1 + qz * 5]) * xweight * (1.0f - zweight)
                            + static_cast<float>(thicks[qx + (qz + 1) * 5]) * (1.0f - xweight) * zweight
                            + static_cast<float>(thicks[qx + 1 + (qz + 1) * 5]) * xweight * zweight
                            - 4.0f;

                        if (clearingForStructureNearby) {
                            const int32_t rx = dX - hx;
                            const int32_t rz = dZ - hz;
                            // (int) Mth.sqrt(float)
                            const int32_t dist = static_cast<int32_t>(
                                std::sqrt(static_cast<float>(rx * rx + rz * rz)));
                            if (dist < 24) {
                                thickness -= static_cast<float>(24 - dist);
                            }
                        }

                        if (thickness > 1.0f) {
                            const int32_t dY = chunk->getHeight(
                                static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), dX, dZ);
                            const core::BlockPos pos(surfacePos.getX(), dY, surfacePos.getZ());
                            // Skip any blocks over water (BlockState.liquid()).
                            BlockState* atPos = chunk->getBlockState(pos);
                            if (atPos != nullptr && atPos->isFluid()) continue;

                            const int32_t treeBottom = pos.getY() + CANOPY_HEIGHT
                                - static_cast<int32_t>(thickness * 0.5f);
                            const int32_t treeTop = treeBottom + static_cast<int32_t>(thickness);
                            for (int32_t y = treeBottom; y < treeTop; ++y) {
                                chunk->setBlockState(core::BlockPos(pos.getX(), y, pos.getZ()),
                                                     s_hardenedDarkLeaves, false);
                            }
                        }
                    }
                }
            }
        }
    }

    // The Twilight Forest's glacier: twilight/chunk_blanket_processors/
    // snowy_forest_glacier.json, a GlacierBlanketProcessor that TF runs right
    // after buildSurface (asmhooks/WorldgenHooks.chunkBlanketing ->
    // ChunkBlanketProcessors.chunkBlanketing). For every column whose biome
    // (at the first free block above WORLD_SURFACE_WG, through the same
    // fuzzed BiomeManager lookup as the surface) is twilightforest:glacier:
    // packed_ice from that block up 32, capped with ice. Both providers are
    // simple_state_provider, so the processor's forked random is never drawn.
    {
        using minecraft::world::level::block::Blocks;
        static BlockState* const s_packedIce = Blocks::getDefaultState("minecraft:packed_ice");
        static BlockState* const s_ice = Blocks::getDefaultState("minecraft:ice");
        constexpr int32_t GLACIER_HEIGHT = 32;
        if (s_packedIce != nullptr && s_ice != nullptr) {
            const ::world::ChunkPos chunkPos = chunk->getPos();
            for (int32_t dX = 0; dX < 16; ++dX) {
                for (int32_t dZ = 0; dZ < 16; ++dZ) {
                    const int32_t firstAvailableY = chunk->getHeight(
                        static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), dX, dZ) + 1;
                    const core::BlockPos aboveFloor(chunkPos.getMinBlockX() + dX, firstAvailableY,
                                                    chunkPos.getMinBlockZ() + dZ);
                    world::biome::BiomeHolder biome = biomeGetter(aboveFloor);
                    if (!biome || biome->getName() != "twilightforest:glacier") continue;

                    const int32_t maxY = firstAvailableY + GLACIER_HEIGHT;
                    chunk->setBlockState(core::BlockPos(aboveFloor.getX(), maxY, aboveFloor.getZ()),
                                         s_ice, false);
                    for (int32_t y = maxY - 1; y >= firstAvailableY; --y) {
                        chunk->setBlockState(core::BlockPos(aboveFloor.getX(), y, aboveFloor.getZ()),
                                             s_packedIce, false);
                    }
                }
            }
        }
    }

}

void NoiseBasedChunkGenerator::generateCarvers(RandomState& randomState, const TerrainContext& context,
                                               ::world::IChunk* chunk, density::NoiseChunk& noiseChunk) {
    // Reference: NoiseBasedChunkGenerator.generateCarvers (26.3).
    // World Properties: caves/canyons toggle (non-vanilla; default true).
    if (!WorldGenTweaks::get().carversEnabled) return;
    if (m_biomeSource == nullptr) return;

    const std::string defaultBlockId =
        m_settings->defaultBlock() != nullptr ? m_settings->defaultBlock()->getIdentifier() : std::string();
    // The Aether: every biome JSON (data/aether/worldgen/biome/*.json) has
    // "carvers": {}. The End: no End biome has a carver (26.3 end biomes'
    // "carvers": []); applyCarvingMask replaces anything but #uncarvable, so
    // carving there would hollow out the islands.
    if (defaultBlockId == "minecraft:holystone" || defaultBlockId == "minecraft:end_stone") return;
    const bool isNetherDimension = defaultBlockId == "minecraft:netherrack";
    const bool isHushDimension = defaultBlockId == "minecraft:hushstone";
    const bool isTwilightDimension = m_settings->isTwilightForest();

    // this.biomeSource.createUncachedResolver(randomState).
    world::biome::Climate::Sampler* uncached = randomState.sampler();
    world::biome::BiomeSource* biomeSource = m_biomeSource;
    world::biome::BiomeSource::BiomeResolver biomeResolver = [biomeSource, uncached](int32_t qx, int32_t qy,
                                                                                     int32_t qz) {
        return biomeSource->getNoiseBiome(qx, qy, qz, *uncached);
    };
    // getBiomeGenerationSettingsForCarver: the source chunk's biome at
    // (QuartPos.fromBlock(minBlockX), 0, QuartPos.fromBlock(minBlockZ)).
    auto carverBiome = [&biomeResolver](const ::world::ChunkPos& sourcePos, int32_t blockY) {
        return world::biome::Biomes::get(biomeResolver(core::QuartPos::fromBlock(sourcePos.getMinBlockX()),
                                                        core::QuartPos::fromBlock(blockY),
                                                        core::QuartPos::fromBlock(sourcePos.getMinBlockZ())));
    };

    // new WorldgenRandom(new LegacyRandomSource(RandomSupport.generateUniqueSeed())).
    LegacyRandomSource random(RandomSupport::generateUniqueSeed());
    const ::world::ChunkPos pos = chunk->getPos();
    // new WorldGenerationContext(this, chunk.getHeightAccessorForGeneration()).
    carver::CarvingContext carvingContext(std::max(chunk->getMinBuildHeight(), getMinY()),
                                          std::min(chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(),
                                                   getGenDepth()),
                                          &noiseChunk, &randomState, materialRule());
    // The vanilla dimensions carve as 26.3 does: mark the mask, then one
    // applyCarvingMask pass. The Twilight Forest and the Hush keep their
    // mods' per-ellipsoid carving (written against the pre-26.3 carver API).
    carvingContext.maskOnly = !isTwilightDimension && !isHushDimension;
    carver::CarvingMask mask(chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(), chunk->getMinBuildHeight());

    // The fuzzed biome lookup the carvers' top-material pass reads:
    // biomeManager.withDifferentSource(biomeResolver).
    ResolverBiomeSource resolverSource(biomeResolver);
    world::biome::BiomeManager correctBiomeManager(&resolverSource,
                                                   world::biome::BiomeManager::obfuscateSeed(context.seed));
    auto correctBiomeGetter = [&correctBiomeManager](const core::BlockPos& blockPos) -> world::biome::BiomeHolder {
        return correctBiomeManager.getBiome(blockPos);
    };

    for (int32_t dx = -8; dx <= 8; ++dx) {
        for (int32_t dz = -8; dz <= 8; ++dz) {
            const ::world::ChunkPos sourcePos(pos.x() + dx, pos.z() + dz);
            const world::biome::BiomeHolder sourceBiome = carverBiome(sourcePos, 0);
            const world::biome::BiomeGenerationSettings* genSettings = nullptr;
            if (sourceBiome && isTwilightDimension) {
                // Every TF biome carries at most one configured carver
                // (worldgen/biome/*.json "carvers"):
                //   twilightforest:tf_caves        all biomes but the four below
                //   twilightforest:highland_caves  highlands, highlands_underground
                //   none                           final_plateau, lake, stream, thornlands
                // configured_carver/tf_caves.json: probability 0.1, y uniform
                // aboveBottom(16)..absolute(-8), yScale 0.6, lava bottom,
                // #twilightforest:carver_replaceables, h/v 1.05, floor -0.7.
                // configured_carver/highland_caves.json: probability 1, y
                // biased_to_bottom(8, 32, inner 16), yScale 0.6, h uniform
                // [1.1, 1.3), v 1.1, floor uniform [-0.9, -0.65).
                static world::biome::BiomeGenerationSettings twilightCaveSettings;
                static world::biome::BiomeGenerationSettings highlandCaveSettings;
                static std::once_flag twilightCarverInitOnce;
                std::call_once(twilightCarverInitOnce, [&] {
                    // #twilightforest:carver_replaceables =
                    // #minecraft:overworld_carver_replaceables + snow_block.
                    const auto& tagValues =
                        blockpredicates::orderedBlockTagValues("minecraft:overworld_carver_replaceables");
                    std::set<std::string> replaceable(tagValues.begin(), tagValues.end());
                    if (replaceable.empty()) {
                        fprintf(stderr, "[carver] #minecraft:overworld_carver_replaceables resolved to"
                                        " nothing - falling back to the stone and dirt family\n");
                        replaceable = {
                            "minecraft:stone", "minecraft:granite", "minecraft:diorite",
                            "minecraft:andesite", "minecraft:tuff", "minecraft:deepslate",
                            "minecraft:dirt", "minecraft:grass_block", "minecraft:podzol",
                            "minecraft:coarse_dirt", "minecraft:mycelium", "minecraft:rooted_dirt",
                            "minecraft:gravel", "minecraft:sand", "minecraft:sandstone",
                            "minecraft:water"
                        };
                    }
                    replaceable.insert("minecraft:snow_block");

                    static std::unique_ptr<carver::TwilightCavesCarver> tfCaves =
                        carver::TwilightCavesCarver::createTwilightCaves();
                    auto* tfHeight = new carver::UniformHeight(
                        VerticalAnchor::aboveBottom(16), VerticalAnchor::absolute(-8));
                    auto* tfConfig = new carver::CaveCarverConfiguration(
                        0.1f, tfHeight, new carver::ConstantFloat(0.6f), VerticalAnchor::bottom(),
                        carver::CarverDebugSettings(), replaceable,
                        new carver::ConstantFloat(1.05f), new carver::ConstantFloat(1.05f),
                        new carver::ConstantFloat(-0.7f));
                    twilightCaveSettings.addCarver(new carver::ConfiguredCaveCarver(tfCaves.get(), *tfConfig));

                    static std::unique_ptr<carver::TwilightCavesCarver> highlandCaves =
                        carver::TwilightCavesCarver::createHighlandCaves();
                    auto* highlandHeight = new carver::BiasedToBottomHeight(
                        VerticalAnchor::absolute(8), VerticalAnchor::absolute(32), 16);
                    auto* highlandConfig = new carver::CaveCarverConfiguration(
                        1.0f, highlandHeight, new carver::ConstantFloat(0.6f), VerticalAnchor::bottom(),
                        carver::CarverDebugSettings(), replaceable,
                        new carver::UniformFloat(1.1f, 1.3f), new carver::ConstantFloat(1.1f),
                        new carver::UniformFloat(-0.9f, -0.65f));
                    highlandCaveSettings.addCarver(
                        new carver::ConfiguredCaveCarver(highlandCaves.get(), *highlandConfig));
                });
                const std::string& biomeName = sourceBiome->getName();
                if (biomeName == "twilightforest:final_plateau" || biomeName == "twilightforest:lake"
                    || biomeName == "twilightforest:stream" || biomeName == "twilightforest:thornlands") {
                    continue;
                }
                genSettings = (biomeName == "twilightforest:highlands"
                               || biomeName == "twilightforest:highlands_underground")
                    ? &highlandCaveSettings
                    : &twilightCaveSettings;
            } else if (sourceBiome && isNetherDimension) {
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
            } else if (sourceBiome && isHushDimension) {
                // The Hush carves like the Overworld (the same three carvers,
                // lava at aboveBottom(8)) through its own stone family:
                // BlockTags #minecraft:hush_carver_replaceables from
                // data/minecraft/tags/block/hush_carver_replaceables.json
                // (hushstone, polished hushstone, sculk loam, hush moss, echo
                // ore, water, gravel, #base_stone_overworld, #dirt), resolved
                // through the runtime tag registry rather than a hand copy.
                static world::biome::BiomeGenerationSettings hushSettings;
                static world::biome::BiomeGenerationSettings hushDeepSettings;
                static std::once_flag hushCarverInitOnce;
                std::call_once(hushCarverInitOnce, [&] {
                    const auto& tagValues =
                        blockpredicates::orderedBlockTagValues("minecraft:hush_carver_replaceables");
                    std::set<std::string> hushReplaceable(tagValues.begin(), tagValues.end());
                    if (hushReplaceable.empty()) {
                        // A missing tag file would otherwise carve nothing at
                        // all with no error; the stone family is the minimum.
                        fprintf(stderr, "[carver] #minecraft:hush_carver_replaceables resolved to"
                                        " nothing - falling back to the Hush stone family\n");
                        hushReplaceable = {
                            "minecraft:hushstone", "minecraft:polished_hushstone",
                            "minecraft:sculk_loam", "minecraft:hush_moss", "minecraft:echo_ore",
                            "minecraft:water", "minecraft:gravel"
                        };
                    }
                    addOverworldStyleCarvers(hushSettings, hushReplaceable);
                    // Hollow Deep: the same three carvers plus the Hush
                    // deep canyon (carverIndex 3, so the first three keep
                    // their seeds). The vanilla CANYON (Carvers.java line
                    // 35) with every knob opened up:
                    //   probability 0.01 -> 0.08   (the biome is chasm country)
                    //   y uniform(10, 67) -> uniform(28, 72)
                    //   yScale 3 -> 4               (deeper for the same width)
                    //   thickness trapezoid(0, 6, 2) -> trapezoid(3, 11, 3)
                    //                               (horizontal radius up to
                    //                                ~12.5 instead of 7.5)
                    //   distanceFactor uniform(0.75, 1) -> uniform(0.85, 1)
                    //   horizontalRadiusFactor uniform(0.75, 1) -> uniform(0.9, 1.15)
                    // widthSmoothness 3 and the vertical radius factors stay.
                    addOverworldStyleCarvers(hushDeepSettings, hushReplaceable);
                    auto* deepHeight = new carver::UniformHeight(
                        VerticalAnchor::absolute(28), VerticalAnchor::absolute(72));
                    auto* deepShape = new carver::CanyonShapeConfiguration(
                        new carver::UniformFloat(0.85f, 1.0f),
                        new carver::TrapezoidFloat(3.0f, 11.0f, 3.0f),
                        3,
                        new carver::UniformFloat(0.9f, 1.15f),
                        1.0f,
                        0.0f);
                    auto* deepConfig = new carver::CanyonCarverConfiguration(
                        0.08f,
                        deepHeight,
                        new carver::ConstantFloat(4.0f),
                        VerticalAnchor::aboveBottom(8),
                        carver::CarverDebugSettings(),
                        hushReplaceable,
                        new carver::UniformFloat(-0.125f, 0.125f),
                        *deepShape);
                    static carver::CanyonWorldCarver deepCanyonCarver;
                    hushDeepSettings.addCarver(
                        new carver::ConfiguredCanyonCarver(&deepCanyonCarver, *deepConfig));
                });
                // The carver biome is vanilla's y = 0 sample, which in the
                // Hush is usually the Crystal Caverns band; the Hollow Deep is
                // a surface biome, so its canyon is keyed on a second sample
                // at y 64 (the surface band at the Overworld-shaped
                // terrain's usual height).
                const world::biome::BiomeHolder surfaceBiome = carverBiome(sourcePos, 64);
                const bool hollowDeep =
                    sourceBiome->getName() == "minecraft:hollow_deep"
                    || (surfaceBiome && surfaceBiome->getName() == "minecraft:hollow_deep");
                genSettings = hollowDeep ? &hushDeepSettings : &hushSettings;
            } else if (sourceBiome) {
                // Configure all 3 default overworld carvers to match Java exactly.
                // call_once: the old non-atomic bool guard raced when worker
                // threads reached CARVERS for two chunks simultaneously.
                static world::biome::BiomeGenerationSettings defaultSettings;
                static std::once_flag carverInitOnce;
                std::call_once(carverInitOnce, [&] {
                    // Replaceable blocks from BlockTags.OVERWORLD_CARVER_REPLACEABLES
                    // Extracted from Minecraft 26.1-snapshot-1 data/minecraft/tags/block/overworld_carver_replaceables.json
                    static const std::set<std::string> replaceable = {
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
                    // CAVE, CAVE_EXTRA_UNDERGROUND, CANYON — Carvers.java 33-35
                    addOverworldStyleCarvers(defaultSettings, replaceable);
                });
                genSettings = &defaultSettings;
            }

            if (!genSettings) {
                continue;
            }

            const auto& carvers = genSettings->getCarvers();
            for (size_t index = 0; index < carvers.size(); ++index) {
                carver::ConfiguredCarverBase* configuredCarver = carvers[index];
                if (!configuredCarver) continue;
                // random.setLargeFeatureSeed(randomState.seed() + index, sourcePos.x, sourcePos.z)
                random.setLargeFeatureSeed(context.seed + static_cast<int64_t>(index), sourcePos.x(), sourcePos.z());
                if (configuredCarver->isStartChunk(random)) {
                    configuredCarver->carve(
                        carvingContext, chunk,
                        [&correctBiomeGetter](const core::BlockPos& blockPos) -> void* {
                            return const_cast<world::biome::Biome*>(correctBiomeGetter(blockPos));
                        },
                        random, &noiseChunk.aquifer(), sourcePos, mask);
                }
            }
        }
    }

    if (carvingContext.maskOnly) {
        TERRAIN_ZONE_N("applyCarvingMask");
        applyCarvingMask(chunk, mask, randomState, carvingContext, noiseChunk, correctBiomeGetter);
    }
}

// MC 26.3 NoiseBasedChunkGenerator.applyCarvingMask: the block-writing half of
// carving, run once over everything the carvers marked.
//
// CarvingMask.visit walks runs of marked cells column by column — columns in
// x-major order, each column's runs bottom-up, each run top-down — and
// `hasGrass` belongs to one run. Per cell: #minecraft:uncarvable (bedrock
// only) is kept; everything else takes the aquifer's substance at density 0,
// as is (air, water or lava; a null answer is a barrier and leaves the
// block). Carving through grass or mycelium re-covers exposed dirt below with
// the biome's top material.
void NoiseBasedChunkGenerator::applyCarvingMask(
    ::world::IChunk* chunk, const carver::CarvingMask& mask, RandomState& randomState,
    carver::CarvingContext& context, density::NoiseChunk& noiseChunk,
    const std::function<world::biome::BiomeHolder(const core::BlockPos&)>& biomeGetter) {
    (void)randomState;
    using minecraft::world::level::block::Blocks;
    static minecraft::world::level::block::Block* const s_mycelium = Blocks::getBlock("minecraft:mycelium");
    const ::world::ChunkPos chunkPos = chunk->getPos();
    // The range the carvers can mark: CarvingMask(minGenY + 1, top - 7).
    const int32_t protectedBlocksOnTop = chunk->isUpgrading() ? 0 : 7;
    const int32_t minY = context.getMinGenY() + 1;
    const int32_t maxY = context.getMinGenY() + context.getGenDepth() - 1 - protectedBlocksOnTop;
    density::Aquifer& aquifer = noiseChunk.aquifer();
    auto voidBiomeGetter = [&biomeGetter](const core::BlockPos& blockPos) -> void* {
        return const_cast<world::biome::Biome*>(biomeGetter(blockPos));
    };

    core::BlockPos::MutableBlockPos blockPos;
    core::BlockPos::MutableBlockPos helperPos;
    for (int32_t x = 0; x < 16; ++x) {
        const int32_t worldX = chunkPos.getBlockX(x);
        for (int32_t z = 0; z < 16; ++z) {
            const int32_t worldZ = chunkPos.getBlockZ(z);
            int32_t y = minY;
            while (y <= maxY) {
                if (!mask.get(x, y, z)) { ++y; continue; }
                const int32_t bottomY = y;
                while (y + 1 <= maxY && mask.get(x, y + 1, z)) ++y;
                const int32_t topY = y;
                ++y;

                bool hasGrass = false;
                for (int32_t worldY = topY; worldY >= bottomY; --worldY) {
                    blockPos.set(worldX, worldY, worldZ);
                    BlockState* blockState = chunk->getBlockState(blockPos);
                    if (blockState == nullptr || blockState->is(Blocks::BEDROCK)) continue;
                    if (blockState->is(Blocks::GRASS_BLOCK) || blockState->is(s_mycelium)) {
                        hasGrass = true;
                    }

                    BlockState* state = aquifer.computeSubstance(worldX, worldY, worldZ, 0.0);
                    if (state == nullptr) continue;

                    chunk->setBlockState(blockPos, state, false);
                    if (aquifer.shouldScheduleFluidUpdate() && state->isFluid()) {
                        chunk->markPosForPostprocessing(blockPos);
                    }

                    if (hasGrass) {
                        helperPos.setWithOffset(blockPos, 0, -1, 0);
                        BlockState* below = chunk->getBlockState(helperPos);
                        if (below != nullptr && below->is(Blocks::DIRT)) {
                            BlockState* topMaterial =
                                context.topMaterial(voidBiomeGetter, chunk, helperPos, state->isFluid());
                            if (topMaterial != nullptr) {
                                chunk->setBlockState(helperPos, topMaterial, false);
                                if (topMaterial->isFluid()) chunk->markPosForPostprocessing(helperPos);
                            }
                        }
                    }
                }
            }
        }
    }
}

int32_t NoiseBasedChunkGenerator::iterateNoiseColumn(int32_t blockX, int32_t blockZ, RandomState* randomState,
                                                     std::vector<BlockState*>* writeTo,
                                                     const std::function<bool(const BlockState*)>* tester) const {
    // Reference: NoiseBasedChunkGenerator.iterateNoiseColumn (26.3). The
    // height accessor is the generator's own range (every noise dimension's
    // level range equals it).
    const density::NoiseSettings noiseSettings = m_settings->noiseSettings();
    if (noiseSettings.height <= 0 || randomState == nullptr) return INT32_MIN;
    const DensityVolume volume(1, noiseSettings.height, 1, blockX, noiseSettings.minY, blockZ);
    if (writeTo != nullptr) writeTo->assign(static_cast<size_t>(volume.sizeY), nullptr);
    density::NoiseChunk noiseChunk(randomState->density(), nullptr, m_settings->terrain(),
                                   createFluidPicker(*m_settings), volume);
    density::Aquifer& aquifer = noiseChunk.aquifer();
    BlockState* const defaultState = m_settings->defaultBlock();
    density::ScopedBuffer densityBuffer =
        noiseChunk.cachingSamplers().get(m_settings->noiseRouter().finalDensity).sampleVolume(noiseChunk.volume());
    for (int y = volume.sizeY - 1; y >= 0; --y) {
        const float density = densityBuffer->get(volume.indexUnchecked(0, y, 0));
        const int blockY = volume.blockY(y);
        BlockState* baseState = aquifer.computeSubstance(blockX, blockY, blockZ, static_cast<double>(density));
        BlockState* state = baseState == nullptr ? defaultState : baseState;
        if (writeTo != nullptr) (*writeTo)[static_cast<size_t>(y)] = state;
        if (tester != nullptr && (*tester)(state)) {
            return blockY + 1;
        }
    }
    return INT32_MIN;
}

int32_t NoiseBasedChunkGenerator::getBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                                                RandomState* randomState) const {
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

int32_t NoiseBasedChunkGenerator::computeBaseHeight(int32_t x, int32_t z, Heightmap::Types heightmapType,
                                                    RandomState* randomState) const {
    // iterateNoiseColumn(..., type.isOpaque()).orElse(heightAccessor.getMinY())
    TERRAIN_ZONE_N("NBCG.BaseHeight");
    const Heightmap::OpaquePredicate isOpaque = Heightmap::getOpaquePredicate(heightmapType);
    const std::function<bool(const BlockState*)> tester = [&isOpaque](const BlockState* state) {
        return state != nullptr && isOpaque(const_cast<BlockState*>(state));
    };
    const int32_t height = iterateNoiseColumn(x, z, randomState, nullptr, &tester);
    return height == INT32_MIN ? getMinY() : height;
}

void NoiseBasedChunkGenerator::getBaseColumn(int32_t x, int32_t z, RandomState* randomState,
                                             std::vector<BlockState*>& outColumn) const {
    outColumn.clear();
    iterateNoiseColumn(x, z, randomState, &outColumn, nullptr);
    if (outColumn.empty()) outColumn.assign(static_cast<size_t>(std::max(0, getGenDepth())), nullptr);
}

void NoiseBasedChunkGenerator::createBiomes(RandomState* randomState, Blender* blender, ::world::IChunk* chunk) {
    // Reference: ChunkGenerator.doCreateBiomes (26.3).
    (void)blender;
    auto* protoChunk = dynamic_cast<minecraft::world::ProtoChunk*>(chunk);
    if (protoChunk == nullptr || randomState == nullptr || m_biomeSource == nullptr) return;
    TERRAIN_ZONE_N("Biomes.Fill");
    std::unique_ptr<density::DensityBufferPool> bufferPool = randomState->density().acquireDensityBufferPool();
    {
        density::SamplerContext samplerContext =
            density::SamplerContext::builder().enableCaches().useBufferArena(*bufferPool).build();
        const world::biome::Climate::Sampler climateSampler = randomState->createClimateSampler(samplerContext);
        const ::world::ChunkPos pos = chunk->getPos();
        const world::biome::BiomeSource::BiomeResolver biomeResolver = m_biomeSource->createResolverForChunk(
            climateSampler, core::QuartPos::fromBlock(pos.getMinBlockX()),
            core::QuartPos::fromBlock(chunk->getMinBuildHeight()), core::QuartPos::fromBlock(pos.getMinBlockZ()),
            core::QuartPos::fromBlock(16),
            core::QuartPos::fromBlock(chunk->getMaxBuildHeight() - chunk->getMinBuildHeight()),
            core::QuartPos::fromBlock(16));
        protoChunk->fillBiomesFromNoise(biomeResolver);
    }
    randomState->density().releaseDensityBufferPool(std::move(bufferPool));
}

::world::ChunkPos NoiseBasedChunkGenerator::getOrigin(RandomState* randomState) const {
    // Reference: NoiseBasedChunkGenerator.getOrigin (26.3). ChunkGenerator's
    // default origin is ChunkPos.ZERO.
    const auto& spawnTarget = m_settings->terrain().spawnTarget;
    if (spawnTarget.empty() || randomState == nullptr) return ::world::ChunkPos(0, 0);
    density::SamplerContext samplerContext = density::SamplerContext::builder().enableCaches().build();
    const density::DensitySamplerSet samplers = randomState->density().samplersWithContext(samplerContext);
    const core::BlockPos spawn = density::NoiseSpawnFinder::findSpawnPosition(spawnTarget, samplers);
    return ::world::ChunkPos(spawn.getX() >> 4, spawn.getZ() >> 4);
}

void NoiseBasedChunkGenerator::addDebugScreenInfo(std::vector<std::string>& result, RandomState* randomState,
                                                  const core::BlockPos& feetPos,
                                                  density::SamplerContext& samplerContext) const {
    const auto& functions = m_settings->terrain().debugFunctions;
    if (functions.empty() || randomState == nullptr) return;
    const density::DensitySamplerSet samplers = randomState->density().samplersWithContext(samplerContext);
    std::string builder = "Density ";
    for (const density::DebugFunctionEntry& entry : functions) {
        // DecimalFormat("0.000", Locale.ROOT)
        char value[48];
        std::snprintf(value, sizeof(value), "%.3f",
                      static_cast<double>(samplers.sampleValue(entry.function, feetPos.getX(), feetPos.getY(),
                                                               feetPos.getZ())));
        builder += entry.label;
        builder += ": ";
        builder += value;
        builder += ' ';
    }
    builder.pop_back();
    result.push_back(std::move(builder));
}

} // namespace levelgen
} // namespace minecraft
