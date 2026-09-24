#include "levelgen/density/generator/DistanceToPointFunction.h"

#include <limits>

// Reference: generator.DistanceToPointFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

class Sampler final : public DensitySampler {
public:
    Sampler(int x, int y, int z, DistanceMetric metric) : m_x(x), m_y(y), m_z(z), m_metric(metric) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        int index = 0;
        for (int z = 0; z < volume.sizeZ; ++z) {
            const int blockZ = volume.blockZ(z);
            for (int x = 0; x < volume.sizeX; ++x) {
                const int blockX = volume.blockX(x);
                for (int y = 0; y < volume.sizeY; ++y) {
                    const int blockY = volume.blockY(y);
                    outputBuffer.set(index++, sampleValue(context, blockX, blockY, blockZ));
                }
            }
        }
    }

    float sampleValue(SamplerContext&, int blockX, int blockY, int blockZ) const override {
        return compute(m_metric, static_cast<float>(m_x - blockX), static_cast<float>(m_y - blockY),
                       static_cast<float>(m_z - blockZ));
    }

private:
    int m_x;
    int m_y;
    int m_z;
    DistanceMetric m_metric;
};

} // namespace

DensitySamplerPtr DistanceToPointFunction::compileSampler(CompileContext&) const {
    return std::make_shared<Sampler>(m_point.getX(), m_point.getY(), m_point.getZ(), m_metric);
}

Interval DistanceToPointFunction::range() const {
    return Interval::of(0.0f, std::numeric_limits<float>::infinity());
}

bool DistanceToPointFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const DistanceToPointFunction*>(&other);
    return o != nullptr && m_point == o->m_point && m_metric == o->m_metric;
}

size_t DistanceToPointFunction::hash() const {
    size_t h = hashCombine(0x22, static_cast<size_t>(static_cast<uint32_t>(m_point.hashCode())));
    return hashCombine(h, static_cast<size_t>(m_metric));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
