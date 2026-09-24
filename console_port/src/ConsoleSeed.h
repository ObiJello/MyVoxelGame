#pragma once
#include "Random.h"
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <cstdint>
namespace console {
// Empty input requests the original biome-based search.
std::optional<std::int64_t> parseConsoleSeed(std::u16string_view text);
std::array<float,23> consoleBiomeFractions(std::span<const std::uint8_t> biomes);
bool consoleSeedMatches(std::array<float,23> fractions);
class ConsoleSeedSearch {
    Random random_;
public:
    explicit ConsoleSeedSearch(std::int64_t entropy):random_(entropy){}
    // One original candidate per step lets the host service input between candidates.
    std::optional<std::int64_t> step();
};
}
