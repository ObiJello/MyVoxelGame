// File: src/client/renderer/shader/Shader.cpp
#include "Shader.hpp"
#include "../backend/RenderBackend.hpp"
#include <filesystem>
#include "common/core/Log.hpp"

Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
    : vertexFile(vertexPath), fragmentFile(fragmentPath)
{
    if (!Render::g_renderBackend) {
        Log::Error("Shader: No render backend available for %s", vertexPath.c_str());
        return;
    }

    Log::Info("Creating shader: %s + %s", vertexPath.c_str(), fragmentPath.c_str());
    UpdateTimestamps();
    CompileAndLink();
}

Shader::~Shader() {
    if (m_handle != Render::INVALID_SHADER && Render::g_renderBackend) {
        Render::g_renderBackend->DestroyShader(m_handle);
    }
}

void Shader::Use() const {
    if (m_handle == Render::INVALID_SHADER || !Render::g_renderBackend) return;
    Render::g_renderBackend->BindShader(m_handle);
}

void Shader::SetMat4(const std::string& name, const glm::mat4& matrix) const {
    if (m_handle == Render::INVALID_SHADER || !Render::g_renderBackend) return;
    Render::g_renderBackend->SetUniformMat4(m_handle, name, matrix);
}

unsigned int Shader::ID() const {
    if (!Render::g_renderBackend) return 0;
    // Return the native GL program ID for backward compatibility
    return static_cast<unsigned int>(Render::g_renderBackend->GetNativeTextureID(m_handle));
}

void Shader::HotReloadIfNeeded() {
    if (!std::filesystem::exists(vertexFile) || !std::filesystem::exists(fragmentFile)) return;

    auto vertTime = std::filesystem::last_write_time(vertexFile);
    auto fragTime = std::filesystem::last_write_time(fragmentFile);

    if (vertTime != lastWriteVert || fragTime != lastWriteFrag) {
        Log::Info("Shader files changed, recompiling (%s + %s)", vertexFile.c_str(), fragmentFile.c_str());

        // Destroy old handle
        if (m_handle != Render::INVALID_SHADER && Render::g_renderBackend) {
            Render::g_renderBackend->DestroyShader(m_handle);
            m_handle = Render::INVALID_SHADER;
        }

        UpdateTimestamps();
        CompileAndLink();

        Log::Info("Shader recompiled successfully");
    }
}

void Shader::CompileAndLink() {
    if (!Render::g_renderBackend) return;

    m_handle = Render::g_renderBackend->CreateShaderFromFiles(vertexFile, fragmentFile);
    if (m_handle == Render::INVALID_SHADER) {
        Log::Error("Failed to create shader from files: %s + %s", vertexFile.c_str(), fragmentFile.c_str());
    } else {
        Log::Info("Shader created successfully, handle: %u", m_handle);
    }
}

void Shader::UpdateTimestamps() {
    if (std::filesystem::exists(vertexFile)) {
        lastWriteVert = std::filesystem::last_write_time(vertexFile);
    }
    if (std::filesystem::exists(fragmentFile)) {
        lastWriteFrag = std::filesystem::last_write_time(fragmentFile);
    }
}
