#pragma once

#include "levelgen/DensityFunction.h"
#include "levelgen/NoiseRouter.h"
#include "levelgen/NoiseSettings.h"
#include "levelgen/Blender.h"
#include "levelgen/Aquifer.h"
#include "levelgen/BlockStateFiller.h"
#include "levelgen/FluidPicker.h"
#include "levelgen/Beardifier.h"
#include "levelgen/NoiseGeneratorSettings.h"
#include "world/level/block/state/BlockState.h"
#include "world/IChunk.h"
#include "world/biome/Climate.h"
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstdint>
#include <cassert>

// Forward declarations
namespace minecraft {
namespace levelgen {
    class RandomState;
}
}

namespace minecraft {
namespace levelgen {

/**
 * BlendingOutput - Result of blending calculations
 * Reference: Blender.java line 375 (BlendingOutput record)
 */
struct BlendingOutput {
    double alpha;           // 1.0 = full new terrain, 0.0 = full old terrain
    double blendingOffset;  // Offset to apply during blending

    BlendingOutput(double a = 1.0, double o = 0.0) : alpha(a), blendingOffset(o) {}
};

/**
 * NoiseChunk - The heart of Minecraft's terrain generation.
 *
 * Implements a sophisticated 3D noise interpolation system exactly matching Minecraft 26.1:
 * - Divides chunk into cells (e.g., 4x4x8 blocks per cell in Overworld)
 * - Samples density functions at cell corners
 * - Uses trilinear interpolation for smooth values between corners
 * - Employs multiple caching strategies for performance
 *
 * This implementation matches NoiseChunk.java line-by-line for byte-perfect terrain generation.
 */
class NoiseChunk : public density::DensityFunction::FunctionContext,
                   public density::DensityFunction::ContextProvider {
private:
    // Forward declarations of nested classes
    class NoiseInterpolator;
    class CacheAllInCell;
    class Cache2D;
    class CacheOnce;
    class FlatCache;
    class BlendAlpha;
    class BlendOffset;
    class SliceFillingContextProvider;
    class AquiferWithDensity;

    // Friend class for visitor pattern
    friend class WrapVisitor;

public:
    /**
     * Static factory method to create NoiseChunk from a chunk
     * Reference: NoiseChunk.java lines 62-67
     *
     * @param chunk - The chunk to generate terrain for
     * @param randomState - Provides NoiseRouter and random generators
     * @param beardifier - Adds density around structures
     * @param settings - Generator settings (aquifers, ore veins, etc.)
     * @param globalFluidPicker - Chooses fluids for aquifers
     * @param blender - Terrain blending (can be empty)
     * @return New NoiseChunk configured for this chunk
     */
    static NoiseChunk* forChunk(
        ::world::IChunk* chunk,
        RandomState& randomState,
        Beardifier* beardifier,
        const NoiseGeneratorSettings& settings,
        FluidPicker* globalFluidPicker,
        Blender* blender
    );

    /**
     * Constructor matching Java NoiseChunk constructor (line 69-149)
     *
     * @param cellCountXZ - Number of cells horizontally (usually 4 for 16/4)
     * @param randomState - Provides NoiseRouter and random generators
     * @param chunkMinBlockX - Minimum block X for this chunk
     * @param chunkMinBlockZ - Minimum block Z for this chunk
     * @param noiseSettings - Noise sampling configuration
     * @param beardifier - Adds density around structures
     * @param settings - Generator settings (aquifers, ore veins, etc.)
     * @param globalFluidPicker - Chooses fluids for aquifers
     * @param blender - Terrain blending (can be empty)
     */
    NoiseChunk(int cellCountXZ,
               RandomState& randomState,
               int chunkMinBlockX,
               int chunkMinBlockZ,
               const NoiseSettings& noiseSettings,
               Beardifier* beardifier,
               const NoiseGeneratorSettings& settings,
               FluidPicker* globalFluidPicker,
               Blender* blender);

    ~NoiseChunk();

    /**
     * Create a cached climate sampler that wraps the router's climate functions
     * Reference: NoiseChunk.java lines 151-153
     *
     * @param router - The noise router containing climate functions
     * @param spawnTarget - Spawn target parameter points
     * @return Climate sampler that uses wrapped density functions
     */
    world::biome::Climate::Sampler cachedClimateSampler(
        const NoiseRouter& router,
        const std::vector<world::biome::Climate::ParameterPoint>& spawnTarget
    );

    // Accessors
    int cellWidth() const { return m_cellWidth; }
    int cellHeight() const { return m_cellHeight; }

    // Debug accessors (for parity testing)
    int getCellCountXZ() const { return m_cellCountXZ; }
    int getCellCountY() const { return m_cellCountY; }
    int getCellNoiseMinY() const { return m_cellNoiseMinY; }
    int getFirstCellX() const { return m_firstCellX; }
    int getFirstCellZ() const { return m_firstCellZ; }
    int getFirstNoiseX() const { return m_firstNoiseX; }
    int getFirstNoiseZ() const { return m_firstNoiseZ; }
    int getNoiseSizeXZ() const { return m_noiseSizeXZ; }
    int getCellWidth() const { return m_cellWidth; }
    int getCellHeight() const { return m_cellHeight; }

    // Interpolator accessors for debug/parity testing
    size_t getInterpolatorCount() const { return m_interpolators.size(); }
    const std::vector<std::vector<double>>& getInterpolatorSlice0(size_t index) const;
    const std::vector<std::vector<double>>& getInterpolatorSlice1(size_t index) const;

    /**
     * Get the aquifer for this noise chunk
     * Reference: NoiseChunk.java lines 324-326
     */
    Aquifer* aquifer() const { return m_aquifer; }

    //==========================================================================
    // CELL NAVIGATION (matching Java lines 221-234, 266-282, 320-322)
    //==========================================================================

    /**
     * Initialize for the first vertical column (cellX = firstCellX)
     * Java line 221-228
     */
    void initializeForFirstCellX();

    /**
     * Advance to the next X column of cells
     * Java line 231-234
     * @param cellXIndex - The current cell X index (0 to cellCountXZ-1)
     */
    void advanceCellX(int cellXIndex);

    /**
     * Select a specific cell within the current X column
     * Java line 266-282
     * @param cellYIndex - Cell Y index (0 to cellCountY-1)
     * @param cellZIndex - Cell Z index (0 to cellCountXZ-1)
     */
    void selectCellYZ(int cellYIndex, int cellZIndex);

    /**
     * Swap interpolation slices
     * Java line 320-322
     */
    void swapSlices();

    //==========================================================================
    // INTERPOLATION UPDATES (matching Java lines 284-310)
    //==========================================================================

    /**
     * Update interpolators for current Y position within cell
     * Java line 284-290
     * @param posY - Absolute block Y position
     * @param factorY - Position within cell (0.0 to 1.0)
     */
    void updateForY(int posY, double factorY);

    /**
     * Update interpolators for current X position within cell
     * Java line 293-299
     * @param posX - Absolute block X position
     * @param factorX - Position within cell (0.0 to 1.0)
     */
    void updateForX(int posX, double factorX);

    /**
     * Update interpolators for current Z position within cell
     * Java line 302-310
     * @param posZ - Absolute block Z position
     * @param factorZ - Position within cell (0.0 to 1.0)
     */
    void updateForZ(int posZ, double factorZ);

    //==========================================================================
    // BLOCK STATE RETRIEVAL (matching Java line 155-157)
    //==========================================================================

    /**
     * Get the interpolated block state at the current position
     * Java line 155-157
     *
     * Returns:
     * - nullptr if density > 0 (air, above ground)
     * - solid block if density <= 0 and no aquifer fluid
     * - Water/lava if aquifer determines fluid should be here
     * - Ore vein block if ore vein generation is active
     *
     * @return Block to place, or nullptr for air
     */
    BlockState* getInterpolatedState();

    /**
     * Get the current interpolated density value
     * Java line similar to noiseChunk.getInterpolatedValue()
     *
     * The density value determines terrain:
     * - density > 0: air (above ground level)
     * - density <= 0: solid (below ground level)
     *
     * @return Current interpolated density
     */
    double getInterpolatedDensity() const;

    /**
     * Stop all interpolation
     * Java line 312-318
     */
    void stopInterpolation();

    //==========================================================================
    // FunctionContext implementation (Java lines 159-169)
    //==========================================================================

    int blockX() const override { return m_cellStartBlockX + m_inCellX; }
    int blockY() const override { return m_cellStartBlockY + m_inCellY; }
    int blockZ() const override { return m_cellStartBlockZ + m_inCellZ; }

    Blender* getBlender() const override { return m_blender; }

    //==========================================================================
    // ContextProvider implementation (Java lines 236-246, 248-263)
    //==========================================================================

    /**
     * Get context for a specific index within cell
     * Java line 236-246
     */
    density::DensityFunction::FunctionContext* forIndex(int cellIndex) override;

    /**
     * Fill array with density values
     * Java line 248-263
     */
    void fillAllDirectly(double* output, int count,
                         density::DensityFunction* function) override;

    //==========================================================================
    // PRELIMINARY SURFACE LEVEL (Java lines 186-196)
    //==========================================================================

    /**
     * Get preliminary surface level at a position
     * Java line 186-196
     *
     * @param sampleX - Sample X position (will be quantized to quart)
     * @param sampleZ - Sample Z position (will be quantized to quart)
     * @return Estimated surface Y coordinate
     */
    int preliminarySurfaceLevel(int sampleX, int sampleZ);

    /**
     * Get the maximum preliminary surface level within a region.
     * Reference: NoiseChunk.java lines 171-181
     *
     * @param minBlockX - Minimum block X
     * @param minBlockZ - Minimum block Z
     * @param maxBlockX - Maximum block X
     * @param maxBlockZ - Maximum block Z
     * @return Maximum surface Y within the region
     */
    int maxPreliminarySurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX, int maxBlockZ);

private:
    //==========================================================================
    // GRID DIMENSIONS (matching Java fields line 26-48)
    //==========================================================================

    int m_cellCountXZ;       // Number of cells horizontally (usually 4)
    int m_cellCountY;        // Number of cells vertically (usually 48)
    int m_cellNoiseMinY;     // Minimum cell Y index (usually -8) - Java: cellNoiseMinY
    int m_cellWidth;         // Blocks per cell horizontally (usually 4)
    int m_cellHeight;        // Blocks per cell vertically (usually 8)

    int m_firstCellX;        // First cell X for this chunk
    int m_firstCellZ;        // First cell Z for this chunk
    int m_firstNoiseX;       // First noise X in quart coordinates
    int m_firstNoiseZ;       // First noise Z in quart coordinates
    int m_noiseSizeXZ;       // Size of noise grid in quart coordinates

    //==========================================================================
    // CURRENT POSITION TRACKING (Java line 49-59)
    //==========================================================================

    // Current cell start positions (in block coordinates)
    int m_cellStartBlockX;
    int m_cellStartBlockY;
    int m_cellStartBlockZ;

    // Current position within cell (0 to cellWidth-1 or cellHeight-1)
    int m_inCellX;
    int m_inCellY;
    int m_inCellZ;

    // Interpolation state flags
    bool m_interpolating;    // Java line 49
    bool m_fillingCell;      // Java line 50

    // Cache versioning counters
    int64_t m_interpolationCounter;      // Java line 57
    int64_t m_arrayInterpolationCounter; // Java line 58
    int m_arrayIndex;                    // Java line 59

    //==========================================================================
    // DENSITY FUNCTIONS & CACHES (Java line 33-43)
    //==========================================================================

    std::vector<NoiseInterpolator*> m_interpolators;  // Java line 33
    std::vector<CacheAllInCell*> m_cellCaches;        // Java line 34

    // Wrapped density functions (Java line 35 - Map<DensityFunction, DensityFunction>)
    std::unordered_map<density::DensityFunction*, density::DensityFunction*> m_wrapped;

    //==========================================================================
    // ARENA ALLOCATOR for wrapped objects
    // Eliminates individual malloc calls per chunk
    //==========================================================================
    static constexpr size_t WRAP_ARENA_SIZE = 256 * 1024;  // 256KB per chunk
    static constexpr size_t WRAP_ARENA_ALIGN = 16;  // Alignment for SSE/vtable ptrs

    // Use aligned storage for arena
    struct alignas(WRAP_ARENA_ALIGN) ArenaStorage {
        char data[WRAP_ARENA_ALIGN];
    };
    std::vector<ArenaStorage> m_wrapArena;
    size_t m_wrapArenaOffset = 0;

    // Placement new allocator - returns aligned memory from arena
    template<typename T, typename... Args>
    T* arenaAlloc(Args&&... args) {
        // Align offset to type's required alignment
        size_t alignment = alignof(T);
        m_wrapArenaOffset = (m_wrapArenaOffset + alignment - 1) & ~(alignment - 1);

        // Arena should be large enough - assert to catch sizing issues
        assert(m_wrapArenaOffset + sizeof(T) <= WRAP_ARENA_SIZE && "Arena overflow - increase WRAP_ARENA_SIZE");

        char* base = reinterpret_cast<char*>(m_wrapArena.data());
        void* ptr = base + m_wrapArenaOffset;
        m_wrapArenaOffset += sizeof(T);

        return new(ptr) T(std::forward<Args>(args)...);
    }

    density::DensityFunction* m_preliminarySurfaceLevel;  // Java line 38

    // Aquifer for fluid generation (Java line 37)
    Aquifer* m_aquifer;

    // Block state rule for determining block types (Java line 39)
    BlockStateFiller* m_blockStateRule;  // Aquifer + OreVeinifier chain

    // Beardifier for structure density modification (Java line 43)
    Beardifier* m_beardifier;

    //==========================================================================
    // PRELIMINARY SURFACE CACHE (Java line 36)
    //==========================================================================

    // Java uses Long2IntOpenHashMap - we'll use unordered_map
    std::unordered_map<int64_t, int> m_preliminarySurfaceLevelCache;

    //==========================================================================
    // BLENDING (Java line 40-45)
    //==========================================================================

    Blender* m_blender;
    FlatCache* m_blendAlpha;   // Java line 41
    FlatCache* m_blendOffset;  // Java line 42

    int64_t m_lastBlendingDataPos;      // Java line 44
    BlendingOutput m_lastBlendingOutput; // Java line 45

    //==========================================================================
    // CONTEXT PROVIDER FOR SLICE FILLING (Java line 60, 72-95)
    //==========================================================================

    SliceFillingContextProvider* m_sliceFillingContextProvider;

    //==========================================================================
    // HELPER METHODS (Java lines 192-219, 348-386)
    //==========================================================================

    /**
     * Compute preliminary surface level for a column (uncached)
     * Java line 192-196
     */
    int computePreliminarySurfaceLevel(int64_t key);

    /**
     * Fill a vertical slice of cell corners
     * Java line 202-219
     */
    void fillSlice(bool slice0, int cellX);

    /**
     * Get or compute blending output for a position
     * Reference: NoiseChunk.java lines 336-346
     *
     * @param blockX - Block X coordinate
     * @param blockZ - Block Z coordinate
     * @return BlendingOutput with alpha and blendingOffset
     */
    BlendingOutput getOrComputeBlendingOutput(int blockX, int blockZ);

    /**
     * Wrap a density function with appropriate cache/interpolation
     * Java line 348-350 - inlined for performance
     */
    inline density::DensityFunction* wrap(density::DensityFunction* function) {
        // Use try_emplace for single lookup (like Java's computeIfAbsent)
        auto [it, inserted] = m_wrapped.try_emplace(function, nullptr);
        if (inserted) {
            it->second = wrapNew(function);
        }
        return it->second;
    }

    /**
     * Create a new wrapped density function
     * Java line 352-386
     */
    density::DensityFunction* wrapNew(density::DensityFunction* function);
};

} // namespace levelgen
} // namespace minecraft
