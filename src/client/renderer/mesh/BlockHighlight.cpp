// File: src/client/renderer/mesh/BlockHighlight.cpp
#include "BlockHighlight.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    // Global instance
    BlockHighlight g_blockHighlight;

    // Shader sources (used by GL backend's CreateShader from source)
    const char* BlockHighlight::vertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;
uniform vec3 uBlockPos;

void main() {
    vec3 worldPos = aPos + uBlockPos;
    gl_Position = uMVP * vec4(worldPos, 1.0);
}
)";

    const char* BlockHighlight::fragmentShaderSource = R"(
#version 330 core

out vec4 FragColor;

void main() {
    FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

    BlockHighlight::BlockHighlight() = default;

    BlockHighlight::~BlockHighlight() {
        if (g_renderBackend) {
            if (m_mesh != INVALID_MESH)         g_renderBackend->DestroyMesh(m_mesh);
            if (m_vb != INVALID_BUFFER)         g_renderBackend->DestroyBuffer(m_vb);
            if (m_ib != INVALID_BUFFER)         g_renderBackend->DestroyBuffer(m_ib);
            if (m_dummyTexture != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_dummyTexture);
            if (m_shader != INVALID_SHADER)     g_renderBackend->DestroyShader(m_shader);
        }
    }

    bool BlockHighlight::Initialize() {
        Log::Info("Initializing block highlight system");

        if (!g_renderBackend) {
            Log::Error("Block highlight: No render backend available");
            return false;
        }

        // Create shader — try SPIR-V files first (Vulkan), fall back to source (GL)
        m_shader = g_renderBackend->CreateShaderFromFiles("shaders/highlight.vert", "shaders/highlight.frag");
        if (m_shader == INVALID_SHADER) {
            // Fall back to compiling from source (OpenGL path)
            m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Error("Block highlight: Failed to create shader");
            return false;
        }

        // Create 1x1 white dummy texture (Vulkan pipeline layout requires a descriptor set)
        unsigned char white[] = {255, 255, 255, 255};
        m_dummyTexture = g_renderBackend->CreateTexture2D(1, 1, TextureFormat::RGBA8, white);

        // Build wireframe cube geometry using 12-float vertex format
        // Small offset to avoid z-fighting
        const float e = 0.005f;
        const float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f; // black
        // 8 vertices: pos(3) + norm(3) + uv(2) + color(4)
        float verts[] = {
            0-e,0-e,0-e, 0,0,0, 0,0, r,g,b,a,  // 0
            1+e,0-e,0-e, 0,0,0, 0,0, r,g,b,a,  // 1
            1+e,0-e,1+e, 0,0,0, 0,0, r,g,b,a,  // 2
            0-e,0-e,1+e, 0,0,0, 0,0, r,g,b,a,  // 3
            0-e,1+e,0-e, 0,0,0, 0,0, r,g,b,a,  // 4
            1+e,1+e,0-e, 0,0,0, 0,0, r,g,b,a,  // 5
            1+e,1+e,1+e, 0,0,0, 0,0, r,g,b,a,  // 6
            0-e,1+e,1+e, 0,0,0, 0,0, r,g,b,a,  // 7
        };
        uint32_t indices[] = {
            0,1, 1,2, 2,3, 3,0, // bottom
            4,5, 5,6, 6,7, 7,4, // top
            0,4, 1,5, 2,6, 3,7  // verticals
        };

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, sizeof(verts), verts);
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index, sizeof(indices), indices);
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        Log::Info("Block highlight system initialized successfully");
        return true;
    }

    void BlockHighlight::Render(const glm::ivec3& blockPos, const glm::mat4& viewProjectionMatrix) {
        if (m_mesh == INVALID_MESH || m_shader == INVALID_SHADER || !g_renderBackend) return;

        PipelineState state;
        state.depthTestEnabled = true;
        state.depthWriteEnabled = false;
        state.blendEnabled = true;
        state.srcBlendFactor = BlendFactor::SrcAlpha;
        state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        state.cullMode = CullMode::None;
        state.primitiveType = PrimitiveType::Lines;
        state.lineWidth = 3.0f;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);

        // Set uniforms
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", viewProjectionMatrix);
        g_renderBackend->SetUniformVec3(m_shader, "uBlockPos",
            glm::vec3(static_cast<float>(blockPos.x),
                      static_cast<float>(blockPos.y),
                      static_cast<float>(blockPos.z)));

        g_renderBackend->DrawIndexed(m_mesh, 24); // 12 edges * 2 indices

        // Restore default pipeline state
        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        defaultState.polygonMode = PolygonMode::Fill;
        g_renderBackend->SetPipelineState(defaultState);
    }

    bool BlockHighlight::IsValidHighlight(const std::optional<Game::RaycastHit>& hit) {
        if (!hit.has_value()) return false;
        if (hit->distance > Game::PlayerController::INTERACTION_RANGE) return false;
        if (hit->blockId == Game::BlockID::Air) return false;
        // Verify block exists in registry
        Game::BlockRegistry::Get(hit->blockId);
        return true;
    }

} // namespace Render
