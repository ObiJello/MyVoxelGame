#include "levelgen/density/generator/GradientFunction.h"

#include "levelgen/density/JavaMath.h"

#include <algorithm>
#include <stdexcept>

// Reference: generator.GradientFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

// Direction.Axis.choose(x, y, z).
int choose(Axis axis, int x, int y, int z) {
    switch (axis) {
        case Axis::X: return x;
        case Axis::Y: return y;
        case Axis::Z: return z;
    }
    throw std::logic_error("GradientFunction: bad axis");
}

// GradientFunction.GradientSampler.
class GradientSampler : public DensitySampler {
public:
    explicit GradientSampler(Axis axis) : m_axis(axis) {}

    virtual float compute(int coordinate) const = 0;

    void sampleVolume(SamplerContext&, DensityBuffer& output, const DensityVolume& volume) const override {
        switch (m_axis) {
            case Axis::X:
                for (int x = 0; x < volume.sizeX; ++x) {
                    const float value = compute(volume.blockX(x));
                    for (int z = 0; z < volume.sizeZ; ++z) {
                        output.setRange(volume.indexUnchecked(x, 0, z), volume.sizeY, value);
                    }
                }
                return;
            case Axis::Y:
                for (int y = 0; y < volume.sizeY; ++y) {
                    const float value = compute(volume.blockY(y));
                    for (int z = 0; z < volume.sizeZ; ++z) {
                        for (int x = 0; x < volume.sizeX; ++x) {
                            output.set(volume.indexUnchecked(x, y, z), value);
                        }
                    }
                }
                return;
            case Axis::Z:
                for (int z = 0; z < volume.sizeZ; ++z) {
                    const float value = compute(volume.blockZ(z));
                    output.setRange(volume.indexUnchecked(0, 0, z), volume.sizeX * volume.sizeY, value);
                }
                return;
        }
    }

    float sampleValue(SamplerContext&, int blockX, int blockY, int blockZ) const override {
        return compute(choose(m_axis, blockX, blockY, blockZ));
    }

protected:
    Axis m_axis;
};

class ClampedSampler final : public GradientSampler {
public:
    ClampedSampler(Axis axis, int fromCoordinate, int minCoordinate, int maxCoordinate, float fromValue,
                   float coordinateFactor)
        : GradientSampler(axis), m_fromCoordinate(fromCoordinate), m_minCoordinate(minCoordinate),
          m_maxCoordinate(maxCoordinate), m_fromValue(fromValue), m_coordinateFactor(coordinateFactor) {}

    float compute(int coordinate) const override {
        const int relativeCoordinate = jmath::clamp(coordinate, m_minCoordinate, m_maxCoordinate) - m_fromCoordinate;
        return m_fromValue + static_cast<float>(relativeCoordinate) * m_coordinateFactor;
    }

private:
    int m_fromCoordinate;
    int m_minCoordinate;
    int m_maxCoordinate;
    float m_fromValue;
    float m_coordinateFactor;
};

class RepeatSampler final : public GradientSampler {
public:
    RepeatSampler(Axis axis, int fromCoordinate, int coordinateRange, float fromValue, float coordinateFactor)
        : GradientSampler(axis), m_fromCoordinate(fromCoordinate), m_coordinateRange(coordinateRange),
          m_fromValue(fromValue), m_coordinateFactor(coordinateFactor) {}

    float compute(int coordinate) const override {
        const int relativeCoordinate = coordinate - m_fromCoordinate;
        return m_fromValue + static_cast<float>(jmath::floorMod(relativeCoordinate, m_coordinateRange)) * m_coordinateFactor;
    }

private:
    int m_fromCoordinate;
    int m_coordinateRange;
    float m_fromValue;
    float m_coordinateFactor;
};

class MirroredRepeatSampler final : public GradientSampler {
public:
    MirroredRepeatSampler(Axis axis, int fromCoordinate, int coordinateRange, float fromValue, float coordinateFactor)
        : GradientSampler(axis), m_fromCoordinate(fromCoordinate), m_coordinateRange(coordinateRange),
          m_fromValue(fromValue), m_coordinateFactor(coordinateFactor) {}

    float compute(int coordinate) const override {
        const int relativeCoordinate = coordinate - m_fromCoordinate;
        const int tileIndex = jmath::floorDiv(relativeCoordinate, m_coordinateRange);
        const int localCoordinate = relativeCoordinate - tileIndex * m_coordinateRange;
        return (tileIndex & 1) == 0
                   ? m_fromValue + static_cast<float>(localCoordinate) * m_coordinateFactor
                   : m_fromValue + static_cast<float>(m_coordinateRange - localCoordinate) * m_coordinateFactor;
    }

private:
    int m_fromCoordinate;
    int m_coordinateRange;
    float m_fromValue;
    float m_coordinateFactor;
};

} // namespace

DensitySamplerPtr GradientFunction::compileSampler(CompileContext&) const {
    const int coordinateRange = m_toCoordinate - m_fromCoordinate;
    const float coordinateFactor = (m_toValue - m_fromValue) / static_cast<float>(coordinateRange);
    switch (m_tiling) {
        case TilingMode::CLAMP_TO_EDGE: {
            const int minCoordinate = std::min(m_fromCoordinate, m_toCoordinate);
            const int maxCoordinate = std::max(m_fromCoordinate, m_toCoordinate);
            return std::make_shared<ClampedSampler>(m_axis, m_fromCoordinate, minCoordinate, maxCoordinate,
                                                    m_fromValue, coordinateFactor);
        }
        case TilingMode::REPEAT:
            return std::make_shared<RepeatSampler>(m_axis, m_fromCoordinate, coordinateRange, m_fromValue,
                                                   coordinateFactor);
        case TilingMode::MIRRORED_REPEAT:
            return std::make_shared<MirroredRepeatSampler>(m_axis, m_fromCoordinate, coordinateRange, m_fromValue,
                                                           coordinateFactor);
    }
    throw std::logic_error("GradientFunction: bad tiling mode");
}

bool GradientFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const GradientFunction*>(&other);
    return o != nullptr && m_axis == o->m_axis && m_tiling == o->m_tiling &&
           m_fromCoordinate == o->m_fromCoordinate && m_toCoordinate == o->m_toCoordinate &&
           floatEq(m_fromValue, o->m_fromValue) && floatEq(m_toValue, o->m_toValue);
}

size_t GradientFunction::hash() const {
    size_t h = hashCombine(0x24, static_cast<size_t>(m_axis));
    h = hashCombine(h, static_cast<size_t>(m_tiling));
    h = hashCombine(h, std::hash<int>()(m_fromCoordinate));
    h = hashCombine(h, std::hash<int>()(m_toCoordinate));
    h = hashCombine(h, hashFloat(m_fromValue));
    return hashCombine(h, hashFloat(m_toValue));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
