// File: src/client/renderer/debug/Crosshair.cpp
#include "Crosshair.hpp"
#include "../backend/RenderBackend.hpp"
#include "common/core/Log.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <vector>
#include <filesystem>

// Use stb_image for texture loading
#include "../ext/stb_image/stb_image.h"

namespace Render {

    // Global crosshair instance
    Crosshair g_crosshair;

    // Shader sources for 2D orthographic rendering
    const char* Crosshair::vertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uProjection;
uniform vec2 uPosition;
uniform vec2 uSize;

out vec2 TexCoord;

void main() {
    vec2 screenPos = aPos * uSize + uPosition;
    gl_Position = uProjection * vec4(screenPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

    const char* Crosshair::fragmentShaderSource = R"(
#version 330 core

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uOpacity;

void main() {
    vec4 texColor = texture(uTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * uOpacity);
}
)";

    Crosshair::Crosshair()
        : isVisible(true), crosshairSize(32), isInitialized(false) {
    }

    Crosshair::~Crosshair() {
        if (g_renderBackend) {
            if (m_mesh != INVALID_MESH)       g_renderBackend->DestroyMesh(m_mesh);
            if (m_vb != INVALID_BUFFER)       g_renderBackend->DestroyBuffer(m_vb);
            if (m_ib != INVALID_BUFFER)       g_renderBackend->DestroyBuffer(m_ib);
            if (m_texture != INVALID_TEXTURE) g_renderBackend->DestroyTexture(m_texture);
            if (m_shader != INVALID_SHADER)   g_renderBackend->DestroyShader(m_shader);
        }
    }

    bool Crosshair::Initialize(const std::string& texturePath) {
        Log::Info("Initializing crosshair system with texture: %s", texturePath.c_str());

        if (!g_renderBackend) {
            Log::Error("Crosshair: No render backend available");
            return false;
        }

        // Create shader — try SPIR-V files first (Vulkan), fall back to source (GL)
        m_shader = g_renderBackend->CreateShaderFromFiles("shaders/crosshair.vert", "shaders/crosshair.frag");
        if (m_shader == INVALID_SHADER) {
            m_shader = g_renderBackend->CreateShader(vertexShaderSource, fragmentShaderSource);
        }
        if (m_shader == INVALID_SHADER) {
            Log::Error("Crosshair: Failed to create shader");
            return false;
        }

        // Load texture
        bool textureLoaded = false;
        if (!texturePath.empty() && std::filesystem::exists(texturePath)) {
            textureLoaded = LoadTexture(texturePath);
        }
        if (!textureLoaded) {
            Log::Info("Creating fallback crosshair texture");
            CreateFallbackTexture();
        }

        if (m_texture == INVALID_TEXTURE) {
            Log::Error("Crosshair: Failed to create texture");
            return false;
        }

        // Create quad geometry using 12-float vertex format (pos3 + norm3 + uv2 + color4)
        float verts[] = {
            0.0f, 0.0f, 0.0f, 0,0,1, 0.0f, 0.0f, 1,1,1,1,  // TL
            1.0f, 0.0f, 0.0f, 0,0,1, 1.0f, 0.0f, 1,1,1,1,  // TR
            1.0f, 1.0f, 0.0f, 0,0,1, 1.0f, 1.0f, 1,1,1,1,  // BR
            0.0f, 1.0f, 0.0f, 0,0,1, 0.0f, 1.0f, 1,1,1,1,  // BL
        };
        uint32_t indices[] = { 0, 1, 2, 0, 2, 3 };

        m_vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, sizeof(verts), verts);
        m_ib = g_renderBackend->CreateBuffer(BufferUsage::Index, sizeof(indices), indices);
        m_mesh = g_renderBackend->CreateMesh(m_vb, m_ib, GetBlockVertexLayout());

        isInitialized = true;
        Log::Info("Crosshair system initialized successfully");
        return true;
    }

    void Crosshair::Render(int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight) {
        if (!isInitialized || !isVisible || framebufferWidth <= 0 || framebufferHeight <= 0) return;
        if (m_mesh == INVALID_MESH || m_shader == INVALID_SHADER || m_texture == INVALID_TEXTURE || !g_renderBackend) return;

        // Set up 2D pipeline state
        PipelineState state;
        state.depthTestEnabled = false;
        state.depthWriteEnabled = false;
        state.blendEnabled = true;
        state.srcBlendFactor = BlendFactor::SrcAlpha;
        state.dstBlendFactor = BlendFactor::OneMinusSrcAlpha;
        state.cullMode = CullMode::None;
        g_renderBackend->SetPipelineState(state);
        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_texture, 0);

        // Orthographic projection: map [0, fbW] x [0, fbH] to clip space
        float fbW = static_cast<float>(framebufferWidth);
        float fbH = static_cast<float>(framebufferHeight);
        float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
        float scaledSize = crosshairSize * scaleX;
        float left = (fbW - scaledSize) * 0.5f;
        float top  = (fbH - scaledSize) * 0.5f;

        // Build ortho + translate + scale to position the quad
        glm::mat4 ortho = glm::ortho(0.0f, fbW, fbH, 0.0f, -1.0f, 1.0f);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(left, top, 0.0f));
        model = glm::scale(model, glm::vec3(scaledSize, scaledSize, 1.0f));
        glm::mat4 mvp = ortho * model;

        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->DrawIndexed(m_mesh, 6);

        // Restore default state
        PipelineState defaultState;
        defaultState.depthTestEnabled = true;
        defaultState.depthWriteEnabled = true;
        defaultState.blendEnabled = false;
        defaultState.cullMode = CullMode::Back;
        g_renderBackend->SetPipelineState(defaultState);
    }

    bool Crosshair::LoadTexture(const std::string& texturePath) {
        int width = 0, height = 0, channels = 0;
        stbi_set_flip_vertically_on_load(
            g_renderBackend->GetType() == BackendType::OpenGL ? 1 : 0);

        unsigned char* data = stbi_load(texturePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!data) {
            Log::Error("Failed to load crosshair texture: %s - %s", texturePath.c_str(), stbi_failure_reason());
            return false;
        }

        m_texture = g_renderBackend->CreateTexture2D(width, height, TextureFormat::RGBA8, data);
        stbi_image_free(data);

        if (m_texture == INVALID_TEXTURE) return false;

        g_renderBackend->SetTextureFilter(m_texture, TextureFilter::Nearest, TextureFilter::Nearest);
        g_renderBackend->SetTextureWrap(m_texture, TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        Log::Info("Loaded crosshair texture: %dx%d, handle: %u", width, height, m_texture);
        return true;
    }

    void Crosshair::CreateFallbackTexture() {
        const int size = 16;
        std::vector<unsigned char> data(size * size * 4, 0);

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int idx = (y * size + x) * 4;

                // Horizontal line
                if (y == size / 2 && (x >= size / 2 - 6 && x <= size / 2 + 6)) {
                    data[idx] = data[idx+1] = data[idx+2] = data[idx+3] = 255;
                }
                // Vertical line
                else if (x == size / 2 && (y >= size / 2 - 6 && y <= size / 2 + 6)) {
                    data[idx] = data[idx+1] = data[idx+2] = data[idx+3] = 255;
                }

                // Black outline
                bool isOutline = false;
                if (y == size / 2 && (x == size / 2 - 7 || x == size / 2 + 7)) isOutline = true;
                if (x == size / 2 && (y == size / 2 - 7 || y == size / 2 + 7)) isOutline = true;

                if (isOutline) {
                    data[idx] = data[idx+1] = data[idx+2] = 0;
                    data[idx+3] = 255;
                }
            }
        }

        m_texture = g_renderBackend->CreateTexture2D(size, size, TextureFormat::RGBA8, data.data());
        g_renderBackend->SetTextureFilter(m_texture, TextureFilter::Nearest, TextureFilter::Nearest);
        Log::Info("Created fallback crosshair texture (%dx%d), handle: %u", size, size, m_texture);
    }

} // namespace Render
