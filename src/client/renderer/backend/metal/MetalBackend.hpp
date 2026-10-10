// File: src/client/renderer/backend/metal/MetalBackend.hpp
#pragma once

#ifdef HAS_METAL

// Objective-C++ only: included by the metal/*.mm files, never by renderers
// (they see RenderBackend; RenderBackend.cpp reaches this through
// CreateMetalBackend()).
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "../RenderBackend.hpp"
#include "../SpirvUniforms.hpp"
#include "MetalBindings.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <vector>

#ifdef TRACY_ENABLE
// Tracy GPU zones, one per encoder, fed by the backend's own stage-boundary
// timestamps (MetalInstrumentation.mm). Not TracyMetal.hmm: its zone sets
// startOfVertex + endOfFragment with the other two indices DontSample on a
// second sample-buffer attachment, which this GPU never writes (every zone
// came back as Tracy's 5 ns "gave up" stub, 2026-10-06).
#include <tracy/Tracy.hpp>
#else
namespace tracy { struct SourceLocationData; }
#endif

namespace Render {

    // The native Metal backend (--metal). It draws with the Vulkan shader
    // family — the MSL that tools/gen_metal_shaders.py generates from the
    // _vk SPIR-V — and therefore with Vulkan's conventions and VKBackend's
    // design wherever that design encodes an engine rule:
    //
    //   - frames overlap on the GPU (kFramesInFlight = 2): nothing a frame
    //     writes is an object the previous frame may still use — per-slot
    //     depth, scene and stand-in images, per-frame copies of a texture
    //     updated while frames sample it, per-slot uniform rings;
    //   - the frame is one render pass on the drawable, suspended around
    //     render targets, Improved Transparency passes, copies and read-backs,
    //     and opened lazily (EnsureEncoder), so the half-res rain can be drawn
    //     ahead of it from the previous frame's depth;
    //   - vsync off is a mailbox: a frame draws into a real drawable only
    //     when CoreAnimation has one free, else into its slot's stand-in,
    //     unpresented (see MailboxDrawableFree).
    //
    // What Metal does for us and Vulkan made us do: hazard tracking (no
    // barriers, no layouts), resource lifetime (a command buffer retains what
    // it uses, so every destroy is immediate and safe), dynamic depth/stencil,
    // cull, winding, fill and bias state (pipelines key on shader, blend and
    // attachment formats alone), and clip-space Y (Metal's NDC is y-up, so
    // Vulkan's flipped viewport is Metal's plain one — see ToMetalViewport).
    class MetalBackend : public RenderBackend {   // Metal4Backend derives (docs/metal4.md)
    public:
        // Three slots, ONE frame queued (frame pacing). With two slots the
        // CPU could not begin frame N until N-2 had completed, long after the
        // GPU had finished N-1's vertex stage: the tiler idled until N
        // arrived (Metal System Trace 2026-10-07: one GPU channel active
        // 67 % of the time, both 28 %). Three slots with the plain semaphore
        // fixed that (+6 % mean) but let the CPU queue two frames: 1 %-lows
        // −8 %, GPU start latency 0.18 -> 0.53 ms. So the slots are three
        // for resource safety, and BeginFrame also waits until frame N-1 has
        // STARTED on the GPU (SharedState::frameStarted, signalled by the
        // command buffer's scheduled handler): at most one frame waits
        // behind the executing one. OBEY_MTL_PACING=0 drops that wait (the
        // plain three-deep queue, for the A/B).
        // Measured (cooled ABBA, tour at RD 32 in rain, 2026-10-07): 2 slots
        // 267 / 235 fps, 1 %-low 91 / 93 — paced 3 slots 270 / 312 fps,
        // 1 %-low 97 / 120, 0.1 %-low 75 / 96 vs 62 / 79. The pace gate
        // itself waits ~20 µs a frame (the semaphore paces); it is the
        // insurance for a GPU hitch, when it stops the CPU from queueing
        // two more frames behind it. Cost: GPU start latency +0.2 ms after
        // commit, +1 slot of per-frame images (~140 MB at 3420x2146 before
        // the memoryless depth gives 110 MB back).
        static constexpr int kFramesInFlight = 3;

        MetalBackend();
        ~MetalBackend() override;

        // Lifecycle
        bool Initialize(GLFWwindow* window) override;
        void Shutdown() override;
        BackendType GetType() const override { return BackendType::Metal; }
        const char* GetName() const override { return "Metal"; }
        GpuDeviceInfo GetDeviceInfo() const override;
        GLFWwindow* GetWindow() const override { return m_window; }
        void SetVSync(bool enabled) override;

        // Frame
        void BeginFrame() override;
        void EndFrame(GLFWwindow* window) override;
        void WarmPipelines() override;
        void SetClearColor(float r, float g, float b, float a) override;
        void Clear(bool color, bool depth, bool stencil = false) override;
        void SetViewport(int x, int y, int width, int height) override;
        void SetScissorRect(int x, int y, int w, int h) override;
        void ClearScissorRect() override;
        bool RequestBackbufferReadback(int x, int y, int w, int h) override;
        bool TakeBackbufferReadback(std::vector<uint8_t>& outRgba, int& outW, int& outH) override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size, const void* data,
                                  BufferAccess access = BufferAccess::Static) override;
        void UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) override;
        void UpdateBufferUnsynchronized(BufferHandle handle, size_t offset, size_t size, const void* data) override;
        void DestroyBuffer(BufferHandle handle) override;
        void DeferredDestroyBuffer(BufferHandle handle) override { DestroyBuffer(handle); }
        void BindUniformBuffer(BufferHandle handle, size_t offset, size_t size) override;
        const void* DebugGetMappedBufferPtr(BufferHandle handle) const override;

        // Textures
        TextureHandle CreateTexture2D(int width, int height, TextureFormat format, const void* data) override;
        TextureHandle CreateEmptyTexture2D(int width, int height, TextureFormat format, int maxLevel) override;
        TextureHandle CreateTexture2DArray(int width, int height, int layers, int mipLevels,
                                           TextureFormat format) override;
        int MaxTextureArrayLayers() const override { return 2048; }   // MTLTextureDescriptor.arrayLength's limit
        void UpdateTextureArrayLevel(TextureHandle handle, int layer, int level, int x, int y,
                                     int width, int height, const void* data) override;
        void UploadTextureArrayLevel(TextureHandle handle, int level, int width, int height, int layers,
                                     const void* data) override;
        void UpdateTexture2D(TextureHandle handle, int x, int y, int width, int height, const void* data) override;
        void SetTextureFilter(TextureHandle handle, TextureFilter min, TextureFilter mag) override;
        void SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) override;
        void GenerateMipmaps(TextureHandle handle) override;
        void SetTextureAnisotropy(TextureHandle handle, float maxAnisotropy) override;
        void ReserveTextureMipLevels(TextureHandle handle, int maxLevel) override;
        void UploadTextureRegionNow(TextureHandle handle, int level, int x, int y,
                                    int width, int height, const void* data) override;
        void UploadTextureMipLevel(TextureHandle handle, int level, int width, int height,
                                   const void* data) override;
        void UpdateTexture2DLevel(TextureHandle handle, int level, int x, int y,
                                  int width, int height, const void* data) override;
        void DestroyTexture(TextureHandle handle) override;
        void DeferredDestroyTexture(TextureHandle handle) override { DestroyTexture(handle); }
        void BindTexture(TextureHandle handle, uint32_t slot = 0) override;
        TextureHandle CreateBufferTexture(BufferHandle buffer, TextureFormat format) override;
        uintptr_t GetNativeTextureID(TextureHandle handle) const override;

        // Render targets
        RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
        void DestroyRenderTarget(RenderTargetHandle rt) override;
        void BindRenderTarget(RenderTargetHandle rt) override;
        void BindRenderTargetOverwriting(RenderTargetHandle rt) override;
        TextureHandle GetRenderTargetColorTexture(RenderTargetHandle rt) const override;
        void ResizeRenderTarget(RenderTargetHandle rt, int w, int h) override;
        bool CopyFramebufferToRenderTarget(RenderTargetHandle dst) override;

        // Scaled scene (Render Resolution)
        void RequestScaledScene(int width, int height) override;
        bool ScaledSceneActive() const override { return m_sceneActive; }
        void GetScaledSceneSize(int& width, int& height) const override {
            if (m_sceneActive) { width = static_cast<int>(m_sceneWidth); height = static_cast<int>(m_sceneHeight); }
        }
        void ResolveScaledScene() override;

        // Improved Transparency
        void SetOitStage(OitStage stage, const glm::vec4& projParams, bool depthBoundsWriteDepth) override;
        bool OitEnsureTargets(int width, int height) override;
        void OitDestroyTargets() override;
        bool OitBeginPass(OitPass pass, bool clearColor) override;
        void OitEndPass() override;
        TextureHandle OitTexture(OitImage image) const override;
        ShaderHandle CreateOitShaderFromFiles(const std::string& vertexPath, const std::string& fragmentPath) override;

        // Frame depth, handoff, depth bands
        void SetFrameDepthPreserved(bool preserved, FrameDepthUser user = FrameDepthUser::ImprovedTransparency) override;
        bool FrameDepthPreserved() const override { return m_frameDepthPreserved; }
        // Every frame depth texture is sampleable here: being capable costs nothing.
        void SetFrameDepthCapable(bool /*capable*/) override {}
        void SetDepthHandoff(bool handoff) override { m_depthHandoff = handoff; }
        TextureHandle PreviousFrameDepthTexture() override;
        uint64_t FrameNumber() const override { return m_frameNumber; }
        uint32_t FramesInFlight() const override { return kFramesInFlight; }
        bool PrefersBufferPrefetch() const override { return m_metal4; }
        void SetDepthRange(float minDepth, float maxDepth) override;
        void ClearDepthRect(int x, int y, int width, int height, float depth) override;
        bool FrameDepthIsFloat() const override { return true; }   // Depth32Float_Stencil8
        TextureHandle FrameDepthTexture() override;
        uint32_t FrameSlot() const override { return m_currentFrame; }

        // Shaders
        ShaderHandle CreateShader(const std::string& vertexSource, const std::string& fragmentSource) override;
        ShaderHandle CreateShaderFromFiles(const std::string& vertexPath, const std::string& fragmentPath) override;
        ShaderHandle CreateShaderFromFilesPortal(const std::string& vertexPath, const std::string& fragmentPath) override;
        void RegisterShaderVertexLayout(ShaderHandle shader, const VertexLayout& layout) override;
        void RegisterShaderInstanceLayout(ShaderHandle shader, const VertexLayout& layout) override;
        void SetShaderIgnoresCommonMatrices(ShaderHandle shader) override;
        void DestroyShader(ShaderHandle handle) override;
        void BindShader(ShaderHandle handle) override;
        // A pack program (CreatePackShader) takes its uniforms into its own
        // block by name; every other shader through the fixed layout.
        void SetUniformMat4(ShaderHandle h, const std::string& name, const glm::mat4& value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetMat4(name, value); }
        void SetUniformVec4(ShaderHandle h, const std::string& name, const glm::vec4& value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetVec4(name, value); }
        void SetUniformVec3(ShaderHandle h, const std::string& name, const glm::vec3& value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetVec3(name, value); }
        void SetUniformVec2(ShaderHandle h, const std::string& name, const glm::vec2& value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetVec2(name, value); }
        void SetUniformFloat(ShaderHandle h, const std::string& name, float value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetFloat(name, value); }
        void SetUniformInt(ShaderHandle h, const std::string& name, int value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetInt(name, value); }
        void SetUniformIVec3(ShaderHandle h, const std::string& name, const glm::ivec3& value) override { if (!SetPackUniform(h, name, &value, sizeof(value))) m_uniforms.SetIVec3(name, value); }
        void SetUniformIVec2(ShaderHandle h, const std::string& name, const glm::ivec2& value) override { SetPackUniform(h, name, &value, sizeof(value)); }
        bool PackShadersSupported() const override { return true; }
        ShaderHandle CreatePackShader(const PackShaderDesc& desc) override;
        void SetShaderOverrideMode(bool on, RenderTargetHandle defaultTarget) override;
        void SetShaderOverride(ShaderHandle engine, ShaderHandle pack, RenderTargetHandle target) override;
        void ClearShaderOverrides() override;
        std::vector<ShaderHandle> FindShadersBySource(
            const std::function<bool(const std::string&, const std::string&)>& match) override;
        RenderTargetHandle CreateRenderTargetFromTextures(const TextureHandle* colors, int colorCount, TextureHandle depth) override;
        void BlitRenderTargetDepth(RenderTargetHandle src, RenderTargetHandle dst) override;
        bool CopyTexture(TextureHandle src, TextureHandle dst) override;

        // Meshes
        MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer, const VertexLayout& layout) override;
        MeshHandle CreateInstancedMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer, BufferHandle instanceBuffer,
                                       const VertexLayout& vertexLayout, const VertexLayout& instanceLayout) override;
        void DestroyMesh(MeshHandle handle) override;
        void DeferredDestroyMesh(MeshHandle handle) override { DestroyMesh(handle); }

        // Pipeline state
        void SetPipelineState(const PipelineState& state) override;
        void InvalidateStateCache() override {}
        void SetStencilOverride(bool enabled, CompareOp compareOp, StencilOp passOp, uint32_t reference,
                                uint32_t readMask, uint32_t writeMask) override;

        // Drawing
        void DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset = 0) override;
        void DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset,
                                  uint32_t instanceCount, uint32_t instanceByteOffset = 0) override;
        void DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex = 0) override;
        void BindVertexBuffer(BufferHandle vbo, uint32_t stride) override;
        void BindIndexBuffer(BufferHandle ibo) override;
        void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                   IndexType indexType = IndexType::Uint32) override;
        void MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                        const int32_t* baseVertices, uint32_t drawCount,
                                        IndexType indexType = IndexType::Uint32) override;

        // GPU debug groups — see m_debugGroups.
        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;

        // GPU timers and instrumentation — MetalInstrumentation.mm, see m_gpu.
        GPUTimerHandle BeginGPUTimer(const std::string& name) override;
        void EndGPUTimer(GPUTimerHandle handle) override;
        float GetGPUTimerResultMs(GPUTimerHandle handle) override;
        void SetGpuTimersEnabled(bool enabled) override { m_gpu.requested = enabled; }
        bool GpuTimersActive() const override { return m_gpu.active; }
        float GetLastFrameGpuMs() const override { return m_gpu.lastFrameGpuMs; }
        bool GetGpuEncoderTimings(std::vector<GpuEncoderTiming>& out) const override;
        bool RequestGpuCapture() override;
        void SetDebugLabel(DebugLabelKind kind, uint32_t handle, const char* label) override;
        std::string DebugStateSummary() override;

        GPUMemoryStats GetMemoryStats() const override { return m_memStats; }

        // ImGui
        void ImGuiInit(GLFWwindow* window) override;
        void ImGuiNewFrame() override;
        void ImGuiRender() override;
        void ImGuiShutdown() override;

    protected:
        // ── Device and presentation ──────────────────────────────────────
        GLFWwindow*         m_window = nullptr;
        id<MTLDevice>       m_device = nil;
        id<MTLCommandQueue> m_queue  = nil;
        CAMetalLayer*       m_layer  = nil;
        bool                m_appleGpu = false;        // an Apple-family (tile-based) GPU
        // Storage for CPU-written buffers: shared on unified memory (Apple
        // silicon, Intel integrated), managed on a discrete GPU so the GPU
        // reads its own copy instead of system memory over PCIe.
        MTLResourceOptions  m_hostVisibleOptions = MTLResourceStorageModeShared;
        bool                m_managedBuffers = false;
        uint32_t            m_drawableWidth = 0, m_drawableHeight = 0;
        static constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
        static constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;
        // Resizes the layer's drawables and every window-sized image.
        void SyncDrawableSize();

        bool m_vsync = true;
        // Vsync off is tear-free: the layer keeps display sync, the mailbox
        // below decides which frame is handed to the next refresh and the
        // rest are never presented, so the frame rate stays uncapped and a
        // shown frame is at most one refresh old (Fast Sync / Enhanced Sync
        // on the PC drivers). OBEY_MTL_TEARING=1 turns display sync off with
        // vsync, for latency tests: the presented frame flips mid-scan.
        bool m_tearing = false;
        void ApplyDisplaySync();
        // Mailbox (vsync off; OBEY_MTL_MAILBOX=0 turns it off): a frame is
        // shown only if a drawable can be had without waiting — no sooner
        // than half a refresh after the last present, and with at most
        // m_mailboxPending presents not yet on screen. Otherwise it draws
        // into its slot's stand-in and is never presented. nextDrawable is
        // asked only when the frame will be shown.
        bool m_mailboxWanted = true;
        int  m_mailboxPending = 1;
        uint64_t m_presentsIssued = 0;
        std::chrono::steady_clock::time_point m_lastPresent{};
        std::chrono::steady_clock::duration   m_presentInterval{};
        bool MailboxDrawableFree() const;

        // State the command buffers' handlers write, shared so a handler
        // that fires after Shutdown touches nothing freed.
        struct SharedState {
            std::atomic<uint64_t> completedFrame{0};   // highest frame number whose command buffer completed
            std::atomic<uint64_t> presentsDone{0};     // presented (or dropped) drawables
            dispatch_semaphore_t  framesInFlight = nullptr;
            dispatch_semaphore_t  frameStarted   = nullptr;   // one signal per frame, when the GPU begins it
            dispatch_semaphore_t  uploadsFree    = nullptr;   // Metal 4: upload command buffers the GPU is done with
        };
        std::shared_ptr<SharedState> m_shared;

        // ── Frame ────────────────────────────────────────────────────────
        uint32_t m_currentFrame = 0;          // frame slot being recorded
        uint64_t m_frameNumber  = 0;          // advanced by every BeginFrame that records
        bool     m_frameActive  = false;
        bool     m_pacing       = true;       // wait for frame N-1 to start before recording N (see kFramesInFlight)
        id<MTLCommandBuffer> m_cmd = nil;     // this frame's
        std::array<id<MTLCommandBuffer>, kFramesInFlight> m_slotCommandBuffers{};   // each slot's last
        // The drawable is asked for when the frame first needs its colour
        // (FrameColorTexture), as late as possible; m_presenting says
        // whether this frame draws into it or into the stand-in.
        id<CAMetalDrawable> m_drawable = nil;
        bool m_drawableDecided = false;
        bool m_presenting = false;
        // SetClearColor's colour, and the one the frame's own pass clears to
        // when it opens (BeginFrame's, or a Clear before the pass opened).
        std::array<float, 4> m_clearColor{0.5f, 0.7f, 1.0f, 1.0f};
        std::array<float, 4> m_frameClearColor{0.5f, 0.7f, 1.0f, 1.0f};

        struct SlotImages {
            id<MTLTexture> depth   = nil;    // the frame's depth (window size)
            id<MTLTexture> standIn = nil;    // mailbox: the colour of a frame not shown
            id<MTLTexture> sceneColor = nil; // scaled scene (Render Resolution != 100 %)
            id<MTLTexture> sceneDepth = nil;
            // Per-draw uniform ring: Common / Bones copies (256-byte aligned
            // windows); replaced by a bigger one when a frame runs it dry.
            id<MTLBuffer>  ring = nil;
            size_t         ringUsed = 0;
            TextureHandle  depthTexture = INVALID_TEXTURE;   // FrameDepthTexture's entry over `depth`
            TextureHandle  sceneDepthTexture = INVALID_TEXTURE;
            // Storage of `depth` / `sceneDepth` right now (SettleDepthStorage).
            bool depthMemoryless = false;
            bool sceneDepthMemoryless = false;
        };
        std::array<SlotImages, kFramesInFlight> m_slots{};
        static constexpr size_t kRingInitialBytes = 4u << 20;
        // A 256-aligned window of `bytes` in this frame's ring (offset out).
        id<MTLBuffer> RingAllocate(size_t bytes, size_t& outOffset, const void* data);
        bool CreateWindowImages();

        // ── Passes ───────────────────────────────────────────────────────
        // Whatever receives draws now: the frame (drawable or scene), a render
        // target, or an Improved Transparency pass. A pass is BEGUN as a
        // descriptor and OPENED (its encoder made) on first use; a clear
        // before that folds into its load actions. Leaving a pass that never
        // opened costs nothing unless it had a clear to perform.
        enum class PassKind : uint8_t { None, Frame, Target, Oit };
        struct Pass {
            PassKind kind = PassKind::None;
            RenderTargetHandle target = INVALID_RENDER_TARGET;
            MTLRenderPassDescriptor* desc = nil;
            uint32_t width = 0, height = 0;
            uint8_t  config = 0;          // attachment formats (pipeline key): 0 frame / target, 1-3 OIT, 4 target without depth
            uint8_t  colorCount = 1;
            bool     clears = false;      // an unopened pass with clears to perform
            std::vector<MTLPixelFormat> formats;   // configs 5 / 6: the attachments' formats
            uint32_t attachmentsKey = 0;
        };
        Pass m_pass;
        // MTLRenderCommandEncoder, or MTL4RenderCommandEncoder on the
        // Metal 4 path (m_metal4): the two answer the same selectors for
        // state, debug groups, store actions and non-indexed draws, so one
        // `id` serves both; binding, indexed draws and the encoder's
        // creation branch (MetalFour.mm).
        id m_encoder = nil;
        bool m_frameOpened = false;       // the frame's own pass has opened this frame
        // EndFrame's own EndPass: the frame's last segment. An unpresented
        // (mailbox stand-in) frame's colour is read by nothing afterwards,
        // so that segment's store is DontCare — 29 MB a frame at 3420x2146
        // that never left the tile memory (OBEY_STANDIN_STORE=1 stores it,
        // the A/B). Every other segment stores: a resumed pass loads it, a
        // readback copies it, the drawable shows it.
        bool m_endingFrame = false;
        RenderTargetHandle m_activeTarget = INVALID_RENDER_TARGET;
        bool m_bindDiscardColor = false;   // BindRenderTargetOverwriting: the bind's colour loads are DontCare
        void BeginFramePass(bool resume);
        bool EnsureEncoder();
        void EndPass();
        id<MTLTexture> FrameColorTexture();
        id<MTLTexture> FrameDepthTex(uint32_t slot) const {
            return m_sceneActive ? m_slots[slot].sceneDepth : m_slots[slot].depth;
        }
        uint32_t FrameWidth() const  { return m_sceneActive ? m_sceneWidth : m_drawableWidth; }
        uint32_t FrameHeight() const { return m_sceneActive ? m_sceneHeight : m_drawableHeight; }
        uint32_t ActiveWidth() const  { return m_pass.kind != PassKind::None ? m_pass.width : FrameWidth(); }
        uint32_t ActiveHeight() const { return m_pass.kind != PassKind::None ? m_pass.height : FrameHeight(); }

        // Viewport (a Vulkan-convention one, see ToMetalViewport), scissor
        // and depth range: CPU state applied to every encoder.
        struct VkStyleViewport { float x, y, width, height; };
        VkStyleViewport m_viewport{0, 0, 0, 0};
        float m_depthRangeMin = 0.0f, m_depthRangeMax = 1.0f;
        MTLScissorRect m_scissor{0, 0, 0, 0};
        void ApplyViewport();
        void ApplyScissor();
        static MTLViewport ToMetalViewport(const VkStyleViewport& v, float zMin, float zMax);

        // The bindings the open encoder holds, so a draw that repeats the
        // previous draw's state records nothing for it. A fresh encoder's
        // state is Metal's defaults (cull none, clockwise front, fill, no
        // bias, clip, stencil reference 0), which the initial values mirror
        // (as CullMode / FrontFace / PolygonMode integers); Unknown() is for
        // after code we do not control (ImGui) recorded into the encoder.
        struct EncoderCache {
            __unsafe_unretained id<MTLRenderPipelineState> pipeline = nil;
            __unsafe_unretained id<MTLDepthStencilState>   depthStencil = nil;
            // -1: not set on this encoder yet (PrepareDraw's winding value is
            // 1 for a counter-clockwise front, 0 for clockwise — an
            // initial 1 once matched it and left a fresh encoder on Metal's
            // clockwise default, culling the whole world's front faces).
            int cull = 0, winding = -1, fill = 0, clip = 0;
            bool  depthBiasValid = true;
            float biasConstant = 0.0f, biasSlope = 0.0f;
            uint32_t stencilRef = 0;
            bool viewportValid = false;
            MTLViewport viewport{};
            bool scissorValid = false;
            MTLScissorRect scissor{};
            __unsafe_unretained id<MTLBuffer> vertexBuffers[31] = {};
            NSUInteger vertexOffsets[31] = {};
            __unsafe_unretained id<MTLBuffer> fragmentBuffers[8] = {};
            NSUInteger fragmentOffsets[8] = {};
            __unsafe_unretained id<MTLTexture> vertexTextures[MetalBindings::kTextureIndices] = {};
            __unsafe_unretained id<MTLTexture> fragmentTextures[MetalBindings::kTextureIndices] = {};
            __unsafe_unretained id<MTLSamplerState> vertexSamplers[MetalBindings::kTextureIndices] = {};
            __unsafe_unretained id<MTLSamplerState> fragmentSamplers[MetalBindings::kTextureIndices] = {};
            bool pushValid = false;
            PushConstantBlock push;
            static EncoderCache Unknown() {
                EncoderCache c;
                c.cull = c.winding = c.fill = c.clip = -1;
                c.depthBiasValid = false;
                c.stencilRef = 0xFFFFFFFFu;
                return c;
            }
        };
        EncoderCache m_cache;

        // Debug groups (PushDebugGroup): Metal keeps them per encoder, a stage
        // may begin before its encoder opens or span a pass break, so the
        // logical stack lives here — replayed onto each encoder as it opens,
        // closed on it before it ends. Names are string literals; their
        // NSStrings are made once.
        std::vector<const char*> m_debugGroups;
        size_t m_encoderGroups = 0;   // how many of m_debugGroups the open encoder holds
        std::unordered_map<const char*, NSString*> m_debugGroupNames;
        NSString* DebugGroupName(const char* name);

        // ── Clears inside a pass ─────────────────────────────────────────
        // A clear after the pass has opened is a full-rect quad (Metal has no
        // in-pass clear; MoltenVK draws one too), with its own small
        // pipelines (by attachment config) and depth-stencil states.
        id<MTLLibrary> m_internalLibrary = nil;
        std::unordered_map<uint64_t, id<MTLRenderPipelineState>> m_clearPipelines;   // config | colour | attachment set
        bool CreateInternalPipelines();
        void DrawClearQuad(bool color, bool depth, bool stencil, float depthValue,
                           const MTLScissorRect& rect);

        // ── Uniforms ─────────────────────────────────────────────────────
        SpirvUniformState m_uniforms;
        // The ring windows of the last Common / Bones copies this frame.
        __unsafe_unretained id<MTLBuffer> m_commonBuffer = nil;
        size_t m_commonOffset = 0;
        bool   m_haveCommon = false;
        __unsafe_unretained id<MTLBuffer> m_bonesBuffer = nil;
        size_t m_bonesOffset = 0;
        bool   m_haveBones = false;
        BufferHandle m_boundUniformBuffer = INVALID_BUFFER;
        size_t       m_boundUniformOffset = 0;

        // ── Resources ────────────────────────────────────────────────────
        uint32_t m_nextHandle = 1;
        uint32_t AllocHandle() { return m_nextHandle++; }

        struct BufferInfo {
            id<MTLBuffer> buffer = nil;
            size_t        size = 0;
            size_t        allocated = 0;         // the driver's allocatedSize (what TrackAlloc counted)
            BufferUsage   usage = BufferUsage::Vertex;
            bool          hostVisible = false;   // Dynamic / Streaming: written through contents
            std::string   label;                 // SetDebugLabel's, else a creation-time default
        };
        std::unordered_map<uint32_t, BufferInfo> m_buffers;

        struct TextureInfo {
            id<MTLTexture> texture = nil;
            int      width = 0, height = 0;
            uint32_t mipLevels = 1;
            uint32_t layers = 1;              // > 1: a 2D array (CreateTexture2DArray)
            MTLPixelFormat format = MTLPixelFormatRGBA8Unorm;
            uint32_t bytesPerPixel = 4;
            size_t   memorySize = 0;
            bool     owned = true;            // false: a view of an image owned elsewhere (target, frame depth, OIT)
            bool     bufferTexture = false;   // CreateBufferTexture (texture_buffer)
            // Sampler state; the sampler object comes from the cache.
            MTLSamplerMinMagFilter minFilter = MTLSamplerMinMagFilterNearest;
            MTLSamplerMinMagFilter magFilter = MTLSamplerMinMagFilterNearest;
            MTLSamplerMipFilter    mipFilter = MTLSamplerMipFilterNearest;
            MTLSamplerAddressMode  wrapS = MTLSamplerAddressModeClampToEdge;
            MTLSamplerAddressMode  wrapT = MTLSamplerAddressModeClampToEdge;
            float    maxAnisotropy = 1.0f;
            // Levels the sampler may reach: 0 until SetTextureFilter (or a
            // reserve) rebuilds it against the chain, as on Vulkan.
            float    lodMax = 0.0f;
            id<MTLSamplerState> sampler = nil;
            bool     everBound = false;       // drawn with or handed to ImGui
            uint64_t lastUsedFrame = 0;
            std::string label;                // SetDebugLabel's, else a creation-time default; re-applied on recreation
            // Per-frame copies (VKBackend::VKTextureInfo::frameCopies): from
            // the first queued update after the texture was drawn with, frame
            // slot s samples and writes only frameCopies[s].
            struct FrameCopy {
                id<MTLTexture> texture = nil;
                uint64_t syncedFrame = 0;     // holds every update flushed up to this frame
            };
            std::vector<FrameCopy> frameCopies;
            struct CarriedUpdate {
                uint32_t stagingSlot;
                size_t   stagingOffset;
                uint32_t mipLevel;
                int x, y, width, height;
                uint32_t layer;
            };
            std::vector<std::pair<uint64_t, std::vector<CarriedUpdate>>> carried;   // (frame, updates), oldest first
        };
        std::unordered_map<uint32_t, TextureInfo> m_textures;
        id<MTLTexture> FrameTexture(const TextureInfo& tex) const {
            return tex.frameCopies.empty() ? tex.texture : tex.frameCopies[m_currentFrame].texture;
        }
        TextureHandle CreateTextureImpl(int width, int height, TextureFormat format, const void* data,
                                        uint32_t mipLevels);
        TextureHandle WrapTexture(id<MTLTexture> texture, MTLSamplerMinMagFilter filter);
        // Drops an entry from m_textures, unbinding it from every slot first.
        void EraseTextureEntry(TextureHandle handle);
        void UpdateSampler(TextureInfo& tex);
        id<MTLSamplerState> SamplerFor(MTLSamplerMinMagFilter minF, MTLSamplerMinMagFilter magF,
                                       MTLSamplerMipFilter mipF, MTLSamplerAddressMode s,
                                       MTLSamplerAddressMode t, float aniso, float lodMax);
        std::unordered_map<uint64_t, id<MTLSamplerState>> m_samplers;
        float m_maxAnisotropy = 16.0f;

        // Batched texture updates (VKBackend's design): the pixels go straight
        // into the staging slot the next BeginFrame copies from, and the copy
        // rides that frame's command buffer ahead of its render passes.
        struct PendingTextureUpdate {
            uint32_t texture;
            uint32_t mipLevel;
            int x, y, width, height;
            size_t stagingOffset;
            size_t byteSize;
            uint32_t bytesPerRow;
            uint32_t layer;             // array slice (UpdateTextureArrayLevel); 0 for a 2D texture
        };
        std::vector<PendingTextureUpdate> m_pendingTextureUpdates;
        struct StagingSlot {
            id<MTLBuffer> buffer = nil;
            size_t capacity = 0;
            size_t used = 0;
        };
        // 2 x frames in flight: see VKBackend::kTexStagingSlots for the proof.
        static constexpr uint32_t kStagingSlots = 2 * kFramesInFlight;
        std::array<StagingSlot, kStagingSlots> m_staging{};
        uint8_t* ReserveStaging(size_t bytes, size_t& outOffset);
        void QueueTextureUpdate(uint32_t texture, uint32_t mipLevel, int x, int y, int width, int height,
                                const void* data, uint32_t layer = 0);
        void FlushPendingTextureUpdates();
        bool PromoteToFrameCopies(id blit, TextureInfo& tex);
        std::vector<uint32_t> m_frameCopyTextures;

        // Uploads outside the frame (texture creation, Static buffers, the
        // immediate region uploads): one blit encoder on its own command
        // buffer, committed before the frame's — so it executes first.
        id<MTLCommandBuffer>      m_uploadCmd = nil;
        id m_uploadBlit = nil;   // MTLBlitCommandEncoder, or a MTL4ComputeCommandEncoder (same copy selectors)
        id UploadBlit();   // a blit encoder, or the Metal 4 upload compute encoder (same copy selectors)
        void FlushUploads();

        struct MeshInfo {
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
            VertexLayout layout;
            BufferHandle instanceBuffer = INVALID_BUFFER;
            VertexLayout instanceLayout;
        };
        std::unordered_map<uint32_t, MeshInfo> m_meshes;
        BufferHandle m_megaVBO = INVALID_BUFFER;
        BufferHandle m_megaIBO = INVALID_BUFFER;

        struct TargetInfo {
            int width = 0, height = 0;
            id<MTLTexture> color = nil;                      // colors[0]
            id<MTLTexture> depth = nil;                      // nil: RenderTargetDesc::depth false (config 4)
            bool           hasDepth = true;
            TextureHandle  colorTexture = INVALID_TEXTURE;   // registered in m_textures (not owned)
            std::string    label;                            // "RT#<handle>" until SetDebugLabel names it
            // A target over textures the caller owns (CreateRenderTargetFromTextures):
            // up to eight colour attachments in their own formats, the
            // depth a sampleable texture; nothing here is freed with it.
            bool ownsImages = true;
            std::vector<id<MTLTexture>> colors;
            std::vector<MTLPixelFormat> formats;
            uint32_t attachmentsKey = 0;                     // the formats and depth, for the pipeline key
        };
        static constexpr uint8_t kConfigTargetNoDepth = 4;
        // Targets over the caller's textures: their pipelines bake the pass's
        // attachment formats (Pass::formats), with or without depth.
        static constexpr uint8_t kConfigTextures = 5;
        static constexpr uint8_t kConfigTexturesNoDepth = 6;
        static constexpr bool ConfigFromPass(uint8_t config) { return config == kConfigTextures || config == kConfigTexturesNoDepth; }
        std::unordered_map<uint32_t, TargetInfo> m_targets;
        // Every distinct (colour formats, depth) a texture target has had,
        // numbered from 1: TargetInfo::attachmentsKey, 18 bits of the
        // pipeline key.
        std::map<std::vector<uint32_t>, uint32_t> m_attachmentSets;
        uint32_t AttachmentSetId(const std::vector<MTLPixelFormat>& formats, bool depth);
        bool CreateTargetImages(TargetInfo& rt);
        void ClearDepthTextureOnUpload(id<MTLTexture> depth, double value, const char* label);
        void CopyBetweenPasses(id<MTLTexture> src, id<MTLTexture> dst, NSString* label);   // ends the pass, blits, resumes
        void ApplyTargetLabels(TargetInfo& rt);

        // ── Shaders and pipelines ────────────────────────────────────────
        struct ShaderInfo {
            id<MTLFunction> vertex = nil;
            id<MTLFunction> fragment = nil;
            id<MTLLibrary>  vertLibrary = nil;   // where each came from: the Metal 4 compiler's function descriptors
            id<MTLLibrary>  fragLibrary = nil;
            std::string vertPath, fragPath;      // as the caller asked (the manifest's names)
            // The GLSL sources behind the paths, read on the first
            // FindShadersBySource (the pack pipeline's family lookup).
            std::string vertexSource, fragmentSource;
            bool sourcesLoaded = false;
            VertexLayout vertexLayout;           // empty: the 24-byte block layout
            VertexLayout instanceLayout;
            bool ignoresCommonMatrices = false;
            // 0 block, 1 portal (Common block), 3 / 4: portal / block + the
            // Improved Transparency set — the OIT variants and composites;
            // 5: a shader pack program (its own uniform block and 16 slots).
            int layoutType = 0;
            // Pack programs: the uniform block's CPU copy, written by name
            // (SetPackUniform) and uploaded to the ring when a draw needs it.
            uint32_t packBlockSize = 0;
            std::vector<uint8_t> packBlock;
            bool packDirty = true;
            uint64_t packRingFrame = ~0ull;      // the frame the ring window was written in
            id<MTLBuffer> packRing = nil;
            size_t packRingOffset = 0;
            std::unordered_map<std::string, PackUniformDesc> packUniforms;
            uint32_t packSampler2D = 0;          // PackShaderDesc::sampler2DSlots
        };
        static constexpr int kLayoutPack = 5;
        bool SetPackUniform(ShaderHandle shader, const std::string& name, const void* data, size_t bytes);
        // The shader-override mode (RenderBackend::SetShaderOverrideMode): the
        // pack pipeline's programs and targets in place of the engine's.
        struct ShaderOverride { ShaderHandle shader = INVALID_SHADER; RenderTargetHandle target = INVALID_RENDER_TARGET; };
        bool m_overrideMode = false;
        RenderTargetHandle m_overrideDefaultTarget = INVALID_RENDER_TARGET;
        std::unordered_map<uint32_t, ShaderOverride> m_shaderOverrides;
        std::unordered_map<uint32_t, ShaderInfo> m_shaders;
        // shaders/metal/shaders.metallib (CMake, with the Metal toolchain):
        // every shader precompiled, with source and line tables for the
        // Metal debugger. nil: each .metal compiles at startup instead.
        id<MTLLibrary> m_shaderLibrary = nil;
        std::unordered_map<std::string, id<MTLLibrary>> m_libraries;   // runtime-compiled, by .metal path
        id<MTLFunction> LoadFunction(const std::string& glslPath, const char* stage, id<MTLLibrary>* outLibrary = nullptr);
        ShaderHandle m_boundShader = INVALID_SHADER;
        const ShaderInfo* m_boundShaderInfo = nullptr;

        struct PipelineRecord {
            PipelineState state;
            ShaderHandle  shader = INVALID_SHADER;
            uint8_t       config = 0;
            id<MTLRenderPipelineState> pipeline = nil;
        };
        // The key: the shader, config, blend and attachment set in `a`; in
        // `b` the engine shader whose vertex layout a pack program draws
        // with (the override's), 0 otherwise.
        struct PipelineKeyT {
            uint64_t a = 0;
            uint32_t b = 0;
            bool operator==(const PipelineKeyT& o) const { return a == o.a && b == o.b; }
        };
        struct PipelineKeyHash {
            size_t operator()(const PipelineKeyT& k) const { return std::hash<uint64_t>()(k.a ^ (static_cast<uint64_t>(k.b) * 0x9E3779B97F4A7C15ull)); }
        };
        std::unordered_map<PipelineKeyT, PipelineRecord, PipelineKeyHash> m_pipelines;
        // Under an override, the engine shader the bound pack program stands
        // in for: its vertex layout is the pipeline's (the pack program's
        // own registered layout when bound directly).
        ShaderHandle m_packLayoutSource = INVALID_SHADER;
        // A pack program drew this frame: on Metal 4 (no hazard tracking)
        // every render encoder from then on waits for the fragment work
        // before it, cleared attachments or not — a pack pass samples what
        // the pass before it drew (the final pass into the frame samples
        // the composites), which the load-action rule alone misses.
        bool m_packFrame = false;
        // The last plain 2D texture bound to each slot: what a pack program
        // samples there when the engine has since bound a 2D array or a
        // buffer texture to the slot (the terrain's sprite arrays on 4 / 5,
        // where a pack's normals / specular live — OpenGL keeps the two
        // targets apart, Metal has one index).
        const TextureInfo* m_lastTexture2D[MetalBindings::kPackTextureSlots] = {};
        std::unordered_map<uint64_t, id<MTLDepthStencilState>> m_depthStencilStates;
        id<MTLRenderPipelineState> PipelineFor(const PipelineState& state, ShaderHandle shader, uint8_t config);
        id<MTLDepthStencilState> DepthStencilFor(const PipelineState& state);
        PipelineKeyT PipelineKey(const PipelineState& state, ShaderHandle shader, uint8_t config) const;
        // The pipeline manifest (VKBackend's warm-up design): every (shader
        // paths, attachment config, state) this machine has built, rebuilt
        // behind the loading screen next session.
        std::string m_manifestFile;
        bool m_pipelinesWarmed = false;
        uint32_t m_pipelinesSinceSave = 0;
        uint64_t m_lastPipelineFrame = 0;
        // `synchronous` writes inline (Shutdown); otherwise on m_manifestWriter,
        // so the write never lands on a frame.
        void SaveManifest(bool synchronous);
        std::thread m_manifestWriter;

        PipelineState m_requestedState;   // the caller's, before overrides
        PipelineState m_state;            // what draws use
        struct StencilOverride {
            bool enabled = false;
            CompareOp compareOp = CompareOp::Always;
            StencilOp passOp = StencilOp::Keep;
            uint32_t reference = 0, readMask = 0xFFu, writeMask = 0xFFu;
        } m_stencilOverride;

        // Shared front half of every draw: open the encoder, pipeline,
        // depth-stencil and raster state, uniforms, textures. False = skip.
        bool PrepareDraw();
        void BindVertexStream(uint32_t index, id<MTLBuffer> buffer, NSUInteger offset);
        void BindUniforms();
        void BindTextures();
        static constexpr uint32_t kMaxTextureSlots = MetalBindings::kPackTextureSlots;   // the engine binds 0..5, a pack program 0..15
        std::array<TextureHandle, kMaxTextureSlots> m_boundTextures{};
        // Their entries, resolved at BindTexture (an unordered_map element
        // stays put until erased; DestroyTexture unbinds it first).
        std::array<const TextureInfo*, kMaxTextureSlots> m_boundTextureInfo{};
        // The pipeline and depth-stencil state the current (state, shader,
        // pass) resolve to, looked up again only after one of them changed.
        id<MTLRenderPipelineState> m_drawPipeline = nil;
        id<MTLDepthStencilState>   m_drawDepthStencil = nil;
        bool    m_drawStateDirty = true;
        uint8_t m_drawConfig = 0xFF;
        static MTLPrimitiveType ToPrimitive(PrimitiveType p);

        // ── Frame depth: preserved, handoff ──────────────────────────────
        bool m_frameDepthPreserved = false;   // this frame
        std::array<bool, static_cast<size_t>(FrameDepthUser::Count)> m_frameDepthUsers{};
        bool m_depthHandoff = false;
        uint64_t m_frameDepthGeneration = 1;  // bumped whenever a frame or scene depth image is (re)made
        // The frame's depth lives in tile memory only (MTLStorageModeMemoryless)
        // while no user preserves it: it is cleared on load and dropped on
        // store, so nothing ever needs it in system memory — 37 MB a slot at
        // 3420x2146 (Xcode's Memory insight, 2026-10-07). A preserving user
        // (Improved Transparency; the half-res rain on Vulkan) gets a
        // Private, sampleable image back at the next BeginFrame of each
        // slot. Apple GPUs only (Intel/AMD have no tile memory to speak
        // of); OBEY_MTL_DEPTH_PRIVATE=1 keeps every depth Private.
        bool m_memorylessDepthOk = false;

        // ── Metal 4 path (docs/metal4.md; Metal4Backend sets it up) ──────
        // Everything typed `id`: the MTL4 protocols are macOS 26 API and the
        // members exist on every macOS. The frame and upload command buffers
        // are reusable, begun per frame from the slot's allocators; the two
        // argument tables carry every binding (snapshotted at each draw);
        // the residency set holds every buffer and texture the backend
        // makes (Metal 4 command buffers retain nothing), and an object a
        // frame may still read goes to `garbage` tagged with that frame,
        // released once the frame completed (M4CollectGarbage).
        bool m_metal4 = false;
        struct Metal4 {
            id queue = nil;                                  // id<MTL4CommandQueue>
            id compiler = nil;                               // id<MTL4Compiler>
            id residency = nil;                              // id<MTLResidencySet>, on the queue
            id frameAllocators[kFramesInFlight] = {};        // id<MTL4CommandAllocator>
            id uploadAllocators[kFramesInFlight] = {};
            id frameCmds[kFramesInFlight] = {};              // id<MTL4CommandBuffer>, reused
            id uploadCmds[kFramesInFlight] = {};
            id vertexArgs = nil;                             // id<MTL4ArgumentTable>, MTLRenderStageVertex
            id fragmentArgs = nil;                           // ... MTLRenderStageFragment
            bool residencyDirty = false;
            // A second, small set for the staging buffers an upload makes
            // and drops within a frame or two: a commit's cost grows with
            // the set it is on (median 0.45 ms, 6.7 ms peaks on the main
            // set of ~1,300 allocations, 2026-10-08), and the staging churn
            // was committing it on most streaming frames. Staging goes here;
            // the main set now changes only when a real resource comes or
            // goes. Per-frame counters feed the Mtl/M4* plots.
            id   residencyTransient = nil;                   // id<MTLResidencySet>, on the queue
            bool transientDirty = false;
            uint32_t adds = 0, removes = 0, commits = 0;     // this frame's, reset by the plots
            // Sampled textures come from these heaps on Metal 4 (M4HeapTexture):
            // one residency-set allocation per 64 MB heap instead of one per
            // texture — the set's commit cost grows with its allocation
            // count, and the 956 prewarmed entity sheets had taken it to
            // ~1,460 allocations and 0.6–8 ms per commit (2026-10-08).
            std::vector<id> textureHeaps;                    // id<MTLHeap>
            // A commit's cost is the page mapping of what became resident
            // since the last one — 5–10 ms for a 32 MB terrain slab, on the
            // main thread, between frames (2026-10-08). Allocations of
            // kBackgroundResidencyBytes or more are added and committed on
            // this serial queue instead; every mutation of either set takes
            // the mutex, and a command buffer is committed only once the
            // group has drained (M4WaitResidency), so nothing the GPU runs
            // can reference an allocation whose commit is still in flight.
            dispatch_queue_t residencyQueue = nullptr;
            dispatch_group_t residencyGroup = nullptr;
            std::mutex residencyMutex;
            static constexpr size_t kBackgroundResidencyBytes = 4u << 20;
            struct Garbage { uint64_t frame; id object; };
            std::deque<Garbage> garbage;
            double gpuStart[kFramesInFlight] = {};           // commit feedback of each slot's last frame
            double gpuEnd[kFramesInFlight] = {};
            uint64_t uploadsInFlight = 0;
            // Frame pacing: the queue signals `pacingEvent` with the frame
            // number just before the frame commits — once everything before
            // it completed — and the listener opens the gate (SharedState::
            // frameStarted) for the frame after it. Metal 4 has no scheduled
            // handler; this keeps at most one frame queued behind the one
            // executing, as the Metal 3 gate did.
            id pacingEvent = nil;                            // id<MTLSharedEvent>
            id pacingListener = nil;                         // MTLSharedEventListener
            // The ImGui overlay: a Metal 3 command buffer after the frame,
            // ordered with `overlayEvent`, which draws it and presents.
            id overlayEvent = nil;                           // id<MTLEvent>
            void* overlayData = nullptr;                     // ImDrawData* for this frame, or null
            // Timestamps (counter heap): the open render encoder's first
            // sample; the open timed copy encoder and its end sample, written
            // inside it by EndBlitEncoder.
            uint32_t encFirst = UINT32_MAX;
            id       copyEncoder = nil;                      // id<MTL4ComputeCommandEncoder>
            uint32_t copyEnd = UINT32_MAX;
            // Counter heap tick calibration (MetalInstrumentation.mm): the
            // heap's unit is not the sample buffers' nanosecond. The tick is
            // the slope of the commit feedback's GPU start time over the
            // frame's first stamp, measured from the first frame seen
            // (baseline grows, the per-frame jitter of the two clocks'
            // offset averages out); `anchor*` map a stamp onto the
            // sampleTimestamps GPU timebase Tracy's context was created in.
            uint64_t calStamp = 0;                           // the baseline's first stamp ...
            double   calStart = 0.0;                         // ... and GPU start (CACurrentMediaTime seconds)
            double   tickNs = 0.0;                           // 0 until the baseline is long enough
            bool     tickLogged = false;
            uint64_t anchorStamp = 0;                        // a recent stamp ...
            double   anchorGpuNs = 0.0;                      // ... in the sampleTimestamps GPU timebase
            double   anchorAt = 0.0;                         // when the anchor was taken (seconds)
        };
        Metal4 m_m4;
        id m_cmd4 = nil;         // this frame's MTL4 command buffer (m_cmd stays nil on the Metal 4 path)
        id m_uploadCmd4 = nil;   // the uploads', while one is open
        // MetalFour.mm
        bool M4Setup();                                          // objects, tables, residency of what exists
        void M4Resident(id allocation);                          // into the residency set (committed at BeginFrame)
        void M4ResidentTransient(id allocation);                 // into the staging set (see Metal4::residencyTransient)
        id   M4HeapTexture(MTLTextureDescriptor* desc);          // a sampled texture from the texture heaps, or nil
        void M4CommitResidency();                                // commit whichever set changed
        void M4WaitResidency();                                  // background commits done (before a command buffer commits)
        void M4Release(id object);                               // to garbage, tagged with the frame that may read it
        void M4CollectGarbage();                                 // after the slot wait: what completed frames held
        void M4BeginFrameCommandBuffer();                        // allocator reset + begin
        void M4CommitFrame();                                    // present waits/signals, commit with feedback
        id   M4UploadEncoder();                                  // the upload command buffer's compute encoder
        void M4FlushUploads();                                   // end + commit
        id   M4MakeRenderEncoder(id cmd, MTLRenderPassDescriptor* desc, uint32_t width, uint32_t height, bool resumed,
                                 uint32_t sampleFirst = UINT32_MAX);
        void M4WriteEncoderEnd();                                // the render encoder's stage timestamps, before it ends
        void M4WriteCopyEnd(id encoder);                         // the timed copy encoder's end timestamp, before it ends
        void M4CalibrateHeap(uint64_t firstStamp, double gpuStart);   // the heap tick + Tracy anchor (MetalInstrumentation.mm)
        void M4PresentWithOverlay();                             // the ImGui overlay's Metal 3 command buffer, then present
        void M4EndUploadEncoder();                               // the copies' producer barrier, then end
        id   M4MakeCopyEncoder(id cmd, NSString* label, bool afterFragment);
        void M4BindBuffer(bool fragment, uint32_t index, id<MTLBuffer> buffer, NSUInteger offset);
        void M4BindBytes(bool fragment, uint32_t index, const void* data, size_t bytes);
        void M4BindTexture(bool fragment, uint32_t index, id<MTLTexture> texture);
        void M4BindSampler(bool fragment, uint32_t index, id<MTLSamplerState> sampler);
        void M4DrawIndexed(MTLPrimitiveType primitive, uint32_t indexCount, MTLIndexType type,
                           id<MTLBuffer> indices, size_t byteOffset, uint32_t instances, int32_t baseVertex);
        void M4Shutdown();
        id<MTLTexture> MakeDepthTexture(uint32_t width, uint32_t height, bool memoryless, NSString* label);
        void SettleDepthStorage(uint32_t slotIndex);
        struct DepthStamp {
            uint64_t frame = 0;               // m_frameNumber it was drawn in; 0 = none
            uint64_t generation = 0;
            bool     scene = false;
            uint32_t width = 0, height = 0;
        };
        std::array<DepthStamp, kFramesInFlight> m_depthStamps{};
        TextureHandle FrameDepthTextureFor(uint32_t slot, bool scene);

        // ── Scaled scene ─────────────────────────────────────────────────
        uint32_t m_sceneWidth = 0, m_sceneHeight = 0;     // the slots' size; 0 = none built
        uint32_t m_sceneRequestW = 0, m_sceneRequestH = 0;
        bool     m_sceneActive = false;
        ShaderHandle m_upscaleShader = INVALID_SHADER;
        bool EnsureSceneTargets(uint32_t width, uint32_t height);
        void DestroySceneTargets();

        // ── Improved Transparency ────────────────────────────────────────
        OitStage  m_oitStage = OitStage::None;
        OitStage  m_oitSamplerStage = OitStage::None;
        glm::vec4 m_oitProjParams{0.0f};
        bool      m_oitDbWritesDepth = false;
        bool      m_oitSkipDraw = false;
        bool      m_oitPassOpen = false;
        OitPass   m_oitOpenPass = OitPass::DepthBounds;
        bool      m_oitCreated = false;
        bool      m_oitOnScene = false;
        int       m_oitWidth = 0, m_oitHeight = 0;
        struct OitSlot {
            id<MTLTexture> depthBounds, culled, coeff0, coeff1, accumulate, cloudDepth;
            TextureHandle depthBoundsTex = INVALID_TEXTURE, culledTex = INVALID_TEXTURE,
                          coeff0Tex = INVALID_TEXTURE, coeff1Tex = INVALID_TEXTURE,
                          accumulateTex = INVALID_TEXTURE, frameDepthTex = INVALID_TEXTURE;
        };
        std::array<OitSlot, kFramesInFlight> m_oitSlots{};
        TextureHandle m_oitDummy = INVALID_TEXTURE;
        struct OitVariantSet { std::array<ShaderHandle, 4> shader{}; std::array<bool, 4> tried{}; };
        std::unordered_map<uint32_t, OitVariantSet> m_oitVariants;
        ShaderHandle OitVariantFor(ShaderHandle engine);
        static uint8_t OitPassConfig(OitPass pass);

        // ── Read-backs ───────────────────────────────────────────────────
        struct Readback {
            id<MTLBuffer> buffer = nil;
            size_t capacity = 0;
            int width = 0, height = 0;
            uint64_t frameNumber = 0;
            id<MTLCommandBuffer> cmd = nil;   // whose completion proves the copy
        };
        static constexpr size_t kMaxReadbacksInFlight = 4;
        std::deque<Readback> m_readbacks;
        std::vector<Readback> m_readbackFree;
        // Ends the frame's pass, records `copy` on a blit encoder, and leaves
        // the frame to resume (colour loaded) on its next use.
        bool SuspendFrameForCopy();

        // ── GPU timing: counter sample buffers at encoder boundaries ─────
        // (MetalInstrumentation.mm.) Apple GPUs sample only at stage
        // boundaries (this M4: atStageBoundary yes, draw/blit/dispatch no),
        // so the unit the hardware times is the ENCODER: a render encoder
        // yields vertex start/end and fragment start/end, a blit encoder
        // start/end. The frame's encoder carries every stage drawn into it;
        // a stage that owns an encoder (a render target, an OIT pass) is
        // exact. Off unless OBEY_MTL_GPU_TIMERS=1|split or
        // SetGpuTimersEnabled(true): while a sample buffer is attached,
        // Apple's Metal HUD (MTL_HUD_ENCODER_TIMING_ENABLED=1) shows no
        // encoder times, so the default must stay off. `split` makes every
        // top-level debug group on the frame's pass its own encoder (exact
        // per stage, one tile store + load per split).
        struct GpuTiming {
            enum class Mode : uint8_t { Off, Encoders, Split };
            Mode  envMode = Mode::Off;       // OBEY_MTL_GPU_TIMERS
            bool  requested = false;         // SetGpuTimersEnabled (the "GPU Pass Timers" toggle)
            bool  active = false;            // this frame attaches samples (decided in BeginFrame)
            bool  supported = false;
            bool  probed = false;
            id<MTLCounterSet> counterSet = nil;
            static constexpr uint32_t kSamplesPerSlot = 512;   // 4 per render encoder, 2 per blit
            struct Encoder {
                uint32_t    first;           // its first sample index in the slot's buffer
                uint8_t     kind;            // GpuEncoderTiming::kind
                bool        blit;
                bool        shared;          // the frame's pass: outlives the stage that opened it
                const char* label;           // persistent (a literal, or InternLabel's)
                const char* stage;           // top debug group at open, or nullptr
            };
            struct Slot {
                id<MTLCounterSampleBuffer> buffer = nil;
                id heap = nil;                    // Metal 4: id<MTL4CounterHeap> of timestamps, the same layout
                uint32_t used = 0;
                std::vector<Encoder> encoders;
                std::vector<float>   durations;   // per encoder, ms (filled by the resolve)
                uint64_t frame = 0;
                double   commitHostTime = 0.0;    // CACurrentMediaTime() just before commit
            };
            std::array<Slot, kFramesInFlight> slots{};
            // GPU ticks -> ns: the slope of two (cpu, gpu) pairs from
            // sampleTimestamps:gpuTimestamp:, refreshed each active frame
            // (~1.0 on Apple GPUs, not on Intel / AMD).
            MTLTimestamp cpuPrev = 0, gpuPrev = 0, cpuNow = 0, gpuNow = 0;
            double ticksToNs = 1.0;
            bool   ratioLogged = false;
            // The last resolved frame.
            std::vector<GpuEncoderTiming> results;
            uint64_t resultsFrame = 0;
            float lastFrameGpuMs = -1.0f;    // the command buffer's span; needs no counters
            uint32_t missing = 0, dropped = 0;   // samples not written / encoders past the slot's capacity
        } m_gpu;
        bool ProbeGpuTiming();
        // Returns the encoder's first sample index, or UINT32_MAX when none
        // was attached (timers off, slot full).
        uint32_t AttachRenderSamples(MTLRenderPassDescriptor* desc, uint8_t kind, bool shared, const char* label);
        // A blit encoder, or on Metal 4 a compute encoder whose barrier waits
        // for prior copies — and for prior fragment work only when
        // `afterFragment` (a copy that reads what the frame drew; the texture
        // flush writes this slot's own copies, last read three frames ago).
        id MakeBlitEncoder(id<MTLCommandBuffer> cmd, NSString* label, bool afterFragment = true);
        void EndBlitEncoder(id blit);   // every MakeBlitEncoder encoder ends through this (its end timestamp)
        void ResolveGpuTiming(uint32_t slot);
        const char* InternLabel(NSString* label);
        std::unordered_map<std::string, std::unique_ptr<std::string>> m_internedLabels;
        const char* m_nextEncoderLabel = nullptr;   // consumed by the next EnsureEncoder ("Upscale")

        // A GPU timer (RenderBackend API) brackets ENCODERS: its result is
        // the GPU time of every encoder opened inside it (one already open
        // at Begin is not counted). Without `split`, a bracket inside the
        // frame's pass spans no encoder and reads 0 — honest, not the frame.
        struct GpuTimer {
            uint64_t frame = 0;
            uint32_t slot = 0;
            uint32_t beginOrdinal = 0, endOrdinal = 0;
            bool ended = false;
        };
        std::unordered_map<uint32_t, GpuTimer> m_gpuTimers;

        // Tracy GPU zones (TRACY_ENABLE builds, same gate as the timers): a
        // GPU context whose query ids are (slot, sample index); the zone's
        // begin/end are queued when the encoder opens, its GPU times when
        // the slot resolves.
        struct TracyGpu {
            int32_t  context = -1;      // Tracy's context id; -1 = not created
            int64_t  gpu0 = 0;          // calibration: GPU ticks ...
            int64_t  mostRecent = 0;    // ... and the latest resolved tick (for unsampled encoders)
        } m_tracyGpu;
        void TracyGpuInit();
        void TracyGpuZone(uint32_t slot, uint32_t beginIndex, uint32_t endIndex, const tracy::SourceLocationData* srcloc);
        void TracyGpuTime(uint32_t slot, uint32_t index, uint64_t ticks);
        std::unordered_map<std::string, tracy::SourceLocationData*> m_tracySrclocs;   // leaked on purpose: Tracy keeps the pointer
        const tracy::SourceLocationData* TracySrcloc(const char* encoderLabel, const char* stage, uint32_t color);

        // Per-frame counters -> Tracy plots (EmitFramePlots) and the HUD line.
        struct FrameCounters {
            uint32_t draws = 0, encoders = 0, blits = 0, pipelinesBuilt = 0, texUpdates = 0;
            size_t   uploadBytes = 0, stagingBytes = 0;
            double   drawableWaitUs = 0.0, frameWaitUs = 0.0, paceWaitUs = 0.0;
        } m_counters;
        void EmitFramePlots();
        uint32_t m_signpostOrdinal = 0;
        bool     m_signpostsOn = false;

        // Command buffers: errors are always reported from the completed
        // handler; OBEY_MTL_ERRORS=1 adds per-encoder execution status.
        bool m_errorOptions = false;
        id<MTLCommandBuffer> MakeCommandBuffer(NSString* label, bool watchErrors);
        void ReportCommandBufferError(id<MTLCommandBuffer> cmd, const char* what);
        std::atomic<uint32_t> m_cmdErrors{0};

        GPUMemoryStats m_memStats;
        void TrackAlloc(size_t& bucket, size_t bytes, size_t& count);
        void SampleDeviceMemory();

        bool m_imguiReady = false;

        // Dev: OBEY_MTL_CAPTURE_AT=<seconds> writes one frame, the first that
        // begins that long after Initialize, as an Xcode GPU trace
        // (<obeycraft>/captures/metal-<time>.gputrace) for the Metal debugger;
        // RequestGpuCapture (F3+U) asks for the next frame. Needs
        // MTL_CAPTURE_ENABLED=1 in the launch environment. The frame is also
        // an MTLCaptureScope (the device's default), so Xcode's own capture
        // button takes exactly one frame.
        double m_captureAt = -1.0;
        bool   m_captureRequested = false;
        bool   m_capturing = false;
        id<MTLCaptureScope> m_captureScope = nil;
        std::chrono::steady_clock::time_point m_initTime{};
        void BeginGpuCaptureIfDue();
        void EndGpuCapture();
    };

} // namespace Render

#endif // HAS_METAL
