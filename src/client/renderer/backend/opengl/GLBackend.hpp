// File: src/client/renderer/backend/opengl/GLBackend.hpp
#pragma once

#include "../RenderBackend.hpp"
#include <array>
#include <deque>
#include <glad/glad.h>
#include <string>
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
        // UpdateBufferUnsynchronized maps with GL_MAP_UNSYNCHRONIZED_BIT: the
        // driver queues about a frame of work, so two alternating sets (what
        // every streaming renderer kept before the count came from here).
        uint32_t FramesInFlight() const override { return 2; }
        const char* GetName() const override { return "OpenGL 3.3"; }
        GpuDeviceInfo GetDeviceInfo() const override { return m_deviceInfo; }
        GLFWwindow* GetWindow() const override { return m_window; }
        void SetVSync(bool enabled) override;

        // Frame
        void BeginFrame() override;
        void EndFrame(GLFWwindow* window) override;
        void SetClearColor(float r, float g, float b, float a) override;
        void Clear(bool color, bool depth, bool stencil = false) override;
        void SetViewport(int x, int y, int width, int height) override;
        void SetScissorRect(int x, int y, int w, int h) override;
        void ClearScissorRect() override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                 const void* data, BufferAccess access) override;
        void UpdateBufferStreaming(BufferHandle handle, size_t offset,
                                   size_t size, const void* data) override;
        void UpdateBufferUnsynchronized(BufferHandle handle, size_t offset,
                                        size_t size, const void* data) override;
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
        void ReserveTextureMipLevels(TextureHandle handle, int maxLevel) override;
        void UploadTextureMipLevel(TextureHandle handle, int level,
                                   int width, int height, const void* data) override;
        void UpdateTexture2DLevel(TextureHandle handle, int level, int x, int y,
                                  int width, int height, const void* data) override;
        void UpdateTexture2DLevelStaged(TextureHandle handle, int level, int x, int y,
                                        int width, int height, const void* data) override;
        TextureHandle CreateTexture2DArray(int width, int height, int layers, int mipLevels,
                                           TextureFormat format) override;
        int MaxTextureArrayLayers() const override;
        void UpdateTextureArrayLevel(TextureHandle handle, int layer, int level, int x, int y,
                                     int width, int height, const void* data) override;
        void UploadTextureArrayLevel(TextureHandle handle, int level, int width, int height, int layers,
                                     const void* data) override;
        void DestroyTexture(TextureHandle handle) override;
        void BindTexture(TextureHandle handle, uint32_t slot) override;
        TextureHandle CreateBufferTexture(BufferHandle buffer, TextureFormat format) override;
        uintptr_t GetNativeTextureID(TextureHandle handle) const override;

        // Shaders
        ShaderHandle CreateShader(const std::string& vertexSource,
                                 const std::string& fragmentSource) override;
        ShaderHandle CreateShaderFromFiles(const std::string& vertexPath,
                                          const std::string& fragmentPath) override;
        void DestroyShader(ShaderHandle handle) override;
        void BindShader(ShaderHandle handle) override;
        void SetUniformMat4(ShaderHandle handle, const std::string& name, const glm::mat4& value) override;
        void SetUniformVec4(ShaderHandle handle, const std::string& name, const glm::vec4& value) override;
        void SetUniformVec3(ShaderHandle handle, const std::string& name, const glm::vec3& value) override;
        void SetUniformVec2(ShaderHandle handle, const std::string& name, const glm::vec2& value) override;
        void SetUniformFloat(ShaderHandle handle, const std::string& name, float value) override;
        void SetUniformInt(ShaderHandle handle, const std::string& name, int value) override;
        void SetUniformIVec3(ShaderHandle handle, const std::string& name, const glm::ivec3& value) override;

        // Meshes
        MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                             const VertexLayout& layout) override;
        void DestroyMesh(MeshHandle handle) override;

        // Pipeline state
        void SetPipelineState(const PipelineState& state) override;
        void InvalidateStateCache() override;

        void CopyFramebufferToTexture(TextureHandle dst) override;
        bool CopyFramebufferDepthToTexture(TextureHandle dst) override;

        // Backbuffer read-back — see RenderBackend.hpp. GL reads the pixels
        // at Request time (glReadPixels stalls on the queue anyway).
        void SetTextureAnisotropy(TextureHandle handle, float maxAnisotropy) override;
        void UploadTextureRegionNow(TextureHandle handle, int level, int x, int y,
                                    int width, int height, const void* data) override;
        bool RequestBackbufferReadback(int x, int y, int w, int h) override;
        bool TakeBackbufferReadback(std::vector<uint8_t>& outRgba, int& outW, int& outH) override;

        // Render targets (offscreen FBOs) — see RenderBackend.hpp.
        RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
        void               DestroyRenderTarget(RenderTargetHandle rt) override;
        void               BindRenderTarget(RenderTargetHandle rt) override;
        TextureHandle      GetRenderTargetColorTexture(RenderTargetHandle rt) const override;
        void               ResizeRenderTarget(RenderTargetHandle rt, int w, int h) override;
        RenderTargetHandle CreateRenderTargetFromTextures(const TextureHandle* colors, int colorCount,
                                                          TextureHandle depth) override;
        void SetUniformIVec2(ShaderHandle handle, const std::string& name, const glm::ivec2& value) override;
        void BlitRenderTargetDepth(RenderTargetHandle src, RenderTargetHandle dst) override;
        bool CopyFramebufferToRenderTarget(RenderTargetHandle dst) override;
        // Scaled scene (Render Resolution) — see RenderBackend.hpp.
        void RequestScaledScene(int width, int height) override;
        bool ScaledSceneActive() const override { return m_sceneActive; }
        void GetScaledSceneSize(int& width, int& height) const override {
            if (m_sceneActive) { width = m_sceneWidth; height = m_sceneHeight; }
        }
        void ResolveScaledScene() override;
        // Improved Transparency: MC 26.3 OIT (see RenderBackend.hpp).
        void SetOitStage(OitStage stage, const glm::vec4& projParams, bool depthBoundsWriteDepth) override;
        bool OitEnsureTargets(int width, int height) override;
        void OitDestroyTargets() override;
        bool OitBeginPass(OitPass pass, bool clearColor) override;
        void OitEndPass() override;
        TextureHandle OitTexture(OitImage image) const override;
        ShaderHandle CreateOitShaderFromFiles(const std::string& vertexPath,
                                              const std::string& fragmentPath) override;
        bool FrameDepthPreserved() const override { return true; }   // FBO 0 keeps its depth
        void SetShaderOverrideMode(bool on, RenderTargetHandle defaultTarget) override;
        void SetShaderOverride(ShaderHandle engine, ShaderHandle pack, RenderTargetHandle target) override;
        void ClearShaderOverrides() override;
        std::vector<ShaderHandle> FindShadersBySource(
            const std::function<bool(const std::string&, const std::string&)>& match) override;
        void CheckErrors(const char* where) override;
        bool ReadDepthPixel(RenderTargetHandle rt, int x, int y, float& out) override;
        std::string DebugStateSummary() override;

        void SetStencilOverride(bool enabled,
                                CompareOp compareOp = CompareOp::Always,
                                StencilOp passOp     = StencilOp::Keep,
                                uint32_t  reference  = 0,
                                uint32_t  readMask   = 0xFFu,
                                uint32_t  writeMask  = 0xFFu) override;

        // Drawing
        void DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) override;
        MeshHandle CreateInstancedMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                       BufferHandle instanceBuffer,
                                       const VertexLayout& vertexLayout,
                                       const VertexLayout& instanceLayout) override;
        void DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount,
                                  uint32_t indexOffset, uint32_t instanceCount,
                                  uint32_t instanceByteOffset = 0) override;
        void DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) override;
        void UnbindMesh() override;

        // Mega-buffer rendering
        void BindVertexBuffer(BufferHandle vbo, uint32_t stride) override;
        void BindIndexBuffer(BufferHandle ibo) override;
        void BindUniformBuffer(BufferHandle handle, size_t offset, size_t size) override;
        void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                   IndexType indexType = IndexType::Uint32) override;
        void MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                        const int32_t* baseVertices, uint32_t drawCount,
                                        IndexType indexType = IndexType::Uint32) override;

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
        GpuDeviceInfo m_deviceInfo;

        // Handle → GL ID mappings
        uint32_t m_nextHandle = 1;
        uint32_t AllocHandle() { return m_nextHandle++; }

        struct GLBufferInfo {
            GLuint glId = 0;
            GLenum target = GL_ARRAY_BUFFER;
            size_t size = 0;
            uint64_t lastStreamFrame = 0;   // UpdateBufferStreaming: the frame that last wrote it
        };
        std::unordered_map<uint32_t, GLBufferInfo> m_buffers;

        struct GLTextureInfo {
            GLuint glId = 0;
            int width = 0, height = 0;
            size_t memorySize = 0;
            // Kept so an explicit mip-level upload re-declares the level with
            // the same storage the base was created with — re-uploading level 0
            // as RGBA8 over an SRGB8_A8 texture would silently drop the decode.
            GLenum internalFormat = GL_RGBA8;
            GLenum dataFormat     = GL_RGBA;
            GLenum dataType       = GL_UNSIGNED_BYTE;
            // GL_TEXTURE_2D, GL_TEXTURE_BUFFER for CreateBufferTexture or
            // GL_TEXTURE_2D_ARRAY for CreateTexture2DArray — BindTexture and
            // the parameter setters bind whichever the texture is. LAST on
            // purpose: CreateTexture2D initialises this struct positionally.
            GLenum target = GL_TEXTURE_2D;
        };
        std::unordered_map<uint32_t, GLTextureInfo> m_textures;

        // CopyFramebufferDepthToTexture: the first copy is checked for a GL
        // error once; a driver that refuses it is not asked again.
        bool m_depthCopyChecked = false;
        bool m_depthCopyBroken  = false;

        struct GLShaderInfo {
            GLuint programId = 0;
            mutable std::unordered_map<std::string, GLint> uniformCache;
            GLint GetUniform(const std::string& name) const;
            // Kept for FindShadersBySource (shader packs match the engine's
            // programs by what they declare).
            std::string vertexSource;
            std::string fragmentSource;
        };
        struct GLShaderOverride {
            ShaderHandle       shader = INVALID_SHADER;
            RenderTargetHandle target = INVALID_RENDER_TARGET;
        };
        std::unordered_map<uint32_t, GLShaderOverride> m_shaderOverrides;
        bool               m_overrideMode = false;
        RenderTargetHandle m_overrideDefaultTarget = INVALID_RENDER_TARGET;
        ShaderHandle ResolveShader(ShaderHandle handle) const {
            // An OIT stage (Improved Transparency) draws the engine shader's
            // variant; uniforms set on the engine handle land there.
            if (m_oitStage != OitStage::None) return ResolveOit(handle);
            if (!m_overrideMode) return handle;
            auto it = m_shaderOverrides.find(handle);
            return it != m_shaderOverrides.end() && it->second.shader != INVALID_SHADER ? it->second.shader : handle;
        }
        std::unordered_map<uint32_t, GLShaderInfo> m_shaders;

        struct GLMeshInfo {
            GLuint vao = 0;
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
            // Instanced meshes: the per-instance buffer and its layout, kept
            // so DrawIndexedInstanced can re-point the instance attributes at
            // a byte offset (GL 4.1 has no base-instance draw).
            BufferHandle instanceBuffer = INVALID_BUFFER;
            VertexLayout instanceLayout;
            uint32_t     lastInstanceOffset = 0;
        };
        std::unordered_map<uint32_t, GLMeshInfo> m_meshes;

        struct GLTimerInfo {
            GLuint queryId = 0;
            bool active = false;
            bool resultReady = false;
            float resultMs = 0.0f;
        };
        std::unordered_map<uint32_t, GLTimerInfo> m_timers;

        // Render target (FBO + color texture + depth attachment).
        struct GLRenderTargetInfo {
            GLuint        fbo               = 0;
            GLuint        depthRBO          = 0;  // renderbuffer for depth (if no depth texture); 0 = none (RenderTargetDesc::depth false)
            bool          hasDepth          = true;
            TextureHandle colorTexture      = INVALID_TEXTURE;
            // CreateRenderTargetFromTextures: the framebuffer only borrows
            // its attachments, so Destroy leaves them alone.
            bool          ownsTextures      = true;
            int           width             = 0;
            int           height            = 0;
            TextureFormat colorFormat       = TextureFormat::RGBA16F;
            TextureFormat depthFormat       = TextureFormat::Depth24Stencil8;
        };
        std::unordered_map<uint32_t, GLRenderTargetInfo> m_renderTargets;

        // Memory tracking
        GPUMemoryStats m_memStats;

        // Render state cache (avoid redundant GL calls)
        PipelineState m_currentState;
        bool m_stateInitialized = false;

        // Stencil override (see RenderBackend::SetStencilOverride).
        struct StencilOverride {
            bool      enabled    = false;
            CompareOp compareOp  = CompareOp::Always;
            StencilOp passOp     = StencilOp::Keep;
            uint32_t  reference  = 0;
            uint32_t  readMask   = 0xFFu;
            uint32_t  writeMask  = 0xFFu;
        };
        StencilOverride m_stencilOverride;

        // ── Improved Transparency: MC 26.3 OIT ──────────────────────────
        // One target set (OpenGL has no frame overlap). The OIT passes can't
        // attach FBO 0's depth, so they test against `depthCopy`, a D24S8
        // snapshot of it taken when the depth-bounds pass opens (the cull
        // writes into the snapshot; the composite writes the frame's).
        struct OitTargets {
            int width = 0, height = 0;
            TextureHandle depthBounds = INVALID_TEXTURE, culled = INVALID_TEXTURE;
            TextureHandle coeff0 = INVALID_TEXTURE, coeff1 = INVALID_TEXTURE, accumulate = INVALID_TEXTURE;
            TextureHandle depthCopy = INVALID_TEXTURE, cloudDepth = INVALID_TEXTURE;
            TextureHandle dummy = INVALID_TEXTURE;       // 1x1: a sampler no stage reads
            std::array<RenderTargetHandle, 7> fbos{};    // by OitPass
        };
        OitTargets    m_oit;
        // Texture units of the stage samplers (MC's DepthBoundsSampler,
        // Coeff0, Coeff1) — above every unit the engine's renderers use.
        static constexpr uint32_t kOitUnitDepthBounds = 12;
        static constexpr uint32_t kOitUnitCoeff0 = 13;
        static constexpr uint32_t kOitUnitCoeff1 = 14;
        bool          m_oitPassOpen = false;
        OitStage      m_oitStage = OitStage::None;
        OitStage      m_oitSamplerStage = OitStage::None;   // last non-None: which samplers are bound
        glm::vec4     m_oitProjParams{0.0f};
        bool          m_oitDbWritesDepth = false;
        bool          m_oitSkipDraw = false;                // bound shader has no OIT variant
        // Per engine shader, its variant per stage (INVALID = compile failed
        // or never asked; `tried` separates the two).
        struct OitVariants { std::array<ShaderHandle, 4> shader{}; std::array<bool, 4> tried{}; };
        std::unordered_map<uint32_t, OitVariants> m_oitVariants;
        std::string   m_oitLibrary;                         // shaders/oit_lib.glsl, read once
        PipelineState m_requestedState;                     // the caller's, re-spliced on a stage change
        ShaderHandle  OitVariantFor(ShaderHandle engine);
        bool          SpliceOitLibrary(std::string& fragmentSource);
        void          AssignOitSamplerUnits(ShaderHandle shader);
        ShaderHandle  ResolveOit(ShaderHandle handle) const;

        // Window reference
        GLFWwindow* m_window = nullptr;

        // ── Scaled scene (RequestScaledScene) ──────────────────────────
        // "The frame" is m_defaultFbo: 0 (the window) normally, the scene
        // FBO from a scaled frame's BeginFrame until ResolveScaledScene.
        // Every place that used to name FBO 0 as the frame names this. The
        // scene's depth-stencil is a D24S8 TEXTURE, the window's own format,
        // so CopyFramebufferDepthToTexture copies from it exactly as from
        // the window. Raw GL objects, not handles: nothing outside sees them.
        GLuint m_defaultFbo     = 0;
        GLuint m_sceneFbo       = 0;
        GLuint m_sceneColorTex  = 0;
        GLuint m_sceneDepthTex  = 0;
        int    m_sceneWidth     = 0, m_sceneHeight = 0;     // the allocated size
        int    m_sceneReqWidth  = 0, m_sceneReqHeight = 0;  // RequestScaledScene, for the next BeginFrame
        bool   m_sceneActive    = false;
        bool   m_sceneBroken    = false;                    // incomplete FBO once: native for the session
        int    m_sceneMaxDim    = 0;                        // min(max renderbuffer, max texture); 0 = not asked yet
        bool   EnsureSceneTarget(int width, int height);
        void   DestroySceneTarget();
        // Tracked from SetViewport so SetScissorRect can flip a top-left rect
        // into GL's bottom-left origin. glGet on every scissor call would be a
        // driver round-trip; this costs a store per viewport change.
        int m_viewportHeight = 0;

        // Requested read-backs, oldest first. glReadPixels copies at the
        // request, so each is ready at once; they wait here in order.
        struct Readback {
            std::vector<uint8_t> pixels;
            int w = 0, h = 0;
        };
        std::deque<Readback> m_readbacks;

        // Currently bound handles
        ShaderHandle m_boundShader = INVALID_SHADER;

        // UpdateTexture2DLevelStaged: one pixel-unpack buffer split into
        // kUploadSlots per-frame regions. A frame's writes fill its region
        // (mapped unsynchronized — the fence proves the GPU is done with it)
        // and each glTexSubImage2D reads from there; EndFrame fences the
        // region and moves on.
        static constexpr int kUploadSlots = 3;
        struct UploadSlot {
            GLsync fence = nullptr;
            size_t used = 0;
        };
        GLuint     m_uploadPbo = 0;
        size_t     m_uploadSlotSize = 0;
        UploadSlot m_uploadSlots[kUploadSlots];
        // The staged copy itself: a 2D texture's level (`layer` < 0) or an
        // array's layer, through the ring, with the direct sub-image as the
        // fallback when the ring cannot take it.
        void StagedSubImage(TextureHandle handle, int layer, int level, int x, int y,
                            int width, int height, const void* data);
        int        m_uploadSlot = 0;
        bool       m_uploadSlotReady = false;   // this frame's region waited on
        void EndUploadFrame();
        void DestroyUploadBuffer();

        // Frame fences (UpdateBufferStreaming): one GLsync per frame, placed
        // at EndFrame, kept for kFrameFences frames — the proof that a
        // frame's draws have run. Frame f's is m_frameFences[f % kFrameFences];
        // m_frameNumber is the frame being recorded. A frame older than the
        // ring is taken as complete: GL cannot queue 8 frames ahead of the
        // GPU here (the swapchain holds three).
        static constexpr int kFrameFences = 8;
        GLsync   m_frameFences[kFrameFences] = {};
        uint64_t m_frameNumber = 1;
        void WaitFrameComplete(uint64_t frame);

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
        GLenum ToGLStencilOp(StencilOp op) const;
        std::string ReadFileContents(const std::string& path) const;
        GLuint CompileGLShader(GLenum type, const std::string& source) const;
    };

} // namespace Render
