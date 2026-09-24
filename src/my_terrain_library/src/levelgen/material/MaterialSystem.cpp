#include "levelgen/material/MaterialSystem.h"

#include "levelgen/Heightmap.h"
#include "levelgen/WorldGenerationContext.h"
#include "levelgen/density/JavaMath.h"
#include "levelgen/density/terrain/NoiseChunk.h"
#include "levelgen/density/terrain/RandomState.h"
#include "world/IChunk.h"
#include "world/LevelChunkSection.h"
#include "world/biome/Biome.h"
#include "world/biome/Biomes.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

// Reference: levelgen.material.MaterialSystem (26.3).

namespace minecraft {
namespace levelgen {
namespace material {

using world::level::block::Block;
using world::level::block::Blocks;

namespace {

// DimensionType.WAY_BELOW_MIN_Y = MIN_Y << 4.
constexpr int32_t WAY_BELOW_MIN_Y = -2032 * 16;

constexpr int HEIGHTMAP_WORLD_SURFACE_WG = static_cast<int>(Heightmap::Types::WORLD_SURFACE_WG);

// Math.min(double, double): NaN wins, -0.0 < +0.0.
double javaMin(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? a : b;
    return a <= b ? a : b;
}

// Math.abs(double).
double javaAbs(double a) {
    return std::fabs(a);
}

BlockState* defaultState(Block* block, const char* name) {
    if (block == nullptr) throw std::runtime_error(std::string("MaterialSystem: block not registered: ") + name);
    return block->defaultBlockState();
}

// !state.getFluidState().isEmpty(), memoised per state for one pass: the
// engine answers it from the block id and properties, and a surface pass asks
// it for every block of every column.
class FluidStateCache {
public:
    bool hasFluid(const BlockState* state) {
        for (int i = 0; i < m_size; ++i) {
            if (m_states[i] == state) return m_values[i];
        }
        const bool value = state->hasAnyFluid();
        const int slot = m_size < CAPACITY ? m_size++ : (m_next++ % CAPACITY);
        m_states[slot] = state;
        m_values[slot] = value;
        return value;
    }

private:
    static constexpr int CAPACITY = 16;
    const BlockState* m_states[CAPACITY] = {};
    bool m_values[CAPACITY] = {};
    int m_size = 0;
    int m_next = 0;
};

// ChunkAccess.getHighestFilledSectionIndex: -1 when every section is air.
int32_t highestFilledSectionIndex(world::IChunk* chunk) {
    for (int32_t i = chunk->getSectionsCount() - 1; i >= 0; --i) {
        if (!chunk->getSection(i).hasOnlyAir()) return i;
    }
    return -1;
}

} // namespace

// The anonymous BlockColumn of buildSurface: reads and writes one column of
// the chunk, skipping writes outside the build height, and schedules fluid
// states for post-processing.
class MaterialSystem::ChunkColumn {
public:
    ChunkColumn(world::IChunk* chunk, int32_t minY, int32_t maxY, FluidStateCache& fluids)
        : m_chunk(chunk), m_minY(minY), m_maxY(maxY), m_fluids(fluids), m_air(Blocks::AIR->defaultBlockState()) {}

    void setXZ(int32_t blockX, int32_t blockZ) {
        m_pos.setX(blockX);
        m_pos.setZ(blockZ);
    }

    BlockState* getBlock(int32_t blockY) {
        m_pos.setY(blockY);
        BlockState* state = m_chunk->getBlockState(m_pos);
        return state != nullptr ? state : m_air;
    }

    void setBlock(int32_t blockY, BlockState* state) {
        if (blockY >= m_minY && blockY <= m_maxY) {
            m_pos.setY(blockY);
            m_chunk->setBlockState(m_pos, state, false);
            if (m_fluids.hasFluid(state)) m_chunk->markPosForPostprocessing(m_pos);
        }
    }

private:
    world::IChunk* m_chunk;
    int32_t m_minY;
    int32_t m_maxY;
    FluidStateCache& m_fluids;
    BlockState* m_air;
    core::BlockPos::MutableBlockPos m_pos;
};

MaterialSystem::MaterialSystem(density::RandomState& randomState, BlockState* defaultBlock, int seaLevel,
                               density::DensityFunctionPtr preliminarySurfaceFunction,
                               random::AnyPositionalRandomFactory noiseRandom)
    : m_defaultBlock(defaultBlock),
      m_seaLevel(seaLevel),
      m_preliminarySurfaceFunction(std::move(preliminarySurfaceFunction)),
      m_noiseRandom(noiseRandom) {
    if (m_defaultBlock == nullptr) throw std::invalid_argument("MaterialSystem: null default block");
    if (!m_preliminarySurfaceFunction) throw std::invalid_argument("MaterialSystem: null preliminary surface function");
    m_clayBandsOffsetNoise = randomState.getOrCreateNoise("minecraft:clay_bands_offset");
    random::AnyRandomSource clayBandsRandom = m_noiseRandom.fromHashOf("minecraft:clay_bands");
    m_clayBands = generateBands(clayBandsRandom);
    m_surfaceNoise = randomState.getOrCreateNoise("minecraft:surface");
    m_surfaceSecondaryNoise = randomState.getOrCreateNoise("minecraft:surface_secondary");
    m_badlandsPillarNoise = randomState.getOrCreateNoise("minecraft:badlands_pillar");
    m_badlandsPillarRoofNoise = randomState.getOrCreateNoise("minecraft:badlands_pillar_roof");
    m_badlandsSurfaceNoise = randomState.getOrCreateNoise("minecraft:badlands_surface");
    m_icebergPillarNoise = randomState.getOrCreateNoise("minecraft:iceberg_pillar");
    m_icebergPillarRoofNoise = randomState.getOrCreateNoise("minecraft:iceberg_pillar_roof");
    m_icebergSurfaceNoise = randomState.getOrCreateNoise("minecraft:iceberg_surface");
}

GenerationContext MaterialSystem::generationContext(const levelgen::WorldGenerationContext& context) const {
    GenerationContext result;
    result.minGenY = context.getMinGenY();
    result.genDepth = context.getGenDepth();
    result.seaLevel = m_seaLevel;
    return result;
}

void MaterialSystem::buildSurface(density::RandomState& randomState, const BiomeGetter& biomeGetter,
                                  const levelgen::WorldGenerationContext& generationContext,
                                  world::IChunk* protoChunk, density::NoiseChunk& noiseChunk,
                                  const MaterialRule& ruleSource, const PossibleBiomes* possibleBiomes) const {
    const world::ChunkPos chunkPos = protoChunk->getPos();
    const int32_t minBlockX = chunkPos.getMinBlockX();
    const int32_t minBlockZ = chunkPos.getMinBlockZ();
    // getHeightAccessorForGeneration() is the chunk itself; LevelHeightAccessor
    // .getMaxY() is inclusive (the engine's getMaxBuildHeight() is not).
    const int32_t minY = protoChunk->getMinY();
    const int32_t maxY = protoChunk->getMaxBuildHeight() - 1;
    FluidStateCache fluids;
    ChunkColumn column(protoChunk, minY, maxY, fluids);

    // getSectionYFromSectionIndex(i) = i + getMinSectionY().
    const int32_t highestFilledSection = highestFilledSectionIndex(protoChunk);
    const int32_t maxBlockY = ((minY >> 4) + highestFilledSection) * 16 + 15;
    const density::DensityVolume& fullVolume = noiseChunk.volume();
    const density::DensityVolume narrowedVolume(fullVolume.sizeX, std::max(maxBlockY - fullVolume.minBlockY + 1, 1),
                                                fullVolume.sizeZ, fullVolume.minBlockX, fullVolume.minBlockY,
                                                fullVolume.minBlockZ);
    MaterialRuleContext context(*this, randomState, narrowedVolume, noiseChunk.cachingSamplers(), biomeGetter,
                                this->generationContext(generationContext), possibleBiomes);
    RuleEvaluatorPtr rule = ruleSource.compile(context);
    core::BlockPos::MutableBlockPos blockPos;

    for (int32_t x = 0; x < 16; ++x) {
        for (int32_t z = 0; z < 16; ++z) {
            const int32_t blockX = minBlockX + x;
            const int32_t blockZ = minBlockZ + z;
            const int32_t startingHeight = protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, x, z) + 1;
            column.setXZ(blockX, blockZ);
            const world::biome::Biome* surfaceBiome = biomeGetter(blockPos.set(blockX, startingHeight, blockZ));
            if (surfaceBiome != nullptr && surfaceBiome->is(world::biome::BiomeKeys::ERODED_BADLANDS)) {
                erodedBadlandsExtension(column, blockX, blockZ, startingHeight, protoChunk);
            }

            const int32_t height = protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, x, z) + 1;
            const int32_t gradientX = getSurfaceGradientX(protoChunk, x, z);
            const int32_t gradientZ = getSurfaceGradientZ(protoChunk, x, z);
            context.updateXZ(blockX, blockZ, gradientX, gradientZ);
            int32_t stoneAboveDepth = 0;
            int32_t waterHeight = INT32_MIN;
            int32_t nextCeilingStoneY = INT32_MAX;
            const int32_t endY = protoChunk->getMinY();

            for (int32_t y = height; y >= endY; --y) {
                BlockState* old = column.getBlock(y);
                if (old->isAir()) {
                    stoneAboveDepth = 0;
                    waterHeight = INT32_MIN;
                } else if (fluids.hasFluid(old)) {
                    if (waterHeight == INT32_MIN) waterHeight = y + 1;
                } else {
                    if (nextCeilingStoneY >= y) {
                        nextCeilingStoneY = WAY_BELOW_MIN_Y;
                        for (int32_t lookaheadY = y - 1; lookaheadY >= endY - 1; --lookaheadY) {
                            BlockState* state = column.getBlock(lookaheadY);
                            // isStone: !isAir() && getFluidState().isEmpty()
                            if (state->isAir() || fluids.hasFluid(state)) {
                                nextCeilingStoneY = lookaheadY + 1;
                                break;
                            }
                        }
                    }

                    ++stoneAboveDepth;
                    const int32_t stoneBelowDepth = y - nextCeilingStoneY + 1;
                    context.updateY(stoneAboveDepth, stoneBelowDepth, waterHeight, y);
                    if (y >= minY && y <= maxY) {
                        BlockState* state = rule->tryApply(blockX, y, blockZ);
                        if (state != nullptr) column.setBlock(y, state);
                    }
                }
            }

            if (surfaceBiome != nullptr && (surfaceBiome->is(world::biome::BiomeKeys::FROZEN_OCEAN) ||
                                            surfaceBiome->is(world::biome::BiomeKeys::DEEP_FROZEN_OCEAN))) {
                frozenOceanExtension(context.getMinSurfaceLevel(), surfaceBiome, column, blockPos, blockX, blockZ,
                                     startingHeight);
            }
        }
    }
}

int32_t MaterialSystem::getSurfaceGradientX(world::IChunk* protoChunk, int32_t x, int32_t z) {
    return protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, std::min(x + 1, 15), z) -
           protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, std::max(x - 1, 0), z);
}

int32_t MaterialSystem::getSurfaceGradientZ(world::IChunk* protoChunk, int32_t x, int32_t z) {
    return protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, x, std::min(z + 1, 15)) -
           protoChunk->getHeight(HEIGHTMAP_WORLD_SURFACE_WG, x, std::max(z - 1, 0));
}

int32_t MaterialSystem::getSurfaceDepth(int32_t blockX, int32_t blockZ) const {
    const double noiseValue =
        static_cast<double>(m_surfaceNoise->get(static_cast<double>(blockX), 0.0, static_cast<double>(blockZ)));
    random::AnyRandomSource random = m_noiseRandom.at(blockX, 0, blockZ);
    return density::jmath::d2i(noiseValue * 2.75 + 3.0 + random.nextDouble() * 0.25);
}

double MaterialSystem::getSurfaceSecondary(int32_t blockX, int32_t blockZ) const {
    return static_cast<double>(
        m_surfaceSecondaryNoise->get(static_cast<double>(blockX), 0.0, static_cast<double>(blockZ)));
}

BlockState* MaterialSystem::topMaterial(const MaterialRule& ruleSource, density::RandomState& randomState,
                                        const levelgen::WorldGenerationContext& worldGenerationContext,
                                        const BiomeGetter& biomeGetter, world::IChunk* chunk,
                                        const density::DensitySamplerSet& densitySamplers, const core::BlockPos& pos,
                                        bool underFluid) const {
    const density::DensityVolume volume(1, 1, 1, pos.getX(), pos.getY(), pos.getZ());
    MaterialRuleContext context(*this, randomState, volume, densitySamplers, biomeGetter,
                                generationContext(worldGenerationContext), nullptr);
    RuleEvaluatorPtr rule = ruleSource.compile(context);
    const int32_t blockX = pos.getX();
    const int32_t blockY = pos.getY();
    const int32_t blockZ = pos.getZ();
    // SectionPos.sectionRelative.
    const int32_t gradientX = getSurfaceGradientX(chunk, pos.getX() & 15, pos.getZ() & 15);
    const int32_t gradientZ = getSurfaceGradientZ(chunk, pos.getX() & 15, pos.getZ() & 15);
    context.updateXZ(blockX, blockZ, gradientX, gradientZ);
    context.updateY(1, 1, underFluid ? blockY + 1 : INT32_MIN, blockY);
    return rule->tryApply(blockX, blockY, blockZ);
}

void MaterialSystem::erodedBadlandsExtension(ChunkColumn& column, int32_t blockX, int32_t blockZ, int32_t height,
                                             world::IChunk* protoChunk) const {
    const double pillarBuffer = javaMin(
        javaAbs(static_cast<double>(m_badlandsSurfaceNoise->get(static_cast<double>(blockX), 0.0,
                                                                static_cast<double>(blockZ))) *
                8.25),
        static_cast<double>(m_badlandsPillarNoise->get(static_cast<double>(blockX) * 0.2, 0.0,
                                                       static_cast<double>(blockZ) * 0.2) *
                            15.0f));
    if (pillarBuffer <= 0.0) return;

    const double pillarFloor = javaAbs(
        static_cast<double>(m_badlandsPillarRoofNoise->get(static_cast<double>(blockX) * 0.75, 0.0,
                                                           static_cast<double>(blockZ) * 0.75)) *
        1.5);
    const double extensionTop =
        64.0 + javaMin(pillarBuffer * pillarBuffer * 2.5, std::ceil(pillarFloor * 50.0) + 24.0);
    const int32_t startY = density::jmath::floor(extensionTop);
    if (height > startY) return;

    Block* defaultBlockType = m_defaultBlock->getBlock();
    for (int32_t y = startY; y >= protoChunk->getMinY(); --y) {
        BlockState* oldState = column.getBlock(y);
        if (oldState->is(defaultBlockType)) break;
        if (oldState->is(Blocks::WATER)) return;
    }

    for (int32_t y = startY; y >= protoChunk->getMinY() && column.getBlock(y)->isAir(); --y) {
        column.setBlock(y, m_defaultBlock);
    }
}

void MaterialSystem::frozenOceanExtension(int32_t minSurfaceLevel, const world::biome::Biome* surfaceBiome,
                                          ChunkColumn& column, core::BlockPos::MutableBlockPos& blockPos,
                                          int32_t blockX, int32_t blockZ, int32_t height) const {
    const double iceberg = javaMin(
        javaAbs(static_cast<double>(m_icebergSurfaceNoise->get(static_cast<double>(blockX), 0.0,
                                                               static_cast<double>(blockZ))) *
                8.25),
        static_cast<double>(m_icebergPillarNoise->get(static_cast<double>(blockX) * 1.28, 0.0,
                                                      static_cast<double>(blockZ) * 1.28) *
                            15.0f));
    if (iceberg <= 1.8) return;

    const double icebergRoof = javaAbs(
        static_cast<double>(m_icebergPillarRoofNoise->get(static_cast<double>(blockX) * 1.17, 0.0,
                                                          static_cast<double>(blockZ) * 1.17)) *
        1.5);
    double top = javaMin(iceberg * iceberg * 1.2, std::ceil(icebergRoof * 40.0) + 14.0);
    // Biome.shouldMeltFrozenOceanIcebergSlightly: getTemperature(pos, seaLevel) > 0.1F.
    if (surfaceBiome->getTemperature(blockPos.set(blockX, m_seaLevel, blockZ), m_seaLevel) > 0.1f) {
        top -= 2.0;
    }
    if (top <= 2.0) return;

    const double extensionBottom = static_cast<double>(m_seaLevel) - top - 7.0;
    top += static_cast<double>(m_seaLevel);
    const double extensionTop = top;
    random::AnyRandomSource random = m_noiseRandom.at(blockX, 0, blockZ);
    const int32_t maxSnowDepth = 2 + random.nextInt(4);
    const int32_t minSnowHeight = m_seaLevel + 18 + random.nextInt(10);
    int32_t snowDepth = 0;
    const int32_t extensionTopY = density::jmath::d2i(extensionTop);
    const int32_t extensionBottomY = density::jmath::d2i(extensionBottom);
    BlockState* snowBlock = defaultState(Blocks::SNOW_BLOCK, "minecraft:snow_block");
    BlockState* packedIce = defaultState(Blocks::PACKED_ICE, "minecraft:packed_ice");

    for (int32_t y = std::max(height, extensionTopY + 1); y >= minSurfaceLevel; --y) {
        if ((column.getBlock(y)->isAir() && y < extensionTopY && random.nextDouble() > 0.01) ||
            (column.getBlock(y)->is(Blocks::WATER) && y > extensionBottomY && y < m_seaLevel &&
             random.nextDouble() > 0.15)) {
            if (snowDepth <= maxSnowDepth && y > minSnowHeight) {
                column.setBlock(y, snowBlock);
                ++snowDepth;
            } else {
                column.setBlock(y, packedIce);
            }
        }
    }
}

std::array<BlockState*, MaterialSystem::CLAY_BAND_COUNT> MaterialSystem::generateBands(random::AnyRandomSource& random) {
    BlockState* const terracotta = defaultState(Blocks::TERRACOTTA, "minecraft:terracotta");
    BlockState* const orangeTerracotta = defaultState(Blocks::ORANGE_TERRACOTTA, "minecraft:orange_terracotta");
    BlockState* const yellowTerracotta = defaultState(Blocks::YELLOW_TERRACOTTA, "minecraft:yellow_terracotta");
    BlockState* const brownTerracotta = defaultState(Blocks::BROWN_TERRACOTTA, "minecraft:brown_terracotta");
    BlockState* const redTerracotta = defaultState(Blocks::RED_TERRACOTTA, "minecraft:red_terracotta");
    BlockState* const whiteTerracotta = defaultState(Blocks::WHITE_TERRACOTTA, "minecraft:white_terracotta");
    BlockState* const lightGrayTerracotta =
        defaultState(Blocks::LIGHT_GRAY_TERRACOTTA, "minecraft:light_gray_terracotta");

    std::array<BlockState*, CLAY_BAND_COUNT> clayBands;
    clayBands.fill(terracotta);
    const int32_t length = CLAY_BAND_COUNT;

    // The loop both steps (++i) and skips (i += nextInt(5) + 1) - as written.
    for (int32_t i = 0; i < length; ++i) {
        i += random.nextInt(5) + 1;
        if (i < length) clayBands[static_cast<size_t>(i)] = orangeTerracotta;
    }

    makeBands(random, clayBands, 1, yellowTerracotta);
    makeBands(random, clayBands, 2, brownTerracotta);
    makeBands(random, clayBands, 1, redTerracotta);
    // nextIntBetweenInclusive(9, 15).
    const int32_t whiteBandCount = random.nextInt(15 - 9 + 1) + 9;
    int32_t i = 0;
    for (int32_t start = 0; i < whiteBandCount && start < length; start += random.nextInt(16) + 4) {
        clayBands[static_cast<size_t>(start)] = whiteTerracotta;
        if (start - 1 > 0 && random.nextBoolean()) {
            clayBands[static_cast<size_t>(start - 1)] = lightGrayTerracotta;
        }
        if (start + 1 < length && random.nextBoolean()) {
            clayBands[static_cast<size_t>(start + 1)] = lightGrayTerracotta;
        }
        ++i;
    }
    return clayBands;
}

void MaterialSystem::makeBands(random::AnyRandomSource& random, std::array<BlockState*, CLAY_BAND_COUNT>& clayBands,
                               int32_t baseWidth, BlockState* state) {
    const int32_t length = CLAY_BAND_COUNT;
    // nextIntBetweenInclusive(6, 15).
    const int32_t bandCount = random.nextInt(15 - 6 + 1) + 6;
    for (int32_t i = 0; i < bandCount; ++i) {
        const int32_t width = baseWidth + random.nextInt(3);
        const int32_t start = random.nextInt(length);
        for (int32_t p = 0; start + p < length && p < width; ++p) {
            clayBands[static_cast<size_t>(start + p)] = state;
        }
    }
}

BlockState* MaterialSystem::getBand(int32_t worldX, int32_t y, int32_t worldZ) const {
    const int32_t offset = density::jmath::round(
        m_clayBandsOffsetNoise->get(static_cast<double>(worldX), 0.0, static_cast<double>(worldZ)) * 4.0f);
    const int32_t length = CLAY_BAND_COUNT;
    // Java's % keeps the dividend's sign; a negative index is Java's
    // ArrayIndexOutOfBoundsException.
    const int32_t index = (y + offset + length) % length;
    if (index < 0) {
        throw std::out_of_range("MaterialSystem.getBand: clay band index " + std::to_string(index) + " at y " +
                                std::to_string(y));
    }
    return m_clayBands[static_cast<size_t>(index)];
}

} // namespace material
} // namespace levelgen
} // namespace minecraft
