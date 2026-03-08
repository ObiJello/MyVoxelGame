// File: src/client/renderer/core/Vertex.hpp
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Render {

    struct Vertex {
        glm::vec3 pos;    // World-space position of the vertex
        glm::vec3 nrm;    // Face normal
        glm::vec2 uv;     // Texture UV (0..1)
        glm::vec4 color;  // Vertex color (tint * AO * directional shade baked in)

        Vertex() : pos(0.0f), nrm(0.0f, 1.0f, 0.0f), uv(0.0f), color(1.0f) {}

        Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord,
               const glm::vec4& vertexColor = glm::vec4(1.0f))
            : pos(position), nrm(normal), uv(texCoord), color(vertexColor) {}
    };

} // namespace Render