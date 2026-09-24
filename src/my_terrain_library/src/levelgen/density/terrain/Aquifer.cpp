#include "levelgen/density/terrain/Aquifer.h"
#include "levelgen/density/JavaMath.h"
#include "core/BlockPos.h"
#include "world/ChunkPos.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/state/BlockState.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Reference: levelgen.Aquifer (26.3), NoiseBasedAquifer line for line.

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

using ::minecraft::world::level::block::Blocks;

BlockState* airState() {
    static BlockState* state = Blocks::getDefaultState("minecraft:air");
    return state;
}
BlockState* lavaState() {
    static BlockState* state = Blocks::getDefaultState("minecraft:lava");
    return state;
}
bool isBlock(const BlockState* state, const BlockState* reference) {
    return state != nullptr && reference != nullptr && state->getBlock() == reference->getBlock();
}
bool isLava(const BlockState* state) { return isBlock(state, lavaState()); }
bool isWater(const BlockState* state) {
    static BlockState* water = Blocks::getDefaultState("minecraft:water");
    return isBlock(state, water);
}

// Mth double helpers used here.
double dclamp(double value, double min, double max) { return value < min ? min : std::min(value, max); }
double dlerp(double alpha, double p0, double p1) { return p0 + alpha * (p1 - p0); }
double dinverseLerp(double value, double min, double max) { return (value - min) / (max - min); }
double dclampedLerp(double factor, double min, double max) {
    if (factor < 0.0) return min;
    return factor > 1.0 ? max : dlerp(factor, min, max);
}
double dclampedMap(double value, double fromMin, double fromMax, double toMin, double toMax) {
    return dclampedLerp(dinverseLerp(value, fromMin, fromMax), toMin, toMax);
}
double dmap(double value, double fromMin, double fromMax, double toMin, double toMax) {
    return dlerp(dinverseLerp(value, fromMin, fromMax), toMin, toMax);
}
int quantize(double value, int quantizeResolution) {
    return jmath::floor(value / static_cast<double>(quantizeResolution)) * quantizeResolution;
}

// QuartPos / SectionPos / ChunkPos.pack.
int quartFromBlock(int block) { return block >> 2; }
int quartToBlock(int quart) { return quart << 2; }
int sectionToBlockCoord(int section) { return section << 4; }
int64_t chunkPack(int x, int z) {
    return (static_cast<int64_t>(x) & 0xFFFFFFFFLL) | ((static_cast<int64_t>(z) & 0xFFFFFFFFLL) << 32);
}

class DisabledAquifer final : public Aquifer {
public:
    explicit DisabledAquifer(FluidPicker fluidRule) : m_fluidRule(std::move(fluidRule)) {}
    BlockState* computeSubstance(int blockX, int blockY, int blockZ, double density) override {
        return density > 0.0 ? nullptr : m_fluidRule(blockX, blockY, blockZ).at(blockY);
    }
    bool shouldScheduleFluidUpdate() const override { return false; }
private:
    FluidPicker m_fluidRule;
};

class NoiseBasedAquifer final : public Aquifer {
public:
    NoiseBasedAquifer(const DensitySamplerSet& cachingSamplers, const Config& config,
                      random::AnyPositionalRandomFactory positionalRandomFactory,
                      const DensityVolume& volume, FluidPicker globalFluidPicker)
        : m_barrierNoise(cachingSamplers.get(config.barrierNoise)),
          m_fluidLevelFloodednessNoise(cachingSamplers.get(config.fluidLevelFloodednessNoise)),
          m_fluidLevelSpreadNoise(cachingSamplers.get(config.fluidLevelSpreadNoise)),
          m_lavaNoise(cachingSamplers.get(config.lavaNoise)),
          m_positionalRandomFactory(positionalRandomFactory),
          m_globalFluidPicker(std::move(globalFluidPicker)),
          m_exclusion(cachingSamplers.get(config.exclusion)),
          m_surfaceLevel(cachingSamplers.get(config.surfaceLevel)) {
        m_minGridX = gridX(volume.minBlockX + SAMPLE_OFFSET_X) + MIN_CELL_SAMPLE_X;
        const int maxGridX = gridX(volume.maxBlockX() + SAMPLE_OFFSET_X) + MAX_CELL_SAMPLE_X;
        m_gridSizeX = maxGridX - m_minGridX + 1;
        m_minGridY = gridY(volume.minBlockY + SAMPLE_OFFSET_Y) + MIN_CELL_SAMPLE_Y;
        const int maxGridY = gridY(volume.maxBlockY() + SAMPLE_OFFSET_Y) + MAX_CELL_SAMPLE_Y;
        const int gridSizeY = maxGridY - m_minGridY + 1;
        m_minGridZ = gridZ(volume.minBlockZ + SAMPLE_OFFSET_Z) + MIN_CELL_SAMPLE_Z;
        const int maxGridZ = gridZ(volume.maxBlockZ() + SAMPLE_OFFSET_Z) + MAX_CELL_SAMPLE_Z;
        m_gridSizeZ = maxGridZ - m_minGridZ + 1;
        const int totalGridSize = m_gridSizeX * gridSizeY * m_gridSizeZ;
        m_aquiferCache.assign(static_cast<size_t>(totalGridSize), FluidStatus{});
        m_aquiferCacheSet.assign(static_cast<size_t>(totalGridSize), false);
        m_aquiferLocationCache.assign(static_cast<size_t>(totalGridSize), std::numeric_limits<int64_t>::max());
        const int maxAdjustedSurfaceLevel = adjustSurfaceLevel(maxSurfaceLevel(
            fromGridX(m_minGridX, 0), fromGridZ(m_minGridZ, 0), fromGridX(maxGridX, 9), fromGridZ(maxGridZ, 9)));
        const int skipSamplingAboveGridY = gridY(maxAdjustedSurfaceLevel + 12) - MIN_CELL_SAMPLE_Y;
        m_skipSamplingAboveY = fromGridY(skipSamplingAboveGridY, 11) - 1;
    }

    BlockState* computeSubstance(int blockX, int blockY, int blockZ, double density) override;
    bool shouldScheduleFluidUpdate() const override { return m_shouldScheduleFluidUpdate; }

private:
    static constexpr int SAMPLE_OFFSET_X = -5;
    static constexpr int SAMPLE_OFFSET_Y = 1;
    static constexpr int SAMPLE_OFFSET_Z = -5;
    static constexpr int MIN_CELL_SAMPLE_X = 0;
    static constexpr int MIN_CELL_SAMPLE_Y = -1;
    static constexpr int MIN_CELL_SAMPLE_Z = 0;
    static constexpr int MAX_CELL_SAMPLE_X = 1;
    static constexpr int MAX_CELL_SAMPLE_Y = 1;
    static constexpr int MAX_CELL_SAMPLE_Z = 1;

    static double flowingUpdateSimilarity() {
        static const double value = similarity(10 * 10, 12 * 12);
        return value;
    }

    static double similarity(int distanceSqr1, int distanceSqr2) {
        return 1.0 - static_cast<double>(distanceSqr2 - distanceSqr1) / 25.0;
    }
    static int gridX(int blockCoord) { return blockCoord >> 4; }
    static int fromGridX(int gridCoord, int blockOffset) { return (gridCoord << 4) + blockOffset; }
    static int gridY(int blockCoord) { return jmath::floorDiv(blockCoord, 12); }
    static int fromGridY(int gridCoord, int blockOffset) { return gridCoord * 12 + blockOffset; }
    static int gridZ(int blockCoord) { return blockCoord >> 4; }
    static int fromGridZ(int gridCoord, int blockOffset) { return (gridCoord << 4) + blockOffset; }
    static int adjustSurfaceLevel(int preliminarySurfaceLevel) { return preliminarySurfaceLevel + 8; }

    int surfaceLevel(int blockX, int blockZ);
    int maxSurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX, int maxBlockZ);
    int getIndex(int gridX, int gridY, int gridZ) const {
        const int x = gridX - m_minGridX;
        const int y = gridY - m_minGridY;
        const int z = gridZ - m_minGridZ;
        return (y * m_gridSizeZ + z) * m_gridSizeX + x;
    }
    double calculatePressure(int blockX, int blockY, int blockZ, double& barrierNoiseValue,
                             const FluidStatus& statusClosest1, const FluidStatus& statusClosest2);
    FluidStatus getAquiferStatus(int index);
    FluidStatus computeFluid(int x, int y, int z);
    int computeSurfaceLevel(int x, int y, int z, const FluidStatus& globalFluid, int lowestSurfaceLevel,
                            bool surfaceAtCenterIsUnderGlobalFluidLevel);
    int computeRandomizedFluidSurfaceLevel(int x, int y, int z, int lowestSurfaceLevel);
    BlockState* computeFluidType(int x, int y, int z, const FluidStatus& globalFluid, int fluidSurfaceLevel);

    BoundSampler m_barrierNoise;
    BoundSampler m_fluidLevelFloodednessNoise;
    BoundSampler m_fluidLevelSpreadNoise;
    BoundSampler m_lavaNoise;
    random::AnyPositionalRandomFactory m_positionalRandomFactory;
    std::vector<FluidStatus> m_aquiferCache;
    std::vector<bool> m_aquiferCacheSet;
    std::vector<int64_t> m_aquiferLocationCache;
    FluidPicker m_globalFluidPicker;
    BoundSampler m_exclusion;
    bool m_shouldScheduleFluidUpdate = false;
    int m_skipSamplingAboveY = 0;
    int m_minGridX = 0;
    int m_minGridY = 0;
    int m_minGridZ = 0;
    int m_gridSizeX = 0;
    int m_gridSizeZ = 0;
    BoundSampler m_surfaceLevel;
    std::unordered_map<int64_t, int> m_surfaceLevelCache;
};

// SURFACE_SAMPLING_OFFSETS_IN_CHUNKS.
constexpr int kSurfaceSamplingOffsets[13][2] = {
    {0, 0}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0}, {-1, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};

int NoiseBasedAquifer::surfaceLevel(int blockX, int blockZ) {
    const int quantizedX = quartToBlock(quartFromBlock(blockX));
    const int quantizedZ = quartToBlock(quartFromBlock(blockZ));
    const int64_t key = chunkPack(quantizedX, quantizedZ);
    auto it = m_surfaceLevelCache.find(key);
    if (it != m_surfaceLevelCache.end()) return it->second;
    const int value = jmath::floor(m_surfaceLevel.sampleValue(quantizedX, 0, quantizedZ));
    m_surfaceLevelCache.emplace(key, value);
    return value;
}

int NoiseBasedAquifer::maxSurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX, int maxBlockZ) {
    const int minQuartX = quartFromBlock(minBlockX);
    const int maxQuartX = quartFromBlock(maxBlockX);
    const int minQuartZ = quartFromBlock(minBlockZ);
    const int maxQuartZ = quartFromBlock(maxBlockZ);
    const DensityVolume volume(maxQuartX - minQuartX + 1, 1, maxQuartZ - minQuartZ + 1,
                               quartToBlock(minQuartX), 0, quartToBlock(minQuartZ), 4, 1, 4);
    ScopedBuffer buffer = m_surfaceLevel.sampleVolume(volume);
    int maxY = std::numeric_limits<int>::min();
    for (int z = 0; z < volume.sizeZ; ++z) {
        for (int x = 0; x < volume.sizeX; ++x) {
            const int surfaceLevel = jmath::floor(buffer->get(volume.indexUnchecked(x, 0, z)));
            m_surfaceLevelCache[chunkPack(volume.blockX(x), volume.blockZ(z))] = surfaceLevel;
            if (surfaceLevel > maxY) maxY = surfaceLevel;
        }
    }
    return maxY;
}

BlockState* NoiseBasedAquifer::computeSubstance(int blockX, int blockY, int blockZ, double density) {
    if (density > 0.0) {
        m_shouldScheduleFluidUpdate = false;
        return nullptr;
    }
    const FluidStatus globalFluid = m_globalFluidPicker(blockX, blockY, blockZ);
    if (blockY > m_skipSamplingAboveY) {
        m_shouldScheduleFluidUpdate = false;
        return globalFluid.at(blockY);
    }
    if (isLava(globalFluid.at(blockY))) {
        m_shouldScheduleFluidUpdate = false;
        return lavaState();
    }

    const int xAnchor = gridX(blockX + SAMPLE_OFFSET_X);
    const int yAnchor = gridY(blockY + SAMPLE_OFFSET_Y);
    const int zAnchor = gridZ(blockZ + SAMPLE_OFFSET_Z);
    int distanceSqr1 = std::numeric_limits<int>::max();
    int distanceSqr2 = std::numeric_limits<int>::max();
    int distanceSqr3 = std::numeric_limits<int>::max();
    int distanceSqr4 = std::numeric_limits<int>::max();
    int closestIndex1 = 0;
    int closestIndex2 = 0;
    int closestIndex3 = 0;
    int closestIndex4 = 0;
    for (int x1 = 0; x1 <= 1; ++x1) {
        for (int y1 = -1; y1 <= 1; ++y1) {
            for (int z1 = 0; z1 <= 1; ++z1) {
                const int spacedGridX = xAnchor + x1;
                const int spacedGridY = yAnchor + y1;
                const int spacedGridZ = zAnchor + z1;
                const int index = getIndex(spacedGridX, spacedGridY, spacedGridZ);
                const int64_t existingLocation = m_aquiferLocationCache[static_cast<size_t>(index)];
                int64_t location;
                if (existingLocation != std::numeric_limits<int64_t>::max()) {
                    location = existingLocation;
                } else {
                    random::AnyRandomSource random = m_positionalRandomFactory.at(spacedGridX, spacedGridY, spacedGridZ);
                    const int ox = random.nextInt(10);
                    const int oy = random.nextInt(9);
                    const int oz = random.nextInt(10);
                    location = core::BlockPos::asLong(fromGridX(spacedGridX, ox), fromGridY(spacedGridY, oy),
                                                      fromGridZ(spacedGridZ, oz));
                    m_aquiferLocationCache[static_cast<size_t>(index)] = location;
                }
                const int dx = core::BlockPos::getPackedX(location) - blockX;
                const int dy = core::BlockPos::getPackedY(location) - blockY;
                const int dz = core::BlockPos::getPackedZ(location) - blockZ;
                const int newDistance = dx * dx + dy * dy + dz * dz;
                if (distanceSqr1 >= newDistance) {
                    closestIndex4 = closestIndex3;
                    closestIndex3 = closestIndex2;
                    closestIndex2 = closestIndex1;
                    closestIndex1 = index;
                    distanceSqr4 = distanceSqr3;
                    distanceSqr3 = distanceSqr2;
                    distanceSqr2 = distanceSqr1;
                    distanceSqr1 = newDistance;
                } else if (distanceSqr2 >= newDistance) {
                    closestIndex4 = closestIndex3;
                    closestIndex3 = closestIndex2;
                    closestIndex2 = index;
                    distanceSqr4 = distanceSqr3;
                    distanceSqr3 = distanceSqr2;
                    distanceSqr2 = newDistance;
                } else if (distanceSqr3 >= newDistance) {
                    closestIndex4 = closestIndex3;
                    closestIndex3 = index;
                    distanceSqr4 = distanceSqr3;
                    distanceSqr3 = newDistance;
                } else if (distanceSqr4 >= newDistance) {
                    closestIndex4 = index;
                    distanceSqr4 = newDistance;
                }
            }
        }
    }

    const FluidStatus closestStatus1 = getAquiferStatus(closestIndex1);
    const double similarity12 = similarity(distanceSqr1, distanceSqr2);
    BlockState* fluidState = closestStatus1.at(blockY);
    BlockState* actualFluidState = fluidState;
    if (similarity12 <= 0.0) {
        if (similarity12 >= flowingUpdateSimilarity()) {
            const FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);
            m_shouldScheduleFluidUpdate = !(closestStatus1 == closestStatus2);
        } else {
            m_shouldScheduleFluidUpdate = false;
        }
        return actualFluidState;
    }
    if (isWater(fluidState) && isLava(m_globalFluidPicker(blockX, blockY - 1, blockZ).at(blockY - 1))) {
        m_shouldScheduleFluidUpdate = true;
        return actualFluidState;
    }

    double barrierNoiseValue = std::numeric_limits<double>::quiet_NaN();
    const FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);
    const double barrier12 = similarity12 * calculatePressure(blockX, blockY, blockZ, barrierNoiseValue,
                                                             closestStatus1, closestStatus2);
    if (density + barrier12 > 0.0) {
        m_shouldScheduleFluidUpdate = false;
        return nullptr;
    }
    const FluidStatus closestStatus3 = getAquiferStatus(closestIndex3);
    const double similarity13 = similarity(distanceSqr1, distanceSqr3);
    if (similarity13 > 0.0) {
        const double barrier13 = similarity12 * similarity13 *
            calculatePressure(blockX, blockY, blockZ, barrierNoiseValue, closestStatus1, closestStatus3);
        if (density + barrier13 > 0.0) {
            m_shouldScheduleFluidUpdate = false;
            return nullptr;
        }
    }
    const double similarity23 = similarity(distanceSqr2, distanceSqr3);
    if (similarity23 > 0.0) {
        const double barrier23 = similarity12 * similarity23 *
            calculatePressure(blockX, blockY, blockZ, barrierNoiseValue, closestStatus2, closestStatus3);
        if (density + barrier23 > 0.0) {
            m_shouldScheduleFluidUpdate = false;
            return nullptr;
        }
    }
    const bool mayFlow12 = !(closestStatus1 == closestStatus2);
    const bool mayFlow23 = similarity23 >= flowingUpdateSimilarity() && !(closestStatus2 == closestStatus3);
    const bool mayFlow13 = similarity13 >= flowingUpdateSimilarity() && !(closestStatus1 == closestStatus3);
    if (!mayFlow12 && !mayFlow23 && !mayFlow13) {
        m_shouldScheduleFluidUpdate = similarity13 >= flowingUpdateSimilarity() &&
                                      similarity(distanceSqr1, distanceSqr4) >= flowingUpdateSimilarity() &&
                                      !(closestStatus1 == getAquiferStatus(closestIndex4));
    } else {
        m_shouldScheduleFluidUpdate = true;
    }
    return actualFluidState;
}

double NoiseBasedAquifer::calculatePressure(int blockX, int blockY, int blockZ, double& barrierNoiseValue,
                                            const FluidStatus& statusClosest1, const FluidStatus& statusClosest2) {
    BlockState* type1 = statusClosest1.at(blockY);
    BlockState* type2 = statusClosest2.at(blockY);
    if ((isLava(type1) && isWater(type2)) || (isWater(type1) && isLava(type2))) {
        return 2.0;
    }
    const int fluidYDiff = std::abs(statusClosest1.fluidLevel - statusClosest2.fluidLevel);
    if (fluidYDiff == 0) return 0.0;
    const double averageFluidY = 0.5 * static_cast<double>(statusClosest1.fluidLevel + statusClosest2.fluidLevel);
    const double howFarAboveAverageFluidPoint = static_cast<double>(blockY) + 0.5 - averageFluidY;
    const double baseValue = static_cast<double>(fluidYDiff) / 2.0;
    const double distanceFromBarrierEdgeTowardsMiddle = baseValue - std::fabs(howFarAboveAverageFluidPoint);
    double gradient;
    double amplitude;
    if (howFarAboveAverageFluidPoint > 0.0) {
        amplitude = 0.0 + distanceFromBarrierEdgeTowardsMiddle;
        gradient = amplitude > 0.0 ? amplitude / 1.5 : amplitude / 2.5;
    } else {
        amplitude = 3.0 + distanceFromBarrierEdgeTowardsMiddle;
        gradient = amplitude > 0.0 ? amplitude / 3.0 : amplitude / 10.0;
    }
    double noiseValue;
    if (!(gradient < -2.0) && !(gradient > 2.0)) {
        if (std::isnan(barrierNoiseValue)) {
            const double barrierNoise = static_cast<double>(m_barrierNoise.sampleValue(blockX, blockY, blockZ));
            barrierNoiseValue = barrierNoise;
            noiseValue = barrierNoise;
        } else {
            noiseValue = barrierNoiseValue;
        }
    } else {
        noiseValue = 0.0;
    }
    return 2.0 * (noiseValue + gradient);
}

Aquifer::FluidStatus NoiseBasedAquifer::getAquiferStatus(int index) {
    if (m_aquiferCacheSet[static_cast<size_t>(index)]) return m_aquiferCache[static_cast<size_t>(index)];
    const int64_t location = m_aquiferLocationCache[static_cast<size_t>(index)];
    const FluidStatus status = computeFluid(core::BlockPos::getPackedX(location), core::BlockPos::getPackedY(location),
                                            core::BlockPos::getPackedZ(location));
    m_aquiferCache[static_cast<size_t>(index)] = status;
    m_aquiferCacheSet[static_cast<size_t>(index)] = true;
    return status;
}

Aquifer::FluidStatus NoiseBasedAquifer::computeFluid(int x, int y, int z) {
    const FluidStatus globalFluid = m_globalFluidPicker(x, y, z);
    int lowestPreliminarySurface = std::numeric_limits<int>::max();
    const int topOfAquiferCell = y + 12;
    const int bottomOfAquiferCell = y - 12;
    bool surfaceAtCenterIsUnderGlobalFluidLevel = false;
    for (const auto& offset : kSurfaceSamplingOffsets) {
        const int sampleX = x + sectionToBlockCoord(offset[0]);
        const int sampleZ = z + sectionToBlockCoord(offset[1]);
        const int surfaceLevelValue = surfaceLevel(sampleX, sampleZ);
        const int adjustedSurfaceLevel = adjustSurfaceLevel(surfaceLevelValue);
        const bool start = offset[0] == 0 && offset[1] == 0;
        if (start && bottomOfAquiferCell > adjustedSurfaceLevel) {
            return globalFluid;
        }
        const bool topOfAquiferCellPokesAboveSurface = topOfAquiferCell > adjustedSurfaceLevel;
        if (topOfAquiferCellPokesAboveSurface || start) {
            const FluidStatus globalFluidAtSurface = m_globalFluidPicker(sampleX, adjustedSurfaceLevel, sampleZ);
            BlockState* atSurface = globalFluidAtSurface.at(adjustedSurfaceLevel);
            if (atSurface != nullptr && !atSurface->isAir()) {
                if (start) surfaceAtCenterIsUnderGlobalFluidLevel = true;
                if (topOfAquiferCellPokesAboveSurface) return globalFluidAtSurface;
            }
        }
        lowestPreliminarySurface = std::min(lowestPreliminarySurface, surfaceLevelValue);
    }
    const int fluidSurfaceLevel = computeSurfaceLevel(x, y, z, globalFluid, lowestPreliminarySurface,
                                                      surfaceAtCenterIsUnderGlobalFluidLevel);
    return FluidStatus{fluidSurfaceLevel, computeFluidType(x, y, z, globalFluid, fluidSurfaceLevel)};
}

int NoiseBasedAquifer::computeSurfaceLevel(int x, int y, int z, const FluidStatus& globalFluid,
                                           int lowestSurfaceLevel, bool surfaceAtCenterIsUnderGlobalFluidLevel) {
    double partiallyFloodedness;
    double fullyFloodidness;
    if (static_cast<double>(m_exclusion.sampleValue(x, y, z)) > 0.0) {
        partiallyFloodedness = -1.0;
        fullyFloodidness = -1.0;
    } else {
        const int fluidSurfaceLevel = adjustSurfaceLevel(lowestSurfaceLevel) - y;
        const double floodednessFactor = surfaceAtCenterIsUnderGlobalFluidLevel
            ? dclampedMap(static_cast<double>(fluidSurfaceLevel), 0.0, 64.0, 1.0, 0.0) : 0.0;
        const double floodednessNoiseValue =
            dclamp(static_cast<double>(m_fluidLevelFloodednessNoise.sampleValue(x, y, z)), -1.0, 1.0);
        const double fullyFloodedThreshold = dmap(floodednessFactor, 1.0, 0.0, -0.3, 0.8);
        const double partiallyFloodedThreshold = dmap(floodednessFactor, 1.0, 0.0, -0.8, 0.4);
        partiallyFloodedness = floodednessNoiseValue - partiallyFloodedThreshold;
        fullyFloodidness = floodednessNoiseValue - fullyFloodedThreshold;
    }
    if (fullyFloodidness > 0.0) return globalFluid.fluidLevel;
    if (partiallyFloodedness > 0.0) return computeRandomizedFluidSurfaceLevel(x, y, z, lowestSurfaceLevel);
    return WAY_BELOW_MIN_Y;
}

int NoiseBasedAquifer::computeRandomizedFluidSurfaceLevel(int x, int y, int z, int lowestSurfaceLevel) {
    const int fluidLevelCellX = jmath::floorDiv(x, 16);
    const int fluidLevelCellY = jmath::floorDiv(y, 40);
    const int fluidLevelCellZ = jmath::floorDiv(z, 16);
    const int fluidCellMiddleY = fluidLevelCellY * 40 + 20;
    const double fluidLevelSpread = static_cast<double>(
        m_fluidLevelSpreadNoise.sampleValue(fluidLevelCellX, fluidLevelCellY, fluidLevelCellZ) * 10.0f);
    const int fluidLevelSpreadQuantized = quantize(fluidLevelSpread, 3);
    const int targetFluidSurfaceLevel = fluidCellMiddleY + fluidLevelSpreadQuantized;
    return std::min(lowestSurfaceLevel, targetFluidSurfaceLevel);
}

BlockState* NoiseBasedAquifer::computeFluidType(int x, int y, int z, const FluidStatus& globalFluid,
                                                int fluidSurfaceLevel) {
    BlockState* fluidType = globalFluid.fluidType;
    if (fluidSurfaceLevel <= -10 && fluidSurfaceLevel != WAY_BELOW_MIN_Y && globalFluid.fluidType != lavaState()) {
        const int fluidTypeCellX = jmath::floorDiv(x, 64);
        const int fluidTypeCellY = jmath::floorDiv(y, 40);
        const int fluidTypeCellZ = jmath::floorDiv(z, 64);
        const double lavaNoiseValue =
            static_cast<double>(m_lavaNoise.sampleValue(fluidTypeCellX, fluidTypeCellY, fluidTypeCellZ));
        if (std::fabs(lavaNoiseValue) > 0.3) {
            fluidType = lavaState();
        }
    }
    return fluidType;
}

} // namespace

BlockState* Aquifer::FluidStatus::at(int blockY) const {
    return blockY < fluidLevel ? fluidType : airState();
}

std::unique_ptr<Aquifer> Aquifer::createDisabled(FluidPicker fluidRule) {
    return std::make_unique<DisabledAquifer>(std::move(fluidRule));
}

std::unique_ptr<Aquifer> Aquifer::Config::create(const DensitySamplerSet& cachingSamplers,
                                                 random::AnyPositionalRandomFactory positionalRandomFactory,
                                                 const DensityVolume& volume, FluidPicker fluidRule) const {
    return std::make_unique<NoiseBasedAquifer>(cachingSamplers, *this, positionalRandomFactory, volume,
                                               std::move(fluidRule));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
