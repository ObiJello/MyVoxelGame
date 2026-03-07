// File: src/client/renderer/backend/RenderBackend.hpp
#pragma once

#include "RenderTypes.hpp"
#include <glm/glm.hpp>
#include <string>
#include <memory>

struct GLFWwindow;

namespace Render {

    // Abstract rendering backend interface.
    // Both OpenGL and Vulkan implement this interface.
    class RenderBackend {
    public:
        virtual ~RenderBackend() = default;

        // ====================================================================
        // LIFECYCLE
        // ====================================================================

        virtual bool Initialize(GLFWwindow* window) = 0;
        virtual void Shutdown() = 0;

        virtual BackendType GetType() const = 0;
        virtual const char* GetName() const = 0;

        // ====================================================================
        // FRAME MANAGEMENT
        // ====================================================================

        virtual void BeginFrame() = 0;
        virtual void EndFrame(GLFWwindow* window) = 0;
        virtual void SetClearColor(float r, float g, float b, float a) = 0;
        virtual void Clear(bool color, bool depth) = 0;
        virtual void SetViewport(int x, int y, int width, int height) = 0;

        // ====================================================================
        // BUFFER MANAGEMENT
        // ====================================================================

        virtual BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                         const void* data, BufferAccess access = BufferAccess::Static) = 0;
        virtual void UpdateBuffer(BufferHandle handle, size_t offset,
                                 size_t size, const void* data) = 0;
        virtual void DestroyBuffer(BufferHandle handle) = 0;

        // ====================================================================
        // TEXTURE MANAGEMENT
        // ====================================================================

        virtual TextureHandle CreateTexture2D(int width, int height,
                                             TextureFormat format,
                                             const void* data) = 0;
        virtual void UpdateTexture2D(TextureHandle handle, int x, int y,
                                    int width, int height, const void* data) = 0;
        virtual void SetTextureFilter(TextureHandle handle,
                                     TextureFilter min, TextureFilter mag) = 0;
        virtual void SetTextureWrap(TextureHandle handle,
                                   TextureWrap s, TextureWrap t) = 0;
        virtual void GenerateMipmaps(TextureHandle handle) = 0;
        virtual void DestroyTexture(TextureHandle handle) = 0;
        virtual void BindTexture(TextureHandle handle, uint32_t slot = 0) = 0;

        // Get the native (platform-specific) texture ID for ImGui interop.
        // Returns the underlying GLuint or VkDescriptorSet cast to uintptr_t.
        virtual uintptr_t GetNativeTextureID(TextureHandle handle) const = 0;

        // ====================================================================
        // SHADER MANAGEMENT
        // ====================================================================

        // Create shader from source strings (GLSL for OpenGL)
        virtual ShaderHandle CreateShader(const std::string& vertexSource,
                                         const std::string& fragmentSource) = 0;
        // Create shader from file paths (auto-detects GLSL vs SPIR-V)
        virtual ShaderHandle CreateShaderFromFiles(const std::string& vertexPath,
                                                  const std::string& fragmentPath) = 0;
        virtual void DestroyShader(ShaderHandle handle) = 0;
        virtual void BindShader(ShaderHandle handle) = 0;

        // Uniform setters
        virtual void SetUniformMat4(ShaderHandle handle, const std::string& name,
                                   const glm::mat4& value) = 0;
        virtual void SetUniformVec3(ShaderHandle handle, const std::string& name,
                                   const glm::vec3& value) = 0;
        virtual void SetUniformVec2(ShaderHandle handle, const std::string& name,
                                   const glm::vec2& value) = 0;
        virtual void SetUniformFloat(ShaderHandle handle, const std::string& name,
                                    float value) = 0;
        virtual void SetUniformInt(ShaderHandle handle, const std::string& name,
                                  int value) = 0;

        // ====================================================================
        // MESH MANAGEMENT (VAO equivalent)
        // ====================================================================

        // Create a renderable mesh from vertex + index buffers with a layout
        virtual MeshHandle CreateMesh(BufferHandle vertexBuffer,
                                     BufferHandle indexBuffer,
                                     const VertexLayout& layout) = 0;
        virtual void DestroyMesh(MeshHandle handle) = 0;

        // ====================================================================
        // PIPELINE STATE
        // ====================================================================

        virtual void SetPipelineState(const PipelineState& state) = 0;

        // Invalidate cached pipeline state, forcing full reapplication on next SetPipelineState
        virtual void InvalidateStateCache() = 0;

        // ====================================================================
        // DRAWING
        // ====================================================================

        virtual void DrawIndexed(MeshHandle mesh, uint32_t indexCount,
                                uint32_t indexOffset = 0) = 0;
        virtual void DrawArrays(MeshHandle mesh, uint32_t vertexCount,
                               uint32_t firstVertex = 0) = 0;

        // ====================================================================
        // GPU TIMER QUERIES
        // ====================================================================

        virtual GPUTimerHandle BeginGPUTimer(const std::string& name) = 0;
        virtual void EndGPUTimer(GPUTimerHandle handle) = 0;
        virtual float GetGPUTimerResultMs(GPUTimerHandle handle) = 0;

        // ====================================================================
        // DEBUG / MEMORY
        // ====================================================================

        virtual GPUMemoryStats GetMemoryStats() const = 0;

        // Window accessor (needed when glfwGetCurrentContext() returns NULL in Vulkan mode)
        virtual GLFWwindow* GetWindow() const = 0;

        // VSync control
        virtual void SetVSync(bool enabled) = 0;

        // ====================================================================
        // IMGUI INTEGRATION
        // ====================================================================

        virtual void ImGuiInit(GLFWwindow* window) = 0;
        virtual void ImGuiNewFrame() = 0;
        virtual void ImGuiRender() = 0;
        virtual void ImGuiShutdown() = 0;
    };

    // Factory function
    std::unique_ptr<RenderBackend> CreateRenderBackend(BackendType type);

    // Global backend instance
    extern std::unique_ptr<RenderBackend> g_renderBackend;

} // namespace Render
