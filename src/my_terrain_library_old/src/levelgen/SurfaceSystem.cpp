#include "levelgen/SurfaceSystem.h"
#include "levelgen/SurfaceRules.h"
#include "levelgen/RandomState.h"
#include "levelgen/NoiseRegistry.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/Heightmap.h"
#include "synth/NormalNoise.h"
#include "random/XoroshiroRandomSource.h"
#include "random/PositionalRandomFactory.h"
#include "world/MinecraftBlockType.h"
#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"
#include "core/BlockPos.h"
#include "math/Mth.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <iostream>

namespace minecraft {
namespace levelgen {

// Reference: SurfaceSystem.java lines 295-307
void SurfaceSystem::makeBands(
    XoroshiroRandomSource& random,
    std::vector<::world::IBlockType*>& clayBands,
    int32_t baseWidth,
    ::world::IBlockType* state
) {
    // int bandCount = random.nextIntBetweenInclusive(6, 15);
    int32_t bandCount = 6 + random.nextInt(10);  // 6-15 inclusive

    for (int32_t i = 0; i < bandCount; ++i) {
        // int width = baseWidth + random.nextInt(3);
        int32_t width = baseWidth + random.nextInt(3);
        // int start = random.nextInt(clayBands.length);
        int32_t start = random.nextInt(static_cast<int32_t>(clayBands.size()));

        for (int32_t p = 0; start + p < static_cast<int32_t>(clayBands.size()) && p < width; ++p) {
            clayBands[start + p] = state;
        }
    }
}

// Reference: SurfaceSystem.java lines 262-293
std::vector<::world::IBlockType*> SurfaceSystem::generateBands(XoroshiroRandomSource& random) {
    // Line 263: BlockState[] clayBands = new BlockState[192];
    std::vector<::world::IBlockType*> clayBands(192, ::world::MinecraftBlocks::TERRACOTTA());

    // Lines 266-271: Add orange terracotta bands
    for (int32_t i = 0; i < static_cast<int32_t>(clayBands.size()); ) {
        i += random.nextInt(5) + 1;
        if (i < static_cast<int32_t>(clayBands.size())) {
            clayBands[i] = ::world::MinecraftBlocks::ORANGE_TERRACOTTA();
        }
    }

    // Lines 273-275: Add colored bands
    makeBands(random, clayBands, 1, ::world::MinecraftBlocks::YELLOW_TERRACOTTA());
    makeBands(random, clayBands, 2, ::world::MinecraftBlocks::BROWN_TERRACOTTA());
    makeBands(random, clayBands, 1, ::world::MinecraftBlocks::RED_TERRACOTTA());

    // Lines 276-290: Add white terracotta bands
    // int whiteBandCount = random.nextIntBetweenInclusive(9, 15);
    int32_t whiteBandCount = 9 + random.nextInt(7);  // 9-15 inclusive
    int32_t bandIndex = 0;

    for (int32_t start = 0; bandIndex < whiteBandCount && start < static_cast<int32_t>(clayBands.size()); start += random.nextInt(16) + 4) {
        clayBands[start] = ::world::MinecraftBlocks::WHITE_TERRACOTTA();

        if (start - 1 > 0 && random.nextBoolean()) {
            clayBands[start - 1] = ::world::MinecraftBlocks::LIGHT_GRAY_TERRACOTTA();
        }

        if (start + 1 < static_cast<int32_t>(clayBands.size()) && random.nextBoolean()) {
            clayBands[start + 1] = ::world::MinecraftBlocks::LIGHT_GRAY_TERRACOTTA();
        }

        ++bandIndex;
    }

    return clayBands;
}

// Reference: SurfaceSystem.java lines 51-65
SurfaceSystem::SurfaceSystem(
    RandomState* randomState,
    ::world::IBlockType* defaultBlock,
    int32_t seaLevel,
    random::PositionalRandomFactory* noiseRandom
)
    : m_defaultBlock(defaultBlock)
    , m_seaLevel(seaLevel)
    , m_noiseRandom(noiseRandom)
    , m_clayBandsOffsetNoise(nullptr)
    , m_badlandsPillarNoise(nullptr)
    , m_badlandsPillarRoofNoise(nullptr)
    , m_badlandsSurfaceNoise(nullptr)
    , m_icebergPillarNoise(nullptr)
    , m_icebergPillarRoofNoise(nullptr)
    , m_icebergSurfaceNoise(nullptr)
    , m_surfaceNoise(nullptr)
    , m_surfaceSecondaryNoise(nullptr)
{
    // Reference: SurfaceSystem.java line 55
    m_clayBandsOffsetNoise = randomState->getOrCreateNoise("clay_bands_offset");

    // Reference: SurfaceSystem.java line 56
    // Generate clay bands using positional random
    XoroshiroRandomSource clayBandRandomValue = m_noiseRandom->fromHashOf("minecraft:clay_bands");
    m_clayBands = generateBands(clayBandRandomValue);

    // Reference: SurfaceSystem.java lines 57-64
    m_surfaceNoise = randomState->getOrCreateNoise("surface");
    m_surfaceSecondaryNoise = randomState->getOrCreateNoise("surface_secondary");
    m_badlandsPillarNoise = randomState->getOrCreateNoise("badlands_pillar");
    m_badlandsPillarRoofNoise = randomState->getOrCreateNoise("badlands_pillar_roof");
    m_badlandsSurfaceNoise = randomState->getOrCreateNoise("badlands_surface");
    m_icebergPillarNoise = randomState->getOrCreateNoise("iceberg_pillar");
    m_icebergPillarRoofNoise = randomState->getOrCreateNoise("iceberg_pillar_roof");
    m_icebergSurfaceNoise = randomState->getOrCreateNoise("iceberg_surface");
}

// Destructor
SurfaceSystem::~SurfaceSystem() {
    // NOTE: We do NOT delete the noise instances here because they are owned by RandomState
}

// Reference: SurfaceSystem.java lines 161-164
int32_t SurfaceSystem::getSurfaceDepth(int32_t blockX, int32_t blockZ) const {
    // double noiseValue = this.surfaceNoise.getValue((double)blockX, 0.0, (double)blockZ);
    double noiseValue = m_surfaceNoise->getValue(
        static_cast<double>(blockX),
        0.0,
        static_cast<double>(blockZ)
    );

    // return (int)(noiseValue * 2.75 + 3.0 + this.noiseRandom.at(blockX, 0, blockZ).nextDouble() * 0.25);
    XoroshiroRandomSource posRandom = m_noiseRandom->at(blockX, 0, blockZ);
    return static_cast<int32_t>(noiseValue * 2.75 + 3.0 + posRandom.nextDouble() * 0.25);
}

// Reference: SurfaceSystem.java lines 166-168
double SurfaceSystem::getSurfaceSecondary(int32_t blockX, int32_t blockZ) const {
    // return this.surfaceSecondaryNoise.getValue((double)blockX, 0.0, (double)blockZ);
    return m_surfaceSecondaryNoise->getValue(
        static_cast<double>(blockX),
        0.0,
        static_cast<double>(blockZ)
    );
}

// Reference: SurfaceSystem.java lines 309-312
::world::IBlockType* SurfaceSystem::getBand(int32_t worldX, int32_t y, int32_t worldZ) const {
    // int offset = (int)Math.round(this.clayBandsOffsetNoise.getValue((double)worldX, 0.0, (double)worldZ) * 4.0);
    int32_t offset = static_cast<int32_t>(std::round(
        m_clayBandsOffsetNoise->getValue(
            static_cast<double>(worldX),
            0.0,
            static_cast<double>(worldZ)
        ) * 4.0
    ));

    // return this.clayBands[(y + offset + this.clayBands.length) % this.clayBands.length];
    int32_t size = static_cast<int32_t>(m_clayBands.size());
    int32_t index = ((y + offset) % size + size) % size;  // Handle negative modulo properly
    return m_clayBands[index];
}

// Reference: SurfaceSystem.java lines 170-172
bool SurfaceSystem::isStone(const ::world::IBlockType* block) const {
    // Check if block is not air and has no fluid state
    // In Minecraft: !state.isAir() && state.getFluidState().isEmpty()
    return block && !block->isAir() && !block->isFluid();
}

// Reference: SurfaceSystem.java lines 67-159
void SurfaceSystem::buildSurface(
    RandomState* randomState,
    std::function<void*(const ::minecraft::core::BlockPos&)> biomeGetter,
    bool useLegacyRandom,
    const WorldGenerationContext& generationContext,
    ::world::IChunk* chunk,
    NoiseChunk* noiseChunk,
    RuleSource* ruleSource
) {
    using namespace ::minecraft::core;

    // Reference: lines 69-71
    ::world::ChunkPos chunkPos = chunk->getPos();
    int32_t minBlockX = chunkPos.getMinBlockX();
    int32_t minBlockZ = chunkPos.getMinBlockZ();
    int32_t minY = chunk->getMinBuildHeight();

    // Reference: lines 97-98
    // Create context and apply rule source
    Context context(this, randomState, chunk, noiseChunk, biomeGetter, generationContext);
    std::unique_ptr<SurfaceRule> rule;
    {
        rule = ruleSource->apply(context);
    }

    BlockPos::MutableBlockPos blockPos;

    // Reference: lines 101-157 - iterate over chunk columns
    for (int32_t x = 0; x < 16; ++x) {
        for (int32_t z = 0; z < 16; ++z) {
            int32_t blockX = minBlockX + x;
            int32_t blockZ = minBlockZ + z;

            // Reference: line 105 - get starting height from heightmap
            // Reference: SurfaceSystem.java line 105
            int32_t startingHeight = chunk->getHeight(
                static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), x, z) + 1;

            // Reference: lines 107-110 - check for eroded badlands
            blockPos.set(blockX, useLegacyRandom ? 0 : startingHeight, blockZ);

            // Check if biome is eroded badlands
            void* biomePtr = biomeGetter(blockPos);

            if (biomePtr != nullptr) {
                const world::biome::Biome* surfaceBiome = static_cast<const world::biome::Biome*>(biomePtr);
                if (surfaceBiome->is(world::biome::BiomeKeys::ERODED_BADLANDS)) {
                    erodedBadlandsExtension(chunk, blockX, blockZ, startingHeight);
                }
            }

            // Reference: line 112 - get height again after potential extension
            // For eroded badlands, the height may have changed. For now, reuse startingHeight.
            // Reference: SurfaceSystem.java line 112
            int32_t height = chunk->getHeight(
                static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG), x, z) + 1;

            // Reference: line 113
            context.updateXZ(blockX, blockZ);

            // Reference: lines 114-117 - tracking variables
            int32_t stoneAboveDepth = 0;
            int32_t waterHeight = std::numeric_limits<int32_t>::min();
            int32_t nextCeilingStoneY = std::numeric_limits<int32_t>::max();
            int32_t endY = minY;

            // Reference: lines 119-151 - scan column from top to bottom
            for (int32_t y = height; y >= endY; --y) {
                // Get current block
                ::world::IBlockType* oldBlock = chunk->getBlockState(x, y, z);
                if (oldBlock == nullptr) continue;

                // Reference: lines 121-127 - check block type
                if (oldBlock->isAir()) {
                    // Air block - reset tracking
                    stoneAboveDepth = 0;
                    waterHeight = std::numeric_limits<int32_t>::min();
                } else if (oldBlock->isFluid()) {
                    // Fluid block - track water height
                    if (waterHeight == std::numeric_limits<int32_t>::min()) {
                        waterHeight = y + 1;
                    }
                } else {
                    // Stone block - apply surface rules
                    // Reference: lines 129-139 - look ahead for ceiling stone
                    if (nextCeilingStoneY >= y) {
                        nextCeilingStoneY = -2000000000;  // DimensionType.WAY_BELOW_MIN_Y

                        for (int32_t lookaheadY = y - 1; lookaheadY >= endY - 1; --lookaheadY) {
                            ::world::IBlockType* nextBlock = chunk->getBlockState(x, lookaheadY, z);
                            if (nextBlock == nullptr) continue;

                            if (!isStone(nextBlock)) {
                                nextCeilingStoneY = lookaheadY + 1;
                                break;
                            }
                        }
                    }

                    // Reference: lines 141-142
                    ++stoneAboveDepth;
                    int32_t stoneBelowDepth = y - nextCeilingStoneY + 1;

                    // Reference: line 143
                    context.updateY(stoneAboveDepth, stoneBelowDepth, waterHeight, blockX, y, blockZ);

                    // Reference: lines 144-149 - apply rule if block is default
                    // Check if current block is the default block (stone)
                    if (oldBlock == m_defaultBlock) {
                        ::world::IBlockType* newBlock = rule->tryApply(blockX, y, blockZ);

                        if (newBlock != nullptr) {
                            chunk->setBlockState(x, y, z, newBlock, false);

                            // Mark for post-processing if it has fluid
                            if (newBlock->isFluid()) {
                                BlockPos fluidPos(blockX, y, blockZ);
                                chunk->markPosForPostprocessing(fluidPos);
                            }
                        }
                    }
                }
            }

            // Reference: lines 153-155 - check for frozen ocean extension
            if (biomePtr != nullptr) {
                const world::biome::Biome* surfaceBiome = static_cast<const world::biome::Biome*>(biomePtr);
                if (surfaceBiome->is(world::biome::BiomeKeys::FROZEN_OCEAN) ||
                    surfaceBiome->is(world::biome::BiomeKeys::DEEP_FROZEN_OCEAN)) {
                    frozenOceanExtension(context.getMinSurfaceLevel(), surfaceBiome, chunk, blockX, blockZ, startingHeight);
                }
            }
        }
    }
}

// Reference: SurfaceSystem.java lines 192-219
void SurfaceSystem::erodedBadlandsExtension(
    ::world::IChunk* chunk,
    int32_t blockX,
    int32_t blockZ,
    int32_t height
) {
    // Reference: lines 193-194
    double pillarNoiseScale = 0.2;
    double pillarBuffer = std::min(
        std::abs(m_badlandsSurfaceNoise->getValue(
            static_cast<double>(blockX), 0.0, static_cast<double>(blockZ)) * 8.25),
        m_badlandsPillarNoise->getValue(
            static_cast<double>(blockX) * pillarNoiseScale,
            0.0,
            static_cast<double>(blockZ) * pillarNoiseScale) * 15.0
    );

    // Reference: line 195
    if (pillarBuffer <= 0.0) {
        return;
    }

    // Reference: lines 196-200
    double floorNoiseSampleResolution = 0.75;
    double floorAmplitude = 1.5;
    double pillarFloor = std::abs(m_badlandsPillarRoofNoise->getValue(
        static_cast<double>(blockX) * floorNoiseSampleResolution,
        0.0,
        static_cast<double>(blockZ) * floorNoiseSampleResolution) * floorAmplitude);

    double extensionTop = 64.0 + std::min(
        pillarBuffer * pillarBuffer * 2.5,
        std::ceil(pillarFloor * 50.0) + 24.0);

    int32_t startY = static_cast<int32_t>(std::floor(extensionTop));

    // Reference: line 201
    if (height > startY) {
        return;
    }

    int32_t minY = chunk->getMinBuildHeight();

    // Reference: lines 202-211 - check for water blocks
    for (int32_t y = startY; y >= minY; --y) {
        ::world::IBlockType* block = chunk->getBlockState(blockX & 15, y, blockZ & 15);
        if (block == nullptr) continue;

        if (block == m_defaultBlock) {
            break;  // Found stone
        }

        if (block->getIdentifier() == "minecraft:water") {
            return;  // Water found - don't extend
        }
    }

    // Reference: lines 213-215 - fill air with default block
    for (int32_t y = startY; y >= minY; --y) {
        ::world::IBlockType* block = chunk->getBlockState(blockX & 15, y, blockZ & 15);
        if (block == nullptr) continue;

        if (!block->isAir()) {
            break;
        }

        chunk->setBlockState(blockX & 15, y, blockZ & 15, m_defaultBlock, false);
    }
}

// Reference: SurfaceSystem.java lines 221-260
void SurfaceSystem::frozenOceanExtension(
    int32_t minSurfaceLevel,
    const world::biome::Biome* surfaceBiome,
    ::world::IChunk* chunk,
    int32_t blockX,
    int32_t blockZ,
    int32_t height
) {
    // Reference: lines 222-223
    double pillarScale = 1.28;
    double iceberg = std::min(
        std::abs(m_icebergSurfaceNoise->getValue(
            static_cast<double>(blockX), 0.0, static_cast<double>(blockZ)) * 8.25),
        m_icebergPillarNoise->getValue(
            static_cast<double>(blockX) * pillarScale,
            0.0,
            static_cast<double>(blockZ) * pillarScale) * 15.0
    );

    // Reference: line 224
    if (iceberg <= 1.8) {
        return;
    }

    // Reference: lines 225-228
    double roofScale = 1.17;
    double roofAmplitude = 1.5;
    double icebergRoof = std::abs(m_icebergPillarRoofNoise->getValue(
        static_cast<double>(blockX) * roofScale,
        0.0,
        static_cast<double>(blockZ) * roofScale) * roofAmplitude);

    double top = std::min(
        iceberg * iceberg * 1.2,
        std::ceil(icebergRoof * 40.0) + 14.0);

    // Reference: lines 229-231 - check if should melt slightly
    // Biome.shouldMeltFrozenOceanIcebergSlightly: return getTemperature(pos, seaLevel) > 0.1F
    if (surfaceBiome != nullptr) {
        core::BlockPos pos(blockX, m_seaLevel, blockZ);
        float temperature = surfaceBiome->getTemperature(pos, m_seaLevel);
        if (temperature > 0.1f) {
            top -= 2.0;
        }
    }

    // Reference: lines 233-240
    double extensionBottom;
    if (top > 2.0) {
        extensionBottom = static_cast<double>(m_seaLevel) - top - 7.0;
        top += static_cast<double>(m_seaLevel);
    } else {
        top = 0.0;
        extensionBottom = 0.0;
    }

    double extensionTop = top;

    // Reference: lines 243-246
    XoroshiroRandomSource random = m_noiseRandom->at(blockX, 0, blockZ);
    int32_t maxSnowDepth = 2 + random.nextInt(4);
    int32_t minSnowHeight = m_seaLevel + 18 + random.nextInt(10);
    int32_t snowDepth = 0;

    // Reference: lines 248-257
    int32_t startY = std::max(height, static_cast<int32_t>(top) + 1);
    for (int32_t y = startY; y >= minSurfaceLevel; --y) {
        ::world::IBlockType* block = chunk->getBlockState(blockX & 15, y, blockZ & 15);
        if (block == nullptr) continue;

        bool shouldPlace = false;
        if (block->isAir() && y < static_cast<int32_t>(extensionTop) && random.nextDouble() > 0.01) {
            shouldPlace = true;
        } else if (block->getIdentifier() == "minecraft:water" &&
                   y > static_cast<int32_t>(extensionBottom) &&
                   y < m_seaLevel &&
                   extensionBottom != 0.0 &&
                   random.nextDouble() > 0.15) {
            shouldPlace = true;
        }

        if (shouldPlace) {
            if (snowDepth <= maxSnowDepth && y > minSnowHeight) {
                // Place snow block
                ::world::IBlockType* snowBlock = ::world::MinecraftBlocks::get("minecraft:snow_block");
                if (snowBlock != nullptr) {
                    chunk->setBlockState(blockX & 15, y, blockZ & 15, snowBlock, false);
                }
                ++snowDepth;
            } else {
                // Place packed ice
                ::world::IBlockType* packedIce = ::world::MinecraftBlocks::get("minecraft:packed_ice");
                if (packedIce != nullptr) {
                    chunk->setBlockState(blockX & 15, y, blockZ & 15, packedIce, false);
                }
            }
        }
    }
}

} // namespace levelgen
} // namespace minecraft
