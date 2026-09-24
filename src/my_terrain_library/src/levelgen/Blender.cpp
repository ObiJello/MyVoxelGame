#include "levelgen/Blender.h"

namespace minecraft {

Blender* Blender::empty() {
    static Blender instance;
    return &instance;
}

} // namespace minecraft
