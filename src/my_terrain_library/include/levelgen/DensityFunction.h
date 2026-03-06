#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace minecraft {

// Forward declarations
class Blender;
class NormalNoise;

namespace density {

/**
 * DensityFunction - Core interface for all terrain density calculations
 *
 * This is the foundation of Minecraft's density-based terrain generation.
 * Every terrain feature (noise, splines, math operations) implements this interface.
 *
 * Reference: net/minecraft/world/level/levelgen/DensityFunction.java
 */
class DensityFunction {
public:
    virtual ~DensityFunction() = default;

    // Forward declarations of nested classes
    class FunctionContext;
    class SinglePointContext;
    class ContextProvider;
    class NoiseHolder;
    class Visitor;

    // Fast type checking to avoid dynamic_cast overhead in wrapNew()
    // These types correspond to special handling in NoiseChunk::wrapNew()
    enum class WrapType {
        None,           // Default - no special wrapping needed
        Marker,         // MarkerOrMarked types (Interpolated, FlatCache, etc.)
        BlendAlpha,     // BlendAlpha singleton
        BlendOffset,    // BlendOffset singleton
        Beardifier      // Beardifier type
    };

    // Marker subtypes - matches MarkerOrMarked::Type
    enum class MarkerType {
        Interpolated,
        FlatCache,
        Cache2D,
        CacheOnce,
        CacheAllInCell
    };

    // Virtual type checking - override in subclasses that need special wrap handling
    virtual WrapType getWrapType() const { return WrapType::None; }
    virtual MarkerType getMarkerType() const { return MarkerType::Interpolated; } // Only valid if WrapType::Marker

    /**
     * FunctionContext - Represents a position in 3D space for density calculations
     * Includes block coordinates and optional blending information
     */
    class FunctionContext {
    public:
        virtual ~FunctionContext() = default;

        virtual int32_t blockX() const = 0;
        virtual int32_t blockY() const = 0;
        virtual int32_t blockZ() const = 0;

        virtual Blender* getBlender() const;  // Returns empty blender by default
    };

    /**
     * SinglePointContext - Simple implementation of FunctionContext for a single point
     * Java: DensityFunction.SinglePointContext (record)
     */
    class SinglePointContext : public FunctionContext {
    public:
        SinglePointContext(int32_t x, int32_t y, int32_t z)
            : m_blockX(x), m_blockY(y), m_blockZ(z) {}

        int32_t blockX() const override { return m_blockX; }
        int32_t blockY() const override { return m_blockY; }
        int32_t blockZ() const override { return m_blockZ; }

    private:
        int32_t m_blockX;
        int32_t m_blockY;
        int32_t m_blockZ;
    };

    /**
     * ContextProvider - Provides contexts for efficient array filling
     * Used by fillArray() to sample multiple points efficiently
     */
    class ContextProvider {
    public:
        virtual ~ContextProvider() = default;

        virtual FunctionContext* forIndex(int32_t index) = 0;
        virtual void fillAllDirectly(double* output, int32_t count, DensityFunction* function) = 0;
    };

    /**
     * NoiseHolder - Wrapper for NormalNoise with optional noise instance
     * Java: DensityFunction.NoiseHolder (record)
     */
    class NoiseHolder {
    public:
        // Constructor with actual noise instance
        NoiseHolder(NormalNoise* noise) : m_noise(noise), m_noiseName("") {}

        // Constructor with noise name (for lazy initialization)
        NoiseHolder(const char* noiseName) : m_noise(nullptr), m_noiseName(noiseName) {}

        double getValue(double x, double y, double z) const;
        double maxValue() const;

        NormalNoise* noise() const { return m_noise; }
        const char* noiseName() const { return m_noiseName; }

        // Set the actual noise instance (used by wiring visitor)
        void setNoise(NormalNoise* noise) { m_noise = noise; }

    private:
        NormalNoise* m_noise;  // Can be nullptr for uninitialized noise
        const char* m_noiseName;  // Name for lazy lookup
    };

    /**
     * Visitor - Visitor pattern for transforming density functions
     * Used to apply transformations to the entire density function tree
     */
    class Visitor {
    public:
        virtual ~Visitor() = default;

        virtual DensityFunction* apply(DensityFunction* input) = 0;

        // NoiseHolder visiting - subclasses can override to transform noise
        virtual NoiseHolder* visitNoise(NoiseHolder* noise) { return noise; }
    };

    /**
     * Core DensityFunction methods
     */

    // Compute density at a single point
    virtual double compute(const FunctionContext& context) const = 0;

    // Fill an array of density values efficiently (for bulk sampling)
    // __restrict tells compiler that output doesn't alias other pointers
    virtual void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const = 0;

    // Apply a visitor transformation to this function and its children
    virtual DensityFunction* mapAll(Visitor& visitor) = 0;

    // Theoretical minimum value this function can produce
    virtual double minValue() const = 0;

    // Theoretical maximum value this function can produce
    virtual double maxValue() const = 0;
};

/**
 * SimpleFunction - Base interface for simple density functions
 * Provides default implementations for fillArray and mapAll
 *
 * Most custom density functions should extend this instead of DensityFunction directly
 * Java: DensityFunction.SimpleFunction
 */
class SimpleFunction : public DensityFunction {
public:
    // Default implementation: fill array by calling compute() for each point
    void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const override {
        contextProvider.fillAllDirectly(output, count, const_cast<SimpleFunction*>(this));
    }

    // Default implementation: just apply visitor to self
    DensityFunction* mapAll(Visitor& visitor) override {
        return visitor.apply(this);
    }
};

} // namespace density
} // namespace minecraft
