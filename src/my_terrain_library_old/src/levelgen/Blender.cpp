#include "levelgen/Blender.h"

namespace minecraft {

// Static empty blender instance
Blender* Blender::s_emptyBlender = nullptr;

Blender* Blender::empty() {
    if (s_emptyBlender == nullptr) {
        s_emptyBlender = new Blender();
    }
    return s_emptyBlender;
}

} // namespace minecraft
