// File: src/render/gfx/Shader.cpp
#include "Shader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <unordered_set>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>  // for glm::value_ptr
#include "../../core/Log.hpp"

// Constructor
Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
    : vertexFile(vertexPath), fragmentFile(fragmentPath)
{
    Log::Info("Creating shader: %s + %s", vertexPath.c_str(), fragmentPath.c_str());
    UpdateTimestamps();
    CompileAndLink();
}

// Destructor: delete the GL program
Shader::~Shader() {
    if (programID != 0) {
        Log::Debug("Deleting shader program ID: %u", programID);
        glDeleteProgram(programID);
    }
}

// Use (bind) this shader
void Shader::Use() const {
    if (programID == 0) {
        Log::Warning("Attempted to use invalid shader program (ID = 0)");
        return;
    }

    glUseProgram(programID);

    // Minimal error checking - only in debug and very rarely
#ifndef NDEBUG
    static int errorCheckCounter = 0;
    if (++errorCheckCounter % 1000 == 0) { // Check every 1000th call
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            Log::Warning("OpenGL error after glUseProgram (ID: %u): 0x%x", programID, error);
        }
    }
#endif
}

// Retrieve uniform location (cached)
int Shader::GetUniformLocation(const std::string& name) const {  // Now const
    if (uniformCache.count(name)) {
        return uniformCache[name];
    }

    if (programID == 0) {
        Log::Warning("Cannot get uniform location for '%s' - invalid shader program", name.c_str());
        uniformCache[name] = -1;  // mutable allows modification
        return -1;
    }

    int loc = glGetUniformLocation(programID, name.c_str());
    uniformCache[name] = loc;  // mutable allows modification

    if (loc == -1) {
        // Only log this once per uniform to avoid spam
        static std::unordered_set<std::string> loggedMissingUniforms;
        if (loggedMissingUniforms.find(name) == loggedMissingUniforms.end()) {
            Log::Debug("Uniform '%s' not found in shader program %u", name.c_str(), programID);
            loggedMissingUniforms.insert(name);
        }
    }

    return loc;
}

// Set a mat4 uniform
void Shader::SetMat4(const std::string& name, const glm::mat4& matrix) const {  // Now const
    int loc = GetUniformLocation(name);
    if (loc != -1) {
        glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(matrix));

        // Very minimal error checking - only check occasionally and only in debug
#ifndef NDEBUG
        static int matrixErrorCheckCounter = 0;
        if (++matrixErrorCheckCounter % 500 == 0) { // Check every 500th call
            GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                Log::Warning("OpenGL error setting mat4 uniform '%s': 0x%x (checked every 500 calls)", name.c_str(), error);
            }
        }
#endif
    }
}

// Hot-reload if either file changed
void Shader::HotReloadIfNeeded() {
    if (!std::filesystem::exists(vertexFile) || !std::filesystem::exists(fragmentFile)) {
        Log::Error("Shader files don't exist: %s or %s", vertexFile.c_str(), fragmentFile.c_str());
        return;
    }

    auto vertTime = std::filesystem::last_write_time(vertexFile);
    auto fragTime = std::filesystem::last_write_time(fragmentFile);

    if (vertTime != lastWriteVert || fragTime != lastWriteFrag) {
        Log::Info("Shader files changed, recompiling shaders (%s + %s)",
                 vertexFile.c_str(), fragmentFile.c_str());

        // Clear cache BEFORE recompiling
        uniformCache.clear();

        UpdateTimestamps();
        CompileAndLink();

        Log::Info("Shader recompiled successfully, uniform cache cleared");
    }
}

// Compile & link the shaders
void Shader::CompileAndLink() {
    Log::Debug("Starting shader compilation...");

    // Clear uniform cache since we're creating a new program
    uniformCache.clear();

    std::string vertCode = ReadFile(vertexFile);
    std::string fragCode = ReadFile(fragmentFile);
    if (vertCode.empty() || fragCode.empty()) {
        Log::Error("Failed to read shader files");
        return;
    }

    Log::Debug("Vertex shader size: %zu chars, Fragment shader size: %zu chars",
              vertCode.size(), fragCode.size());

    const char* vSrc = vertCode.c_str();
    const char* fSrc = fragCode.c_str();

    // Compile vertex shader
    GLuint vShader = glCreateShader(GL_VERTEX_SHADER);
    if (vShader == 0) {
        Log::Error("Failed to create vertex shader object");
        return;
    }

    glShaderSource(vShader, 1, &vSrc, nullptr);
    glCompileShader(vShader);

    GLint success;
    glGetShaderiv(vShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vShader, 512, nullptr, infoLog);
        Log::Error("Vertex shader compilation failed: %s", infoLog);
        glDeleteShader(vShader);
        return;
    } else {
        Log::Debug("Vertex shader compiled successfully");
    }

    // Compile fragment shader
    GLuint fShader = glCreateShader(GL_FRAGMENT_SHADER);
    if (fShader == 0) {
        Log::Error("Failed to create fragment shader object");
        glDeleteShader(vShader);
        return;
    }

    glShaderSource(fShader, 1, &fSrc, nullptr);
    glCompileShader(fShader);

    glGetShaderiv(fShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fShader, 512, nullptr, infoLog);
        Log::Error("Fragment shader compilation failed: %s", infoLog);
        glDeleteShader(vShader);
        glDeleteShader(fShader);
        return;
    } else {
        Log::Debug("Fragment shader compiled successfully");
    }

    // Link program
    GLuint newProgram = glCreateProgram();
    if (newProgram == 0) {
        Log::Error("Failed to create shader program object");
        glDeleteShader(vShader);
        glDeleteShader(fShader);
        return;
    }

    glAttachShader(newProgram, vShader);
    glAttachShader(newProgram, fShader);
    glLinkProgram(newProgram);

    glGetProgramiv(newProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(newProgram, 512, nullptr, infoLog);
        Log::Error("Shader program link failed: %s", infoLog);
        glDeleteShader(vShader);
        glDeleteShader(fShader);
        glDeleteProgram(newProgram);
        return;
    } else {
        Log::Debug("Shader program linked successfully");
    }

    // Validate program (but don't fail on validation errors)
    glValidateProgram(newProgram);
    glGetProgramiv(newProgram, GL_VALIDATE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(newProgram, 512, nullptr, infoLog);
        Log::Debug("Shader program validation warning: %s", infoLog);
        // Don't fail on validation - it's just a warning
    }

    // Test that the program can actually be used
    GLint currentProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);

    glUseProgram(newProgram);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        Log::Error("Cannot use newly created shader program: OpenGL error 0x%x", error);
        glDeleteShader(vShader);
        glDeleteShader(fShader);
        glDeleteProgram(newProgram);
        return;
    }

    // Restore previous program
    glUseProgram(currentProgram);

    // Clean up shaders (they're linked into the program now)
    glDeleteShader(vShader);
    glDeleteShader(fShader);

    // Delete old program AFTER successful linking and testing
    if (programID != 0) {
        Log::Debug("Deleting old shader program %u", programID);
        glDeleteProgram(programID);
    }

    programID = newProgram;
    Log::Info("Shader program created successfully, new ID: %u", programID);

    // Check if our critical uniforms exist (but don't spam the log)
    glUseProgram(programID);
    int mvpLoc = glGetUniformLocation(programID, "uMVP");
    int atlasLoc = glGetUniformLocation(programID, "uTextureAtlas");

    if (mvpLoc == -1 && atlasLoc == -1) {
        Log::Debug("No standard uniforms found - might be a special purpose shader");
    } else {
        Log::Debug("Found uniforms - uMVP: %d, uTextureAtlas: %d", mvpLoc, atlasLoc);
    }

    // Restore previous program
    glUseProgram(currentProgram);
}

// Internal: load file content
std::string Shader::ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::Error("Failed to open shader file: %s", path.c_str());
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    if (content.empty()) {
        Log::Error("Shader file is empty: %s", path.c_str());
    } else {
        Log::Debug("Successfully read shader file: %s (%zu chars)", path.c_str(), content.size());
    }

    return content;
}

// Check & update last-write times
void Shader::UpdateTimestamps() {
    if (std::filesystem::exists(vertexFile)) {
        lastWriteVert = std::filesystem::last_write_time(vertexFile);
    }
    if (std::filesystem::exists(fragmentFile)) {
        lastWriteFrag = std::filesystem::last_write_time(fragmentFile);
    }
}