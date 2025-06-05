#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <filesystem>

class Shader {
public:
    // Construct with paths to vertex and fragment shader files.
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    // Destructor: deletes the GL program
    ~Shader();

    // Use (bind) this shader
    void Use() const;

    // Retrieve uniform location (cached)
    int GetUniformLocation(const std::string& name);

    // Query and report if the underlying files have changed; if so, recompile & relink.
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

    // Uniform location cache
    std::unordered_map<std::string, int> uniformCache;

    // Internal: compile and link shaders, replacing programID.
    void CompileAndLink();

    // Internal: load file content
    static std::string ReadFile(const std::string& path);

    // Internal: check and update last-write timestamps
    void UpdateTimestamps();
};
