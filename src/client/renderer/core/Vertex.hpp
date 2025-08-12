// File: src/client/renderer/core/Vertex.hpp
#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace Render {

    struct Vertex {
        glm::vec3 pos;    // World‐space position of the vertex
        glm::vec3 nrm;    // Face normal
        glm::vec2 uv;     // Texture UV (0..1)
        glm::vec4 color;  // Vertex color/tint (RGBA, 0..1) - NEW for biome tinting
        uint8_t   ao;     // Ambient occlusion (0..255). For MVP, set to 255.

        // Default constructor with white color
        Vertex() : pos(0.0f), nrm(0.0f, 1.0f, 0.0f), uv(0.0f), color(1.0f), ao(255) {}

        // Constructor with parameters
        Vertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord,
               const glm::vec4& vertexColor = glm::vec4(1.0f), uint8_t ambientOcclusion = 255)
            : pos(position), nrm(normal), uv(texCoord), color(vertexColor), ao(ambientOcclusion) {}
    };

} // namespace Render