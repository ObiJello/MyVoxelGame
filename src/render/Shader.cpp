#include "Shader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>  // for glm::value_ptr

// Constructor
Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
    : vertexFile(vertexPath), fragmentFile(fragmentPath)
{
    UpdateTimestamps();
    CompileAndLink();
}

// Destructor: delete the GL program
Shader::~Shader() {
    glDeleteProgram(programID);
}

// Use (bind) this shader
void Shader::Use() const {
    glUseProgram(programID);
}

// Retrieve uniform location (cached)
int Shader::GetUniformLocation(const std::string& name) {
    if (uniformCache.count(name)) {
        return uniformCache[name];
    }
    int loc = glGetUniformLocation(programID, name.c_str());
    uniformCache[name] = loc;
    return loc;
}

// Set a mat4 uniform
void Shader::SetMat4(const std::string& name, const glm::mat4& matrix) {
    int loc = GetUniformLocation(name);
    glUniformMatrix4fv(loc, 1, GL_FALSE, glm::value_ptr(matrix));
}

// Hot-reload if either file changed
void Shader::HotReloadIfNeeded() {
    auto vertTime = std::filesystem::last_write_time(vertexFile);
    auto fragTime = std::filesystem::last_write_time(fragmentFile);

    if (vertTime != lastWriteVert || fragTime != lastWriteFrag) {
        UpdateTimestamps();
        CompileAndLink();
        uniformCache.clear();
    }
}

// Compile & link the shaders
void Shader::CompileAndLink() {
    std::string vertCode = ReadFile(vertexFile);
    std::string fragCode = ReadFile(fragmentFile);
    if (vertCode.empty() || fragCode.empty()) return;

    const char* vSrc = vertCode.c_str();
    const char* fSrc = fragCode.c_str();

    // Compile vertex shader
    GLuint vShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vShader, 1, &vSrc, nullptr);
    glCompileShader(vShader);
    GLint success;
    glGetShaderiv(vShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vShader, 512, nullptr, infoLog);
        std::cerr << "[Shader] Vertex compilation failed: " << infoLog << "\n";
    }

    // Compile fragment shader
    GLuint fShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fShader, 1, &fSrc, nullptr);
    glCompileShader(fShader);
    glGetShaderiv(fShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fShader, 512, nullptr, infoLog);
        std::cerr << "[Shader] Fragment compilation failed: " << infoLog << "\n";
    }

    // Link program
    GLuint newProgram = glCreateProgram();
    glAttachShader(newProgram, vShader);
    glAttachShader(newProgram, fShader);
    glLinkProgram(newProgram);
    glGetProgramiv(newProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(newProgram, 512, nullptr, infoLog);
        std::cerr << "[Shader] Program link failed: " << infoLog << "\n";
    }

    // Delete old shaders and replace program
    glDeleteShader(vShader);
    glDeleteShader(fShader);
    if (programID != 0) {
        glDeleteProgram(programID);
    }
    programID = newProgram;
}

// Internal: load file content
std::string Shader::ReadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Shader] Failed to open file: " << path << "\n";
        return "";
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// Check & update last-write times
void Shader::UpdateTimestamps() {
    lastWriteVert = std::filesystem::last_write_time(vertexFile);
    lastWriteFrag = std::filesystem::last_write_time(fragmentFile);
}
