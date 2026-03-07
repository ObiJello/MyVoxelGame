#pragma once

namespace minecraft {
namespace util {

template<typename T>
class InclusiveRange {
public:
    InclusiveRange(T minInclusive, T maxInclusive)
        : m_minInclusive(minInclusive)
        , m_maxInclusive(maxInclusive) {}

    explicit InclusiveRange(T value)
        : m_minInclusive(value)
        , m_maxInclusive(value) {}

    T minInclusive() const { return m_minInclusive; }
    T maxInclusive() const { return m_maxInclusive; }

private:
    T m_minInclusive;
    T m_maxInclusive;
};

} // namespace util
} // namespace minecraft
