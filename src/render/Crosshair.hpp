// File: src/render/Crosshair.hpp
#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <string>

namespace Render {

    class Crosshair {
    public:
        Crosshair();
        ~Crosshair();

        // Initialize the crosshair renderer (call once)
        bool Initialize(const std::string& texturePath = "assets/textures/gui/sprites/hud/crosshair.png");

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
        GLuint vao;
        GLuint vbo;
        GLuint textureID;
        GLuint shaderProgram;

        bool isVisible;
        int crosshairSize;
        bool isInitialized;

        // Shader source code
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        // Create and compile shaders
        bool CreateShaders();

        // Create the quad geometry for the crosshair
        void CreateGeometry();

        // Load the crosshair texture
        bool LoadTexture(const std::string& texturePath);

        // Utility function to compile a shader
        GLuint CompileShader(GLenum type, const char* source);

        // Create a fallback crosshair if texture loading fails
        void CreateFallbackTexture();
    };

    // Global crosshair renderer instance
    extern Crosshair g_crosshair;

} // namespace Render