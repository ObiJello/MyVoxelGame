// File: src/client/renderer/backend/opengl/GLBackend.hpp
#pragma once

#include "../RenderBackend.hpp"
#include <glad/glad.h>
#include <unordered_map>
#include <mutex>

namespace Render {

    class GLBackend : public RenderBackend {
    public:
        GLBackend();
        ~GLBackend() override;

        // Lifecycle
        bool Initialize(GLFWwindow* window) override;
        void Shutdown() override;
        BackendType GetType() const override { return BackendType::OpenGL; }
        const char* GetName() const override { return "OpenGL 3.3"; }
        GLFWwindow* GetWindow() const override { return m_window; }
        void SetVSync(bool enabled) override;

        // Frame
        void BeginFrame() override;
        void EndFrame(GLFWwindow* window) override;
        void SetClearColor(float r, float g, float b, float a) override;
        void Clear(bool color, bool depth) override;
        void SetViewport(int x, int y, int width, int height) override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                 const void* data, BufferAccess access) override;
        void UpdateBuffer(BufferHandle handle, size_t offset,
                         size_t size, const void* data) override;
        void DestroyBuffer(BufferHandle handle) override;

        // Textures
        TextureHandle CreateTexture2D(int width, int height, TextureFormat format,
                                     const void* data) override;
        void UpdateTexture2D(TextureHandle handle, int x, int y,
                            int width, int height, const void* data) override;
        void SetTextureFilter(TextureHandle handle, TextureFilter min, TextureFilter mag) override;
        void SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) override;
        void GenerateMipmaps(TextureHandle handle) override;
        void DestroyTexture(TextureHandle handle) override;
        void BindTexture(TextureHandle handle, uint32_t slot) override;
        uintptr_t GetNativeTextureID(TextureHandle handle) const override;

        // Shaders
        ShaderHandle CreateShader(const std::string& vertexSource,
                                 const std::string& fragmentSource) override;
        ShaderHandle CreateShaderFromFiles(const std::string& vertexPath,
                                          const std::string& fragmentPath) override;
        void DestroyShader(ShaderHandle handle) override;
        void BindShader(ShaderHandle handle) override;
        void SetUniformMat4(ShaderHandle handle, const std::string& name, const glm::mat4& value) override;
        void SetUniformVec3(ShaderHandle handle, const std::string& name, const glm::vec3& value) override;
        void SetUniformVec2(ShaderHandle handle, const std::string& name, const glm::vec2& value) override;
        void SetUniformFloat(ShaderHandle handle, const std::string& name, float value) override;
        void SetUniformInt(ShaderHandle handle, const std::string& name, int value) override;

        // Meshes
        MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                             const VertexLayout& layout) override;
        void DestroyMesh(MeshHandle handle) override;

        // Pipeline state
        void SetPipelineState(const PipelineState& state) override;
        void InvalidateStateCache() override;

        // Drawing
        void DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) override;
        void DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) override;

        // Mega-buffer rendering
        void BindVertexBuffer(BufferHandle vbo, uint32_t stride) override;
        void BindIndexBuffer(BufferHandle ibo) override;
        void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex) override;
        void MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                        const int32_t* baseVertices, uint32_t drawCount) override;

        // Shared block vertex format (VAO)
        void SetupBlockVertexFormat() override;
        void BindBlockVertexFormat() override;
        void DestroyBlockVertexFormat() override;

        // GPU timers
        GPUTimerHandle BeginGPUTimer(const std::string& name) override;
        void EndGPUTimer(GPUTimerHandle handle) override;
        float GetGPUTimerResultMs(GPUTimerHandle handle) override;

        // Debug/Memory
        GPUMemoryStats GetMemoryStats() const override;

        // ImGui
        void ImGuiInit(GLFWwindow* window) override;
        void ImGuiNewFrame() override;
        void ImGuiRender() override;
        void ImGuiShutdown() override;

    private:
        // Handle → GL ID mappings
        uint32_t m_nextHandle = 1;
        uint32_t AllocHandle() { return m_nextHandle++; }

        struct GLBufferInfo {
            GLuint glId = 0;
            GLenum target = GL_ARRAY_BUFFER;
            size_t size = 0;
        };
        std::unordered_map<uint32_t, GLBufferInfo> m_buffers;

        struct GLTextureInfo {
            GLuint glId = 0;
            int width = 0, height = 0;
            size_t memorySize = 0;
        };
        std::unordered_map<uint32_t, GLTextureInfo> m_textures;

        struct GLShaderInfo {
            GLuint programId = 0;
            mutable std::unordered_map<std::string, GLint> uniformCache;
            GLint GetUniform(const std::string& name) const;
        };
        std::unordered_map<uint32_t, GLShaderInfo> m_shaders;

        struct GLMeshInfo {
            GLuint vao = 0;
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
        };
        std::unordered_map<uint32_t, GLMeshInfo> m_meshes;

        struct GLTimerInfo {
            GLuint queryId = 0;
            bool active = false;
            bool resultReady = false;
            float resultMs = 0.0f;
        };
        std::unordered_map<uint32_t, GLTimerInfo> m_timers;

        // Memory tracking
        GPUMemoryStats m_memStats;

        // Render state cache (avoid redundant GL calls)
        PipelineState m_currentState;
        bool m_stateInitialized = false;

        // Window reference
        GLFWwindow* m_window = nullptr;

        // Currently bound handles
        ShaderHandle m_boundShader = INVALID_SHADER;

        // Shared block vertex format (GL_ARB_vertex_attrib_binding)
        GLuint m_sharedBlockVAO = 0;
        bool m_hasVertexAttribBinding = false;

        // Helpers
        GLenum ToGLBufferTarget(BufferUsage usage) const;
        GLenum ToGLBufferUsage(BufferAccess access) const;
        GLenum ToGLFilter(TextureFilter filter) const;
        GLenum ToGLWrap(TextureWrap wrap) const;
        GLenum ToGLBlendFactor(BlendFactor factor) const;
        GLenum ToGLCompareOp(CompareOp op) const;
        std::string ReadFileContents(const std::string& path) const;
        GLuint CompileGLShader(GLenum type, const std::string& source) const;
    };

} // namespace Render
