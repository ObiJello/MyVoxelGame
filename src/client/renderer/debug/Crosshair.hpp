// File: src/client/renderer/debug/Crosshair.hpp
#pragma once

#include <glm/glm.hpp>
#include <string>
#include "../backend/RenderTypes.hpp"

namespace Render {

    class Crosshair {
    public:
        Crosshair();
        ~Crosshair();

        // Initialize the crosshair renderer (call once)
        bool Initialize(const std::string& texturePath = "assets/textures/gui/sprites/hud/crosshair.png");

        // Release GPU resources (safe to call multiple times)
        void Shutdown();

        // Render the crosshair at screen center with proper Retina support
        void Render(int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight);

        // Legacy method for backward compatibility
        void Render(int screenWidth, int screenHeight) {
            Render(screenWidth, screenHeight, screenWidth, screenHeight);
        }

        // Enable/disable crosshair visibility
        void SetVisible(bool visible) { isVisible = visible; }
        bool IsVisible() const { return isVisible; }

        // Set crosshair size (default is 16x16 like Minecraft)
        void SetSize(int size) { crosshairSize = size; }
        int GetSize() const { return crosshairSize; }

    private:
        bool isVisible;
        int crosshairSize;
        bool isInitialized;

        // Backend handles
        ShaderHandle m_shader = INVALID_SHADER;
        TextureHandle m_texture = INVALID_TEXTURE;
        BufferHandle m_vb = INVALID_BUFFER;
        BufferHandle m_ib = INVALID_BUFFER;
        MeshHandle m_mesh = INVALID_MESH;

        // Shader source code (for GL backend's CreateShader)
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        // Load the crosshair texture through backend
        bool LoadTexture(const std::string& texturePath);

        // Create a fallback crosshair texture
        void CreateFallbackTexture();
    };

    // Global crosshair renderer instance
    extern Crosshair g_crosshair;

} // namespace Render
