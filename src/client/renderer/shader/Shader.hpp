// File: src/client/renderer/mesh/Shader.hpp
#pragma once

#include <string>
#include <filesystem>
#include <glm/glm.hpp>
#include "../backend/RenderTypes.hpp"

namespace Render { class RenderBackend; }

class Shader {
public:
    // Construct with paths to vertex and fragment shader files.
    Shader(const std::string& vertexPath, const std::string& fragmentPath);

    // Destructor: destroys the backend shader handle
    ~Shader();

    // Use (bind) this shader
    void Use() const;

    // Set a mat4 uniform (e.g. uMVP)
    void SetMat4(const std::string& name, const glm::mat4& matrix) const;

    // Query and report if the underlying files have changed; if so, recompile & relink.
    void HotReloadIfNeeded();

    // Return the backend shader handle
    Render::ShaderHandle GetHandle() const { return m_handle; }

    // Return the legacy OpenGL program ID (for backward compatibility)
    unsigned int ID() const;

private:
    std::string vertexFile;
    std::string fragmentFile;
    Render::ShaderHandle m_handle = Render::INVALID_SHADER;

    // Track last write times for hot-reload
    std::filesystem::file_time_type lastWriteVert;
    std::filesystem::file_time_type lastWriteFrag;

    // Internal: compile and link shaders via backend
    void CompileAndLink();

    // Internal: check and update last-write timestamps
    void UpdateTimestamps();
};
