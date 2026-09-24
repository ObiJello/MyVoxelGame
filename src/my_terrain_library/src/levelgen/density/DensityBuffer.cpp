#include "levelgen/density/DensityBuffer.h"
#include "levelgen/density/JavaMath.h"

// Reference: densityfunction.ScopedDensityBuffer / DensityBufferArena /
// DensityBufferPool (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

void ScopedBufferRelease::operator()(ScopedDensityBuffer* buffer) const {
    if (buffer == nullptr || buffer->closed()) return;
    buffer->markClosed();
    buffer->arena()->release(buffer);
}

namespace {
class GlobalArena final : public DensityBufferArena {
public:
    ScopedBuffer acquire(int size) override {
        return ScopedBuffer(new ScopedDensityBuffer(this, size, size));
    }
    void release(ScopedDensityBuffer* buffer) override { delete buffer; }
};
} // namespace

DensityBufferArena& DensityBufferArena::global() {
    static GlobalArena arena;
    return arena;
}

DensityBufferPool::~DensityBufferPool() = default;

ScopedBuffer DensityBufferPool::acquire(int size) {
    const int roundedMinSize = jmath::roundToward(size, BUFFER_SIZE_INCREMENT);
    ScopedDensityBuffer* buffer = tryTakeBest(roundedMinSize, roundedMinSize * 2);
    if (buffer == nullptr) {
        return ScopedBuffer(new ScopedDensityBuffer(this, roundedMinSize, size));
    }
    buffer->restore(size);
    return ScopedBuffer(buffer);
}

ScopedDensityBuffer* DensityBufferPool::tryTakeBest(int minCapacity, int maxCapacity) {
    int bestIndex = -1;
    int bestCapacity = maxCapacity + 1;
    for (int i = static_cast<int>(m_buffers.size()) - 1; i >= 0; --i) {
        const int capacity = m_buffers[static_cast<size_t>(i)]->capacity();
        if (capacity == minCapacity) {
            ScopedDensityBuffer* taken = m_buffers[static_cast<size_t>(i)].release();
            m_buffers.erase(m_buffers.begin() + i);
            return taken;
        }
        if (capacity > minCapacity && capacity < bestCapacity) {
            bestIndex = i;
            bestCapacity = capacity;
        }
    }
    if (bestIndex == -1) return nullptr;
    ScopedDensityBuffer* taken = m_buffers[static_cast<size_t>(bestIndex)].release();
    m_buffers.erase(m_buffers.begin() + bestIndex);
    return taken;
}

void DensityBufferPool::release(ScopedDensityBuffer* buffer) {
    m_buffers.emplace_back(buffer);
}

void DensityBufferPool::garbageCollect() {
    m_buffers.erase(std::remove_if(m_buffers.begin(), m_buffers.end(),
                                   [this](const std::unique_ptr<ScopedDensityBuffer>& buffer) {
                                       return buffer->incrementAge() > m_maxAge;
                                   }),
                    m_buffers.end());
}

void DensityBufferPool::clear() {
    m_buffers.clear();
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
