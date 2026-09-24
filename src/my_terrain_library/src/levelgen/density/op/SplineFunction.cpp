#include "levelgen/density/op/SplineFunction.h"

#include "levelgen/density/SamplerContext.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// Reference: op.SplineFunction (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

namespace {

const SplineFunction::Coordinate& asCoordinate(const BoundedFloatFunctionPtr& coordinate) {
    const auto* result = dynamic_cast<const SplineFunction::Coordinate*>(coordinate.get());
    if (result == nullptr) {
        throw std::logic_error("SplineFunction: spline coordinate is not a SplineFunction::Coordinate");
    }
    return *result;
}

// SplineFunction.SplineInput: the C the compiled spline passes to its coordinates.
class SplineInput : public BoundedFloatFunction::Argument {
public:
    virtual float sampleCoordinate(const DensitySampler& coordinateSampler, int coordinateIndex) = 0;
};

// SplineFunction.PointSplineInput: one block; each coordinate sampled at most once.
class PointSplineInput final : public SplineInput {
public:
    PointSplineInput(SamplerContext& context, int blockX, int blockY, int blockZ, int coordinateCount)
        : m_context(context), m_blockX(blockX), m_blockY(blockY), m_blockZ(blockZ) {
        if (coordinateCount > kInlineCount) {
            m_heapValues.resize(static_cast<size_t>(coordinateCount));
            m_cachedValues = m_heapValues.data();
        } else {
            m_cachedValues = m_inlineValues;
        }
        for (int i = 0; i < coordinateCount; ++i) {
            m_cachedValues[i] = std::numeric_limits<float>::quiet_NaN();
        }
    }

    float sampleCoordinate(const DensitySampler& coordinateSampler, int coordinateIndex) override {
        const float cachedValue = m_cachedValues[coordinateIndex];
        if (!std::isnan(cachedValue)) {
            return cachedValue;
        }
        const float value = coordinateSampler.sampleValue(m_context, m_blockX, m_blockY, m_blockZ);
        m_cachedValues[coordinateIndex] = value;
        return value;
    }

private:
    static constexpr int kInlineCount = 16;

    SamplerContext& m_context;
    int m_blockX;
    int m_blockY;
    int m_blockZ;
    float m_inlineValues[kInlineCount];
    std::vector<float> m_heapValues;
    float* m_cachedValues;
};

// SplineFunction.BufferSplineInput: a volume; each coordinate's buffer is
// sampled the first time any cell needs it.
class BufferSplineInput final : public SplineInput {
public:
    BufferSplineInput(SamplerContext& context, const DensityVolume& volume, int coordinateCount)
        : m_context(context), m_volume(volume), m_buffers(static_cast<size_t>(coordinateCount)) {}
    ~BufferSplineInput() override { close(); }

    BufferSplineInput(const BufferSplineInput&) = delete;
    BufferSplineInput& operator=(const BufferSplineInput&) = delete;

    float sampleCoordinate(const DensitySampler& coordinateSampler, int coordinateIndex) override {
        ScopedBuffer& buffer = m_buffers[static_cast<size_t>(coordinateIndex)];
        if (!buffer) {
            buffer = m_context.acquireBuffer(m_volume);
            coordinateSampler.sampleVolume(m_context, *buffer, m_volume);
        }
        return buffer->get(index);
    }

    // Java closes the buffers in index order.
    void close() {
        for (ScopedBuffer& buffer : m_buffers) {
            buffer.reset();
        }
    }

    int index = 0;

private:
    SamplerContext& m_context;
    const DensityVolume& m_volume;
    std::vector<ScopedBuffer> m_buffers;
};

// SplineFunction.SamplerCoordinate.
class SamplerCoordinate final : public BoundedFloatFunction {
public:
    SamplerCoordinate(DensitySamplerPtr sampler, int index, Interval range)
        : m_sampler(std::move(sampler)), m_index(index), m_range(range) {}

    float apply(Argument& input) const override {
        return static_cast<SplineInput&>(input).sampleCoordinate(*m_sampler, m_index);
    }
    Interval range() const override { return m_range; }

private:
    DensitySamplerPtr m_sampler;
    int m_index;
    Interval m_range;
};

// SplineFunction.Sampler.
class SplineSampler final : public DensitySampler {
public:
    SplineSampler(CompileContext& context, const std::shared_ptr<const CubicSpline>& spline) {
        std::unordered_map<DensityFunctionPtr, BoundedFloatFunctionPtr, DensityFunctionHash, DensityFunctionEquals>
            coordinates;
        m_sampler = CubicSpline::asSampler(spline->mapCoordinates(
            [&context, &coordinates](const BoundedFloatFunctionPtr& coordinate) -> BoundedFloatFunctionPtr {
                // coordinates.computeIfAbsent(coordinate.function(), ...)
                const DensityFunctionPtr& function = asCoordinate(coordinate).function();
                auto it = coordinates.find(function);
                if (it != coordinates.end()) {
                    return it->second;
                }
                const int index = static_cast<int>(coordinates.size());
                DensitySamplerPtr sampler = function->compileSampler(context);
                BoundedFloatFunctionPtr samplerCoordinate =
                    std::make_shared<SamplerCoordinate>(sampler, index, function->range());
                coordinates.emplace(function, samplerCoordinate);
                return samplerCoordinate;
            }));
        m_coordinateCount = static_cast<int>(coordinates.size());
    }

    void sampleVolume(SamplerContext& context, DensityBuffer& outputBuffer, const DensityVolume& volume) const override {
        BufferSplineInput input(context, volume, m_coordinateCount);
        for (int i = 0; i < outputBuffer.size(); ++i) {
            input.index = i;
            outputBuffer.set(i, m_sampler->apply(input));
        }
        input.close();
    }

    float sampleValue(SamplerContext& context, int blockX, int blockY, int blockZ) const override {
        PointSplineInput input(context, blockX, blockY, blockZ, m_coordinateCount);
        return m_sampler->apply(input);
    }

private:
    BoundedFloatFunctionPtr m_sampler;
    int m_coordinateCount = 0;
};

} // namespace

// ---- Coordinate ------------------------------------------------------------------

bool SplineFunction::Coordinate::equals(const BoundedFloatFunction& other) const {
    const auto* o = dynamic_cast<const Coordinate*>(&other);
    return o != nullptr && functionsEqual(m_function, o->m_function);
}

// ---- SplineFunction ----------------------------------------------------------------

DensitySamplerPtr SplineFunction::compileSampler(CompileContext& context) const {
    return std::make_shared<SplineSampler>(context, m_spline);
}

int SplineFunction::domainAxes() const {
    int axes = 0;
    m_spline->forEachCoordinate([&axes](const BoundedFloatFunctionPtr& coordinate) {
        axes = axes | asCoordinate(coordinate).function()->domainAxes();
    });
    return axes;
}

DensityFunctionPtr SplineFunction::rewriteChildren(const DfRewriteRule& rule) const {
    bool changed = false;
    std::shared_ptr<const CubicSpline> newSpline = m_spline->mapCoordinates(
        [&rule, &changed](const BoundedFloatFunctionPtr& coordinate) -> BoundedFloatFunctionPtr {
            const DensityFunctionPtr& function = asCoordinate(coordinate).function();
            DensityFunctionPtr newFunction = rule.rewrite(function);
            if (newFunction != function) {
                changed = true;
                return std::make_shared<Coordinate>(newFunction);
            }
            return coordinate;
        });
    return !changed ? self() : std::make_shared<SplineFunction>(newSpline);
}

bool SplineFunction::equals(const DensityFunction& other) const {
    const auto* o = dynamic_cast<const SplineFunction*>(&other);
    return o != nullptr && (m_spline == o->m_spline || m_spline->equals(*o->m_spline));
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
