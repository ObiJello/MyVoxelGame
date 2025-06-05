#include "Shader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <glad/glad.h>

// Constructor
Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
    : vertexFile(vertexPath), fragmentFile(fragmentPath)
{
    UpdateTimestamps();
    CompileAndLink();
}

// Destructor
Shader::~Shader() {
    if (programID) {
        glDeleteProgram(programID);
    }
}

void Shader::Use() const {
    glUseProgram(programID);
}

int Shader::GetUniformLocation(const std::string& name) {
    if (uniformCache.count(name)) {
        return uniformCache[name];
    }
    int loc = glGetUniformLocation(programID, name.c_str());
    uniformCache[name] = loc;
    return loc;
}

void Shader::HotReloadIfNeeded() {
    bool shouldReload = false;

    auto vertTime = std::filesystem::last_write_time(vertexFile);
    auto fragTime = std::filesystem::last_write_time(fragmentFile);

    if (vertTime != lastWriteVert || fragTime != lastWriteFrag) {
        shouldReload = true;
    }

    if (shouldReload) {
        lastWriteVert = vertTime;
        lastWriteFrag = fragTime;
        CompileAndLink();
    }
}

void Shader::CompileAndLink() {
    // 1) Read source
    std::string vertCode = ReadFile(vertexFile);
    std::string fragCode = ReadFile(fragmentFile);
    const char* vSrc = vertCode.c_str();
    const char* fSrc = fragCode.c_str();

    // 2) Compile vertex shader
    unsigned int vertShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertShader, 1, &vSrc, nullptr);
    glCompileShader(vertShader);
    {
        int success;
        char infoLog[512];
        glGetShaderiv(vertShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(vertShader, 512, nullptr, infoLog);
            std::cerr << "[Shader] Failed to compile vertex shader:\n" << infoLog << "\n";
        }
    }

    // 3) Compile fragment shader
    unsigned int fragShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragShader, 1, &fSrc, nullptr);
    glCompileShader(fragShader);
    {
        int success;
        char infoLog[512];
        glGetShaderiv(fragShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(fragShader, 512, nullptr, infoLog);
            std::cerr << "[Shader] Failed to compile fragment shader:\n" << infoLog << "\n";
        }
    }

    // 4) Link program
    unsigned int newProgram = glCreateProgram();
    glAttachShader(newProgram, vertShader);
    glAttachShader(newProgram, fragShader);
    glLinkProgram(newProgram);
    {
        int success;
        char infoLog[512];
        glGetProgramiv(newProgram, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(newProgram, 512, nullptr, infoLog);
            std::cerr << "[Shader] Failed to link shader program:\n" << infoLog << "\n";
        }
    }

    // 5) Clean up shaders
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    // 6) Replace the existing program
    if (programID) {
        glDeleteProgram(programID);
    }
    programID = newProgram;
    uniformCache.clear(); // clear cache, since locations may have changed
}

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

void Shader::UpdateTimestamps() {
    lastWriteVert = std::filesystem::last_write_time(vertexFile);
    lastWriteFrag = std::filesystem::last_write_time(fragmentFile);
}
