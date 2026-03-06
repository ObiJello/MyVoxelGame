#include "core/SectionPos.h"
#include "math/Mth.h"

namespace minecraft {
namespace core {

// Convert double coordinate to section coordinate (SectionPos.java lines 84-86)
int32_t SectionPos::blockToSectionCoord(double coord) {
    return Mth::floor(coord) >> 4;
}

} // namespace core
} // namespace minecraft
