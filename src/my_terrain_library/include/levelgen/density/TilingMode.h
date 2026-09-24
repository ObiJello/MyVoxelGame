#pragma once

#include <stdexcept>

// Reference: densityfunction.TilingMode (26.3).

namespace minecraft {
namespace levelgen {
namespace density {

enum class TilingMode {
    CLAMP_TO_EDGE,
    REPEAT,
    MIRRORED_REPEAT,
};

inline const char* serializedName(TilingMode mode) {
    switch (mode) {
        case TilingMode::CLAMP_TO_EDGE: return "clamp_to_edge";
        case TilingMode::REPEAT: return "repeat";
        case TilingMode::MIRRORED_REPEAT: return "mirrored_repeat";
    }
    throw std::logic_error("TilingMode: bad value");
}

} // namespace density
} // namespace levelgen
} // namespace minecraft
