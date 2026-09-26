// File: src/client/renderer/backend/RenderBackend.hpp
#pragma once

#include "RenderTypes.hpp"
#include <vector>
#include <glm/glm.hpp>
#include <string>
#include <functional>
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
        // Vendor / device / driver strings for the F3 system_specs lines.
        virtual GpuDeviceInfo GetDeviceInfo() const { GpuDeviceInfo i; i.backendName = GetName(); return i; }

        // ====================================================================
        // FRAME MANAGEMENT
        // ====================================================================

        virtual void BeginFrame() = 0;
        virtual void EndFrame(GLFWwindow* window) = 0;
        // Build, now, every pipeline this backend built in earlier sessions
        // (a manifest kept next to its on-disk cache), so the first frames
        // of play do not stall on lazy pipeline creation — on MoltenVK a
        // Metal pipeline compile, 300 ms of first-frame hitch in a capture.
        // Called once per process behind the world-load screen; a no-op on
        // backends that compile at link time (OpenGL).
        virtual void WarmPipelines() {}
        virtual void SetClearColor(float r, float g, float b, float a) = 0;
        // The `stencil` flag (default false) clears the stencil buffer to 0.
        // Existing callers that pass only color+depth keep working unchanged.
        virtual void Clear(bool color, bool depth, bool stencil = false) = 0;
        virtual void SetViewport(int x, int y, int width, int height) = 0;

        // Clip subsequent draws to a rectangle, in FRAMEBUFFER PIXELS with the
        // origin at the TOP-LEFT (GUI convention). Backends flip as needed —
        // GL's glScissor is bottom-left, Vulkan's is top-left like ours.
        //
        // Callers must pair every SetScissorRect with a ClearScissorRect; the
        // state is sticky and would otherwise clip the rest of the frame.
        // Defaults are no-ops so a backend without scissor support simply draws
        // unclipped rather than failing to draw.
        virtual void SetScissorRect(int /*x*/, int /*y*/, int /*w*/, int /*h*/) {}
        virtual void ClearScissorRect() {}

        // ====================================================================
        // BUFFER MANAGEMENT
        // ====================================================================

        virtual BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                         const void* data, BufferAccess access = BufferAccess::Static) = 0;
        virtual void UpdateBuffer(BufferHandle handle, size_t offset,
                                 size_t size, const void* data) = 0;

        // Bind [offset, offset+size) of a BufferUsage::Uniform buffer as the
        // shader's user uniform block: GL block binding point 0 (a program's
        // block named "SectionOrigins" is wired to it at link), Vulkan
        // descriptor set 3 binding 0 of the portal pipeline layout, with
        // `offset` as the dynamic offset. One slot — the terrain mega buffer
        // is its only client (per-slab section-origin table; see
        // ChunkMegaBuffer::BindSlab). Offset must respect the device's UBO
        // offset alignment (256 covers every driver).
        virtual void BindUniformBuffer(BufferHandle /*handle*/, size_t /*offset*/, size_t /*size*/) {}

        // A BUFFER TEXTURE: `buffer`'s bytes seen by fragment shaders as
        // texels of `format`, sampled with texelFetch on a samplerBuffer
        // (GL: glTexBuffer over the buffer object; Vulkan: a VkBufferView in
        // a uniform-texel-buffer descriptor, set 4 of the portal pipeline
        // layout). The texture owns no storage — destroy it before the
        // buffer. Bound like any texture (BindTexture slot). Used for the
        // terrain face map (ChunkMegaBuffer). INVALID_TEXTURE when the
        // backend cannot make one.
        virtual TextureHandle CreateBufferTexture(BufferHandle /*buffer*/, TextureFormat /*format*/) {
            return INVALID_TEXTURE;
        }
        // Destroy a texture once no in-flight frame can still read it (see
        // DeferredDestroyBuffer). Default: immediate.
        virtual void DeferredDestroyTexture(TextureHandle handle) { DestroyTexture(handle); }

        // Overwrite a range WITHOUT waiting for in-flight draws that read it.
        //
        // Only safe when a torn read is still valid to draw. The one caller is
        // the translucency re-sort, where the write is a PERMUTATION of the same
        // quad range at the same length (ChunkMegaBuffer::UpdateSectionIndices
        // rejects anything else), so every index the GPU can observe — old, new,
        // or a mix — points at a real quad in that section. Worst case is one
        // frame of imperfect blend order, which the design already tolerates:
        // sorts go stale between re-sorts by construction.
        //
        // Do NOT use for mesh uploads. Those write NEW geometry, so a torn read
        // is genuine garbage, not a stale ordering.
        //
        // Default forwards to the synchronised path, so a backend that has no
        // cheaper option stays correct by doing the safe thing.
        virtual void UpdateBufferUnsynchronized(BufferHandle handle, size_t offset,
                                                size_t size, const void* data) {
            UpdateBuffer(handle, offset, size, data);
        }

        // A write into one buffer of a RING of per-use stream buffers (each
        // written and drawn in one frame, and reused several frames later —
        // the particle slots). GL: waits for the fence of the frame that
        // last wrote this buffer (normally long signalled) and then writes
        // through an UNSYNCHRONIZED map. A plain glBufferSubData made Apple's
        // driver wait for the GPU on every such upload — MobParticles.Upload,
        // 7.5 ms in 6% of frames (2026-09-25) — and orphaning did not help
        // (Apple's driver waits on that too). Not for a single buffer
        // rewritten every frame: that would wait for the previous frame.
        // Default: the plain write (Vulkan's streaming buffers are written
        // without waiting; the ring is the caller's).
        virtual void UpdateBufferStreaming(BufferHandle handle, size_t offset,
                                           size_t size, const void* data) {
            UpdateBuffer(handle, offset, size, data);
        }
        virtual void DestroyBuffer(BufferHandle handle) = 0;

        // Deferred destroy — delays destruction until GPU is done with the resource.
        // Default implementation calls immediate destroy (correct for OpenGL).
        virtual void DeferredDestroyBuffer(BufferHandle handle) { DestroyBuffer(handle); }

        // DEBUG ONLY (F8 CullDump): the CPU-visible mapping of a host-visible
        // buffer, so diagnostics can read back what the GPU actually sees.
        // Null when the backend has no persistent mapping for it (GL).
        virtual const void* DebugGetMappedBufferPtr(BufferHandle) const { return nullptr; }
        // DEBUG: runtime switch for the VK indirect multi-draw path (no-op on GL).
        virtual void DebugSetMultiDrawIndirect(bool) {}
        virtual bool DebugGetMultiDrawIndirect() const { return false; }

        // ====================================================================
        // TEXTURE MANAGEMENT
        // ====================================================================

        virtual TextureHandle CreateTexture2D(int width, int height,
                                             TextureFormat format,
                                             const void* data) = 0;
        // An empty texture whose levels 0..maxLevel exist from the start,
        // for a caller that fills them afterwards (UploadTextureRegionNow /
        // UploadTextureMipLevel). Equivalent to CreateTexture2D(nullptr) +
        // ReserveTextureMipLevels, but a backend that fixes the mip count
        // at creation builds the image once instead of twice.
        virtual TextureHandle CreateEmptyTexture2D(int width, int height,
                                                  TextureFormat format, int maxLevel) {
            TextureHandle handle = CreateTexture2D(width, height, format, nullptr);
            if (handle != INVALID_TEXTURE && maxLevel > 0) ReserveTextureMipLevels(handle, maxLevel);
            return handle;
        }
        virtual void UpdateTexture2D(TextureHandle handle, int x, int y,
                                    int width, int height, const void* data) = 0;
        virtual void SetTextureFilter(TextureHandle handle,
                                     TextureFilter min, TextureFilter mag) = 0;
        virtual void SetTextureWrap(TextureHandle handle,
                                   TextureWrap s, TextureWrap t) = 0;
        virtual void GenerateMipmaps(TextureHandle handle) = 0;
        // Anisotropic filtering, 1 = off, up to the device's maximum
        // (clamped). Smooths textures seen at a grazing angle — the
        // distant ground and walls — where mip selection alone blurs or
        // shimmers. Default no-op for a backend without it.
        virtual void SetTextureAnisotropy(TextureHandle /*handle*/, float /*maxAnisotropy*/) {}

        // ── CPU-authored mip chains ─────────────────────────────────────────
        //
        // GenerateMipmaps above hands the whole texture to the driver, which
        // box-filters raw RGBA. That is wrong for a cutout atlas (see
        // texture/MipmapGenerator.hpp), so the block atlas builds its chain on
        // the CPU per sprite, the way MC does, and uploads each level here.
        //
        // Defaulted to no-ops rather than pure virtual: a backend that has no
        // mip support at all stays correct by simply sampling level 0, which is
        // what the Vulkan backend does today (its GenerateMipmaps is a stub and
        // its images are created with mipLevels = 1).

        // Declares that this texture will carry levels 0..maxLevel, and caps
        // sampling to them so the sampler cannot walk off the end of a
        // hand-built chain.
        //
        // MUST be called BEFORE uploading any level: Vulkan fixes an image's
        // mip count at allocation and cannot grow one afterwards, so this is
        // where it reallocates. For the same reason the texture's CONTENTS ARE
        // UNDEFINED once this returns — callers upload every level, including
        // level 0, after reserving.
        virtual void ReserveTextureMipLevels(TextureHandle /*handle*/, int /*maxLevel*/) {}

        // Replaces the whole of one mip level. `level` 0 is the base image.
        // A sub-rectangle of one level, uploaded on the same immediate path
        // as UploadTextureMipLevel: the pixels are copied out before this
        // returns (the caller's buffer is free) and the transfer is
        // submitted at once, ahead of the frame's draws. The panorama
        // capture streams each face up a tile at a time with it, so no
        // frame carries a whole face.
        virtual void UploadTextureRegionNow(TextureHandle /*handle*/, int /*level*/,
                                            int /*x*/, int /*y*/, int /*width*/, int /*height*/,
                                            const void* /*data*/) {}

        virtual void UploadTextureMipLevel(TextureHandle /*handle*/, int /*level*/,
                                           int /*width*/, int /*height*/,
                                           const void* /*data*/) {}

        // Sub-rectangle update of one mip level, for animated sprites whose
        // frames change after the atlas is built.
        virtual void UpdateTexture2DLevel(TextureHandle /*handle*/, int /*level*/,
                                          int /*x*/, int /*y*/,
                                          int /*width*/, int /*height*/,
                                          const void* /*data*/) {}

        // UpdateTexture2DLevel for a texture queued frames may still be
        // sampling. GL copies through a rotating pixel-unpack buffer, so the
        // write into the texture is the GPU's job, ordered after the frames
        // that read it, instead of a CPU wait inside glTexSubImage2D; Vulkan
        // already stages UpdateTexture2DLevel into the next frame.
        virtual void UpdateTexture2DLevelStaged(TextureHandle handle, int level, int x, int y,
                                                int width, int height, const void* data) {
            UpdateTexture2DLevel(handle, level, x, y, width, height, data);
        }

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
        // Integer vec3: the render origin (RenderOrigin.hpp) is an integer
        // block position that a float would round past ±16.7 M blocks.
        virtual void SetUniformIVec3(ShaderHandle handle, const std::string& name,
                                    const glm::ivec3& value) = 0;
        // Integer vec2 (shader packs' eyeBrightness). Optional.
        virtual void SetUniformIVec2(ShaderHandle /*handle*/, const std::string& /*name*/,
                                    const glm::ivec2& /*value*/) {}

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

        // Copy the DEFAULT framebuffer's depth (and stencil) into `dst`, a
        // Depth24Stencil8 texture sized exactly to it, as it stands at this
        // call — a snapshot later draws can sample while the frame goes on
        // (the volumetric beams end their rays at the scene with it). False
        // when it cannot: an offscreen target is bound, or the backend has no
        // mid-pass depth copy (Vulkan: the frame pass does not store depth).
        virtual bool CopyFramebufferDepthToTexture(TextureHandle /*dst*/) { return false; }

        // ====================================================================
        // BACKBUFFER READ-BACK
        // ====================================================================
        // The panorama capture (PlatformMain's leave capture). Request takes
        // a copy of the given rectangle of the colour buffer AS IT IS AT THIS
        // CALL — x, y from the bottom-left, the way SetViewport counts — for
        // a later Take, which hands it over as tightly packed RGBA8, rows top
        // to bottom, once the GPU is done with it. Take must come on a LATER
        // frame than the Request (after that frame's EndFrame has submitted
        // it); one request may be outstanding at a time, and a new Request
        // drops an untaken one. Both answer false when unsupported. Whatever
        // is drawn after the Request does not reach the copy, so the caller
        // is free to clear and draw the real frame over the captured one.
        virtual bool RequestBackbufferReadback(int /*x*/, int /*y*/, int /*w*/, int /*h*/) {
            return false;
        }
        virtual bool TakeBackbufferReadback(std::vector<uint8_t>& /*outRgba*/,
                                            int& /*outW*/, int& /*outH*/) {
            return false;
        }

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

        // A render target that WRAPS existing textures: `colors[0..count)`
        // become colour attachments 0..count-1 (all drawn), `depth` (may be
        // INVALID_TEXTURE) the depth-stencil attachment. Destroying it frees
        // only the framebuffer, never the textures. The shader-pack pipeline
        // builds its ping-pong pass targets with this. INVALID_RENDER_TARGET
        // on a backend without it.
        virtual RenderTargetHandle CreateRenderTargetFromTextures(const TextureHandle* /*colors*/, int /*colorCount*/,
                                                                  TextureHandle /*depth*/) {
            return INVALID_RENDER_TARGET;
        }
        // Copy the depth-stencil of one render target into another of the
        // same size (a shader pack's depthtex1/depthtex2 snapshots). The
        // bound target is unchanged afterwards. Optional.
        virtual void BlitRenderTargetDepth(RenderTargetHandle /*src*/, RenderTargetHandle /*dst*/) {}

        // ── Shader overrides (shader packs) ─────────────────────────────
        // While override mode is on, BindShader of an ENGINE shader that has
        // an override binds the pack's program instead (and that program's
        // render target), every SetUniform* on the engine handle lands on
        // the pack program (its translation declares the engine's uniform
        // names), and a shader without an override binds `defaultTarget`.
        // A pack program may be its own override, to carry a target. Off,
        // nothing here costs a lookup. Optional; the Vulkan backend has none.
        virtual void SetShaderOverrideMode(bool /*on*/, RenderTargetHandle /*defaultTarget*/) {}
        virtual void SetShaderOverride(ShaderHandle /*engine*/, ShaderHandle /*pack*/, RenderTargetHandle /*target*/) {}
        virtual void ClearShaderOverrides() {}
        // Engine shaders whose sources satisfy `match(vertex, fragment)`:
        // how the pack pipeline finds the renderers' programs without
        // touching the renderers.
        virtual std::vector<ShaderHandle> FindShadersBySource(
            const std::function<bool(const std::string&, const std::string&)>& /*match*/) { return {}; }
        // Log any pending API error, tagged with `where`; each distinct
        // (where, error) is logged once a session. Diagnostics only.
        virtual void CheckErrors(const char* /*where*/) {}
        // The depth value at one pixel of a render target's depth attachment
        // (0..1), for diagnostics. False when unsupported.
        virtual bool ReadDepthPixel(RenderTargetHandle /*rt*/, int /*x*/, int /*y*/, float& /*out*/) { return false; }
        // One line of the API's current raster state (viewport, scissor,
        // masks, tests, bound program), for diagnostics.
        virtual std::string DebugStateSummary() { return {}; }

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

        // Cull inversion: while set, every SetPipelineState swaps Back and
        // Front culling. A mirror portal's reflection flips winding, so the
        // far world is drawn with the opposite face culled — the mod's
        // applyMirrorFaceCulling. Read back with CullInverted() so nested
        // views can restore what they found.
        virtual void SetCullInvert(bool invert) { m_cullInvert = invert; }
        bool CullInverted() const { return m_cullInvert; }

        // ====================================================================
        // DRAWING
        // ====================================================================

        virtual void DrawIndexed(MeshHandle mesh, uint32_t indexCount,
                                uint32_t indexOffset = 0) = 0;

        // ── Instanced drawing ───────────────────────────────────────────────
        //
        // For the case where the same geometry is drawn many times with only a
        // per-instance transform differing. The motivating one: a detonation
        // puts tens of thousands of primed TNT on screen, and one draw call
        // each costs ~1 us on Apple's GL driver before the GPU does anything.
        //
        // NOT supported by every backend, and that is deliberate rather than a
        // gap to paper over: CreateInstancedMesh returns INVALID_MESH where it
        // is unimplemented, and the CALLER MUST keep its per-instance draw path
        // for that case. Silently drawing nothing would be worse than a slow
        // draw.
        virtual MeshHandle CreateInstancedMesh(BufferHandle /*vertexBuffer*/,
                                               BufferHandle /*indexBuffer*/,
                                               BufferHandle /*instanceBuffer*/,
                                               const VertexLayout& /*vertexLayout*/,
                                               const VertexLayout& /*instanceLayout*/) {
            return INVALID_MESH;
        }

        // Draw `instanceCount` copies of [indexOffset, indexOffset+indexCount)
        // from a mesh built by CreateInstancedMesh. `instanceByteOffset` is
        // where in the instance buffer this group's data begins: on Vulkan
        // buffer updates are visible at EXECUTION time, so several groups in
        // one frame must live at distinct offsets (writing each at offset 0,
        // which GL's orphaning semantics tolerated, would make every group
        // render the LAST group's transforms). GL honours the offset by
        // re-pointing the instance attributes.
        virtual void DrawIndexedInstanced(MeshHandle /*mesh*/, uint32_t /*indexCount*/,
                                          uint32_t /*indexOffset*/,
                                          uint32_t /*instanceCount*/,
                                          uint32_t /*instanceByteOffset*/ = 0) {}
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
        virtual void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                           IndexType indexType = IndexType::Uint32) = 0;

        // Batched multi-draw with base vertex. GL: native glMultiDrawElementsBaseVertex.
        // Default: loops DrawIndexedBaseVertex.
        virtual void MultiDrawIndexedBaseVertex(const int32_t* indexCounts,
                                                const size_t* indexByteOffsets,
                                                const int32_t* baseVertices,
                                                uint32_t drawCount,
                                                IndexType indexType = IndexType::Uint32);

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
        // Begin/End bracket GPU work (GL: GL_TIME_ELAPSED — queries cannot be
        // nested, so brackets must be sequential). GetGPUTimerResultMs is
        // NON-BLOCKING: it returns -1.0f while the result is still in flight
        // (poll again next frame; the timer stays alive) and the elapsed
        // milliseconds once ready (the timer is freed). Vulkan: stubbed —
        // Begin returns INVALID_GPU_TIMER, which callers must tolerate.

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

    protected:
        // See SetCullInvert.
        bool m_cullInvert = false;
    };

    // Factory function
    std::unique_ptr<RenderBackend> CreateRenderBackend(BackendType type);

    // Global backend instance
    extern std::unique_ptr<RenderBackend> g_renderBackend;

} // namespace Render
