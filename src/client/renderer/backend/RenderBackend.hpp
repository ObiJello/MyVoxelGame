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
        // The `stencil` flag (default false) clears the stencil buffer to 0.
        // Existing callers that pass only color+depth keep working unchanged.
        virtual void Clear(bool color, bool depth, bool stencil = false) = 0;
        virtual void SetViewport(int x, int y, int width, int height) = 0;

        // ====================================================================
        // BUFFER MANAGEMENT
        // ====================================================================

        virtual BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                         const void* data, BufferAccess access = BufferAccess::Static) = 0;
        virtual void UpdateBuffer(BufferHandle handle, size_t offset,
                                 size_t size, const void* data) = 0;
        virtual void DestroyBuffer(BufferHandle handle) = 0;

        // Deferred destroy — delays destruction until GPU is done with the resource.
        // Default implementation calls immediate destroy (correct for OpenGL).
        virtual void DeferredDestroyBuffer(BufferHandle handle) { DestroyBuffer(handle); }

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
        virtual void SetUniformVec4(ShaderHandle handle, const std::string& name,
                                   const glm::vec4& value) = 0;
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
        virtual void DeferredDestroyMesh(MeshHandle handle) { DestroyMesh(handle); }

        // ====================================================================
        // PIPELINE STATE
        // ====================================================================

        virtual void SetPipelineState(const PipelineState& state) = 0;

        // Invalidate cached pipeline state, forcing full reapplication on next SetPipelineState
        virtual void InvalidateStateCache() = 0;

        // Copy the current default framebuffer's color attachment into the
        // given texture. The texture must be RGBA8 and sized exactly to
        // the framebuffer (renderer is responsible for sizing — backend
        // doesn't reallocate). Used by the portal renderer to capture the
        // see-through view as a sampleable texture for the refraction
        // sub-pass. Default impl is a no-op so backends that don't
        // support framebuffer copy silently skip the refraction effect.
        virtual void CopyFramebufferToTexture(TextureHandle /*dst*/) {}

        // ====================================================================
        // RENDER TARGETS (offscreen framebuffers)
        // ====================================================================
        // Optional infra — added for the portal feature's HDR + bloom +
        // recursion pipelines. Both backends should implement; default
        // impls return INVALID_RENDER_TARGET so callers fall back gracefully
        // on backends that don't yet have RT support.
        //
        // Lifecycle: Create → (Resize as window resizes) → Bind/Unbind
        // around offscreen passes → Destroy on shutdown.
        // `INVALID_RENDER_TARGET` passed to BindRenderTarget binds the
        // default backbuffer (FBO 0 on GL).

        virtual RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& /*desc*/) {
            return INVALID_RENDER_TARGET;
        }
        virtual void DestroyRenderTarget(RenderTargetHandle /*rt*/) {}
        virtual void BindRenderTarget(RenderTargetHandle /*rt*/) {}

        // Return the color attachment as a sampleable texture (for tone
        // map / bloom / refraction sub-pass shaders). Returns
        // INVALID_TEXTURE on unsupported backends.
        virtual TextureHandle GetRenderTargetColorTexture(RenderTargetHandle /*rt*/) const {
            return INVALID_TEXTURE;
        }
        // Resize the RT in place (used when the window framebuffer size
        // changes). Recreates underlying texture/FBO at the new
        // resolution.
        virtual void ResizeRenderTarget(RenderTargetHandle /*rt*/, int /*w*/, int /*h*/) {}

        // Stencil override: while ENABLED, every subsequent SetPipelineState
        // call gets its stencil fields replaced with these values, regardless
        // of what the caller passed. Used by the portal renderer's see-
        // through pass — we need ChunkRenderer's per-pass SetPipelineState
        // calls to honour our stencil mask without modifying ChunkRenderer's
        // API. Default implementation is a no-op so backends without state-
        // based stencil (the Vulkan path, where stencil is partly baked into
        // the pipeline cache) silently fall back to non-stenciled rendering.
        // Pair every SetStencilOverride(true, ...) with a SetStencilOverride(false, ...).
        virtual void SetStencilOverride(bool /*enabled*/,
                                        CompareOp /*compareOp*/  = CompareOp::Always,
                                        StencilOp /*passOp*/     = StencilOp::Keep,
                                        uint32_t  /*reference*/  = 0,
                                        uint32_t  /*readMask*/   = 0xFFu,
                                        uint32_t  /*writeMask*/  = 0xFFu) {}

        // ====================================================================
        // DRAWING
        // ====================================================================

        virtual void DrawIndexed(MeshHandle mesh, uint32_t indexCount,
                                uint32_t indexOffset = 0) = 0;
        virtual void DrawArrays(MeshHandle mesh, uint32_t vertexCount,
                               uint32_t firstVertex = 0) = 0;

        // Unbind any currently-bound mesh/VAO to prevent state leakage.
        // GL: glBindVertexArray(0). VK: no-op.
        virtual void UnbindMesh() {}

        // ====================================================================
        // MEGA-BUFFER RENDERING
        // ====================================================================
        // Low-level buffer binding + draw for the mega-buffer slab pool pattern.
        // Use these instead of MeshHandle-based draws for chunk rendering.

        // Bind a raw vertex/index buffer for subsequent DrawIndexedBaseVertex calls.
        virtual void BindVertexBuffer(BufferHandle vbo, uint32_t stride) = 0;
        virtual void BindIndexBuffer(BufferHandle ibo) = 0;

        // Draw indexed geometry using currently-bound VBO + IBO with base vertex offset.
        virtual void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex) = 0;

        // Batched multi-draw with base vertex. GL: native glMultiDrawElementsBaseVertex.
        // Default: loops DrawIndexedBaseVertex.
        virtual void MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount);

        // ====================================================================
        // SHARED BLOCK VERTEX FORMAT
        // ====================================================================
        // GL: creates/binds a shared VAO with vertex attrib binding.
        // VK: no-op (vertex input is part of pipeline state).

        virtual void SetupBlockVertexFormat() {}
        virtual void BindBlockVertexFormat() {}
        virtual void DestroyBlockVertexFormat() {}

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
