#include "levelgen/density/generator/EndIslandFunction.h"

#include "levelgen/density/JavaMath.h"
#include "levelgen/density/synth/SimplexNoise.h"

#include <cmath>
#include <cstdint>
#include <memory>

// Reference: generator.EndIslandFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

float EndIslandFunction::getHeightValue(const synth::SimplexNoise& islandNoise, int sectionX, int sectionZ) {
    const int chunkX = sectionX / 2;
    const int chunkZ = sectionZ / 2;
    const int subSectionX = sectionX % 2;
    const int subSectionZ = sectionZ % 2;
    float doffs = -100.0f;

    for (int xo = -12; xo <= 12; ++xo) {
        for (int zo = -12; zo <= 12; ++zo) {
            const int64_t totalChunkX = static_cast<int64_t>(chunkX + xo);
            const int64_t totalChunkZ = static_cast<int64_t>(chunkZ + zo);
            if (totalChunkX * totalChunkX + totalChunkZ * totalChunkZ > 4096LL &&
                islandNoise.get(static_cast<double>(totalChunkX), static_cast<double>(totalChunkZ)) < -0.9f) {
                const float islandSize = std::fmod(std::fabs(static_cast<float>(totalChunkX)) * 3439.0f +
                                                       std::fabs(static_cast<float>(totalChunkZ)) * 147.0f,
                                                   13.0f) + 9.0f;
                const float xd = static_cast<float>(subSectionX - xo * 2);
                const float zd = static_cast<float>(subSectionZ - zo * 2);
                float newDoffs = 100.0f - jmath::sqrt(xd * xd + zd * zd) * islandSize;
                newDoffs = jmath::clamp(newDoffs, -100.0f, 80.0f);
                doffs = jmath::fmax(doffs, newDoffs);
            }
        }
    }

    return doffs;
}

namespace {

class Sampler final : public DensitySampler {
public:
    explicit Sampler(std::shared_ptr<const synth::SimplexNoise> islandNoise) : m_islandNoise(std::move(islandNoise)) {}

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        for (int z = 0; z < volume.sizeZ; ++z) {
            const int blockZ = volume.blockZ(z);
            for (int x = 0; x < volume.sizeX; ++x) {
                const int blockX = volume.blockX(x);
                const float value = sampleValue(context, blockX, 0, blockZ);
                const int index = volume.indexUnchecked(x, 0, z);
                outputBuffer.setRange(index, volume.sizeY, value);
            }
        }
    }

    float sampleValue(SamplerContext&, int blockX, int, int blockZ) const override {
        return (EndIslandFunction::getHeightValue(*m_islandNoise, blockX / 8, blockZ / 8) - 8.0f) / 128.0f;
    }

private:
    std::shared_ptr<const synth::SimplexNoise> m_islandNoise;
};

} // namespace

DensitySamplerPtr EndIslandFunction::compileSampler(CompileContext& context) const {
    random::AnyRandomSource islandRandom = context.createEndIslandRandom();
    islandRandom.consumeCount(17292);
    auto islandNoise = std::make_shared<const synth::SimplexNoise>(islandRandom, true);
    return std::make_shared<Sampler>(std::move(islandNoise));
}

bool EndIslandFunction::equals(const DensityFunction& other) const {
    return dynamic_cast<const EndIslandFunction*>(&other) != nullptr;
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
