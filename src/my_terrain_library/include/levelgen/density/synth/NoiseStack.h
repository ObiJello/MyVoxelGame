#pragma once

#include "levelgen/density/synth/Noise.h"

#include <memory>
#include <vector>

// Reference: synth.NoiseStack (26.3) - a sum of noise layers, each sampled at
// its own frequency and scaled by its own amplitude. The Perlin and
// SmearedPerlin subclasses Java builds for all-Perlin / all-smeared stacks
// compute exactly what the base class does (they exist for the JIT); they are
// kept so the class structure matches.

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

class NoiseStack;
using NoiseStackPtr = std::shared_ptr<const NoiseStack>;

class NoiseStack : public Noise {
public:
    // NoiseStack.Layer (record).
    struct Layer {
        NoisePtr noise;
        double frequency;
        float amplitude;
    };

    class Builder {
    public:
        Builder& add(NoisePtr noise, double frequency, float amplitude);
        Builder& addStack(const NoiseStack& stack, double frequency, float amplitude);
        NoiseStackPtr build() const;

    private:
        friend class NoiseStack;
        Builder() = default;
        std::vector<Layer> m_layers;
    };

    static Builder builder() { return Builder(); }

    Interval range() const override { return m_range; }
    float get(double x, double y, double z) const override;
    float get(double x, double y) const override;
    void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                     double xzScale, double yScale, float amplitude) const override;

    // @VisibleForTesting
    const NoisePtr& getLayer(int index) const { return m_layers[static_cast<size_t>(index)].noise; }
    const std::vector<Layer>& layers() const { return m_layers; }

protected:
    explicit NoiseStack(std::vector<Layer> layers);

    std::vector<Layer> m_layers;

private:
    class SmearedPerlin;
    class Perlin;

    Interval m_range;
};

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
