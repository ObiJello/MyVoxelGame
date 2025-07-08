// File: src/render/gfx/Shader.hpp
#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>
#include <glm/glm.hpp>  // for glm::mat4 in SetMat4

class Shader {
public:
    // Construct with paths to vertex and fragment shader files.
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    // Destructor: deletes the GL program
    ~Shader();

    // Use (bind) this shader - const because it doesn't modify the shader object
    void Use() const;

    // Retrieve uniform location (cached) - mutable cache allows const method
    int GetUniformLocation(const std::string& name) const;

    // Set a mat4 uniform (e.g. uMVP) - const because it doesn't modify the shader object
    void SetMat4(const std::string& name, const glm::mat4& matrix) const;

    // Query and report if the underlying files have changed; if so, recompile & relink.
    // This is NOT const because it can modify the shader program
    void HotReloadIfNeeded();

    // Return the OpenGL program ID
    unsigned int ID() const { return programID; }

private:
    std::string vertexFile;
    std::string fragmentFile;
    unsigned int programID = 0;

    // Track last write times for hot-reload
    std::filesystem::file_time_type lastWriteVert;
    std::filesystem::file_time_type lastWriteFrag;

    // Uniform location cache - mutable so it can be modified in const methods
    mutable std::unordered_map<std::string, int> uniformCache;

    // Internal: compile and link shaders, replacing programID.
    void CompileAndLink();

    // Internal: load file content
    static std::string ReadFile(const std::string& path);

    // Internal: check and update last-write timestamps
    void UpdateTimestamps();
};