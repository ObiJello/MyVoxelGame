#include "levelgen/density/synth/NoiseStack.h"

#include "levelgen/density/synth/PerlinNoise.h"
#include "levelgen/density/synth/SmearedPerlinNoise.h"

#include <algorithm>
#include <typeinfo>

// Reference: synth.NoiseStack (26.3).

namespace minecraft {
namespace levelgen {
namespace density {
namespace synth {

NoiseStack::NoiseStack(std::vector<Layer> layers)
    : m_layers(std::move(layers)), m_range(Interval::ofExact(0.0f)) {
    Interval range = Interval::ofExact(0.0f);
    for (const Layer& layer : m_layers) {
        const Interval layerRange = Interval::mul(layer.noise->range(), Interval::ofExact(layer.amplitude));
        range = Interval::add(range, layerRange);
    }
    m_range = range;
}

float NoiseStack::get(double x, double y, double z) const {
    float value = 0.0f;
    for (const Layer& layer : m_layers) {
        const double frequency = layer.frequency;
        value += layer.amplitude * layer.noise->get(x * frequency, y * frequency, z * frequency);
    }
    return value;
}

float NoiseStack::get(double x, double y) const {
    float value = 0.0f;
    for (const Layer& layer : m_layers) {
        const double frequency = layer.frequency;
        value += layer.amplitude * layer.noise->get(x * frequency, y * frequency);
    }
    return value;
}

void NoiseStack::addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                             double xzScale, double yScale, float amplitude) const {
    for (const Layer& layer : m_layers) {
        const double frequency = layer.frequency;
        layer.noise->addToVolume(buffer, volume, xzScale * frequency, yScale * frequency, amplitude * layer.amplitude);
    }
}

// NoiseStack.SmearedPerlin: same sums as the base class.
class NoiseStack::SmearedPerlin final : public NoiseStack {
public:
    explicit SmearedPerlin(std::vector<Layer> layers) : NoiseStack(std::move(layers)) {}

    using NoiseStack::get;
    float get(double x, double y, double z) const override {
        float value = 0.0f;
        for (const Layer& layer : m_layers) {
            const double frequency = layer.frequency;
            value += layer.amplitude * layer.noise->get(x * frequency, y * frequency, z * frequency);
        }
        return value;
    }

    void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                     double xzScale, double yScale, float amplitude) const override {
        for (const Layer& layer : m_layers) {
            const double frequency = layer.frequency;
            layer.noise->addToVolume(buffer, volume, xzScale * frequency, yScale * frequency, amplitude * layer.amplitude);
        }
    }
};

// NoiseStack.Perlin: same sums as the base class.
class NoiseStack::Perlin final : public NoiseStack {
public:
    explicit Perlin(std::vector<Layer> layers) : NoiseStack(std::move(layers)) {}

    using NoiseStack::get;
    float get(double x, double y, double z) const override {
        float value = 0.0f;
        for (const Layer& layer : m_layers) {
            const double frequency = layer.frequency;
            value += layer.amplitude * layer.noise->get(x * frequency, y * frequency, z * frequency);
        }
        return value;
    }

    void addToVolume(DensityBuffer& buffer, const DensityVolume& volume,
                     double xzScale, double yScale, float amplitude) const override {
        for (const Layer& layer : m_layers) {
            const double frequency = layer.frequency;
            layer.noise->addToVolume(buffer, volume, xzScale * frequency, yScale * frequency, amplitude * layer.amplitude);
        }
    }
};

NoiseStack::Builder& NoiseStack::Builder::add(NoisePtr noise, double frequency, float amplitude) {
    m_layers.push_back(Layer{std::move(noise), frequency, amplitude});
    return *this;
}

NoiseStack::Builder& NoiseStack::Builder::addStack(const NoiseStack& stack, double frequency, float amplitude) {
    for (const Layer& layer : stack.m_layers) {
        m_layers.push_back(Layer{layer.noise, layer.frequency * frequency, layer.amplitude * amplitude});
    }
    return *this;
}

NoiseStackPtr NoiseStack::Builder::build() const {
    // layer.noise.getClass() == PerlinNoise.class: the exact class, not a subclass.
    const bool allPerlin = std::all_of(m_layers.begin(), m_layers.end(), [](const Layer& layer) {
        const Noise& noise = *layer.noise;
        return typeid(noise) == typeid(PerlinNoise);
    });
    if (allPerlin) {
        return std::make_shared<const Perlin>(m_layers);
    }
    const bool allSmeared = std::all_of(m_layers.begin(), m_layers.end(), [](const Layer& layer) {
        const Noise& noise = *layer.noise;
        return typeid(noise) == typeid(SmearedPerlinNoise);
    });
    if (allSmeared) {
        return std::make_shared<const SmearedPerlin>(m_layers);
    }
    return std::shared_ptr<const NoiseStack>(new NoiseStack(m_layers));
}

} // namespace synth
} // namespace density
} // namespace levelgen
} // namespace minecraft
