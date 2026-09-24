#pragma once

#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/DensitySampler.h"
#include "levelgen/density/DensityVolume.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Reference: densityfunction.SamplerContext (26.3) and util.context
// ContextKey/ContextMap - the per-use state a compiled sampler runs in: user
// fields (the beardifier, the blender), the buffer arena, and the cache cells
// behind every `cache` function. A cache cell keeps the last VOLUME it filled
// and answers a later point query inside it from that volume — which is why a
// point value can carry the volume path's rounding, and why the caching must
// match Java exactly.

namespace minecraft {
namespace levelgen {
namespace density {

// A typed key. Keys are compared by identity, like Java's ContextKey.
template <typename T>
class ContextKey {
public:
    explicit ContextKey(const char* name) : m_name(name) {}
    const char* name() const { return m_name; }
private:
    const char* m_name;
};

// Non-owning values keyed by ContextKey identity: whoever builds the context
// (a NoiseChunk) owns the values and outlives it.
class ContextMap {
public:
    template <typename T>
    ContextMap& set(const ContextKey<T>& key, const T* value) {
        m_values[&key] = value;
        return *this;
    }
    template <typename T>
    const T* get(const ContextKey<T>& key) const {
        auto it = m_values.find(&key);
        return it != m_values.end() ? static_cast<const T*>(it->second) : nullptr;
    }
private:
    std::unordered_map<const void*, const void*> m_values;
};

class SamplerContext {
public:
    class Builder {
    public:
        Builder& setUserFields(ContextMap fields) { m_fields = std::move(fields); return *this; }
        Builder& useBufferArena(DensityBufferArena& arena) { m_arena = &arena; return *this; }
        Builder& enableCaches() { m_enableCaches = true; return *this; }
        SamplerContext build() { return SamplerContext(std::move(m_fields), *m_arena, m_enableCaches); }
    private:
        ContextMap m_fields;
        DensityBufferArena* m_arena = &DensityBufferArena::global();
        bool m_enableCaches = false;
    };

    static Builder builder() { return Builder(); }
    // No caches, the global arena: safe to share between threads.
    static SamplerContext& emptyUncached();

    SamplerContext(SamplerContext&&) = default;
    SamplerContext& operator=(SamplerContext&&) = default;
    SamplerContext(const SamplerContext&) = delete;
    SamplerContext& operator=(const SamplerContext&) = delete;
    ~SamplerContext();

    template <typename T>
    const T* getField(const ContextKey<T>& key) const { return m_userFields.get(key); }
    template <typename T>
    const T* getFieldOrDefault(const ContextKey<T>& key, const T* defaultValue) const {
        const T* value = m_userFields.get(key);
        return value != nullptr ? value : defaultValue;
    }

    ScopedBuffer acquireBuffer(const DensityVolume& volume) { return m_arena->acquire(volume.size()); }

    void sampleVolumeCached(int cacheId, const DensitySampler& input, DensityBuffer& outputBuffer,
                            const DensityVolume& volume);
    float sampleValueCached(int cacheId, const DensitySampler& input, int blockX, int blockY, int blockZ);

private:
    SamplerContext(ContextMap fields, DensityBufferArena& arena, bool enableCaches)
        : m_userFields(std::move(fields)), m_arena(&arena), m_cachesEnabled(enableCaches) {}

    struct CacheCell {
        bool hasVolume = false;
        DensityVolume volume;
        ScopedBuffer buffer;
        int64_t valueKey = 0;
        float value;             // NaN = no cached point value (Java's marker)
        CacheCell();
    };
    CacheCell* getCacheCell(int cacheId);

    ContextMap m_userFields;
    DensityBufferArena* m_arena;
    bool m_cachesEnabled;
    // Heap cells: a nested sample may grow this list while an outer frame
    // still holds a cell.
    std::vector<std::unique_ptr<CacheCell>> m_cacheCells;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
