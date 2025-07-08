// File: src/render/debug/Crosshair.cpp
#include "Crosshair.hpp"
#include "../../core/Log.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <vector>
#include <filesystem>

// Use stb_image for texture loading
#include "../../../ext/stb_image/stb_image.h"

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

    // Normal rendering
    FragColor = vec4(texColor.rgb, texColor.a * uOpacity);
}
)";

    Crosshair::Crosshair()
        : vao(0), vbo(0), textureID(0), shaderProgram(0)
        , isVisible(true), crosshairSize(32), isInitialized(false) {
    }

    Crosshair::~Crosshair() {
        if (vao != 0) glDeleteVertexArrays(1, &vao);
        if (vbo != 0) glDeleteBuffers(1, &vbo);
        if (textureID != 0) glDeleteTextures(1, &textureID);
        if (shaderProgram != 0) glDeleteProgram(shaderProgram);
    }

    bool Crosshair::Initialize(const std::string& texturePath) {
        Log::Info("=== CROSSHAIR INITIALIZATION START ===");
        Log::Info("Initializing crosshair system with texture: %s", texturePath.c_str());

        if (!CreateShaders()) {
            Log::Error("Failed to create crosshair shaders");
            return false;
        }
        Log::Info("✓ Crosshair shaders created successfully");

        CreateGeometry();
        Log::Info("✓ Crosshair geometry created successfully");

        // **DEBUG**: Always try to load the texture first, then fallback
        bool textureLoaded = false;
        if (!texturePath.empty() && std::filesystem::exists(texturePath)) {
            Log::Info("Texture file exists: %s", texturePath.c_str());
            textureLoaded = LoadTexture(texturePath);
        } else {
            Log::Warning("Texture file does not exist or path is empty: %s", texturePath.c_str());
        }

        if (!textureLoaded) {
            Log::Info("Creating fallback crosshair texture");
            CreateFallbackTexture();
        }

        // **DEBUG**: Verify texture was created
        if (textureID == 0) {
            Log::Error("Failed to create crosshair texture (ID is 0)");
            return false;
        }

        // **DEBUG**: Check if texture is valid
        glBindTexture(GL_TEXTURE_2D, textureID);
        GLint width, height;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glBindTexture(GL_TEXTURE_2D, 0);

        Log::Info("✓ Crosshair texture created - ID: %u, Size: %dx%d", textureID, width, height);

        isInitialized = true;
        Log::Info("=== CROSSHAIR INITIALIZATION COMPLETE ===");
        return true;
    }

    void Crosshair::Render(int windowWidth, int windowHeight, int framebufferWidth, int framebufferHeight) {
    if (!isInitialized || !isVisible || framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    // **CRITICAL**: Store current OpenGL state
    GLint currentProgram;
    GLboolean depthTestEnabled;
    GLboolean blendEnabled;
    GLint blendSrc, blendDst;
    GLint currentVAO;
    GLint activeTexture;
    GLint boundTexture;
    GLint viewport[4];
    GLboolean cullFaceEnabled;

    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    blendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDst);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &currentVAO);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTexture);
    glGetIntegerv(GL_VIEWPORT, viewport);
    cullFaceEnabled = glIsEnabled(GL_CULL_FACE);

    // Set up proper 2D rendering state
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Use our shader program
    glUseProgram(shaderProgram);

    // **FIX 1**: Use framebuffer size for viewport (this matches the actual rendering resolution)
    glViewport(0, 0, framebufferWidth, framebufferHeight);

    // **FIX 2**: Use framebuffer coordinates for projection (this is what actually gets rendered to)
    glm::mat4 projection = glm::ortho(
        0.0f, static_cast<float>(framebufferWidth),    // left, right
        static_cast<float>(framebufferHeight), 0.0f,   // bottom, top (flipped for screen coords)
        -1.0f, 1.0f                                    // near, far
    );

    // **FIX 3**: Calculate position for CENTER of crosshair, then adjust for top-left corner
    float halfSize = crosshairSize * 0.5f;

    // Calculate scale factor between window and framebuffer (for Retina displays)
    float scaleX = static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth);
    float scaleY = static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight);

    // Scale the crosshair size appropriately for the display
    float scaledSize = crosshairSize * scaleX; // Use X scale factor (should be same as Y on most displays)
    float scaledHalfSize = scaledSize * 0.5f;

    // Position for the TOP-LEFT corner of the crosshair (shader expects this)
    glm::vec2 position(
        (framebufferWidth * 0.5f) - scaledHalfSize,   // Center X minus half width
        (framebufferHeight * 0.5f) - scaledHalfSize   // Center Y minus half height
    );

    glm::vec2 size(scaledSize, scaledSize);

    // Debug output (remove this later)
    static int debugCounter = 0;
    /*if (++debugCounter % 60 == 0) { // Every second at 60fps
        Log::Info("Crosshair Debug:");
        Log::Info("  Window: %dx%d, Framebuffer: %dx%d", windowWidth, windowHeight, framebufferWidth, framebufferHeight);
        Log::Info("  Scale: %.2fx%.2f", scaleX, scaleY);
        Log::Info("  Crosshair size: %.1f, Position: (%.1f, %.1f)", scaledSize, position.x, position.y);
        Log::Info("  Should be at framebuffer center: (%.1f, %.1f)", framebufferWidth * 0.5f, framebufferHeight * 0.5f);
    }*/

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
        glUniform1f(opacityLoc, 1.0f);
    }

    // Bind texture and VAO
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glBindVertexArray(vao);

    // Draw the crosshair
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Check for errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR && debugCounter % 60 == 0) {
        Log::Error("OpenGL error after crosshair draw: 0x%x", error);
    }

    // Restore ALL OpenGL state exactly
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
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
    if (cullFaceEnabled) {
        glEnable(GL_CULL_FACE);
    }
}

    // **ALSO FIX**: Make sure geometry is created correctly
    void Crosshair::CreateGeometry() {
        Log::Info("Creating crosshair geometry...");

        // Create a simple quad (triangle strip)
        // Make sure winding order is correct
        float vertices[] = {
            // Position  // TexCoord
            0.0f, 0.0f,  0.0f, 0.0f,  // Top-left
            1.0f, 0.0f,  1.0f, 0.0f,  // Top-right
            0.0f, 1.0f,  0.0f, 1.0f,  // Bottom-left
            1.0f, 1.0f,  1.0f, 1.0f   // Bottom-right
        };

        // Generate and bind VAO
        glGenVertexArrays(1, &vao);
        if (vao == 0) {
            Log::Error("Failed to generate VAO for crosshair");
            return;
        }

        glBindVertexArray(vao);

        // Generate and upload vertex buffer
        glGenBuffers(1, &vbo);
        if (vbo == 0) {
            Log::Error("Failed to generate VBO for crosshair");
            return;
        }

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // **CRITICAL**: Set up vertex attributes correctly
        // Position attribute (location 0)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        // Texture coordinate attribute (location 1)
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        // **IMPORTANT**: Unbind VAO to prevent accidental modification
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Verify the setup worked
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log::Error("OpenGL error creating crosshair geometry: 0x%x", error);
        } else {
            Log::Info("✓ Crosshair geometry created: VAO=%u, VBO=%u", vao, vbo);
        }
    }

    bool Crosshair::LoadTexture(const std::string& texturePath) {
        Log::Info("Attempting to load texture: %s", texturePath.c_str());

        // Check if file exists
        if (!std::filesystem::exists(texturePath)) {
            Log::Error("Texture file not found: %s", texturePath.c_str());
            return false;
        }

        Log::Info("File exists, loading with stb_image...");

        // Load image
        int width, height, channels;
        stbi_set_flip_vertically_on_load(1);  // Flip for OpenGL

        // **FIX**: Force RGBA loading to avoid alignment issues
        unsigned char* data = stbi_load(texturePath.c_str(), &width, &height, &channels, STBI_rgb_alpha);

        if (!data) {
            Log::Error("stb_image failed to load texture: %s - %s", texturePath.c_str(), stbi_failure_reason());
            return false;
        }

        Log::Info("stb_image loaded texture: %dx%d, %d channels (forced to RGBA)", width, height, channels);

        // **FIX**: Set pixel alignment to 1 to avoid stride issues
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        // Generate OpenGL texture
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);

        // Upload texture data - always use RGBA since we forced loading as RGBA
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

        // Set texture parameters for pixel-perfect rendering
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Generate mipmaps
        glGenerateMipmap(GL_TEXTURE_2D);

        // Clean up
        stbi_image_free(data);
        glBindTexture(GL_TEXTURE_2D, 0);

        // Reset pixel alignment to default
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        Log::Info("✓ Loaded crosshair texture: %dx%d, texture ID: %u", width, height, textureID);
        return true;
    }

    void Crosshair::CreateFallbackTexture() {
        Log::Info("Creating fallback crosshair texture");

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

        Log::Info("✓ Created fallback crosshair texture (%dx%d), texture ID: %u", size, size, textureID);
    }

    // [Keep all other methods unchanged...]
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

} // namespace Render