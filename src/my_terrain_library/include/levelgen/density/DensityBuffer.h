#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

// Reference: densityfunction.DensityBuffer / ScopedDensityBuffer /
// DensityBufferArena / DensityBufferPool (26.3).
//
// A DensityBuffer is the float array a sampler fills for a DensityVolume.
// Scoped buffers come from an arena and go back to it when released; Java
// closes them with try-with-resources, here a ScopedBuffer handle releases on
// destruction. The GLOBAL arena allocates fresh buffers; a pool (one per
// NoiseChunk) reuses them.

namespace minecraft {
namespace levelgen {
namespace density {

class DensityBuffer {
public:
    explicit DensityBuffer(int size) : m_values(static_cast<size_t>(size)), m_size(size) {}
    virtual ~DensityBuffer() = default;

    static std::unique_ptr<DensityBuffer> createUnpooled(int size) {
        return std::make_unique<DensityBuffer>(size);
    }

    void set(int index, float value) { m_values[static_cast<size_t>(index)] = value; }
    void setRange(int index, int size, float value) {
        std::fill(m_values.begin() + index, m_values.begin() + index + size, value);
    }
    void addTo(int index, float value) { m_values[static_cast<size_t>(index)] += value; }
    float get(int index) const { return m_values[static_cast<size_t>(index)]; }
    void fill(float value) { std::fill(m_values.begin(), m_values.begin() + m_size, value); }
    void copyFrom(const DensityBuffer& other) {
        if (size() != other.size()) {
            throw std::invalid_argument("Cannot copy from a DensityBuffer of another size");
        }
        std::copy(other.m_values.begin(), other.m_values.begin() + m_size, m_values.begin());
    }
    int capacity() const { return static_cast<int>(m_values.size()); }
    int size() const { return m_size; }
    float* data() { return m_values.data(); }
    const float* data() const { return m_values.data(); }

protected:
    std::vector<float> m_values;
    int m_size;
};

class DensityBufferArena;

class ScopedDensityBuffer : public DensityBuffer {
public:
    ScopedDensityBuffer(DensityBufferArena* arena, int capacity, int size)
        : DensityBuffer(capacity), m_arena(arena) {
        m_size = size;
    }

    void restore(int size) {
        if (size > static_cast<int>(m_values.size())) {
            throw std::invalid_argument("Cannot set size larger than buffer capacity");
        }
        if (!m_closed) {
            throw std::invalid_argument("Buffer is already in use");
        }
        m_age = 0;
        m_closed = false;
        m_size = size;
    }
    int incrementAge() { return ++m_age; }
    DensityBufferArena* arena() const { return m_arena; }
    void markClosed() { m_closed = true; }
    bool closed() const { return m_closed; }

private:
    DensityBufferArena* m_arena;
    int m_age = 0;
    bool m_closed = false;
};

// Releases the buffer to its arena when destroyed (Java close()).
struct ScopedBufferRelease {
    void operator()(ScopedDensityBuffer* buffer) const;
};
using ScopedBuffer = std::unique_ptr<ScopedDensityBuffer, ScopedBufferRelease>;

class DensityBufferArena {
public:
    virtual ~DensityBufferArena() = default;
    virtual ScopedBuffer acquire(int size) = 0;
    // Takes the buffer back (ownership included).
    virtual void release(ScopedDensityBuffer* buffer) = 0;

    // Allocates a fresh buffer per acquire and frees it on release.
    static DensityBufferArena& global();
};

class DensityBufferPool final : public DensityBufferArena {
public:
    explicit DensityBufferPool(int maxAge) : m_maxAge(maxAge) {}
    ~DensityBufferPool() override;

    ScopedBuffer acquire(int size) override;
    void release(ScopedDensityBuffer* buffer) override;
    void garbageCollect();
    void clear();
    bool isEmpty() const { return m_buffers.empty(); }
    int size() const { return static_cast<int>(m_buffers.size()); }

private:
    ScopedDensityBuffer* tryTakeBest(int minCapacity, int maxCapacity);

    static constexpr int BUFFER_SIZE_INCREMENT = 16;
    int m_maxAge;
    std::vector<std::unique_ptr<ScopedDensityBuffer>> m_buffers;
};

} // namespace density
} // namespace levelgen
} // namespace minecraft
