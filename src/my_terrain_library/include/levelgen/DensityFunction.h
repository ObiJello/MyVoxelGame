#pragma once
#include <string>
#include <cstdint>

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

        // Memo hooks used by DensityFunction::mapAll (see its comment).
        // Default: no memoisation, i.e. the old per-visit behaviour.
        virtual DensityFunction* lookupMapped(const DensityFunction* original) {
            (void)original;
            return nullptr;
        }
        virtual void rememberMapped(const DensityFunction* original, DensityFunction* mapped) {
            (void)original;
            (void)mapped;
        }
        virtual bool memoises() const { return false; }
        virtual DensityFunction* lookupPreMapped(const std::string& key) { (void)key; return nullptr; }
        virtual void rememberPreMapped(const std::string& key, DensityFunction* mapped) { (void)key; (void)mapped; }

        // NoiseHolder visiting - subclasses can override to transform noise
        virtual NoiseHolder* visitNoise(NoiseHolder* noise) { return noise; }

        // Type-erased ownership sink for nodes heap-allocated during mapAll
        // (`new Clamp(...)`, mapped CubicSpline nodes, ...). Visitors whose
        // mapped tree has a bounded lifetime (WrapVisitor → NoiseChunk)
        // override this to record the node for later deletion; the default is
        // a no-op (node leaks, as before, for one-shot registry visitors).
        // Every `new` inside a mapAll implementation must be routed through
        // own()/ownObject().
        virtual void takeOwnership(void* obj, void (*deleter)(void*)) {
            (void)obj;
            (void)deleter;
        }

        template<typename T>
        T* ownObject(T* obj) {
            takeOwnership(obj, [](void* p) { delete static_cast<T*>(p); });
            return obj;
        }

        // DensityFunction nodes dominate the mapped tree, so they get their
        // own overridable sink — owners can store them as bare pointers
        // (deleted via the virtual dtor) instead of pointer+deleter pairs.
        virtual DensityFunction* own(DensityFunction* node) { return ownObject(node); }
    };

    /**
     * Core DensityFunction methods
     */

    // Compute density at a single point
    virtual double compute(const FunctionContext& context) const = 0;

    // Fill an array of density values efficiently (for bulk sampling)
    // __restrict tells compiler that output doesn't alias other pointers
    virtual void fillArray(double* __restrict output, int32_t count, ContextProvider& contextProvider) const = 0;

    // Apply a visitor transformation to this function and its children.
    //
    // NOT virtual on purpose. Java's NoiseChunk keeps `wrapped`, a HashMap
    // keyed by the density-function RECORDS, so structurally identical
    // subtrees (the router references continents/erosion/ridges/offset/
    // factor/depth from many places) map to ONE wrapper per chunk. The port's
    // wrap map was keyed by pointer and mapAll creates fresh nodes, so it
    // never hit: every NoiseChunk expanded the shared DAG into a ~7,000-node
    // tree (measured 2026-08-29: 11 ms of the 15 ms biome step). The visitor
    // may memoise original node -> mapped node; the router is one shared DAG,
    // so memoising on the original identity reproduces Java's sharing.
    DensityFunction* mapAll(Visitor& visitor) {
        if (DensityFunction* memo = visitor.lookupMapped(this)) return memo;
        // Only for memoising visitors (NoiseChunk's WrapVisitor): the key pass
        // maps the children and mapAllImpl maps them again, which is a memo
        // hit there and an EXPONENTIAL re-walk for a visitor without one
        // (RandomState's noise wiring hung world load, 2026-08-29).
        if (!visitor.memoises()) return mapAllImpl(visitor);
        // Key of the node this WOULD map to (children mapped, memoised). An
        // equal node already mapped for this visitor is returned as-is —
        // no construction, no allocation: Java's record dedupe, one level up.
        std::string key;
        keyImpl(key, &visitor);
        if (DensityFunction* existing = visitor.lookupPreMapped(key)) {
            visitor.rememberMapped(this, existing);
            return existing;
        }
        DensityFunction* mapped = mapAllImpl(visitor);
        visitor.rememberMapped(this, mapped);
        visitor.rememberPreMapped(key, mapped);
        return mapped;
    }
    virtual DensityFunction* mapAllImpl(Visitor& visitor) = 0;

    // ── Structural identity (Java record semantics) ─────────────────────────
    // Java's density functions are records, so NoiseChunk's `wrapped` HashMap
    // treats two structurally identical subtrees as the SAME key and hands
    // back one wrapper. Ours are plain classes; keyImpl appends a byte string
    // that plays the role of record equals/hashCode: a type tag, the value
    // fields, and the CHILD POINTERS. With `v == nullptr` the children are
    // this node's own (key of an already-mapped node, used by
    // NoiseChunk::wrap); with a visitor the children are MAPPED first
    // (memoised), which is the key the mapped node WOULD have — so mapAll can
    // find an existing equal node and skip constructing a duplicate at all.
    //
    // Default: identity. Right for anything stateful or unique (NoiseChunk's
    // own cache wrappers, the Beardifier, BlendAlpha/Offset singletons).
    virtual void keyImpl(std::string& out, Visitor* v) const {
        (void)v;
        keyTag(out, 0);
        keyPtr(out, this);
    }
    void structuralKey(std::string& out) const { keyImpl(out, nullptr); }
    void mappedKey(std::string& out, Visitor& v) const { keyImpl(out, &v); }

    static void keyTag(std::string& out, uint16_t tag) {
        out.append(reinterpret_cast<const char*>(&tag), sizeof tag);
    }
    static void keyPtr(std::string& out, const void* p) {
        const uintptr_t val = reinterpret_cast<uintptr_t>(p);
        out.append(reinterpret_cast<const char*>(&val), sizeof val);
    }
    static void keyDouble(std::string& out, double d) {
        out.append(reinterpret_cast<const char*>(&d), sizeof d);   // bit pattern
    }
    static void keyFloat(std::string& out, float f) {
        out.append(reinterpret_cast<const char*>(&f), sizeof f);
    }
    static void keyInt(std::string& out, int64_t i) {
        out.append(reinterpret_cast<const char*>(&i), sizeof i);
    }
    // A child: its mapped identity under `v`, or itself when keying a node
    // that is already mapped.
    static DensityFunction* keyChild(Visitor* v, DensityFunction* child) {
        return (v && child) ? child->mapAll(*v) : child;
    }
    static void keyNoise(std::string& out, Visitor* v, NoiseHolder* n) {
        NoiseHolder* h = (v && n) ? v->visitNoise(n) : n;
        keyPtr(out, h ? static_cast<const void*>(h->noise()) : nullptr);
        if (h && !h->noise() && h->noiseName()) out.append(h->noiseName());
    }

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
    DensityFunction* mapAllImpl(Visitor& visitor) override {
        return visitor.apply(this);
    }
};

} // namespace density
} // namespace minecraft
