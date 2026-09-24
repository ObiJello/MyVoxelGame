#include "levelgen/density/SamplerContext.h"
#include "levelgen/density/JavaMath.h"
#include "core/BlockPos.h"

#include <cmath>
#include <limits>

// Reference: densityfunction.SamplerContext (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {
constexpr int CACHE_SIZE_STEP = 16;
}

SamplerContext::CacheCell::CacheCell() : value(std::numeric_limits<float>::quiet_NaN()) {}

SamplerContext::~SamplerContext() {
    // Cells hand their buffers back to the arena first (they may be pooled).
    m_cacheCells.clear();
}

SamplerContext& SamplerContext::emptyUncached() {
    static SamplerContext context(ContextMap(), DensityBufferArena::global(), false);
    return context;
}

SamplerContext::CacheCell* SamplerContext::getCacheCell(int cacheId) {
    if (!m_cachesEnabled) return nullptr;
    const int oldSize = static_cast<int>(m_cacheCells.size());
    if (cacheId >= oldSize) {
        const int newSize = jmath::roundToward(cacheId + 1, CACHE_SIZE_STEP);
        m_cacheCells.reserve(static_cast<size_t>(newSize));
        for (int i = oldSize; i < newSize; ++i) {
            m_cacheCells.push_back(std::make_unique<CacheCell>());
        }
    }
    return m_cacheCells[static_cast<size_t>(cacheId)].get();
}

void SamplerContext::sampleVolumeCached(int cacheId, const DensitySampler& input,
                                        DensityBuffer& outputBuffer, const DensityVolume& volume) {
    CacheCell* cell = getCacheCell(cacheId);
    if (cell == nullptr) {
        input.sampleVolume(*this, outputBuffer, volume);
        return;
    }
    if (!cell->buffer || !cell->hasVolume || !(volume == cell->volume)) {
        cell->buffer.reset();
        cell->volume = volume;
        cell->hasVolume = true;
        cell->buffer = acquireBuffer(volume);
        input.sampleVolume(*this, *cell->buffer, volume);
    }
    outputBuffer.copyFrom(*cell->buffer);
}

float SamplerContext::sampleValueCached(int cacheId, const DensitySampler& input,
                                        int blockX, int blockY, int blockZ) {
    CacheCell* cell = getCacheCell(cacheId);
    if (cell == nullptr) {
        return input.sampleValue(*this, blockX, blockY, blockZ);
    }
    const int64_t cacheKey = core::BlockPos::asLong(blockX, blockY, blockZ);
    if (cell->valueKey == cacheKey && !std::isnan(cell->value)) {
        return cell->value;
    }
    if (cell->buffer && cell->hasVolume) {
        const int index = cell->volume.indexOfBlock(blockX, blockY, blockZ);
        if (index != DensityVolume::NO_BLOCK) {
            return cell->buffer->get(index);
        }
    }
    const float value = input.sampleValue(*this, blockX, blockY, blockZ);
    cell->valueKey = cacheKey;
    cell->value = value;
    return value;
}

ScopedBuffer BoundSampler::sampleVolume(const DensityVolume& volume) const {
    ScopedBuffer buffer = context->acquireBuffer(volume);
    sampler->sampleVolume(*context, *buffer, volume);
    return buffer;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
