#include "core/Vec3i.h"

namespace minecraft {
namespace core {

// Static ZERO instance (Vec3i.java line 19)
const Vec3i& Vec3i::ZERO() {
    static Vec3i zero(0, 0, 0);
    return zero;
}

} // namespace core
} // namespace minecraft
