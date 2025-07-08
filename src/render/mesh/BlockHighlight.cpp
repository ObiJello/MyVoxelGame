// File: src/render/mesh/BlockHighlight.cpp
#include "BlockHighlight.hpp"
#include "../../core/Log.hpp"
#include "../../engine/block/BlockRegistry.hpp"
#include <vector>

namespace Render {

    // Global instance
    BlockHighlight g_blockHighlight;

    // Shader sources
    const char* BlockHighlight::vertexShaderSource = R"(
#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;
uniform vec3 uBlockPos;

void main() {
    // Offset the wireframe slightly to prevent z-fighting
    vec3 worldPos = aPos + uBlockPos + vec3(0.001, 0.001, 0.001);
    gl_Position = uMVP * vec4(worldPos, 1.0);
}
)";

    const char* BlockHighlight::fragmentShaderSource = R"(
#version 330 core

out vec4 FragColor;

void main() {
    // Minecraft-style black outline
    FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
)";

    BlockHighlight::BlockHighlight()
        : vao(0), vbo(0), ebo(0), shaderProgram(0) {
    }

    BlockHighlight::~BlockHighlight() {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        if (ebo != 0) glDeleteBuffers(1, &ebo);
        if (shaderProgram != 0) glDeleteProgram(shaderProgram);
    }

    bool BlockHighlight::Initialize() {
        Log::Info("Initializing block highlight system");

        if (!CreateShaders()) {
            Log::Error("Failed to create highlight shaders");
            return false;
        }

        CreateGeometry();

        Log::Info("Block highlight system initialized successfully");
        return true;
    }

    void BlockHighlight::Render(const glm::ivec3& blockPos, const glm::mat4& viewProjectionMatrix) {
        // Clear any pending OpenGL errors before we start
        while (glGetError() != GL_NO_ERROR) {
            // Clear error queue
        }

        // Store ALL current OpenGL state that we might change
        GLint currentProgram;
        GLboolean depthMask;
        GLboolean blendEnabled;
        GLint polygonMode[2];
        GLfloat lineWidth;
        GLint blendSrc, blendDst;
        GLint currentVAO;
        GLint currentArrayBuffer;
        GLint currentElementBuffer;

        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
        blendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_POLYGON_MODE, polygonMode);
        glGetFloatv(GL_LINE_WIDTH, &lineWidth);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVAO);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &currentArrayBuffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &currentElementBuffer);

        // Set up our rendering state
        glUseProgram(shaderProgram);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(2.0f);

        // Set uniforms ONLY on our shader (we're sure it's active now)
        GLint mvpLoc = glGetUniformLocation(shaderProgram, "uMVP");
        GLint blockPosLoc = glGetUniformLocation(shaderProgram, "uBlockPos");

        if (mvpLoc != -1) {
            glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, &viewProjectionMatrix[0][0]);
        }

        if (blockPosLoc != -1) {
            glUniform3f(blockPosLoc,
                       static_cast<float>(blockPos.x),
                       static_cast<float>(blockPos.y),
                       static_cast<float>(blockPos.z));
        }

        // Render the wireframe cube
        glBindVertexArray(vao);
        glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, nullptr);

        // Restore ALL OpenGL state exactly as it was
        glBindVertexArray(currentVAO);
        glBindBuffer(GL_ARRAY_BUFFER, currentArrayBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, currentElementBuffer);
        glUseProgram(currentProgram);
        glPolygonMode(GL_FRONT_AND_BACK, polygonMode[0]);
        glDepthMask(depthMask);
        glBlendFunc(blendSrc, blendDst);
        if (!blendEnabled) {
            glDisable(GL_BLEND);
        }
        glLineWidth(lineWidth);

        // Clear any errors that might have occurred during our rendering
        // (without logging them since they're not from the main renderer)
        while (glGetError() != GL_NO_ERROR) {
            // Silently clear error queue
        }
    }

    bool BlockHighlight::IsValidHighlight(const std::optional<Game::RaycastHit>& hit) {
        if (!hit.has_value()) {
            return false;
        }

        // Check if the block is within interaction range
        if (hit->distance > Game::PlayerController::INTERACTION_RANGE) {
            return false;
        }

        // Check if it's a solid block (not air)
        if (hit->blockId == Game::BlockID::Air) {
            return false;
        }

        // Check if the block exists in the registry
        const Game::Block& block = Game::BlockRegistry::Get(hit->blockId);

        // Don't highlight transparent blocks like water or glass in some cases
        // For now, we'll highlight all non-air blocks
        return true;
    }

    bool BlockHighlight::CreateShaders() {
        // Compile vertex shader
        GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
        if (vertexShader == 0) {
            return false;
        }

        // Compile fragment shader
        GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
        if (fragmentShader == 0) {
            glDeleteShader(vertexShader);
            return false;
        }

        // Create shader program
        shaderProgram = glCreateProgram();
        glAttachShader(shaderProgram, vertexShader);
        glAttachShader(shaderProgram, fragmentShader);
        glLinkProgram(shaderProgram);

        // Check linking status
        GLint success;
        glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(shaderProgram, 512, nullptr, infoLog);
            Log::Error("Block highlight shader linking failed: %s", infoLog);

            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(shaderProgram);
            shaderProgram = 0;
            return false;
        }

        // Clean up individual shaders
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        Log::Debug("Block highlight shaders compiled and linked successfully");
        return true;
    }

    void BlockHighlight::CreateGeometry() {
        // Define the 8 vertices of a unit cube
        std::vector<glm::vec3> vertices = {
            // Bottom face
            {0.0f, 0.0f, 0.0f}, // 0: bottom-back-left
            {1.0f, 0.0f, 0.0f}, // 1: bottom-back-right
            {1.0f, 0.0f, 1.0f}, // 2: bottom-front-right
            {0.0f, 0.0f, 1.0f}, // 3: bottom-front-left

            // Top face
            {0.0f, 1.0f, 0.0f}, // 4: top-back-left
            {1.0f, 1.0f, 0.0f}, // 5: top-back-right
            {1.0f, 1.0f, 1.0f}, // 6: top-front-right
            {0.0f, 1.0f, 1.0f}  // 7: top-front-left
        };

        // Define the 12 edges of the cube (24 indices for 12 lines)
        std::vector<unsigned int> indices = {
            // Bottom face edges
            0, 1,  1, 2,  2, 3,  3, 0,

            // Top face edges
            4, 5,  5, 6,  6, 7,  7, 4,

            // Vertical edges
            0, 4,  1, 5,  2, 6,  3, 7
        };

        // Generate and bind VAO
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        // Generate and upload vertex buffer
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     vertices.size() * sizeof(glm::vec3),
                     vertices.data(),
                     GL_STATIC_DRAW);

        // Generate and upload index buffer
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     indices.size() * sizeof(unsigned int),
                     indices.data(),
                     GL_STATIC_DRAW);

        // Set up vertex attributes
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);

        // Unbind VAO to prevent accidental modification
        glBindVertexArray(0);

        Log::Debug("Block highlight geometry created: %zu vertices, %zu indices",
                  vertices.size(), indices.size());
    }

    GLuint BlockHighlight::CompileShader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        // Check compilation status
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);
            const char* shaderType = (type == GL_VERTEX_SHADER) ? "vertex" : "fragment";
            Log::Error("Block highlight %s shader compilation failed: %s", shaderType, infoLog);
            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

} // namespace Render