#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Render {

    struct Vertex {
        glm::vec3 pos;    // World‐space position of the vertex
        glm::vec3 nrm;    // Face normal
        glm::vec2 uv;     // Texture UV (0..1)
        uint8_t   ao;     // Ambient occlusion (0..255). For MVP, set to 255.
    };

} // namespace Render
