// File: src/client/renderer/mesh/BlockHighlight.cpp
#include "BlockHighlight.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>
#include <vector>

namespace Render {

    // Global instance
    BlockHighlight g_blockHighlight;

    // Shader sources (used by GL backend's CreateShader from source)
    const char* BlockHighlight::vertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;

uniform mat4 uMVP;
uniform vec3 uBlockPos;
uniform vec2 uScreenSize;
uniform float uLineWidth;

out vec4 vColor;

void main() {
    vec4 clipPos  = uMVP * vec4(aPos, 1.0);
    vec4 clipPos2 = uMVP * vec4(aPos + aNormal, 1.0);

    vec2 ndcPos  = clipPos.xy  / clipPos.w;
    vec2 ndcPos2 = clipPos2.xy / clipPos2.w;

    vec2 screenDir = (ndcPos2 - ndcPos) * uScreenSize;
    vec2 perp = normalize(vec2(-screenDir.y, screenDir.x));

    float side = (gl_VertexID % 2 == 0) ? -1.0 : 1.0;
    vec2 offset = perp * uLineWidth * side / uScreenSize;

    gl_Position = clipPos + vec4(offset * clipPos.w, 0.0, 0.0);
    vColor = aColor;
}
)";

    const char* BlockHighlight::fragmentShaderSource = R"(
#version 330 core

in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
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

        // Build screen-space quad geometry for each edge (Minecraft-style)
        // Small offset to avoid z-fighting
        const float e = 0.005f;
        const float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.4f; // black at 40% opacity

        glm::vec3 corners[8] = {
            {0-e, 0-e, 0-e}, {1+e, 0-e, 0-e}, {1+e, 0-e, 1+e}, {0-e, 0-e, 1+e},
            {0-e, 1+e, 0-e}, {1+e, 1+e, 0-e}, {1+e, 1+e, 1+e}, {0-e, 1+e, 1+e},
        };
        int edges[12][2] = {
            {0,1}, {1,2}, {2,3}, {3,0},  // bottom
            {4,5}, {5,6}, {6,7}, {7,4},  // top
            {0,4}, {1,5}, {2,6}, {3,7},  // verticals
        };

        // 4 verts per edge (quad), 6 indices per edge (2 triangles)
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        verts.reserve(12 * 4 * 12);
        indices.reserve(12 * 6);

        for (int i = 0; i < 12; i++) {
            glm::vec3 p0 = corners[edges[i][0]];
            glm::vec3 p1 = corners[edges[i][1]];
            glm::vec3 dir = glm::normalize(p1 - p0);
            uint32_t base = i * 4;

            // 4 vertices: even gl_VertexID = side -1, odd = side +1
            for (int v = 0; v < 2; v++) {
                glm::vec3 p = (v == 0) ? p0 : p1;
                // Two vertices at each endpoint (one per side)
                verts.insert(verts.end(), {p.x, p.y, p.z, dir.x, dir.y, dir.z, 0,0, r,g,b,a});
                verts.insert(verts.end(), {p.x, p.y, p.z, dir.x, dir.y, dir.z, 0,0, r,g,b,a});
            }

            indices.insert(indices.end(), {base+0, base+1, base+3, base+0, base+3, base+2});
        }

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex,
            verts.size() * sizeof(float), verts.data());
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index,
            indices.size() * sizeof(uint32_t), indices.data());
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
        state.primitiveType = PrimitiveType::Triangles;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_dummyTexture, 0);

        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(static_cast<float>(blockPos.x),
                      static_cast<float>(blockPos.y),
                      static_cast<float>(blockPos.z)));
        glm::mat4 mvp = viewProjectionMatrix * model;
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformVec3(m_shader, "uBlockPos", glm::vec3(0.0f));

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(g_renderBackend->GetWindow(), &fbWidth, &fbHeight);
        g_renderBackend->SetUniformVec2(m_shader, "uScreenSize",
            glm::vec2(static_cast<float>(fbWidth), static_cast<float>(fbHeight)));
        g_renderBackend->SetUniformFloat(m_shader, "uLineWidth", 2.0f);

        g_renderBackend->DrawIndexed(m_mesh, 72); // 12 edges * 6 indices

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
