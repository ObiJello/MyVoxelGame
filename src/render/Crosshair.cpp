// File: src/render/Crosshair.cpp
#include "Crosshair.hpp"
#include "../core/Log.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>  // For glm::ortho
#include <vector>
#include <filesystem>

// Use stb_image for texture loading
#include "../../ext/stb_image/stb_image.h"

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
    // Transform from screen space to NDC
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

    // Discard fully transparent pixels
    if (texColor.a < 0.01) {
        discard;
    }

    // Apply opacity and output
    FragColor = vec4(texColor.rgb, texColor.a * uOpacity);
}
)";

    Crosshair::Crosshair()
        : vao(0), vbo(0), textureID(0), shaderProgram(0)
        , isVisible(true), crosshairSize(16), isInitialized(false) {
    }

    Crosshair::~Crosshair() {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        if (textureID != 0) glDeleteTextures(1, &textureID);
        if (shaderProgram != 0) glDeleteProgram(shaderProgram);
    }

    bool Crosshair::Initialize(const std::string& texturePath) {
        Log::Info("Initializing crosshair system");

        if (!CreateShaders()) {
            Log::Error("Failed to create crosshair shaders");
            return false;
        }

        CreateGeometry();

        if (!LoadTexture(texturePath)) {
            Log::Warning("Failed to load crosshair texture, using fallback");
            CreateFallbackTexture();
        }

        isInitialized = true;
        Log::Info("Crosshair system initialized successfully");
        return true;
    }

    void Crosshair::Render(int screenWidth, int screenHeight) {
        if (!isInitialized || !isVisible || screenWidth <= 0 || screenHeight <= 0) {
            return;
        }

        // Debug output (only occasionally to avoid spam)
        static int debugCounter = 0;
        if (++debugCounter % 300 == 0) { // Every 5 seconds at 60 FPS
            Log::Debug("Rendering crosshair: screen=%dx%d, size=%d, visible=%s",
                      screenWidth, screenHeight, crosshairSize, isVisible ? "true" : "false");
        }

        // Store current OpenGL state
        GLint currentProgram;
        GLboolean depthTestEnabled;
        GLboolean blendEnabled;
        GLint blendSrc, blendDst;
        GLint currentVAO;
        GLint activeTexture;
        GLint boundTexture;

        glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
        depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
        blendEnabled = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVAO);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);

        // Set up 2D rendering state
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Use our shader
        glUseProgram(shaderProgram);

        // Set up orthographic projection for screen space rendering
        // Map (0,0) to top-left, (screenWidth, screenHeight) to bottom-right
        glm::mat4 projection = glm::ortho(
            0.0f, static_cast<float>(screenWidth),    // left, right
            static_cast<float>(screenHeight), 0.0f,  // bottom, top (flipped for screen coords)
            -1.0f, 1.0f                              // near, far
        );

        // Calculate crosshair position (center of screen)
        float halfSize = crosshairSize * 0.5f;
        glm::vec2 position(
            screenWidth * 0.5f - halfSize,
            screenHeight * 0.5f - halfSize
        );
        glm::vec2 size(crosshairSize, crosshairSize);

        // Debug output for positioning
        if (debugCounter % 300 == 0) {
            Log::Debug("Crosshair position: (%.1f, %.1f), size: (%.1f, %.1f)",
                      position.x, position.y, size.x, size.y);
        }

        // Set uniforms
        GLint projLoc = glGetUniformLocation(shaderProgram, "uProjection");
        GLint posLoc = glGetUniformLocation(shaderProgram, "uPosition");
        GLint sizeLoc = glGetUniformLocation(shaderProgram, "uSize");
        GLint texLoc = glGetUniformLocation(shaderProgram, "uTexture");
        GLint opacityLoc = glGetUniformLocation(shaderProgram, "uOpacity");

        if (projLoc != -1) {
            glUniformMatrix4fv(projLoc, 1, GL_FALSE, &projection[0][0]);
        }
        if (posLoc != -1) {
            glUniform2f(posLoc, position.x, position.y);
        }
        if (sizeLoc != -1) {
            glUniform2f(sizeLoc, size.x, size.y);
        }
        if (texLoc != -1) {
            glUniform1i(texLoc, 0);
        }
        if (opacityLoc != -1) {
            glUniform1f(opacityLoc, 1.0f);  // Full opacity
        }

        // Bind texture
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Render the quad
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Debug: Check for OpenGL errors
        GLenum error = glGetError();
        if (error != GL_NO_ERROR && debugCounter % 300 == 0) {
            Log::Warning("OpenGL error during crosshair rendering: 0x%x", error);
        }

        // Restore OpenGL state
        glBindVertexArray(currentVAO);
        glActiveTexture(activeTexture);
        glBindTexture(GL_TEXTURE_2D, boundTexture);
        glUseProgram(currentProgram);
        glBlendFunc(blendSrc, blendDst);

        if (!blendEnabled) {
            glDisable(GL_BLEND);
        }
        if (depthTestEnabled) {
            glEnable(GL_DEPTH_TEST);
        }
    }

    bool Crosshair::CreateShaders() {
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
            Log::Error("Crosshair shader linking failed: %s", infoLog);

            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(shaderProgram);
            shaderProgram = 0;
            return false;
        }

        // Clean up individual shaders
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        Log::Debug("Crosshair shaders compiled and linked successfully");
        return true;
    }

    void Crosshair::CreateGeometry() {
        // Create a simple quad (two triangles as triangle strip)
        // Positions and texture coordinates for a unit quad
        float vertices[] = {
            // Position  // TexCoord
            0.0f, 0.0f,  0.0f, 0.0f,  // Top-left
            1.0f, 0.0f,  1.0f, 0.0f,  // Top-right
            0.0f, 1.0f,  0.0f, 1.0f,  // Bottom-left
            1.0f, 1.0f,  1.0f, 1.0f   // Bottom-right
        };

        // Generate and bind VAO
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);

        // Generate and upload vertex buffer
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Set up vertex attributes
        // Position attribute (location 0)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        // Texture coordinate attribute (location 1)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        // Unbind VAO
        glBindVertexArray(0);

        Log::Debug("Crosshair geometry created");
    }

    bool Crosshair::LoadTexture(const std::string& texturePath) {
        // Check if file exists
        if (!std::filesystem::exists(texturePath)) {
            Log::Warning("Crosshair texture file not found: %s", texturePath.c_str());
            return false;
        }

        // Load image
        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);  // Flip for OpenGL
        unsigned char* data = stbi_load(texturePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!data) {
            Log::Error("Failed to load crosshair texture: %s - %s", texturePath.c_str(), stbi_failure_reason());
            return false;
        }

        // Generate OpenGL texture
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Upload texture data
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

        // Set texture parameters for pixel-perfect rendering
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Clean up
        stbi_image_free(data);
        glBindTexture(GL_TEXTURE_2D, 0);

        Log::Info("Loaded crosshair texture: %dx%d from %s", width, height, texturePath.c_str());
        return true;
    }

    GLuint Crosshair::CompileShader(GLenum type, const char* source) {
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
            Log::Error("Crosshair %s shader compilation failed: %s", shaderType, infoLog);
            glDeleteShader(shader);
            return 0;
        }

        return shader;
    }

    void Crosshair::CreateFallbackTexture() {
        // Create a simple white plus sign crosshair as fallback
        const int size = 16;
        std::vector<unsigned char> data(size * size * 4, 0);  // RGBA, initially transparent

        // Draw a simple plus sign
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                int idx = (y * size + x) * 4;

                // Horizontal line (center row)
                if (y == size / 2 && (x >= size / 2 - 6 && x <= size / 2 + 6)) {
                    data[idx + 0] = 255;  // R
                    data[idx + 1] = 255;  // G
                    data[idx + 2] = 255;  // B
                    data[idx + 3] = 255;  // A
                }
                // Vertical line (center column)
                else if (x == size / 2 && (y >= size / 2 - 6 && y <= size / 2 + 6)) {
                    data[idx + 0] = 255;  // R
                    data[idx + 1] = 255;  // G
                    data[idx + 2] = 255;  // B
                    data[idx + 3] = 255;  // A
                }

                // Add black outline for better visibility
                bool isOutline = false;
                if (y == size / 2 && (x >= size / 2 - 7 && x <= size / 2 + 7)) {
                    if (x == size / 2 - 7 || x == size / 2 + 7) isOutline = true;
                }
                if (x == size / 2 && (y >= size / 2 - 7 && y <= size / 2 + 7)) {
                    if (y == size / 2 - 7 || y == size / 2 + 7) isOutline = true;
                }

                if (isOutline) {
                    data[idx + 0] = 0;    // R
                    data[idx + 1] = 0;    // G
                    data[idx + 2] = 0;    // B
                    data[idx + 3] = 255;  // A
                }
            }
        }

        // Generate OpenGL texture
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Upload texture data
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());

        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, 0);

        Log::Info("Created fallback crosshair texture (%dx%d)", size, size);
    }

} // namespace Render