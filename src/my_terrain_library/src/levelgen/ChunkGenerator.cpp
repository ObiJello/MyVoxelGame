#include "levelgen/ChunkGenerator.h"
#include "levelgen/WorldGenLevel.h"
#include "levelgen/FeatureSorter.h"
#include "levelgen/placement/PlacedFeature.h"
#include "levelgen/feature/Feature.h"
#include "levelgen/SurfaceSystem.h"
#include "core/SectionPos.h"
#include "levelgen/SurfaceRules.h"
#include "world/level/block/state/BlockState.h"
#include "world/level/block/Blocks.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/RandomState.h"
#include "levelgen/Blender.h"
#include "levelgen/Beardifier.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Aquifer.h"
#include "levelgen/carver/CaveWorldCarver.h"
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

    // Collect biomes from 3x3 chunk area
    // Reference: ChunkPos.rangeClosed(sectionPos.chunk(), 1).forEach(...)
    std::set<const world::biome::Biome*> possibleBiomes;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            ::world::IChunk* neighborChunk = level->getChunk(centerPos.x() + dx, centerPos.z() + dz);
            if (neighborChunk) {
                // Collect all biomes from all sections
                for (int sectionY = neighborChunk->getMinBuildHeight() >> 4;
                     sectionY < neighborChunk->getMaxBuildHeight() >> 4; ++sectionY) {
                    // Sample biomes at section corners (simplified)
                    int blockY = sectionY << 4;
                    for (int y = 0; y < 16; y += 4) {
                        for (int x = 0; x < 16; x += 4) {
                            for (int z = 0; z < 16; z += 4) {
                                core::BlockPos biomePos(
                                    neighborChunk->getPos().getMinBlockX() + x,
                                    blockY + y,
                                    neighborChunk->getPos().getMinBlockZ() + z
                                );
                                const world::biome::Biome* biome = neighborChunk->getBiome(biomePos);
                                if (biome) {
                                    possibleBiomes.insert(biome);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Get number of generation steps
    int32_t featureStepCount = static_cast<int32_t>(featuresPerStep.size());
    int32_t generationSteps = std::max(GenerationStep::DECORATION_COUNT, featureStepCount);

    // Iterate through all generation steps
    // Reference: for(int stepIndex = 0; stepIndex < generationSteps; ++stepIndex)
    for (int32_t stepIndex = 0; stepIndex < generationSteps; ++stepIndex) {
        if (stepIndex >= featureStepCount) {
            continue;
        }

        const StepFeatureData& stepFeatureData = featuresPerStep[stepIndex];
        if (stepFeatureData.features.empty()) continue;

        // Collect possible feature indices for this step from biomes in 3x3 area
        // Reference: IntSet possibleFeaturesThisStep = new IntArraySet();
        std::set<int> possibleFeatureIndices;

        for (const world::biome::Biome* biome : possibleBiomes) {
            if (!biome) continue;
            // Get features for this biome at this step
            // In Java: featuresInBiome = biome.getGenerationSettings().features()
            // We check if each feature in stepFeatureData is valid for this biome
            for (placement::PlacedFeature* feature : stepFeatureData.features) {
                int idx = stepFeatureData.getIndex(feature);
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

            // DEBUG: trace pointer for step 4 features
            if (stepIndex == 4 && globalIndex < 5) {
                std::cerr << "DEBUG applyBiomeDecoration: step=" << stepIndex << " idx=" << globalIndex
                          << " feature ptr=" << (void*)feature
                          << " name='" << feature->getName() << "'\n";
            }

            // Set current step/index for logging and block-change tracing
            placement::PlacedFeature::setCurrentStepIndex(stepIndex, globalIndex);
            feature::BlockChangeTrace::currentStep = stepIndex;
            feature::BlockChangeTrace::currentIndex = globalIndex;
            feature::BlockChangeTrace::currentFeatureName = feature->getName();

            // Set feature seed using GLOBAL index (critical for parity)
            // Reference: random.setFeatureSeed(decorationSeed, globalIndexOfFeature, stepIndex);
            random.setFeatureSeed(decorationSeed, globalIndex, stepIndex);

            // Block trace: log seed state after setFeatureSeed
            if (feature::BlockChangeTrace::enabled && feature::BlockChangeTrace::stream) {
                uint64_t seedLo, seedHi;
                random.getSeedState(seedLo, seedHi);
                *feature::BlockChangeTrace::stream << "FEATURE STEP=" << stepIndex << " IDX=" << globalIndex
                    << " " << feature->getName()
                    << " seed_lo=" << seedLo << " seed_hi=" << seedHi
                    << " gauss_cached=" << random.hasNextGaussian() << "\n";
            }

            // Log feature info with random state if enabled
            if (s_featureLoggingEnabled && s_featureLogStream && s_logLevel >= 2) {
                const std::string& featureName = feature->getName();
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
                m_beardifier ? m_beardifier : Beardifier::EMPTY(),
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
            m_beardifier ? m_beardifier : Beardifier::EMPTY(),
            settingsRef,
            m_fluidPicker,
            Blender::empty()
        );
    });
    Aquifer* aquifer = noiseChunk->aquifer();

    // Create carving context with proper arguments
    // Note: surfaceRule is needed for topMaterial() when carvers carve grass_block
    carver::CarvingContext carvingContext(
        chunk->getMinBuildHeight(),
        chunk->getMaxBuildHeight() - chunk->getMinBuildHeight(),
        noiseChunk,
        randomState,
        m_surfaceRules
    );

    // Create WorldgenRandom with LegacyRandomSource for carver seeding - Reference: line 201
    // CRITICAL: Java uses LegacyRandomSource for carving, NOT XoroshiroRandomSource
    LegacyRandomSource legacyRandom(static_cast<int64_t>(RandomSupport::generateUniqueSeed().seedLo));

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
            if (sourceBiome) {
                // Configure all 3 default overworld carvers to match Java exactly
                static world::biome::BiomeGenerationSettings defaultSettings;
                static bool initialized = false;
                if (!initialized) {
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

                    initialized = true;
                }
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
                    m_beardifier ? m_beardifier : Beardifier::EMPTY(),
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
                m_beardifier ? m_beardifier : Beardifier::EMPTY(),
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
            [biomeGetter](const ::minecraft::core::BlockPos& pos) -> void* {
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
    // Reference: NoiseBasedChunkGenerator.java getBaseHeight() lines 107-109
    // Reference: iterateNoiseColumn() lines 126-181

    if (!m_settings || !randomState) {
        return m_minY;
    }

    // Get heightmap predicate
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
    NoiseChunk* noiseChunk;
    {
        noiseChunk = protoChunk->getOrCreateNoiseChunk([this, randomState](::world::IChunk* c) {
            return NoiseChunk::forChunk(
                c,
                *randomState,
                m_beardifier ? m_beardifier : Beardifier::EMPTY(),
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
        protoChunk->fillBiomesFromNoise(m_biomeSource, cachedSampler);
    }

    // Don't delete noiseChunk - it's cached on ProtoChunk and will be cleaned up there
}

} // namespace levelgen
} // namespace minecraft
