#pragma once

#include "levelgen/BlockStateFiller.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/DensityFunction.h"
#include "levelgen/NoiseRouter.h"
#include "random/XoroshiroRandomSource.h"
#include "world/ChunkPos.h"
#include <vector>
#include <cstdint>

namespace minecraft {

namespace levelgen {

// Forward declaration
class NoiseChunk;

/**
 * Aquifer - Handles underground water/lava generation
 * Reference: Aquifer.java
 *
 * The aquifer system simulates underground water tables and lava pockets.
 * It uses a grid-based approach with interpolation between aquifer cells.
 */
class Aquifer : public BlockStateFiller {
public:
    virtual ~Aquifer() = default;

    /**
     * Compute the block state at the given context based on density
     * Returns nullptr for air/solid blocks, or a fluid block state
     *
     * Reference: Aquifer.java line 116
     */
    virtual ::world::IBlockType* computeSubstance(
        const density::DensityFunction::FunctionContext& context,
        double density
    ) = 0;

    /**
     * Check if fluid updates should be scheduled for this position
     * Used for fluid physics simulation
     *
     * Reference: Aquifer.java line 253
     */
    virtual bool shouldScheduleFluidUpdate() const = 0;

    /**
     * BlockStateFiller implementation - delegates to computeSubstance
     */
    ::world::IBlockType* calculate(
        const density::DensityFunction::FunctionContext& context
    ) const override;

    /**
     * Create a disabled aquifer (no fluid generation)
     * Reference: Aquifer.java lines 22-31
     */
    static Aquifer* createDisabled(FluidPicker* fluidPicker);

    /**
     * Create a noise-based aquifer
     * Reference: Aquifer.java lines 18-20
     */
    static Aquifer* create(
        NoiseChunk* noiseChunk,
        const world::ChunkPos& pos,
        const NoiseRouter& router,
        XoroshiroPositionalRandomFactory* positionalRandomFactory,
        int32_t minBlockY,
        int32_t yBlockSize,
        FluidPicker* globalFluidPicker
    );
};

/**
 * DisabledAquifer - Never generates fluids
 * Reference: Aquifer.java lines 32-45
 */
class DisabledAquifer : public Aquifer {
private:
    FluidPicker* m_globalFluidPicker;

public:
    explicit DisabledAquifer(FluidPicker* globalFluidPicker);

    ::world::IBlockType* computeSubstance(
        const density::DensityFunction::FunctionContext& context,
        double density
    ) override;

    bool shouldScheduleFluidUpdate() const override {
        return false;
    }
};

/**
 * NoiseBasedAquifer - Full grid-based aquifer simulation
 * Reference: Aquifer.java lines 38-458
 *
 * This implementation uses a grid of aquifer cells (spaced 16 blocks horizontally, 12 blocks vertically).
 * For each position, it finds the nearest aquifer cells and interpolates between them,
 * calculating barrier pressure to prevent unrealistic fluid spread.
 */
class NoiseBasedAquifer : public Aquifer {
private:
    // Constants from Java (lines 39-60)
    static constexpr int32_t X_RANGE = 10;
    static constexpr int32_t Y_RANGE = 9;
    static constexpr int32_t Z_RANGE = 10;
    static constexpr int32_t X_SEPARATION = 6;
    static constexpr int32_t Y_SEPARATION = 3;
    static constexpr int32_t Z_SEPARATION = 6;
    static constexpr int32_t X_SPACING = 16;
    static constexpr int32_t Y_SPACING = 12;
    static constexpr int32_t Z_SPACING = 16;
    static constexpr int32_t SAMPLE_OFFSET_X = -5;
    static constexpr int32_t SAMPLE_OFFSET_Y = 1;
    static constexpr int32_t SAMPLE_OFFSET_Z = -5;

    // Noise functions (Java lines 62-65)
    density::DensityFunction* m_barrierNoise;
    density::DensityFunction* m_fluidLevelFloodednessNoise;
    density::DensityFunction* m_fluidLevelSpreadNoise;
    density::DensityFunction* m_lavaNoise;

    // Additional density functions for deep dark check (Java lines 70-71)
    density::DensityFunction* m_erosion;
    density::DensityFunction* m_depth;

    // Positional random factory for generating aquifer locations (Java line 66)
    XoroshiroPositionalRandomFactory* m_positionalRandomFactory;

    FluidPicker* m_globalFluidPicker;
    NoiseChunk* m_noiseChunk;  // For preliminarySurfaceLevel

    // Grid parameters (Java lines 74-78)
    int32_t m_minGridX, m_minGridY, m_minGridZ;
    int32_t m_gridSizeX, m_gridSizeY, m_gridSizeZ;
    int32_t m_skipSamplingAboveY;  // Java line 73

    // Aquifer grid caching (Java lines 67-68)
    // Cache stores FluidStatus for aquifer cells in the grid
    std::vector<FluidStatus> m_aquiferCache;
    std::vector<int64_t> m_aquiferLocationCache;

    mutable bool m_shouldScheduleFluidUpdate;  // Java line 72

public:
    /**
     * Construct a noise-based aquifer
     * Reference: Aquifer.java lines 81-107
     */
    NoiseBasedAquifer(
        NoiseChunk* noiseChunk,
        const world::ChunkPos& pos,
        const NoiseRouter& router,
        XoroshiroPositionalRandomFactory* positionalRandomFactory,
        int32_t minBlockY,
        int32_t yBlockSize,
        FluidPicker* globalFluidPicker
    );

    /**
     * Main decision method - determines block type at position
     * Reference: Aquifer.java lines 116-251
     */
    ::world::IBlockType* computeSubstance(
        const density::DensityFunction::FunctionContext& context,
        double density
    ) override;

    bool shouldScheduleFluidUpdate() const override {
        return m_shouldScheduleFluidUpdate;
    }

    // Accessors for testing
    int32_t getMinGridX() const { return m_minGridX; }
    int32_t getMinGridY() const { return m_minGridY; }
    int32_t getMinGridZ() const { return m_minGridZ; }
    int32_t getGridSizeX() const { return m_gridSizeX; }
    int32_t getGridSizeY() const { return m_gridSizeY; }
    int32_t getGridSizeZ() const { return m_gridSizeZ; }
    int32_t getSkipSamplingAboveY() const { return m_skipSamplingAboveY; }

    // Get aquifer location for a grid cell (for debugging)
    int64_t getAquiferLocation(int32_t gridX, int32_t gridY, int32_t gridZ) {
        int32_t index = getIndex(gridX, gridY, gridZ);
        if (index >= 0 && index < static_cast<int32_t>(m_aquiferLocationCache.size())) {
            // Check if already computed, if not compute it
            if (m_aquiferLocationCache[index] == INT64_MIN) {
                // Need to compute it - call getAquiferStatus to populate the cache
                getAquiferStatus(index);
            }
            return m_aquiferLocationCache[index];
        }
        return INT64_MIN;
    }

private:
    // Grid coordinate conversions (Java lines 320-342)
    static int32_t gridX(int32_t blockX) { return blockX >> 4; }  // Divide by 16
    static int32_t gridY(int32_t blockY);  // Uses floorDiv by 12
    static int32_t gridZ(int32_t blockZ) { return blockZ >> 4; }
    static int32_t fromGridX(int32_t gridX, int32_t offset) { return (gridX << 4) + offset; }
    static int32_t fromGridY(int32_t gridY, int32_t offset) { return gridY * 12 + offset; }
    static int32_t fromGridZ(int32_t gridZ, int32_t offset) { return (gridZ << 4) + offset; }

    /**
     * Get the index into the aquifer cache arrays
     * Reference: Aquifer.java lines 109-114
     */
    int32_t getIndex(int32_t gridX, int32_t gridY, int32_t gridZ) const;

    // Aquifer grid sampling (Java lines 344-354)
    FluidStatus getAquiferStatus(int32_t index);

    // Compute fluid at aquifer cell (Java lines 356-392)
    FluidStatus computeFluid(int32_t x, int32_t y, int32_t z);

    // Adjust surface level (Java lines 394-396)
    int32_t adjustSurfaceLevel(int32_t preliminarySurfaceLevel) const {
        return preliminarySurfaceLevel + 8;
    }

    // Fluid level calculation (Java lines 398-426)
    int32_t computeSurfaceLevel(
        int32_t x, int32_t y, int32_t z,
        const FluidStatus& globalFluid,
        int32_t lowestPreliminarySurface,
        bool surfaceAtCenterIsUnderGlobalFluidLevel
    );

    // Randomized fluid level (Java lines 428-440)
    int32_t computeRandomizedFluidSurfaceLevel(
        int32_t x, int32_t y, int32_t z,
        int32_t lowestPreliminarySurface
    );

    // Fluid type determination (Java lines 442-457)
    ::world::IBlockType* computeFluidType(
        int32_t x, int32_t y, int32_t z,
        const FluidStatus& globalFluid,
        int32_t fluidSurfaceLevel
    );

    // Barrier pressure calculation (Java lines 257-260)
    static double similarity(int32_t distanceSqr1, int32_t distanceSqr2);

    // Calculate pressure between two aquifer cells (Java lines 262-318)
    double calculatePressure(
        const density::DensityFunction::FunctionContext& context,
        double& barrierNoiseValue,  // Mutable for caching
        const FluidStatus& status1,
        const FluidStatus& status2
    ) const;
};

// Constants (Java line 79)

// Surface sampling pattern for aquifer (Java line 79)
// Samples 13 positions - center first, then surrounding chunks
// CRITICAL: Order matters - {0,0} must be first for the center check
static const int32_t SURFACE_SAMPLING_OFFSETS_IN_CHUNKS[13][2] = {
    {0, 0},                                    // Center (checked first)
    {-2, -1}, {-1, -1}, {0, -1}, {1, -1},      // Row -1
    {-3, 0}, {-2, 0}, {-1, 0}, {1, 0},         // Row 0 (excluding center)
    {-2, 1}, {-1, 1}, {0, 1}, {1, 1}           // Row 1
};

// Similarity threshold for fluid flow updates (Java line 51)
// Computed as: similarity(Mth.square(10), Mth.square(12)) = similarity(100, 144)
// = 1.0 - (144 - 100) / 25.0 = 1.0 - 44/25 = 1.0 - 1.76 = -0.76
static constexpr double FLOWING_UPDATE_SIMULARITY = -0.76;

// Special Y value indicating "way below minimum" (Java: DimensionType.WAY_BELOW_MIN_Y)
// Java computes this as: MIN_Y << 4 = -2032 << 4 = -32512
// Reference: DimensionType.java line 92
// CRITICAL: Must match Java's value exactly for correct barrier pressure calculations
static constexpr int32_t WAY_BELOW_MIN_Y = -32512;

} // namespace levelgen
} // namespace minecraft
