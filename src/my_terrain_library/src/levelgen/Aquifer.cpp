#include "levelgen/Aquifer.h"
#include "levelgen/NoiseChunk.h"
#include "world/level/block/Blocks.h"
#include "world/level/block/Blocks.h"
#include "world/biome/OverworldBiomeBuilder.h"
#include "core/BlockPos.h"
#include "core/SectionPos.h"
#include "math/Mth.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <iostream>
#include <iomanip>

namespace minecraft {
namespace levelgen {

using namespace minecraft::core;
using namespace minecraft::world;
using Blocks = minecraft::world::level::block::Blocks;

//==============================================================================
// Aquifer Base Class
//==============================================================================

// Reference: Aquifer.java - BlockStateFiller wrapper
BlockState* Aquifer::calculate(
    const density::DensityFunction::FunctionContext& context
) const {
    // The density needs to be computed from the noise chunk's interpolators
    // In Java, this is handled by the NoiseChunk passing density to computeSubstance
    // For now, we'll try to get it from the NoiseChunk context
    // Note: The NoiseChunk is the FunctionContext, so we can try to compute density

    // Since we don't have direct access to the density value here,
    // we'll assume this is being called from NoiseChunk which will handle it properly
    // This is a placeholder that should not normally be reached
    return nullptr;
}

// Reference: Aquifer.java lines 22-31
Aquifer* Aquifer::createDisabled(FluidPicker* fluidPicker) {
    return new DisabledAquifer(fluidPicker);
}

// Reference: Aquifer.java lines 18-20
Aquifer* Aquifer::create(
    NoiseChunk* noiseChunk,
    const world::ChunkPos& pos,
    const NoiseRouter& router,
    XoroshiroPositionalRandomFactory* positionalRandomFactory,
    int32_t minBlockY,
    int32_t yBlockSize,
    FluidPicker* globalFluidPicker
) {
    return new NoiseBasedAquifer(
        noiseChunk, pos, router, positionalRandomFactory,
        minBlockY, yBlockSize, globalFluidPicker
    );
}

//==============================================================================
// DisabledAquifer Implementation
//==============================================================================

DisabledAquifer::DisabledAquifer(FluidPicker* globalFluidPicker)
    : m_globalFluidPicker(globalFluidPicker)
{
}

// Reference: Aquifer.java lines 24-26
BlockState* DisabledAquifer::computeSubstance(
    const density::DensityFunction::FunctionContext& context,
    double density
) {
    // Disabled aquifer: only check if position is air or use global fluid
    if (density > 0.0) {
        return nullptr;  // Air
    }

    // Below density threshold - check global fluid
    FluidStatus globalFluid = m_globalFluidPicker->computeFluid(
        context.blockX(),
        context.blockY(),
        context.blockZ()
    );

    return globalFluid.at(context.blockY());
}

//==============================================================================
// NoiseBasedAquifer Implementation
//==============================================================================

// Reference: Aquifer.java lines 81-107
NoiseBasedAquifer::NoiseBasedAquifer(
    NoiseChunk* noiseChunk,
    const world::ChunkPos& pos,
    const NoiseRouter& router,
    XoroshiroPositionalRandomFactory* positionalRandomFactory,
    int32_t minBlockY,
    int32_t yBlockSize,
    FluidPicker* globalFluidPicker
)
    : m_noiseChunk(noiseChunk)
    , m_barrierNoise(router.barrierNoise())
    , m_fluidLevelFloodednessNoise(router.fluidLevelFloodednessNoise())
    , m_fluidLevelSpreadNoise(router.fluidLevelSpreadNoise())
    , m_lavaNoise(router.lavaNoise())
    , m_erosion(router.erosion())
    , m_depth(router.depth())
    , m_positionalRandomFactory(positionalRandomFactory)
    , m_globalFluidPicker(globalFluidPicker)
    , m_shouldScheduleFluidUpdate(false)
{
    // Calculate grid bounds (Java lines 90-99)
    // Java: this.minGridX = gridX(pos.getMinBlockX() + -5) + 0;
    m_minGridX = gridX(pos.getMinBlockX() + SAMPLE_OFFSET_X) + 0;

    // Java: int maxGridX = gridX(pos.getMaxBlockX() + -5) + 1;
    int32_t maxGridX = gridX(pos.getMaxBlockX() + SAMPLE_OFFSET_X) + 1;
    m_gridSizeX = maxGridX - m_minGridX + 1;

    // Java: this.minGridY = gridY(minBlockY + 1) + -1;
    m_minGridY = gridY(minBlockY + SAMPLE_OFFSET_Y) + (-1);

    // Java: int maxGridY = gridY(minBlockY + yBlockSize + 1) + 1;
    int32_t maxGridY = gridY(minBlockY + yBlockSize + SAMPLE_OFFSET_Y) + 1;
    m_gridSizeY = maxGridY - m_minGridY + 1;

    // Java: this.minGridZ = gridZ(pos.getMinBlockZ() + -5) + 0;
    m_minGridZ = gridZ(pos.getMinBlockZ() + SAMPLE_OFFSET_Z) + 0;

    // Java: int maxGridZ = gridZ(pos.getMaxBlockZ() + -5) + 1;
    int32_t maxGridZ = gridZ(pos.getMaxBlockZ() + SAMPLE_OFFSET_Z) + 1;
    m_gridSizeZ = maxGridZ - m_minGridZ + 1;

    // Initialize caches (Java lines 100-103)
    int32_t totalGridSize = m_gridSizeX * m_gridSizeY * m_gridSizeZ;
    m_aquiferCache.resize(totalGridSize);
    m_aquiferLocationCache.resize(totalGridSize);

    // Initialize cache with invalid values
    // Java: Arrays.fill(this.aquiferLocationCache, Long.MAX_VALUE);
    for (int32_t i = 0; i < totalGridSize; ++i) {
        m_aquiferCache[i] = FluidStatus{WAY_BELOW_MIN_Y, nullptr};  // null in Java
        m_aquiferLocationCache[i] = std::numeric_limits<int64_t>::max();
    }

    // Calculate skipSamplingAboveY (Java lines 104-106)
    // Java: int maxAdjustedSurfaceLevel = this.adjustSurfaceLevel(
    //         noiseChunk.maxPreliminarySurfaceLevel(
    //             fromGridX(this.minGridX, 0), fromGridZ(this.minGridZ, 0),
    //             fromGridX(maxGridX, 9), fromGridZ(maxGridZ, 9)));
    int32_t maxAdjustedSurfaceLevel = adjustSurfaceLevel(
        noiseChunk->maxPreliminarySurfaceLevel(
            fromGridX(m_minGridX, 0), fromGridZ(m_minGridZ, 0),
            fromGridX(maxGridX, X_RANGE - 1), fromGridZ(maxGridZ, Z_RANGE - 1)
        )
    );

    // Java: int skipSamplingAboveGridY = gridY(maxAdjustedSurfaceLevel + 12) - -1;
    int32_t skipSamplingAboveGridY = gridY(maxAdjustedSurfaceLevel + Y_SPACING) - (-1);

    // Java: this.skipSamplingAboveY = fromGridY(skipSamplingAboveGridY, 11) - 1;
    m_skipSamplingAboveY = fromGridY(skipSamplingAboveGridY, Y_RANGE + 2) - 1;
}

// Reference: Aquifer.java lines 109-114
int32_t NoiseBasedAquifer::getIndex(int32_t gx, int32_t gy, int32_t gz) const {
    int32_t x = gx - m_minGridX;
    int32_t y = gy - m_minGridY;
    int32_t z = gz - m_minGridZ;
    return (y * m_gridSizeZ + z) * m_gridSizeX + x;
}

// Reference: Aquifer.java line 328-330
int32_t NoiseBasedAquifer::gridY(int32_t blockY) {
    return Mth::floorDiv(blockY, Y_SPACING);
}

// Reference: Aquifer.java lines 257-260
double NoiseBasedAquifer::similarity(int32_t distanceSqr1, int32_t distanceSqr2) {
    // Java: return (double)1.0F - (double)(distanceSqr2 - distanceSqr1) / (double)25.0F;
    return 1.0 - static_cast<double>(distanceSqr2 - distanceSqr1) / 25.0;
}

// Reference: Aquifer.java lines 116-251
BlockState* NoiseBasedAquifer::computeSubstance(
    const density::DensityFunction::FunctionContext& context,
    double density
) {
    int32_t posX = context.blockX();
    int32_t posY = context.blockY();
    int32_t posZ = context.blockZ();

    // Step 1: Air check (Java line 117)
    if (density > 0.0) {
        m_shouldScheduleFluidUpdate = false;
        return nullptr;  // Air
    }

    // Get global fluid first (Java line 124)
    if (!m_globalFluidPicker) {
        return nullptr;
    }
    FluidStatus globalFluid = m_globalFluidPicker->computeFluid(posX, posY, posZ);

    // Step 2: Above skipSamplingAboveY - use global fluid (Java lines 125-127)
    if (posY > m_skipSamplingAboveY) {
        m_shouldScheduleFluidUpdate = false;
        return globalFluid.at(posY);
    }

    // Step 3: Global fluid is lava - return lava directly (Java lines 128-130)
    if (globalFluid.at(posY) != nullptr && globalFluid.at(posY)->is(minecraft::world::level::block::Blocks::LAVA->defaultBlockState())) {
        m_shouldScheduleFluidUpdate = false;
        return minecraft::world::level::block::Blocks::LAVA->defaultBlockState();
    }

    // Step 4: Find nearest aquifer grid cells (Java lines 132-192)
    int32_t xAnchor = gridX(posX + SAMPLE_OFFSET_X);
    int32_t yAnchor = gridY(posY + SAMPLE_OFFSET_Y);
    int32_t zAnchor = gridZ(posZ + SAMPLE_OFFSET_Z);

    // Track 4 closest aquifer cells and distances
    int32_t distanceSqr1 = std::numeric_limits<int32_t>::max();
    int32_t distanceSqr2 = std::numeric_limits<int32_t>::max();
    int32_t distanceSqr3 = std::numeric_limits<int32_t>::max();
    int32_t distanceSqr4 = std::numeric_limits<int32_t>::max();
    int32_t closestIndex1 = 0;
    int32_t closestIndex2 = 0;
    int32_t closestIndex3 = 0;
    int32_t closestIndex4 = 0;

    // Search 2x3x2 grid of aquifer cells (Java lines 144-192)
    // Java: for(int x1 = 0; x1 <= 1; ++x1)
    //       for(int y1 = -1; y1 <= 1; ++y1)
    //       for(int z1 = 0; z1 <= 1; ++z1)
    for (int32_t x1 = 0; x1 <= 1; ++x1) {
        for (int32_t y1 = -1; y1 <= 1; ++y1) {
            for (int32_t z1 = 0; z1 <= 1; ++z1) {
                int32_t spacedGridX = xAnchor + x1;
                int32_t spacedGridY = yAnchor + y1;
                int32_t spacedGridZ = zAnchor + z1;
                int32_t index = getIndex(spacedGridX, spacedGridY, spacedGridZ);

                // Bounds check
                if (index < 0 || index >= static_cast<int32_t>(m_aquiferLocationCache.size())) {
                    continue;  // Skip this cell
                }

                // Get or compute aquifer location (Java lines 151-159)
                int64_t existingLocation = m_aquiferLocationCache[index];
                int64_t location;

                if (existingLocation != std::numeric_limits<int64_t>::max()) {
                    location = existingLocation;
                } else {
                    // Generate random location within cell using positional random
                    XoroshiroRandomSource random = m_positionalRandomFactory->at(
                        spacedGridX, spacedGridY, spacedGridZ
                    );
                    location = BlockPos::asLong(
                        fromGridX(spacedGridX, random.nextInt(X_RANGE)),
                        fromGridY(spacedGridY, random.nextInt(Y_RANGE)),
                        fromGridZ(spacedGridZ, random.nextInt(Z_RANGE))
                    );
                    m_aquiferLocationCache[index] = location;
                }

                // Calculate distance to this aquifer center (Java lines 161-164)
                int32_t dx = BlockPos::getPackedX(location) - posX;
                int32_t dy = BlockPos::getPackedY(location) - posY;
                int32_t dz = BlockPos::getPackedZ(location) - posZ;
                int32_t newDistance = dx * dx + dy * dy + dz * dz;

                // Update 4 closest cells (Java lines 165-189)
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

    // Step 5: Get fluid status of closest cells (Java lines 194-197)
    FluidStatus closestStatus1 = getAquiferStatus(closestIndex1);
    double similarity12 = similarity(distanceSqr1, distanceSqr2);
    BlockState* fluidState = closestStatus1.at(posY);

    // Step 6: Quick checks (Java lines 198-210)
    if (similarity12 <= 0.0) {
        if (similarity12 >= FLOWING_UPDATE_SIMULARITY) {
            FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);
            m_shouldScheduleFluidUpdate = !(closestStatus1 == closestStatus2);
        } else {
            m_shouldScheduleFluidUpdate = false;
        }
        return fluidState;
    }

    if (fluidState != nullptr && fluidState->is(minecraft::world::level::block::Blocks::WATER->defaultBlockState()) &&
        m_globalFluidPicker->computeFluid(posX, posY - 1, posZ).at(posY - 1) != nullptr &&
        m_globalFluidPicker->computeFluid(posX, posY - 1, posZ).at(posY - 1)->is(minecraft::world::level::block::Blocks::LAVA->defaultBlockState())) {
        m_shouldScheduleFluidUpdate = true;
        return fluidState;
    }

    // Step 7: Complex barrier pressure calculation (Java lines 211-246)
    double barrierNoiseValue = std::nan("");  // Lazy evaluation

    FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);

    double pressure12 = calculatePressure(context, barrierNoiseValue, closestStatus1, closestStatus2);
    double barrier12 = similarity12 * pressure12;

    if (density + barrier12 > 0.0) {
        m_shouldScheduleFluidUpdate = false;
        return nullptr;  // Barrier prevents fluid
    }

    FluidStatus closestStatus3 = getAquiferStatus(closestIndex3);
    double similarity13 = similarity(distanceSqr1, distanceSqr3);

    if (similarity13 > 0.0) {
        double barrier13 = similarity12 * similarity13 *
            calculatePressure(context, barrierNoiseValue, closestStatus1, closestStatus3);
        if (density + barrier13 > 0.0) {
            m_shouldScheduleFluidUpdate = false;
            return nullptr;
        }
    }

    double similarity23 = similarity(distanceSqr2, distanceSqr3);
    if (similarity23 > 0.0) {
        double barrier23 = similarity12 * similarity23 *
            calculatePressure(context, barrierNoiseValue, closestStatus2, closestStatus3);
        if (density + barrier23 > 0.0) {
            m_shouldScheduleFluidUpdate = false;
            return nullptr;
        }
    }

    // Step 8: Check if fluid should flow (Java lines 237-244)
    bool mayFlow12 = !(closestStatus1 == closestStatus2);
    bool mayFlow23 = (similarity23 >= FLOWING_UPDATE_SIMULARITY) && !(closestStatus2 == closestStatus3);
    bool mayFlow13 = (similarity13 >= FLOWING_UPDATE_SIMULARITY) && !(closestStatus1 == closestStatus3);

    if (!mayFlow12 && !mayFlow23 && !mayFlow13) {
        double similarity14 = similarity(distanceSqr1, distanceSqr4);
        m_shouldScheduleFluidUpdate =
            (similarity13 >= FLOWING_UPDATE_SIMULARITY) &&
            (similarity14 >= FLOWING_UPDATE_SIMULARITY) &&
            !(closestStatus1 == getAquiferStatus(closestIndex4));
    } else {
        m_shouldScheduleFluidUpdate = true;
    }

    return fluidState;
}

// Reference: Aquifer.java lines 344-354
FluidStatus NoiseBasedAquifer::getAquiferStatus(int32_t index) {
    // Java: FluidStatus oldStatus = this.aquiferCache[index];
    //       if (oldStatus != null) return oldStatus;
    FluidStatus oldStatus = m_aquiferCache[index];
    if (oldStatus.fluidLevel != WAY_BELOW_MIN_Y || oldStatus.fluidType != nullptr) {
        // Already computed (not null in Java terms)
        return oldStatus;
    }

    int64_t location = m_aquiferLocationCache[index];
    FluidStatus status = computeFluid(
        BlockPos::getPackedX(location),
        BlockPos::getPackedY(location),
        BlockPos::getPackedZ(location)
    );
    m_aquiferCache[index] = status;
    return status;
}

// Reference: Aquifer.java lines 356-392
FluidStatus NoiseBasedAquifer::computeFluid(int32_t x, int32_t y, int32_t z) {
    FluidStatus globalFluid = m_globalFluidPicker->computeFluid(x, y, z);
    int32_t lowestPreliminarySurface = std::numeric_limits<int32_t>::max();
    int32_t topOfAquiferCell = y + Y_SPACING;
    int32_t bottomOfAquiferCell = y - Y_SPACING;
    bool surfaceAtCenterIsUnderGlobalFluidLevel = false;

    // Sample preliminary surface at 13 offset positions (Java lines 363-388)
    for (const auto& offset : SURFACE_SAMPLING_OFFSETS_IN_CHUNKS) {
        int32_t sampleX = x + SectionPos::sectionToBlockCoord(offset[0]);
        int32_t sampleZ = z + SectionPos::sectionToBlockCoord(offset[1]);
        int32_t preliminarySurfaceLevel = m_noiseChunk->preliminarySurfaceLevel(sampleX, sampleZ);
        int32_t adjustedSurfaceLevel = adjustSurfaceLevel(preliminarySurfaceLevel);

        // Java: boolean start = offset[0] == 0 && offset[1] == 0;
        bool isCenter = (offset[0] == 0 && offset[1] == 0);

        if (isCenter && bottomOfAquiferCell > adjustedSurfaceLevel) {
            return globalFluid;  // Fully underground
        }

        bool topOfAquiferCellPokesAboveSurface = (topOfAquiferCell > adjustedSurfaceLevel);
        if (topOfAquiferCellPokesAboveSurface || isCenter) {
            FluidStatus globalFluidAtSurface =
                m_globalFluidPicker->computeFluid(sampleX, adjustedSurfaceLevel, sampleZ);
            BlockState* fluidAtSurface = globalFluidAtSurface.at(adjustedSurfaceLevel);

            if (fluidAtSurface != nullptr && !fluidAtSurface->isAir()) {
                if (isCenter) {
                    surfaceAtCenterIsUnderGlobalFluidLevel = true;
                }
                if (topOfAquiferCellPokesAboveSurface) {
                    return globalFluidAtSurface;
                }
            }
        }

        lowestPreliminarySurface = std::min(lowestPreliminarySurface, preliminarySurfaceLevel);
    }

    int32_t fluidSurfaceLevel = computeSurfaceLevel(
        x, y, z, globalFluid, lowestPreliminarySurface,
        surfaceAtCenterIsUnderGlobalFluidLevel
    );

    return FluidStatus{fluidSurfaceLevel, computeFluidType(x, y, z, globalFluid, fluidSurfaceLevel)};
}

// Reference: Aquifer.java lines 262-318
double NoiseBasedAquifer::calculatePressure(
    const density::DensityFunction::FunctionContext& context,
    double& barrierNoiseValue,
    const FluidStatus& status1,
    const FluidStatus& status2
) const {
    int32_t posY = context.blockY();
    BlockState* type1 = status1.at(posY);
    BlockState* type2 = status2.at(posY);

    // Special case: lava-water boundary always has pressure (Java lines 266-268)
    if ((type1 != nullptr && type1->is(minecraft::world::level::block::Blocks::LAVA->defaultBlockState()) &&
         type2 != nullptr && type2->is(minecraft::world::level::block::Blocks::WATER->defaultBlockState())) ||
        (type1 != nullptr && type1->is(minecraft::world::level::block::Blocks::WATER->defaultBlockState()) &&
         type2 != nullptr && type2->is(minecraft::world::level::block::Blocks::LAVA->defaultBlockState()))) {
        return 2.0;
    }

    int32_t fluidYDiff = std::abs(status1.fluidLevel - status2.fluidLevel);
    if (fluidYDiff == 0) {
        return 0.0;
    }

    // Java: double averageFluidY = (double)0.5F * (double)(statusClosest1.fluidLevel + statusClosest2.fluidLevel);
    double averageFluidY = 0.5 * static_cast<double>(status1.fluidLevel + status2.fluidLevel);

    // Java: double howFarAboveAverageFluidPoint = (double)posY + (double)0.5F - averageFluidY;
    double howFarAboveAverage = static_cast<double>(posY) + 0.5 - averageFluidY;

    // Java: double baseValue = (double)fluidYDiff / (double)2.0F;
    double baseValue = static_cast<double>(fluidYDiff) / 2.0;

    // Complex gradient calculation (Java lines 280-296)
    // Java: double distanceFromBarrierEdgeTowardsMiddle = baseValue - Math.abs(howFarAboveAverageFluidPoint);
    double distanceFromBarrierEdgeTowardsMiddle = baseValue - std::abs(howFarAboveAverage);

    double gradient;
    if (howFarAboveAverage > 0.0) {
        // Java: double centerPoint = (double)0.0F + distanceFromBarrierEdgeTowardsMiddle;
        double centerPoint = 0.0 + distanceFromBarrierEdgeTowardsMiddle;
        if (centerPoint > 0.0) {
            gradient = centerPoint / 1.5;  // furthestHolesFromTopBias
        } else {
            gradient = centerPoint / 2.5;  // furthestRocksFromTopBias
        }
    } else {
        // Java: double centerPoint = (double)3.0F + distanceFromBarrierEdgeTowardsMiddle;
        double centerPoint = 3.0 + distanceFromBarrierEdgeTowardsMiddle;
        if (centerPoint > 0.0) {
            gradient = centerPoint / 3.0;  // furthestHolesFromBottomBias
        } else {
            gradient = centerPoint / 10.0;  // furthestRocksFromBottomBias
        }
    }

    // Sample barrier noise if needed (Java lines 299-311)
    double noiseValue;
    if (gradient >= -2.0 && gradient <= 2.0) {
        if (std::isnan(barrierNoiseValue)) {
            barrierNoiseValue = m_barrierNoise->compute(context);
        }
        noiseValue = barrierNoiseValue;
    } else {
        noiseValue = 0.0;
    }

    return 2.0 * (noiseValue + gradient);
}

// Reference: Aquifer.java lines 398-426
int32_t NoiseBasedAquifer::computeSurfaceLevel(
    int32_t x, int32_t y, int32_t z,
    const FluidStatus& globalFluid,
    int32_t lowestPreliminarySurface,
    bool surfaceAtCenterIsUnderGlobalFluidLevel
) {
    density::DensityFunction::SinglePointContext context(x, y, z);

    double partiallyFloodedness, fullyFloodedness;
    if (biome::OverworldBiomeBuilder::isDeepDarkRegion(m_erosion, m_depth, context)) {
        // Deep dark has no aquifers (Java lines 402-404)
        partiallyFloodedness = -1.0;
        fullyFloodedness = -1.0;
    } else {
        // Java: int distanceBelowSurface = lowestPreliminarySurface + 8 - y;
        int32_t distanceBelowSurface = lowestPreliminarySurface + 8 - y;

        // Java: double floodednessFactor = surfaceAtCenterIsUnderGlobalFluidLevel ?
        //         Mth.clampedMap((double)distanceBelowSurface, 0.0, 64.0, 1.0, 0.0) : 0.0;
        double floodednessFactor = surfaceAtCenterIsUnderGlobalFluidLevel ?
            Mth::clampedMap(static_cast<double>(distanceBelowSurface), 0.0, 64.0, 1.0, 0.0) : 0.0;

        // Java: double floodednessNoiseValue = Mth.clamp(this.fluidLevelFloodednessNoise.compute(context), -1.0, 1.0);
        double floodednessNoiseValue =
            Mth::clamp(m_fluidLevelFloodednessNoise->compute(context), -1.0, 1.0);

        // Java: double fullyFloodedThreshold = Mth.map(floodednessFactor, 1.0, 0.0, -0.3, 0.8);
        double fullyFloodedThreshold = Mth::map(floodednessFactor, 1.0, 0.0, -0.3, 0.8);

        // Java: double partiallyFloodedThreshold = Mth.map(floodednessFactor, 1.0, 0.0, -0.8, 0.4);
        double partiallyFloodedThreshold = Mth::map(floodednessFactor, 1.0, 0.0, -0.8, 0.4);

        partiallyFloodedness = floodednessNoiseValue - partiallyFloodedThreshold;
        fullyFloodedness = floodednessNoiseValue - fullyFloodedThreshold;
    }

    // Java lines 416-425
    if (fullyFloodedness > 0.0) {
        return globalFluid.fluidLevel;
    } else if (partiallyFloodedness > 0.0) {
        return computeRandomizedFluidSurfaceLevel(x, y, z, lowestPreliminarySurface);
    } else {
        return WAY_BELOW_MIN_Y;
    }
}

// Reference: Aquifer.java lines 428-440
int32_t NoiseBasedAquifer::computeRandomizedFluidSurfaceLevel(
    int32_t x, int32_t y, int32_t z,
    int32_t lowestPreliminarySurface
) {
    // Java: int fluidCellWidth = 16;
    //       int fluidCellHeight = 40;
    constexpr int32_t fluidCellWidth = 16;
    constexpr int32_t fluidCellHeight = 40;

    // Java: int fluidLevelCellX = Math.floorDiv(x, 16);
    int32_t fluidLevelCellX = Mth::floorDiv(x, fluidCellWidth);
    int32_t fluidLevelCellY = Mth::floorDiv(y, fluidCellHeight);
    int32_t fluidLevelCellZ = Mth::floorDiv(z, fluidCellWidth);

    // Java: int fluidCellMiddleY = fluidLevelCellY * 40 + 20;
    int32_t fluidCellMiddleY = fluidLevelCellY * fluidCellHeight + 20;

    // Java: double fluidLevelSpread = this.fluidLevelSpreadNoise.compute(
    //         new DensityFunction.SinglePointContext(fluidLevelCellX, fluidLevelCellY, fluidLevelCellZ)) * 10.0F;
    double fluidLevelSpread = m_fluidLevelSpreadNoise->compute(
        density::DensityFunction::SinglePointContext(fluidLevelCellX, fluidLevelCellY, fluidLevelCellZ)
    ) * 10.0;

    // Java: int fluidLevelSpreadQuantized = Mth.quantize(fluidLevelSpread, 3);
    int32_t fluidLevelSpreadQuantized = Mth::quantize(fluidLevelSpread, 3);

    // Java: int targetFluidSurfaceLevel = fluidCellMiddleY + fluidLevelSpreadQuantized;
    int32_t targetFluidSurfaceLevel = fluidCellMiddleY + fluidLevelSpreadQuantized;

    // Java: return Math.min(lowestPreliminarySurface, targetFluidSurfaceLevel);
    return std::min(lowestPreliminarySurface, targetFluidSurfaceLevel);
}

// Reference: Aquifer.java lines 442-457
BlockState* NoiseBasedAquifer::computeFluidType(
    int32_t x, int32_t y, int32_t z,
    const FluidStatus& globalFluid,
    int32_t fluidSurfaceLevel
) {
    BlockState* fluidType = globalFluid.fluidType;

    // Deep underground, randomly replace water with lava (Java lines 444-454)
    // Java: if (fluidSurfaceLevel <= -10 && fluidSurfaceLevel != DimensionType.WAY_BELOW_MIN_Y
    //         && globalFluid.fluidType != Blocks.LAVA.defaultBlockState())
    if (fluidSurfaceLevel <= -10 &&
        fluidSurfaceLevel != WAY_BELOW_MIN_Y &&
        globalFluid.fluidType != nullptr &&
        !globalFluid.fluidType->is(minecraft::world::level::block::Blocks::LAVA->defaultBlockState())) {

        constexpr int32_t fluidTypeCellWidth = 64;
        constexpr int32_t fluidTypeCellHeight = 40;

        int32_t fluidTypeCellX = Mth::floorDiv(x, fluidTypeCellWidth);
        int32_t fluidTypeCellY = Mth::floorDiv(y, fluidTypeCellHeight);
        int32_t fluidTypeCellZ = Mth::floorDiv(z, fluidTypeCellWidth);

        double lavaNoiseValue = m_lavaNoise->compute(
            density::DensityFunction::SinglePointContext(fluidTypeCellX, fluidTypeCellY, fluidTypeCellZ)
        );

        if (std::abs(lavaNoiseValue) > 0.3) {
            fluidType = minecraft::world::level::block::Blocks::LAVA->defaultBlockState();
        }
    }

    return fluidType;
}

} // namespace levelgen
} // namespace minecraft
