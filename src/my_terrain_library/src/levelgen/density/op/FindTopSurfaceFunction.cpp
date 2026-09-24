#include "levelgen/density/op/FindTopSurfaceFunction.h"

#include "levelgen/density/CoreFunctions.h"
#include "levelgen/density/JavaMath.h"
#include "levelgen/density/SamplerContext.h"

#include <stdexcept>
#include <string>

// Reference: op.FindTopSurfaceFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// FindTopSurfaceFunction.Sampler. Only sampleable with sizeY == 1; the
// function wraps it in a Y slice at 0.
class FindTopSurfaceSampler final : public DensitySampler {
public:
    FindTopSurfaceSampler(DensitySamplerPtr density, DensitySamplerPtr upperBound, int lowerBound, int cellHeight)
        : m_density(std::move(density)), m_upperBound(std::move(upperBound)), m_lowerBound(lowerBound),
          m_cellHeight(cellHeight) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        if (volume.sizeY != 1) {
            throw std::invalid_argument("Cannot sample with sizeY=" + std::to_string(volume.sizeY));
        }
        m_upperBound->sampleVolume(context, outputBuffer, volume);
        int index = 0;
        for (int z = 0; z < volume.sizeZ; ++z) {
            const int blockZ = volume.blockZ(z);
            for (int x = 0; x < volume.sizeX; ++x) {
                const int blockX = volume.blockX(x);
                const float upperBound = outputBuffer.get(index);
                outputBuffer.set(index, static_cast<float>(findSurfaceFrom(context, blockX, blockZ, upperBound)));
                ++index;
            }
        }
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        const float upperBound = m_upperBound->sampleValue(context, blockX, blockY, blockZ);
        return static_cast<float>(findSurfaceFrom(context, blockX, blockZ, upperBound));
    }

private:
    int findSurfaceFrom(SamplerContext& context, int x, int z, float upperBound) const {
        const int topY = jmath::floor(upperBound / static_cast<float>(m_cellHeight)) * m_cellHeight;
        if (topY <= m_lowerBound) {
            return m_lowerBound;
        }
        for (int probeY = topY; probeY >= m_lowerBound; probeY -= m_cellHeight) {
            if (m_density->sampleValue(context, x, probeY, z) > 0.0f) {
                return probeY;
            }
        }
        return m_lowerBound;
    }

    DensitySamplerPtr m_density;
    DensitySamplerPtr m_upperBound;
    int m_lowerBound;
    int m_cellHeight;
};

} // namespace

DensitySamplerPtr FindTopSurfaceFunction::compileSampler(CompileContext& context) const {
    // Java compiles the constructor arguments left to right.
    DensitySamplerPtr density = m_density->compileSampler(context);
    DensitySamplerPtr upperBound = m_upperBound->compileSampler(context);
    auto sampler = std::make_shared<FindTopSurfaceSampler>(density, upperBound, m_lowerBound, m_cellHeight);
    return SliceFunction::ySampler(sampler, 0);
}

DensityFunctionPtr FindTopSurfaceFunction::rewriteChildren(const DfRewriteRule& rule) const {
    DensityFunctionPtr density = rule.rewrite(m_density);
    DensityFunctionPtr upperBound = rule.rewrite(m_upperBound);
    return density == m_density && upperBound == m_upperBound
        ? self()
        : std::make_shared<FindTopSurfaceFunction>(density, upperBound, m_lowerBound, m_cellHeight);
}

Interval FindTopSurfaceFunction::range() const {
    return Interval::of(static_cast<float>(m_lowerBound),
                        jmath::fmax(static_cast<float>(m_lowerBound), m_upperBound->range().max()));
}

int FindTopSurfaceFunction::domainAxes() const {
    return (m_density->domainAxes() | m_upperBound->domainAxes()) & -3;
}

bool FindTopSurfaceFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const FindTopSurfaceFunction*>(&other);
    return o != nullptr && functionsEqual(m_density, o->m_density) && functionsEqual(m_upperBound, o->m_upperBound) &&
           m_lowerBound == o->m_lowerBound && m_cellHeight == o->m_cellHeight;
}

size_t FindTopSurfaceFunction::hash() const {
    size_t h = hashCombine(0x35, m_density->hash());
    h = hashCombine(h, m_upperBound->hash());
    h = hashCombine(h, std::hash<int>()(m_lowerBound));
    return hashCombine(h, std::hash<int>()(m_cellHeight));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
