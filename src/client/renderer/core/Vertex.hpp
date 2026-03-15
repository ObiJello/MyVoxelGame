// File: src/client/renderer/core/Vertex.hpp
#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <algorithm>

namespace Render {

    // Compact block vertex: 24 bytes (down from 48).
    // Normals removed (all lighting baked into vertex color during meshing).
    // Color packed as RGBA8 (4 bytes) instead of vec4 float (16 bytes).
    struct Vertex {
        glm::vec3 pos;        // 12 bytes — world-space position
        glm::vec2 uv;         // 8 bytes  — texture atlas UV
        uint32_t packedColor;  // 4 bytes  — RGBA8 (R in low byte, A in high byte)
        // Total: 24 bytes

        Vertex() : pos(0.0f), uv(0.0f), packedColor(0xFFFFFFFF) {}

        Vertex(const glm::vec3& position, const glm::vec2& texCoord, const glm::vec4& color)
            : pos(position), uv(texCoord) {
            SetColor(color);
        }

        // Legacy constructor: accepts and ignores the normal parameter so existing
        // call sites (Mesher, FluidMeshBuilder) compile without changes.
        Vertex(const glm::vec3& position, const glm::vec3& /*normal*/,
               const glm::vec2& texCoord, const glm::vec4& vertexColor = glm::vec4(1.0f))
            : pos(position), uv(texCoord) {
            SetColor(vertexColor);
        }

        void SetColor(const glm::vec4& c) {
            auto toByte = [](float f) -> uint8_t {
                return static_cast<uint8_t>(glm::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            packedColor = toByte(c.r) | (toByte(c.g) << 8) | (toByte(c.b) << 16) | (toByte(c.a) << 24);
        }

        glm::vec4 GetColor() const {
            return glm::vec4(
                (packedColor & 0xFF) / 255.0f,
                ((packedColor >> 8) & 0xFF) / 255.0f,
                ((packedColor >> 16) & 0xFF) / 255.0f,
                ((packedColor >> 24) & 0xFF) / 255.0f
            );
        }
    };

} // namespace Render
