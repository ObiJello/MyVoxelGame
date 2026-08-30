#pragma once

#include "random/LegacyRandomSource.h"
#include "random/XoroshiroRandomSource.h"

#include <string>

// Reference: PositionalRandomFactory.java - an interface implemented by both
// XoroshiroRandomSource.XoroshiroPositionalRandomFactory and
// LegacyRandomSource.LegacyPositionalRandomFactory. The C++ factories return
// VALUE sources, so the dimension-agnostic RandomState fields (C2,
// legacy_random_source support) use this tagged wrapper instead of virtual
// dispatch. The Xoroshiro path routes to the exact same objects and math as
// before, keeping the overworld bit-identical.

namespace minecraft {
namespace random {

class AnyRandomSource {
public:
    explicit AnyRandomSource(XoroshiroRandomSource source)
        : m_legacy(false), m_xoroshiro(source), m_legacySource(0) {}
    explicit AnyRandomSource(LegacyRandomSource source)
        : m_legacy(true), m_xoroshiro(0), m_legacySource(source) {}

    int32_t nextInt(int32_t bound) {
        return m_legacy ? m_legacySource.nextInt(bound) : m_xoroshiro.nextInt(bound);
    }
    int64_t nextLong() {
        return m_legacy ? m_legacySource.nextLong() : m_xoroshiro.nextLong();
    }
    float nextFloat() {
        return m_legacy ? m_legacySource.nextFloat() : m_xoroshiro.nextFloat();
    }
    double nextDouble() {
        return m_legacy ? m_legacySource.nextDouble() : m_xoroshiro.nextDouble();
    }
    bool nextBoolean() {
        return m_legacy ? m_legacySource.nextBoolean() : m_xoroshiro.nextBoolean();
    }

    bool isLegacy() const { return m_legacy; }
    XoroshiroRandomSource& xoroshiro() { return m_xoroshiro; }
    LegacyRandomSource& legacy() { return m_legacySource; }

private:
    bool m_legacy;
    XoroshiroRandomSource m_xoroshiro;
    LegacyRandomSource m_legacySource;
};

class AnyPositionalRandomFactory {
public:
    explicit AnyPositionalRandomFactory(XoroshiroPositionalRandomFactory factory)
        : m_legacy(false), m_xoroshiro(factory), m_legacyFactory(0) {}
    explicit AnyPositionalRandomFactory(LegacyPositionalRandomFactory factory)
        : m_legacy(true), m_xoroshiro(0, 0), m_legacyFactory(factory) {}

    AnyRandomSource at(int32_t x, int32_t y, int32_t z) const {
        return m_legacy ? AnyRandomSource(m_legacyFactory.at(x, y, z))
                        : AnyRandomSource(m_xoroshiro.at(x, y, z));
    }
    AnyRandomSource fromHashOf(const std::string& name) const {
        return m_legacy ? AnyRandomSource(m_legacyFactory.fromHashOf(name))
                        : AnyRandomSource(m_xoroshiro.fromHashOf(name));
    }

    bool isLegacy() const { return m_legacy; }
    XoroshiroPositionalRandomFactory& xoroshiro() { return m_xoroshiro; }
    const LegacyPositionalRandomFactory& legacy() const { return m_legacyFactory; }

private:
    bool m_legacy;
    mutable XoroshiroPositionalRandomFactory m_xoroshiro;
    LegacyPositionalRandomFactory m_legacyFactory;
};

} // namespace random
} // namespace minecraft
