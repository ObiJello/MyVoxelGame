// File: src/render/BlockHighlight.hpp
#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <optional>

#include "PlayerController.hpp"
#include "../game/RayCast.hpp"

namespace Render {

    class BlockHighlight {
    public:
        BlockHighlight();
        ~BlockHighlight();

        // Initialize the highlight renderer (call once)
        bool Initialize();

        // Render the highlight for the given block position
        void Render(const glm::ivec3& blockPos, const glm::mat4& viewProjectionMatrix);

        // Check if a hit is valid for highlighting (in range, solid block, etc.)
        static bool IsValidHighlight(const std::optional<Game::RaycastHit>& hit);

    private:
        GLuint vao;
        GLuint vbo;
        GLuint ebo;
        GLuint shaderProgram;

        // Shader source code
        static const char* vertexShaderSource;
        static const char* fragmentShaderSource;

        // Create and compile shaders
        bool CreateShaders();

        // Create the wireframe cube geometry
        void CreateGeometry();

        // Utility function to compile a shader
        GLuint CompileShader(GLenum type, const char* source);
    };

    // Global highlight renderer instance
    extern BlockHighlight g_blockHighlight;

} // namespace Render