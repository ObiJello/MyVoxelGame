#include "levelgen/NoiseRouter.h"
#include "levelgen/NoiseChunk.h"
#include "levelgen/RandomState.h"
#include "levelgen/DensityFunctions.h"
#include "levelgen/MaterialRuleList.h"
#include "levelgen/OreVeinifier.h"
#include "world/level/block/Blocks.h"
#include "world/ChunkPos.h"
#include "math/Mth.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace minecraft {
namespace levelgen {

//==============================================================================
// NOISE INTERPOLATOR - Nested Class (Java lines 485-584)
// Implements trilinear interpolation between 8 corner values
//==============================================================================

class NoiseChunk::NoiseInterpolator : public density::DensityFunction {
private:
    NoiseChunk* m_owner;
    density::DensityFunction* m_noiseFiller;

    // Two 2D slices - each holds density values for a Y-Z plane
    // Java: private double[][] slice0; private double[][] slice1;
    std::vector<std::vector<double>> m_slice0;
    std::vector<std::vector<double>> m_slice1;

    // 8 corner values for current cell (Java lines 489-496)
    double m_noise000, m_noise001, m_noise100, m_noise101;
    double m_noise010, m_noise011, m_noise110, m_noise111;

    // Intermediate interpolation results (Java lines 497-503)
    double m_valueXZ00, m_valueXZ10, m_valueXZ01, m_valueXZ11;
    double m_valueZ0, m_valueZ1;
    double m_value;

    // Allocate slice arrays (Java lines 514-524)
    std::vector<std::vector<double>> allocateSlice(int cellCountY, int cellCountZ) {
        int sizeZ = cellCountZ + 1;
        int sizeY = cellCountY + 1;
        std::vector<std::vector<double>> result(sizeZ);

        for (int cellZIndex = 0; cellZIndex < sizeZ; ++cellZIndex) {
            result[cellZIndex].resize(sizeY, 0.0);
        }

        return result;
    }

public:
    NoiseInterpolator(NoiseChunk* owner, density::DensityFunction* noiseFiller)
        : m_owner(owner), m_noiseFiller(noiseFiller)
    {
        // Java lines 505-512
        m_slice0 = allocateSlice(owner->m_cellCountY, owner->m_cellCountXZ);
        m_slice1 = allocateSlice(owner->m_cellCountY, owner->m_cellCountXZ);
        m_owner->m_interpolators.push_back(this);

        // Initialize corner values
        m_noise000 = m_noise001 = m_noise100 = m_noise101 = 0.0;
        m_noise010 = m_noise011 = m_noise110 = m_noise111 = 0.0;
        m_valueXZ00 = m_valueXZ10 = m_valueXZ01 = m_valueXZ11 = 0.0;
        m_valueZ0 = m_valueZ1 = 0.0;
        m_value = 0.0;
    }

    // Java lines 526-535
    void selectCellYZ(int cellYIndex, int cellZIndex) {
        m_noise000 = m_slice0[cellZIndex][cellYIndex];
        m_noise001 = m_slice0[cellZIndex + 1][cellYIndex];
        m_noise100 = m_slice1[cellZIndex][cellYIndex];
        m_noise101 = m_slice1[cellZIndex + 1][cellYIndex];
        m_noise010 = m_slice0[cellZIndex][cellYIndex + 1];
        m_noise011 = m_slice0[cellZIndex + 1][cellYIndex + 1];
        m_noise110 = m_slice1[cellZIndex][cellYIndex + 1];
        m_noise111 = m_slice1[cellZIndex + 1][cellYIndex + 1];
    }

    // Java lines 537-542
    void updateForY(double factorY) {
        m_valueXZ00 = Mth::lerp(factorY, m_noise000, m_noise010);
        m_valueXZ10 = Mth::lerp(factorY, m_noise100, m_noise110);
        m_valueXZ01 = Mth::lerp(factorY, m_noise001, m_noise011);
        m_valueXZ11 = Mth::lerp(factorY, m_noise101, m_noise111);
    }

    // Java lines 544-547
    void updateForX(double factorX) {
        m_valueZ0 = Mth::lerp(factorX, m_valueXZ00, m_valueXZ10);
        m_valueZ1 = Mth::lerp(factorX, m_valueXZ01, m_valueXZ11);
    }

    // Java lines 549-551
    void updateForZ(double factorZ) {
        m_value = Mth::lerp(factorZ, m_valueZ0, m_valueZ1);
    }

    // Java lines 553-561
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        const NoiseChunk* chunkContext = dynamic_cast<const NoiseChunk*>(&context);

        if (chunkContext != m_owner) {
            return m_noiseFiller->compute(context);
        } else if (!m_owner->m_interpolating) {
            throw std::runtime_error("Trying to sample interpolator outside the interpolation loop");
        } else {
            // If filling cell, use trilinear interpolation directly
            if (m_owner->m_fillingCell) {
                double factorX = static_cast<double>(m_owner->m_inCellX) / static_cast<double>(m_owner->m_cellWidth);
                double factorY = static_cast<double>(m_owner->m_inCellY) / static_cast<double>(m_owner->m_cellHeight);
                double factorZ = static_cast<double>(m_owner->m_inCellZ) / static_cast<double>(m_owner->m_cellWidth);
                return Mth::lerp3(factorX, factorY, factorZ,
                                  m_noise000, m_noise100, m_noise010, m_noise110,
                                  m_noise001, m_noise101, m_noise011, m_noise111);
            }
            return m_value;
        }
    }

    // Java lines 563-569
    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        if (m_owner->m_fillingCell) {
            contextProvider.fillAllDirectly(output, count, const_cast<NoiseInterpolator*>(this));
        } else {
            m_noiseFiller->fillArray(output, count, contextProvider);
        }
    }

    density::DensityFunction* wrapped() const {
        return m_noiseFiller;
    }

    // Java lines 575-579
    void swapSlices() {
        m_slice0.swap(m_slice1);
    }

    // DensityFunction interface implementation
    double minValue() const override { return m_noiseFiller->minValue(); }
    double maxValue() const override { return m_noiseFiller->maxValue(); }

    // MapAll implementation - NoiseChunk handles wrapping
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    // Java lines 581-583: Return marker type
    density::DensityFunctions::MarkerOrMarked::Type type() const {
        return density::DensityFunctions::MarkerOrMarked::Type::Interpolated;
    }

    // Expose slices for fillSlice
    std::vector<std::vector<double>>& getSlice0() { return m_slice0; }
    std::vector<std::vector<double>>& getSlice1() { return m_slice1; }
};

//==============================================================================
// CACHE ALL IN CELL - Nested Class (Java lines 447-483)
//==============================================================================

class NoiseChunk::CacheAllInCell : public density::DensityFunction {
private:
    NoiseChunk* m_owner;
    density::DensityFunction* m_noiseFiller;
    std::vector<double> m_values;

public:
    CacheAllInCell(NoiseChunk* owner, density::DensityFunction* noiseFiller)
        : m_owner(owner), m_noiseFiller(noiseFiller)
    {
        // Java lines 451-457
        m_values.resize(owner->m_cellWidth * owner->m_cellWidth * owner->m_cellHeight, 0.0);
        m_owner->m_cellCaches.push_back(this);
    }

    // Java lines 459-470
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        const NoiseChunk* chunkContext = dynamic_cast<const NoiseChunk*>(&context);

        if (chunkContext != m_owner) {
            return m_noiseFiller->compute(context);
        } else if (!m_owner->m_interpolating) {
            throw std::runtime_error("Trying to sample interpolator outside the interpolation loop");
        } else {
            int x = m_owner->m_inCellX;
            int y = m_owner->m_inCellY;
            int z = m_owner->m_inCellZ;

            // Check bounds
            if (x >= 0 && y >= 0 && z >= 0 &&
                x < m_owner->m_cellWidth && y < m_owner->m_cellHeight && z < m_owner->m_cellWidth) {
                // Java index calculation: ((cellHeight - 1 - y) * cellWidth + x) * cellWidth + z
                int index = ((m_owner->m_cellHeight - 1 - y) * m_owner->m_cellWidth + x) * m_owner->m_cellWidth + z;
                return m_values[index];
            } else {
                return m_noiseFiller->compute(context);
            }
        }
    }

    // Java lines 472-474
    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<CacheAllInCell*>(this));
    }

    density::DensityFunction* wrapped() const { return m_noiseFiller; }

    double minValue() const override { return m_noiseFiller->minValue(); }
    double maxValue() const override { return m_noiseFiller->maxValue(); }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    // Java lines 480-482: Return marker type
    density::DensityFunctions::MarkerOrMarked::Type type() const {
        return density::DensityFunctions::MarkerOrMarked::Type::CacheAllInCell;
    }

    // Expose values for filling
    std::vector<double>& getValues() { return m_values; }

    density::DensityFunction* getNoiseFiller() const { return m_noiseFiller; }
};

//==============================================================================
// CACHE ONCE - Nested Class (Java lines 586-636)
//==============================================================================

class NoiseChunk::CacheOnce : public density::DensityFunction {
private:
    NoiseChunk* m_owner;
    density::DensityFunction* m_function;
    int64_t m_lastCounter;
    int64_t m_lastArrayCounter;
    double m_lastValue;
    std::vector<double> m_lastArray;

public:
    CacheOnce(NoiseChunk* owner, density::DensityFunction* function)
        : m_owner(owner), m_function(function),
          m_lastCounter(-1), m_lastArrayCounter(-1), m_lastValue(0.0)
    {
    }

    // Java lines 599-612
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        const NoiseChunk* chunkContext = dynamic_cast<const NoiseChunk*>(&context);

        if (chunkContext != m_owner) {
            return m_function->compute(context);
        } else if (!m_lastArray.empty() && m_lastArrayCounter == m_owner->m_arrayInterpolationCounter) {
            return m_lastArray[m_owner->m_arrayIndex];
        } else if (m_lastCounter == m_owner->m_interpolationCounter) {
            return m_lastValue;
        } else {
            // Need to cast away const to update cache
            CacheOnce* mutableThis = const_cast<CacheOnce*>(this);
            mutableThis->m_lastCounter = m_owner->m_interpolationCounter;
            double value = m_function->compute(context);
            mutableThis->m_lastValue = value;
            return value;
        }
    }

    // Java lines 614-627
    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        // Use count parameter for output size (matches Java's output.length)
        if (!m_lastArray.empty() && m_lastArrayCounter == m_owner->m_arrayInterpolationCounter) {
            std::copy(m_lastArray.begin(), m_lastArray.end(), output);
        } else {
            wrapped()->fillArray(output, count, contextProvider);

            // Need to cast away const to update cache
            CacheOnce* mutableThis = const_cast<CacheOnce*>(this);
            if (!m_lastArray.empty() && static_cast<int>(m_lastArray.size()) == count) {
                std::copy(output, output + count, mutableThis->m_lastArray.begin());
            } else {
                mutableThis->m_lastArray.assign(output, output + count);
            }

            mutableThis->m_lastArrayCounter = m_owner->m_arrayInterpolationCounter;
        }
    }

    density::DensityFunction* wrapped() const { return m_function; }

    double minValue() const override { return m_function->minValue(); }
    double maxValue() const override { return m_function->maxValue(); }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    // Java lines 633-635: Return marker type
    density::DensityFunctions::MarkerOrMarked::Type type() const {
        return density::DensityFunctions::MarkerOrMarked::Type::CacheOnce;
    }
};

//==============================================================================
// CACHE 2D - Nested Class (Java lines 638-673)
//==============================================================================

class NoiseChunk::Cache2D : public density::DensityFunction {
private:
    density::DensityFunction* m_function;
    int64_t m_lastPos2D;
    double m_lastValue;

    static constexpr int64_t INVALID_CHUNK_POS = 0x7FFFFFFFFFFFFFFFL; // ChunkPos.INVALID_CHUNK_POS

public:
    Cache2D(density::DensityFunction* function)
        : m_function(function), m_lastPos2D(INVALID_CHUNK_POS), m_lastValue(0.0)
    {
    }

    // Java lines 648-660
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        int blockX = context.blockX();
        int blockZ = context.blockZ();
        // Pack coordinates into long (matching ChunkPos.asLong)
        int64_t pos2D = Mth::columnPosAsLong(blockX, blockZ);

        if (m_lastPos2D == pos2D) {
            return m_lastValue;
        } else {
            // Need to cast away const to update cache
            Cache2D* mutableThis = const_cast<Cache2D*>(this);
            mutableThis->m_lastPos2D = pos2D;
            double value = m_function->compute(context);
            mutableThis->m_lastValue = value;
            return value;
        }
    }

    // Java lines 662-664
    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        m_function->fillArray(output, count, contextProvider);
    }

    density::DensityFunction* wrapped() const { return m_function; }

    double minValue() const override { return m_function->minValue(); }
    double maxValue() const override { return m_function->maxValue(); }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    // Java lines 670-672: Return marker type
    density::DensityFunctions::MarkerOrMarked::Type type() const {
        return density::DensityFunctions::MarkerOrMarked::Type::Cache2D;
    }
};

//==============================================================================
// FLAT CACHE - Nested Class (Java lines 400-445)
//==============================================================================

class NoiseChunk::FlatCache : public density::DensityFunction {
private:
    NoiseChunk* m_owner;
    density::DensityFunction* m_noiseFiller;
    std::vector<double> m_values;
    int m_sizeXZ;

public:
    FlatCache(NoiseChunk* owner, density::DensityFunction* noiseFiller, bool fill)
        : m_owner(owner), m_noiseFiller(noiseFiller)
    {
        // Java lines 405-423
        m_sizeXZ = owner->m_noiseSizeXZ + 1;
        m_values.resize(m_sizeXZ * m_sizeXZ, 0.0);

        if (fill) {
            for (int x = 0; x <= owner->m_noiseSizeXZ; ++x) {
                int quartX = owner->m_firstNoiseX + x;
                int blockX = Mth::quartToBlock(quartX);

                for (int z = 0; z <= owner->m_noiseSizeXZ; ++z) {
                    int quartZ = owner->m_firstNoiseZ + z;
                    int blockZ = Mth::quartToBlock(quartZ);

                    density::DensityFunction::SinglePointContext ctx(blockX, 0, blockZ);
                    double value = noiseFiller->compute(ctx);
                    m_values[x + z * m_sizeXZ] = value;
                }
            }
        }
    }

    // Java lines 426-432
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        int quartX = Mth::quartFromBlock(context.blockX());
        int quartZ = Mth::quartFromBlock(context.blockZ());
        int x = quartX - m_owner->m_firstNoiseX;
        int z = quartZ - m_owner->m_firstNoiseZ;

        if (x >= 0 && z >= 0 && x < m_sizeXZ && z < m_sizeXZ) {
            return m_values[x + z * m_sizeXZ];
        } else {
            return m_noiseFiller->compute(context);
        }
    }

    // Java lines 434-436
    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<FlatCache*>(this));
    }

    density::DensityFunction* wrapped() const { return m_noiseFiller; }

    double minValue() const override { return m_noiseFiller->minValue(); }
    double maxValue() const override { return m_noiseFiller->maxValue(); }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }

    // Java lines 442-444: Return marker type
    density::DensityFunctions::MarkerOrMarked::Type type() const {
        return density::DensityFunctions::MarkerOrMarked::Type::FlatCache;
    }

    // Expose values for blending
    std::vector<double>& getValues() { return m_values; }
    int getSizeXZ() const { return m_sizeXZ; }
};

//==============================================================================
// AQUIFER WITH DENSITY - Wrapper BlockStateFiller (Java lines 143-145 lambda)
// Computes the full noise density and passes it to aquifer.computeSubstance()
//==============================================================================

class NoiseChunk::AquiferWithDensity : public BlockStateFiller {
private:
    Aquifer* m_aquifer;
    density::DensityFunction* m_fullNoiseValue;

public:
    AquiferWithDensity(Aquifer* aquifer, density::DensityFunction* fullNoiseValue)
        : m_aquifer(aquifer), m_fullNoiseValue(fullNoiseValue)
    {
    }

    // Reference: Java line 145: (context) -> this.aquifer.computeSubstance(context, fullNoiseValue.compute(context))
    BlockState* calculate(
        const density::DensityFunction::FunctionContext& context
    ) const override {
        if (!m_fullNoiseValue) {
            return nullptr;
        }
        double density = m_fullNoiseValue->compute(context);

        if (!m_aquifer) {
            return nullptr;
        }
        return m_aquifer->computeSubstance(context, density);
    }
};

//==============================================================================
// BLEND ALPHA - Nested Class (Java lines 675-708)
//==============================================================================

class NoiseChunk::BlendAlpha : public density::DensityFunction {
private:
    NoiseChunk* m_owner;

public:
    BlendAlpha(NoiseChunk* owner) : m_owner(owner) {}

    density::DensityFunction* wrapped() const {
        // Reference to DensityFunctions.BlendAlpha.INSTANCE - we'll return nullptr for now
        return nullptr;
    }

    // Java lines 689-691
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        BlendingOutput output = m_owner->getOrComputeBlendingOutput(context.blockX(), context.blockZ());
        return output.alpha;
    }

    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<BlendAlpha*>(this));
    }

    double minValue() const override { return 0.0; }
    double maxValue() const override { return 1.0; }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }
};

//==============================================================================
// BLEND OFFSET - Nested Class (Java lines 710-743)
//==============================================================================

class NoiseChunk::BlendOffset : public density::DensityFunction {
private:
    NoiseChunk* m_owner;

public:
    BlendOffset(NoiseChunk* owner) : m_owner(owner) {}

    density::DensityFunction* wrapped() const {
        // Reference to DensityFunctions.BlendOffset.INSTANCE
        return nullptr;
    }

    // Java lines 724-726
    double compute(const density::DensityFunction::FunctionContext& context) const override {
        BlendingOutput output = m_owner->getOrComputeBlendingOutput(context.blockX(), context.blockZ());
        return output.blendingOffset;
    }

    void fillArray(double* __restrict output, int32_t count, density::DensityFunction::ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<BlendOffset*>(this));
    }

    double minValue() const override { return -std::numeric_limits<double>::infinity(); }
    double maxValue() const override { return std::numeric_limits<double>::infinity(); }

    // MapAll implementation
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }
};

//==============================================================================
// SLICE FILLING CONTEXT PROVIDER - Nested Class (Java lines 72-95)
//==============================================================================

class NoiseChunk::SliceFillingContextProvider : public density::DensityFunction::ContextProvider {
private:
    NoiseChunk* m_owner;

public:
    SliceFillingContextProvider(NoiseChunk* owner) : m_owner(owner) {}

    // Java lines 77-83
    density::DensityFunction::FunctionContext* forIndex(int cellYIndex) override {
        m_owner->m_cellStartBlockY = (cellYIndex + m_owner->m_cellNoiseMinY) * m_owner->m_cellHeight;
        ++m_owner->m_interpolationCounter;
        m_owner->m_inCellY = 0;
        m_owner->m_arrayIndex = cellYIndex;
        return m_owner;
    }

    // Java lines 85-94
    void fillAllDirectly(double* output, int count, density::DensityFunction* function) override {
        (void)count;  // Unused - we iterate based on cellCountY
        for (int cellYIndex = 0; cellYIndex < m_owner->m_cellCountY + 1; ++cellYIndex) {
            m_owner->m_cellStartBlockY = (cellYIndex + m_owner->m_cellNoiseMinY) * m_owner->m_cellHeight;
            ++m_owner->m_interpolationCounter;
            m_owner->m_inCellY = 0;
            m_owner->m_arrayIndex = cellYIndex;
            output[cellYIndex] = function->compute(*m_owner);
        }
    }
};

//==============================================================================
// WRAP VISITOR - Used to transform router functions through wrap()
//==============================================================================

/**
 * WrapVisitor - Visitor that wraps density functions through NoiseChunk::wrap()
 * Reference: Java uses this::wrap as the visitor in mapAll
 */
class WrapVisitor : public density::DensityFunction::Visitor {
private:
    NoiseChunk* m_owner;

public:
    explicit WrapVisitor(NoiseChunk* owner) : m_owner(owner) {}

    density::DensityFunction* apply(density::DensityFunction* input) override {
        return m_owner->wrap(input);
    }
};

//==============================================================================
// NOISECHUNK IMPLEMENTATION
//==============================================================================

// Constructor (Java lines 69-149)
NoiseChunk::NoiseChunk(int cellCountXZ,
                       RandomState& randomState,
                       int chunkMinBlockX,
                       int chunkMinBlockZ,
                       const NoiseSettings& noiseSettings,
                       Beardifier* beardifier,
                       const NoiseGeneratorSettings& settings,
                       FluidPicker* globalFluidPicker,
                       Blender* blender)
    : m_cellCountXZ(cellCountXZ),
      m_cellCountY(Mth::floorDiv(noiseSettings.height(), noiseSettings.getCellHeight())),
      m_cellNoiseMinY(Mth::floorDiv(noiseSettings.minY(), noiseSettings.getCellHeight())),
      m_cellWidth(noiseSettings.getCellWidth()),
      m_cellHeight(noiseSettings.getCellHeight()),
      m_firstCellX(0),  // Will be set in constructor body
      m_firstCellZ(0),
      m_firstNoiseX(0),
      m_firstNoiseZ(0),
      m_noiseSizeXZ(0),
      m_cellStartBlockX(0),
      m_cellStartBlockY(0),
      m_cellStartBlockZ(0),
      m_inCellX(0),
      m_inCellY(0),
      m_inCellZ(0),
      m_interpolating(false),
      m_fillingCell(false),
      m_interpolationCounter(0),
      m_arrayInterpolationCounter(0),
      m_arrayIndex(0),
      m_preliminarySurfaceLevel(nullptr),
      m_aquifer(nullptr),
      m_blockStateRule(nullptr),
      m_beardifier(beardifier),
      m_blender(blender),
      m_blendAlpha(nullptr),
      m_blendOffset(nullptr),
      m_lastBlendingDataPos(0x7FFFFFFFFFFFFFFFL),
      m_lastBlendingOutput(1.0, 0.0),
      m_sliceFillingContextProvider(nullptr)
{
    // Java lines 96-107
    m_firstCellX = Mth::floorDiv(chunkMinBlockX, m_cellWidth);
    m_firstCellZ = Mth::floorDiv(chunkMinBlockZ, m_cellWidth);
    m_firstNoiseX = Mth::quartFromBlock(chunkMinBlockX);
    m_firstNoiseZ = Mth::quartFromBlock(chunkMinBlockZ);
    m_noiseSizeXZ = Mth::quartFromBlock(cellCountXZ * m_cellWidth);

    // Initialize position tracking
    m_cellStartBlockX = 0;
    m_cellStartBlockY = 0;
    m_cellStartBlockZ = 0;
    m_inCellX = 0;
    m_inCellY = 0;
    m_inCellZ = 0;

    // Initialize arena for wrapped objects (eliminates ~200 malloc calls)
    // Must be done early since blend caches use it
    // ArenaStorage is WRAP_ARENA_ALIGN bytes each, so we need WRAP_ARENA_SIZE/WRAP_ARENA_ALIGN elements
    m_wrapArena.resize(WRAP_ARENA_SIZE / WRAP_ARENA_ALIGN);
    m_wrapArenaOffset = 0;

    // Create slice filling context provider (Java lines 72-95)
    m_sliceFillingContextProvider = new SliceFillingContextProvider(this);

    // Create blend caches (Java lines 110-128)
    // Using arena allocation for the inner BlendAlpha/BlendOffset and outer FlatCache
    auto* blendAlphaInner = arenaAlloc<BlendAlpha>(this);
    auto* blendOffsetInner = arenaAlloc<BlendOffset>(this);
    m_blendAlpha = arenaAlloc<FlatCache>(this, blendAlphaInner, false);
    m_blendOffset = arenaAlloc<FlatCache>(this, blendOffsetInner, false);

    if (!blender->isEmpty()) {
        // Fill blending caches
        for (int x = 0; x <= m_noiseSizeXZ; ++x) {
            int quartX = m_firstNoiseX + x;
            int blockX = Mth::quartToBlock(quartX);

            for (int z = 0; z <= m_noiseSizeXZ; ++z) {
                int quartZ = m_firstNoiseZ + z;
                int blockZ = Mth::quartToBlock(quartZ);

                double alpha, offset;
                blender->blendOffsetAndFactor(blockX, blockZ, alpha, offset);

                m_blendAlpha->getValues()[x + z * m_blendAlpha->getSizeXZ()] = alpha;
                m_blendOffset->getValues()[x + z * m_blendOffset->getSizeXZ()] = offset;
            }
        }
    } else {
        // Fill with default values (Java lines 126-127)
        std::fill(m_blendAlpha->getValues().begin(), m_blendAlpha->getValues().end(), 1.0);
        std::fill(m_blendOffset->getValues().begin(), m_blendOffset->getValues().end(), 0.0);
    }

    // Get router from RandomState and wrap all functions (Java line 130-132)
    const minecraft::levelgen::NoiseRouter* router = randomState.router();
    // Pre-allocate space in the wrap cache to avoid rehashing
    // Typical density function tree has ~5500 nodes
    m_wrapped.reserve(6000);
    NoiseRouter wrappedRouter = [&]() {
        WrapVisitor wrapVisitor(this);
        return router->mapAll(wrapVisitor);
    }();

    // Get wrapped preliminary surface level (Java line 131)
    m_preliminarySurfaceLevel = wrappedRouter.preliminarySurfaceLevel();
    m_wrappedFinalDensityForDebug = wrappedRouter.finalDensity();
    m_wrappedVeinToggleForDebug = wrappedRouter.veinToggle();
    m_wrappedVeinRidgedForDebug = wrappedRouter.veinRidged();
    m_wrappedVeinGapForDebug = wrappedRouter.veinGap();

    // Create aquifer based on settings (Java lines 133-138)
    // NOTE: Must create aquifer BEFORE fullNoiseValue since we need it
    {
        if (settings.isAquifersEnabled()) {
            // Create full noise-based aquifer using Aquifer::create factory
            // IMPORTANT: Must use wrappedRouter, not the original router (Java line 138)
            world::ChunkPos chunkPos(chunkMinBlockX / 16, chunkMinBlockZ / 16);
            m_aquifer = Aquifer::create(
                this,
                chunkPos,
                wrappedRouter,
                randomState.aquiferRandom(),
                noiseSettings.minY(),
                noiseSettings.height(),
                globalFluidPicker
            );
        } else {
            // Create disabled aquifer
            m_aquifer = Aquifer::createDisabled(globalFluidPicker);
        }
    }

    // Create fullNoiseValue = cacheAllInCell(add(finalDensity, beardifier)).mapAll(wrap)
    // Reference: Java lines 133-139
    // This is the density function that determines if a block is solid or air
    density::DensityFunction* fullNoiseValue = [&]() {
        density::DensityFunction* finalDensity = wrappedRouter.finalDensity();

        // Add beardifier to final density (m_beardifier IS a DensityFunction)
        density::DensityFunction* addedDensity = density::DensityFunctions::add(finalDensity, m_beardifier);

        // Wrap in cacheAllInCell marker
        density::DensityFunction* cacheWrapped = density::DensityFunctions::cacheAllInCell(addedDensity);

        // Map through wrap() to transform markers into actual cache objects
        // This will create a CacheAllInCell wrapper that caches values within each noise cell
        WrapVisitor wrapVisitor2(this);
        return cacheWrapped->mapAll(wrapVisitor2);
    }();

    // Create MaterialRuleList with AquiferWithDensity and ore veinifier (Java line 147-149)
    // Java uses a lambda: (context) -> this.aquifer.computeSubstance(context, fullNoiseValue.compute(context))
    // We use AquiferWithDensity wrapper to achieve the same
    std::vector<BlockStateFiller*> rules;
    rules.push_back(new AquiferWithDensity(m_aquifer, fullNoiseValue));

    // Add OreVeinifier if enabled (Reference: NoiseChunk.java lines 147-149)
    if (settings.oreVeinsEnabled()) {
        // Get the ore vein density functions from the wrapped router
        OreVeinifier* oreVeinifier = new OreVeinifier(
            wrappedRouter.veinToggle(),
            wrappedRouter.veinRidged(),
            wrappedRouter.veinGap(),
            randomState.oreRandom()
        );
        rules.push_back(oreVeinifier);
    }

    m_blockStateRule = new MaterialRuleList(rules);
}

// Static factory method - Java lines 62-67
NoiseChunk* NoiseChunk::forChunk(
    ::world::IChunk* chunk,
    RandomState& randomState,
    Beardifier* beardifier,
    const NoiseGeneratorSettings& settings,
    FluidPicker* globalFluidPicker,
    Blender* blender
) {
    // Reference: NoiseChunk.java lines 62-67
    const NoiseSettings& noiseSettings = settings.noiseSettings();
    int cellCountXZ = 16 / noiseSettings.getCellWidth();  // Java: Mth.floorDiv(16, noiseSettings.getCellWidth())

    minecraft::world::ChunkPos pos = chunk->getPos();
    int chunkMinBlockX = pos.getMinBlockX();
    int chunkMinBlockZ = pos.getMinBlockZ();

    return new NoiseChunk(
        cellCountXZ,
        randomState,
        chunkMinBlockX,
        chunkMinBlockZ,
        noiseSettings,
        beardifier,
        settings,
        globalFluidPicker,
        blender
    );
}

// Java lines 151-153
world::biome::Climate::Sampler NoiseChunk::cachedClimateSampler(
    const NoiseRouter& router,
    const std::vector<world::biome::Climate::ParameterPoint>& spawnTarget
) {
    // Reference: NoiseChunk.java lines 151-153
    // Creates a Climate.Sampler that uses wrapped density functions for caching
    // Note: Java uses humidity/continentalness/weirdness but our router uses vegetation/continents/ridges
    //
    // CRITICAL: Java uses noises.temperature().mapAll(this::wrap) which recursively
    // applies wrap to ALL nested density functions. This is necessary because the
    // Interpolated markers may be nested deep within the density function tree.
    WrapVisitor wrapVisitor(this);
    return world::biome::Climate::Sampler(
        router.temperature()->mapAll(wrapVisitor),
        router.vegetation()->mapAll(wrapVisitor),      // Java: humidity
        router.continents()->mapAll(wrapVisitor),      // Java: continentalness
        router.erosion()->mapAll(wrapVisitor),
        router.depth()->mapAll(wrapVisitor),
        router.ridges()->mapAll(wrapVisitor),          // Java: weirdness
        spawnTarget
    );
}

NoiseChunk::~NoiseChunk() {
    // Arena-allocated objects: call destructor only, no delete
    // The arena (m_wrapArena) is freed automatically when it goes out of scope

    // Track destructed pointers to avoid double destruction
    std::unordered_set<void*> destructed;

    // Interpolators have internal vectors that need destruction
    for (NoiseInterpolator* interp : m_interpolators) {
        interp->~NoiseInterpolator();
        destructed.insert(interp);
    }

    // Cell caches have internal vectors that need destruction
    for (CacheAllInCell* cache : m_cellCaches) {
        cache->~CacheAllInCell();
        destructed.insert(cache);
    }

    // Blend caches are in arena, call destructors
    if (m_blendAlpha) {
        m_blendAlpha->~FlatCache();
        destructed.insert(m_blendAlpha);
    }
    if (m_blendOffset) {
        m_blendOffset->~FlatCache();
        destructed.insert(m_blendOffset);
    }

    // Destruct remaining arena-allocated objects from m_wrapped
    // These include FlatCache (from markers), CacheOnce, Cache2D, BlendAlpha, BlendOffset
    // that have std::vector members needing cleanup
    for (auto& [key, value] : m_wrapped) {
        // Skip if already destructed or if value == key (not wrapped, not arena allocated)
        if (value != key && destructed.find(value) == destructed.end()) {
            // Virtual destructor calls correct derived class destructor
            value->~DensityFunction();
        }
    }

    // These are NOT in arena, need actual delete
    delete m_sliceFillingContextProvider;
    delete m_blockStateRule;  // This also deletes the aquifer inside MaterialRuleList
}

// Java lines 202-219
// OPTIMIZATION: Cache loop bounds
void NoiseChunk::fillSlice(bool slice0, int cellX) {
    m_cellStartBlockX = cellX * m_cellWidth;
    m_inCellX = 0;

    const int cellCountXZ = m_cellCountXZ;
    const int cellWidth = m_cellWidth;
    const int firstCellZ = m_firstCellZ;
    const int cellCountY = m_cellCountY;

    for (int cellZIndex = 0; cellZIndex < cellCountXZ + 1; ++cellZIndex) {
        int cellZ = firstCellZ + cellZIndex;
        m_cellStartBlockZ = cellZ * cellWidth;
        m_inCellZ = 0;
        ++m_arrayInterpolationCounter;

        for (NoiseInterpolator* noiseInterpolator : m_interpolators) {
            auto& slice = slice0 ? noiseInterpolator->getSlice0()[cellZIndex] :
                                   noiseInterpolator->getSlice1()[cellZIndex];
            noiseInterpolator->fillArray(slice.data(), cellCountY + 1, *m_sliceFillingContextProvider);
        }
    }

    ++m_arrayInterpolationCounter;
}

// Java lines 221-228
void NoiseChunk::initializeForFirstCellX() {
    if (m_interpolating) {
        throw std::runtime_error("Starting interpolation twice");
    }
    m_interpolating = true;
    m_interpolationCounter = 0;
    fillSlice(true, m_firstCellX);
}

// Java lines 231-234
void NoiseChunk::advanceCellX(int cellXIndex) {
    fillSlice(false, m_firstCellX + cellXIndex + 1);
    m_cellStartBlockX = (m_firstCellX + cellXIndex) * m_cellWidth;
}

// Java lines 236-246
density::DensityFunction::FunctionContext* NoiseChunk::forIndex(int cellIndex) {
    int zInCell = cellIndex % m_cellWidth;  // Java: Math.floorMod
    int xyIndex = cellIndex / m_cellWidth;   // Java: Math.floorDiv
    int xInCell = xyIndex % m_cellWidth;     // Java: Math.floorMod
    int yInCell = m_cellHeight - 1 - (xyIndex / m_cellWidth);  // Java: Math.floorDiv

    m_inCellX = xInCell;
    m_inCellY = yInCell;
    m_inCellZ = zInCell;
    m_arrayIndex = cellIndex;

    return this;
}

// Java lines 248-263
// OPTIMIZATION: Use local variables and restrict pointer
void NoiseChunk::fillAllDirectly(double*
#ifdef _MSC_VER
    __restrict
#else
    __restrict__
#endif
    output, int count, density::DensityFunction* function) {
    (void)count;  // Unused - we iterate based on cell dimensions

    // Cache dimensions in local variables for faster access
    const int cellHeight = m_cellHeight;
    const int cellWidth = m_cellWidth;
    m_arrayIndex = 0;

    for (int yInCell = cellHeight - 1; yInCell >= 0; --yInCell) {
        m_inCellY = yInCell;

        for (int xInCell = 0; xInCell < cellWidth; ++xInCell) {
            m_inCellX = xInCell;

            for (int zInCell = 0; zInCell < cellWidth; ++zInCell) {
                m_inCellZ = zInCell;
                output[m_arrayIndex++] = function->compute(*this);
            }
        }
    }
}

// Java lines 266-282
// OPTIMIZATION: Cache loop invariants
void NoiseChunk::selectCellYZ(int cellYIndex, int cellZIndex) {
    for (NoiseInterpolator* i : m_interpolators) {
        i->selectCellYZ(cellYIndex, cellZIndex);
    }

    m_fillingCell = true;
    m_cellStartBlockY = (cellYIndex + m_cellNoiseMinY) * m_cellHeight;
    m_cellStartBlockZ = (m_firstCellZ + cellZIndex) * m_cellWidth;
    ++m_arrayInterpolationCounter;

    const int count = m_cellWidth * m_cellWidth * m_cellHeight;

    for (CacheAllInCell* cellCache : m_cellCaches) {
        cellCache->getNoiseFiller()->fillArray(cellCache->getValues().data(), count, *this);
    }

    ++m_arrayInterpolationCounter;
    m_fillingCell = false;
}

// Java lines 284-290
void NoiseChunk::updateForY(int posY, double factorY) {
    m_inCellY = posY - m_cellStartBlockY;

    for (NoiseInterpolator* i : m_interpolators) {
        i->updateForY(factorY);
    }
}

// Java lines 293-299
void NoiseChunk::updateForX(int posX, double factorX) {
    m_inCellX = posX - m_cellStartBlockX;

    for (NoiseInterpolator* i : m_interpolators) {
        i->updateForX(factorX);
    }
}

// Java lines 302-310
void NoiseChunk::updateForZ(int posZ, double factorZ) {
    m_inCellZ = posZ - m_cellStartBlockZ;
    ++m_interpolationCounter;

    for (NoiseInterpolator* i : m_interpolators) {
        i->updateForZ(factorZ);
    }
}

// Java lines 312-318
void NoiseChunk::stopInterpolation() {
    if (!m_interpolating) {
        throw std::runtime_error("Stopping interpolation twice");
    }
    m_interpolating = false;
}

// Java lines 320-322
void NoiseChunk::swapSlices() {
    for (NoiseInterpolator* interp : m_interpolators) {
        interp->swapSlices();
    }
}

// Java lines 155-157
BlockState* NoiseChunk::getInterpolatedState() {
    // Reference: NoiseChunk.java line 156
    // Simply delegate to the block state rule (which contains aquifer + ore veinifier)
    BlockState* result = m_blockStateRule->calculate(*this);

    return result;
}

// Get the current interpolated density from the first interpolator
double NoiseChunk::getInterpolatedDensity() const {
    // The first interpolator typically corresponds to the final density function
    // This returns the trilinearly interpolated value for the current position
    if (!m_interpolators.empty()) {
        // Access the interpolated value - need to compute it through the interpolator
        return m_interpolators[0]->compute(*this);
    }
    return 0.0;
}

// Java lines 186-196
int NoiseChunk::preliminarySurfaceLevel(int sampleX, int sampleZ) {
    // Quantize to quart positions
    int quantizedX = Mth::quartToBlock(Mth::quartFromBlock(sampleX));
    int quantizedZ = Mth::quartToBlock(Mth::quartFromBlock(sampleZ));

    int64_t key = Mth::columnPosAsLong(quantizedX, quantizedZ);

    // Check cache
    auto it = m_preliminarySurfaceLevelCache.find(key);
    if (it != m_preliminarySurfaceLevelCache.end()) {
        return it->second;
    }

    // Compute and cache
    int level = computePreliminarySurfaceLevel(key);
    m_preliminarySurfaceLevelCache[key] = level;
    return level;
}

// Reference: NoiseChunk.java lines 171-181
int NoiseChunk::maxPreliminarySurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX, int maxBlockZ) {
    int maxY = std::numeric_limits<int>::min();

    // Sample every 4 blocks (quart positions)
    for (int blockZ = minBlockZ; blockZ <= maxBlockZ; blockZ += 4) {
        for (int blockX = minBlockX; blockX <= maxBlockX; blockX += 4) {
            int surfaceLevel = preliminarySurfaceLevel(blockX, blockZ);
            maxY = std::max(maxY, surfaceLevel);
        }
    }

    return maxY;
}

// Java lines 192-196
int NoiseChunk::computePreliminarySurfaceLevel(int64_t key) {
    int blockX = Mth::columnPosGetX(key);
    int blockZ = Mth::columnPosGetZ(key);

    density::DensityFunction::SinglePointContext ctx(blockX, 0, blockZ);
    return Mth::floor(m_preliminarySurfaceLevel->compute(ctx));
}

// Java lines 336-346
BlendingOutput NoiseChunk::getOrComputeBlendingOutput(int blockX, int blockZ) {
    int64_t pos2D = Mth::columnPosAsLong(blockX, blockZ);

    if (m_lastBlendingDataPos == pos2D) {
        return m_lastBlendingOutput;
    }

    m_lastBlendingDataPos = pos2D;
    double alpha, offset;
    m_blender->blendOffsetAndFactor(blockX, blockZ, alpha, offset);
    m_lastBlendingOutput = BlendingOutput(alpha, offset);
    return m_lastBlendingOutput;
}

// Java lines 352-386
// Note: wrap() is now inlined in NoiseChunk.h for performance
density::DensityFunction* NoiseChunk::wrapNew(density::DensityFunction* function) {
    // NOTE: Called for functions that aren't already in m_wrapped cache
    auto wrapType = function->getWrapType();

    // Fast path: most functions don't need wrapping (WrapType::None)
    if (wrapType == density::DensityFunction::WrapType::None) [[likely]] {
        return function;
    }

    // Check if it's a Marker (Java line 353)
    if (wrapType == density::DensityFunction::WrapType::Marker) [[unlikely]] {
        // Use static_cast since we've already verified the type via virtual call
        auto* marker = static_cast<density::DensityFunctions::MarkerOrMarked*>(function);
        auto markerType = function->getMarkerType();

        // Java lines 356-365: switch on marker type and create appropriate cache wrapper
        // Using arenaAlloc instead of new to eliminate malloc overhead
        switch (markerType) {
            case density::DensityFunction::MarkerType::Interpolated:
                // Java line 357: new NoiseInterpolator(marker.wrapped())
                return arenaAlloc<NoiseInterpolator>(this, marker->wrapped());

            case density::DensityFunction::MarkerType::FlatCache:
                // Java line 358: new FlatCache(marker.wrapped(), true)
                return arenaAlloc<FlatCache>(this, marker->wrapped(), true);

            case density::DensityFunction::MarkerType::Cache2D:
                // Java line 359: new Cache2D(marker.wrapped())
                return arenaAlloc<Cache2D>(marker->wrapped());

            case density::DensityFunction::MarkerType::CacheOnce:
                // Java line 360: new CacheOnce(marker.wrapped())
                return arenaAlloc<CacheOnce>(this, marker->wrapped());

            case density::DensityFunction::MarkerType::CacheAllInCell:
                // Java line 361: new CacheAllInCell(marker.wrapped())
                return arenaAlloc<CacheAllInCell>(this, marker->wrapped());

            default:
                throw std::runtime_error("Unknown marker type in wrapNew");
        }
    }

    // Java lines 367-375: Check for BlendAlpha/BlendOffset singleton instances
    if (!m_blender->isEmpty()) {
        if (wrapType == density::DensityFunction::WrapType::BlendAlpha) {
            return m_blendAlpha;
        }
        if (wrapType == density::DensityFunction::WrapType::BlendOffset) {
            return m_blendOffset;
        }
    }

    // Java line 377-378: Check for BeardifierMarker
    if (wrapType == density::DensityFunction::WrapType::Beardifier) {
        return m_beardifier;
    }

    // Java line 383: Return function unchanged (should not reach here with valid WrapType)
    return function;
}

// Interpolator slice accessors for debug/parity testing
const std::vector<std::vector<double>>& NoiseChunk::getInterpolatorSlice0(size_t index) const {
    return m_interpolators[index]->getSlice0();
}

const std::vector<std::vector<double>>& NoiseChunk::getInterpolatorSlice1(size_t index) const {
    return m_interpolators[index]->getSlice1();
}

} // namespace levelgen
} // namespace minecraft
