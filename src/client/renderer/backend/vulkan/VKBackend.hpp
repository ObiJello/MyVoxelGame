// File: src/client/renderer/backend/vulkan/VKBackend.hpp
#pragma once

#ifdef HAS_VULKAN

#include "../RenderBackend.hpp"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>
#include <string>
#include <cstdint>
#include <thread>

namespace Render {

    // Maximum number of frames that can be in-flight simultaneously
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;

    class VKBackend : public RenderBackend {
    public:
        VKBackend();
        ~VKBackend() override;

        // Lifecycle
        bool Initialize(GLFWwindow* window) override;
        void Shutdown() override;
        BackendType GetType() const override { return BackendType::Vulkan; }
        const char* GetName() const override { return "Vulkan 1.0 (MoltenVK)"; }
        GpuDeviceInfo GetDeviceInfo() const override;
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

        // Backbuffer read-back — see RenderBackend.hpp. The frame's render
        // pass is ended, the colour image copied to a host buffer, and the
        // frame resumed in m_renderPassLoad; Take waits on that frame's fence.
        bool RequestBackbufferReadback(int x, int y, int w, int h) override;
        bool TakeBackbufferReadback(std::vector<uint8_t>& outRgba, int& outW, int& outH) override;
        void SetTextureAnisotropy(TextureHandle handle, float maxAnisotropy) override;
        void UploadTextureRegionNow(TextureHandle handle, int level, int x, int y,
                                    int width, int height, const void* data) override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                 const void* data, BufferAccess access) override;
        void UpdateBuffer(BufferHandle handle, size_t offset,
                         size_t size, const void* data) override;
        // Same memcpy as UpdateBuffer: host-visible buffers are persistently
        // mapped, so neither path waits on the GPU. Callers that need a range
        // to be stable while a frame is in flight must version it themselves
        // (one region per frame in flight, as the GUI and indirect ring do).
        void UpdateBufferUnsynchronized(BufferHandle handle, size_t offset,
                                        size_t size, const void* data) override;
        void DestroyBuffer(BufferHandle handle) override;
        void DeferredDestroyBuffer(BufferHandle handle) override;
        const void* DebugGetMappedBufferPtr(BufferHandle handle) const override;
        void DebugSetMultiDrawIndirect(bool enable) override { m_multiDrawIndirect = enable; }
        // See PipelineState::depthClampEnabled — a device feature we may lack.
        bool m_depthClampSupported = false;
        bool DebugGetMultiDrawIndirect() const override { return m_multiDrawIndirect; }

        // Textures
        TextureHandle CreateEmptyTexture2D(int width, int height, TextureFormat format,
                                           int maxLevel) override;
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
        void DestroyTexture(TextureHandle handle) override;
        void DeferredDestroyTexture(TextureHandle handle) override;
        void BindTexture(TextureHandle handle, uint32_t slot) override;
        TextureHandle CreateBufferTexture(BufferHandle buffer, TextureFormat format) override;
        uintptr_t GetNativeTextureID(TextureHandle handle) const override;

        // Render targets — see RenderBackend.hpp and the long note at
        // m_renderTargets below for how they sit inside the one-pass frame.
        RenderTargetHandle CreateRenderTarget(const RenderTargetDesc& desc) override;
        void DestroyRenderTarget(RenderTargetHandle rt) override;
        void BindRenderTarget(RenderTargetHandle rt) override;
        TextureHandle GetRenderTargetColorTexture(RenderTargetHandle rt) const override;
        void ResizeRenderTarget(RenderTargetHandle rt, int w, int h) override;

        // Shaders
        ShaderHandle CreateShader(const std::string& vertexSource,
                                 const std::string& fragmentSource) override;
        ShaderHandle CreateShaderFromFiles(const std::string& vertexPath,
                                          const std::string& fragmentPath) override;
        // Portal-feature shaders need richer uniforms than block shaders
        // (mat4 uModel, vec3 uPortalColor, 96-bone palette, etc.) that
        // can't fit in push constants. These use the "portal" pipeline
        // layout which adds a UBO descriptor set on top of the texture
        // sampler. Distinguished from the block-style CreateShaderFromFiles
        // so we can keep both layouts coexisting without breaking existing
        // chunk rendering. Same _vk.spv file convention.
        ShaderHandle CreateShaderFromFilesPortal(const std::string& vertexPath,
                                                 const std::string& fragmentPath);
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
        MeshHandle CreateInstancedMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                       BufferHandle instanceBuffer,
                                       const VertexLayout& vertexLayout,
                                       const VertexLayout& instanceLayout) override;
        void DrawIndexedInstanced(MeshHandle mesh, uint32_t indexCount,
                                  uint32_t indexOffset, uint32_t instanceCount,
                                  uint32_t instanceByteOffset = 0) override;
        void DestroyMesh(MeshHandle handle) override;
        void DeferredDestroyMesh(MeshHandle handle) override;

        // Pipeline state
        void SetPipelineState(const PipelineState& state) override;
        void InvalidateStateCache() override;
        // Portal renderer's stencil override — when enabled, every
        // subsequent SetPipelineState call has its stencil fields
        // replaced with these values. Mirrors the GL backend's
        // implementation so the same C++ portal code paths work on
        // both backends without per-backend special-casing.
        void SetStencilOverride(bool enabled,
                                CompareOp compareOp,
                                StencilOp passOp,
                                uint32_t  reference,
                                uint32_t  readMask,
                                uint32_t  writeMask) override;

        // Drawing
        void DrawIndexed(MeshHandle mesh, uint32_t indexCount, uint32_t indexOffset) override;
        void DrawArrays(MeshHandle mesh, uint32_t vertexCount, uint32_t firstVertex) override;

        // Mega-buffer rendering
        void BindVertexBuffer(BufferHandle vbo, uint32_t stride) override;
        void BindIndexBuffer(BufferHandle ibo) override;
        void BindUniformBuffer(BufferHandle handle, size_t offset, size_t size) override;
        void DrawIndexedBaseVertex(uint32_t indexCount, size_t indexByteOffset, int32_t baseVertex,
                                   IndexType indexType = IndexType::Uint32) override;
        void MultiDrawIndexedBaseVertex(const int32_t* indexCounts, const size_t* indexByteOffsets,
                                        const int32_t* baseVertices, uint32_t drawCount,
                                        IndexType indexType = IndexType::Uint32) override;

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
        // ====================================================================
        // VULKAN CORE OBJECTS
        // ====================================================================
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;

        // Queue family indices
        struct QueueFamilyIndices {
            std::optional<uint32_t> graphicsFamily;
            std::optional<uint32_t> presentFamily;
            bool IsComplete() const { return graphicsFamily.has_value() && presentFamily.has_value(); }
        };
        QueueFamilyIndices m_queueFamilies;

        // ====================================================================
        // SWAPCHAIN
        // ====================================================================
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_SRGB;
        VkExtent2D m_swapchainExtent = {0, 0};
        std::vector<VkImage> m_swapchainImages;
        std::vector<VkImageView> m_swapchainImageViews;

        // Depth buffer
        VkImage m_depthImage = VK_NULL_HANDLE;
        VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
        VkImageView m_depthImageView = VK_NULL_HANDLE;
        VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;

        // ====================================================================
        // RENDER PASS & FRAMEBUFFERS
        // ====================================================================
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        // The same pass with LOAD in place of CLEAR: what a frame resumes
        // into after RequestBackbufferReadback interrupted it to copy the
        // colour image out. Identical attachments, so every pipeline built
        // against m_renderPass is compatible with it (VK render pass
        // compatibility ignores load/store ops and initial layouts).
        VkRenderPass m_renderPassLoad = VK_NULL_HANDLE;
        bool CreateRenderPassVariant(bool loadContents, VkRenderPass& out);
        std::vector<VkFramebuffer> m_framebuffers;
        // Whether the swapchain images were created with TRANSFER_SRC (the
        // surface has to allow it); without it the read-back is refused.
        bool m_swapchainTransferSrc = false;
        bool m_anisotropySupported = false;   // samplerAnisotropy feature enabled on the device

        struct BackbufferReadback {
            VkBuffer       buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            int            width = 0, height = 0;
            uint32_t       frameSlot = 0;   // whose fence proves the copy is done
            uint64_t       frameNumber = 0; // the frame that recorded the copy
            bool           pending = false;
        } m_readback;
        void DestroyReadback(bool waitForGpu);

        // ====================================================================
        // COMMAND BUFFERS
        // ====================================================================
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_commandBuffers;

        // ====================================================================
        // SYNCHRONIZATION
        // ====================================================================
        std::vector<VkSemaphore> m_imageAvailableSemaphores;   // one per frame in flight
        // One per SWAPCHAIN IMAGE, not per frame in flight. The present engine
        // holds an image's semaphore until that image is presented, which can
        // outlive the frame slot (3 images, 2 slots), and re-signalling a
        // semaphore the presentation engine still waits on is what validation
        // flags. Sized in CreateSyncObjects / RecreateSwapchain.
        std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<VkFence> m_inFlightFences;
        bool CreateRenderFinishedSemaphores();
        void DestroyRenderFinishedSemaphores();
        // Monotonic frame number, advanced in BeginFrame once the frame is
        // committed to (after the fence wait and image acquire). Ring
        // resources that are written OUTSIDE the BeginFrame..EndFrame window
        // (texture staging) key on this instead of m_currentFrame — see
        // m_texStaging.
        uint64_t m_frameNumber = 0;

        // Indirect draw ring (one per frame in flight): MultiDrawIndexedBaseVertex
        // writes VkDrawIndexedIndirectCommands here and issues ONE
        // vkCmdDrawIndexedIndirect instead of ~1,600 vkCmdDrawIndexed per
        // frame (measured 2026-08-30). Falls back to the loop when the device
        // lacks multiDrawIndirect or the ring is full.
        bool m_multiDrawIndirect = false;
        static constexpr VkDeviceSize kIndirectRingBytes = 4u << 20;   // 4 MB = 209k commands
        std::vector<VkBuffer> m_indirectBuffers;
        std::vector<VkDeviceMemory> m_indirectMemory;
        std::vector<void*> m_indirectMapped;
        std::vector<VkDeviceSize> m_indirectOffset;
        bool CreateIndirectRing();
        void DestroyIndirectRing();
        uint32_t m_currentFrame = 0;
        uint32_t m_currentImageIndex = 0;
        bool m_framebufferResized = false;
        bool m_frameActive = false;

        // Deferred deletion queue — resources destroyed after GPU is done with them
        struct DeferredDeletion {
            BufferHandle buffer = INVALID_BUFFER;
            MeshHandle mesh = INVALID_MESH;
            TextureHandle texture = INVALID_TEXTURE;   // buffer-texture views, freed before their buffer
        };
        std::array<std::vector<DeferredDeletion>, MAX_FRAMES_IN_FLIGHT> m_deletionQueues;
        // The slot whose fence guards the last frame that can still read a
        // resource being retired now (see the definition).
        uint32_t DeletionSlot() const;

        // ====================================================================
        // BATCHED TEXTURE UPDATES
        // ====================================================================
        // UpdateTexture2D writes the pixels STRAIGHT into a persistently mapped
        // staging buffer and queues a copy record; BeginFrame flushes the
        // records into the frame command buffer before the render pass starts
        // — zero vkQueueWaitIdle, one memcpy per update.
        struct PendingTextureUpdate {
            VkImage image;
            int x, y, width, height;
            uint32_t mipLevel = 0;
            size_t stagingOffset = 0;   // byte offset into the staging ring slot
            size_t byteSize = 0;
        };
        std::vector<PendingTextureUpdate> m_pendingTextureUpdates;

        // A single-time submit that is NOT waited for: texture creation and
        // the immediate uploads. In-order queue execution puts the copy
        // before any later frame that samples the image, so the wait was
        // only ever the GPU backlog (the previous frame's render, up to a
        // frame long — the per-tile jitter in the leave capture). The fence
        // says when the staging buffer and command buffer can go; reclaimed
        // at BeginFrame. DestroyTexture / Reserve / Shutdown all wait for
        // the device to idle, which covers a submit still in flight.
        struct DetachedSubmit {
            VkFence         fence   = VK_NULL_HANDLE;
            VkCommandBuffer cmd     = VK_NULL_HANDLE;
            VkBuffer        staging = VK_NULL_HANDLE;
            VkDeviceMemory  memory  = VK_NULL_HANDLE;
        };
        std::vector<DetachedSubmit> m_detachedSubmits;
        void EndSingleTimeCommandsDetached(VkCommandBuffer cmd, VkBuffer staging, VkDeviceMemory memory);
        void ReclaimDetachedSubmits(bool waitAll);

        // Staging ring: MAX_FRAMES_IN_FLIGHT + 1 slots, indexed by the number
        // of the frame that will flush them. Frame N's BeginFrame flushes slot
        // N % slots; every update queued after that — during frame N's body or
        // in the gap before BeginFrame(N+1) — goes to slot (N+1) % slots.
        //
        // Why +1 and not per frame slot: while frame N is being recorded,
        // frames N and N-1 can both still be executing (two in flight), and
        // each reads its own staging slot. A ring of exactly two would have
        // the writer for N+1 racing frame N-1's reader. With three, slot
        // (N+1) % 3 == (N-2) % 3 was last read by frame N-2, whose fence
        // BeginFrame(N) already waited on.
        struct TexStagingSlot {
            VkBuffer       buffer   = VK_NULL_HANDLE;
            VkDeviceMemory memory   = VK_NULL_HANDLE;
            size_t         capacity = 0;
            uint8_t*       mapped   = nullptr;
            size_t         used     = 0;   // write cursor; reset after flush
        };
        static constexpr uint32_t kTexStagingSlots = MAX_FRAMES_IN_FLIGHT + 1;
        std::array<TexStagingSlot, kTexStagingSlots> m_texStaging;
        // The slot updates are written into = the one the NEXT BeginFrame flushes.
        TexStagingSlot& PendingTexStaging() {
            return m_texStaging[static_cast<size_t>((m_frameNumber + 1) % kTexStagingSlots)];
        }

        void FlushPendingTextureUpdates(VkCommandBuffer cmd);
        // Returns the write pointer for `bytes` more staging data in the current
        // slot (growing it, preserving what is already queued), or nullptr.
        uint8_t* ReserveTexStaging(size_t bytes, size_t& outOffset);
        void DestroyTexStaging();
        void QueueTextureUpdate(VkImage image, uint32_t mipLevel, int x, int y,
                                int width, int height, const void* data);

        // ====================================================================
        // DESCRIPTOR POOL & LAYOUTS
        // ====================================================================
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_textureDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_texelBufferLayout = VK_NULL_HANDLE;
        // Second descriptor set layout used by portal-feature shaders that
        // need richer uniforms (portal renderer, viewmodel skinning, etc.).
        // Slot 0 = sampler2D (matches block shaders for texture reuse),
        // Slot 1 = CommonUBO,
        // Slot 2 = BonesUBO (viewmodel only — bound to a 1-mat4 dummy for
        //                    other portal shaders).
        VkDescriptorSetLayout m_portalDescriptorLayout = VK_NULL_HANDLE;
        // Set 3 of the portal pipeline layout: one dynamic uniform buffer,
        // vertex + fragment — the backend's single user uniform block
        // (RenderBackend::BindUniformBuffer). Only shaders that declare
        // set 3 read it; nothing else needs to bind it.
        VkDescriptorSetLayout m_uniformBlockLayout = VK_NULL_HANDLE;

        // ====================================================================
        // PIPELINE
        // ====================================================================
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        // Second pipeline layout used by portal-feature shaders. Includes
        // the portal descriptor set (texture + UBO + bones UBO) on top of
        // the same 128-byte push-constant range.
        VkPipelineLayout m_portalPipelineLayout = VK_NULL_HANDLE;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        // The cache is persisted to disk (<obeycraft>/cache/vk_pipeline_cache.bin)
        // so a second launch never compiles a pipeline it compiled before —
        // pipeline creation is lazy and lands mid-frame, which is a visible
        // hitch on MoltenVK where it means a Metal shader compile.
        std::string m_pipelineCacheFile;
        // The VkPipelineCache spares the SPIR-V→MSL work, not the Metal
        // pipeline-state build, and creation is still lazy: the first frame
        // of play built every pipeline it touched at once (328 ms in a
        // capture). The manifest is the list of (shader paths, state) every
        // session has built; WarmPipelines rebuilds it behind the loading
        // screen. Grows over sessions, so a dimension first visited later
        // is covered from then on.
        std::string m_pipelineManifestFile;
        bool        m_pipelinesWarmed = false;
        // The manifest lines this session's pipelines make (main thread:
        // reads m_pipelines / m_shaders); the writer thread unions them
        // with the file's.
        std::vector<std::string> PipelineManifestLines() const;
    public:
        void WarmPipelines() override;
    private:
        uint32_t m_pipelinesSinceSave = 0;      // new pipelines not yet on disk
        uint64_t m_lastPipelineFrame  = 0;      // frame of the most recent creation
        std::vector<char> LoadPipelineCacheBlob() const;
        // Fetches the cache blob and the manifest lines (main thread, a
        // millisecond) and writes both files. `synchronous` writes inline
        // (Shutdown); otherwise the writes go to m_pipelineCacheWriter —
        // the save that fires mid-session, ~5 s after a pipeline burst,
        // was a 6 ms hitch on the frame it landed on.
        void SavePipelineCache(bool synchronous);
        std::thread m_pipelineCacheWriter;
        // Every pipeline is remembered with the (state, shader) it was built
        // from so RecreateSwapchain can rebuild the set EAGERLY (one hitch at
        // resize) instead of dropping it and re-hitching lazily per draw.
        struct PipelineRecord {
            PipelineState state;
            ShaderHandle  shader = INVALID_SHADER;
            VkPipeline    pipeline = VK_NULL_HANDLE;
        };
        std::unordered_map<size_t, PipelineRecord> m_pipelines; // key -> record
        void DestroyAllPipelines();
        void RebuildAllPipelines();

        // ====================================================================
        // RESOURCE TRACKING
        // ====================================================================
        uint32_t m_nextHandle = 1;
        uint32_t AllocHandle() { return m_nextHandle++; }

        // Forward decl so we can declare the helper before VKTextureInfo's full def.
        struct VKTextureInfo;
        // Recreate the sampler from the texture's cached filter+wrap state and
        // rewrite its descriptor. Used by SetTextureFilter / SetTextureWrap so
        // each only updates its own piece without clobbering the other.
        static void RecreateSamplerFromCache(VkDevice device, VKTextureInfo& tex);

        struct VKBufferInfo {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            size_t size = 0;
            BufferUsage usage = BufferUsage::Vertex;
            // Host-visible (Dynamic/Streaming) buffers stay mapped for their
            // whole life: UpdateBuffer is then a memcpy instead of a
            // vkMapMemory/vkUnmapMemory pair per call (MoltenVK makes each of
            // those a Metal call). nullptr for device-local Static buffers.
            uint8_t* mapped = nullptr;
            // BufferUsage::Uniform only: the set-3 descriptor (one dynamic
            // UBO binding) created on first BindUniformBuffer, with the
            // window size it was written for. See m_uniformBlockLayout.
            VkDescriptorSet uniformSet = VK_NULL_HANDLE;
            size_t uniformRange = 0;
        };
        std::unordered_map<uint32_t, VKBufferInfo> m_buffers;
        // RenderBackend::BindUniformBuffer state, applied at draw time by
        // BindPortalDescriptorForDraw (set 3 of the portal pipeline layout).
        BufferHandle m_boundUniformBuffer = INVALID_BUFFER;
        uint32_t     m_boundUniformOffset = 0;

        struct VKTextureInfo {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView imageView = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            int width = 0, height = 0;
            uint32_t mipLevels = 1;
            size_t memorySize = 0;
            // Cached sampler state so SetTextureFilter / SetTextureWrap can
            // recreate the sampler while preserving each other's settings.
            // CreateTexture2D initializes these to the defaults it builds the
            // sampler with (NEAREST filter, CLAMP_TO_EDGE wrap).
            VkFilter             magFilter = VK_FILTER_NEAREST;
            VkFilter             minFilter = VK_FILTER_NEAREST;
            VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            float                maxAnisotropy = 1.0f;   // 1 = off; already clamped to the device
            // Selected by the *_MIPMAP_* filter modes. Inert while mipLevels
            // is 1, because the sampler's maxLod is then 0.
            VkSamplerMipmapMode  mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            // Needed to reallocate the image in ReserveTextureMipLevels; an
            // image's mip count is fixed at creation in Vulkan.
            VkFormat             format = VK_FORMAT_R8G8B8A8_UNORM;
            // CreateBufferTexture: a texel-buffer view over another handle's
            // VkBuffer (image/sampler/memory stay null); descriptorSet is
            // then a m_texelBufferLayout set (portal pipeline layout set 4).
            VkBufferView         bufferView = VK_NULL_HANDLE;
            // The frame number (m_frameNumber) of the last bind or upload.
            // A deferred destroy whose flush finds this older than the
            // frame still executing skips the device-wide wait: the fence
            // just passed proves the GPU is done with it.
            uint64_t             lastUsedFrame = 0;
        };
        std::unordered_map<uint32_t, VKTextureInfo> m_textures;
        // The shared body of DestroyTexture: `forceWait` = the immediate
        // form, which always drains; the deferred flush passes false and
        // waits only when the texture was used by the frame in flight.
        void DestroyTextureImpl(TextureHandle handle, bool forceWait);

        struct VKShaderInfo {
            VkShaderModule vertModule = VK_NULL_HANDLE;
            VkShaderModule fragModule = VK_NULL_HANDLE;
            // Which pipeline layout this shader expects. 0 = block (texture
            // sampler only), 1 = portal (texture + UBO + bones UBO).
            int layoutType = 0;
            // Vertex input layout this shader's pipelines should bake in.
            // If empty (default for block shaders), CreateGraphicsPipeline
            // falls back to the hardcoded 24-byte block layout. Portal/
            // viewmodel shaders register their own layout via
            // RegisterShaderVertexLayout so pipelines get the right
            // VkVertexInputAttributeDescription entries.
            VertexLayout vertexLayout;
            // Per-instance attributes (binding 1). Empty = not instanced.
            VertexLayout instanceLayout;
            // The paths the caller asked for — the shader's identity across
            // sessions, which is how the pipeline manifest names it.
            std::string vertPath;
            std::string fragPath;
            // Portal-layout shaders that take their matrices from the push
            // constants and read only the environment block of the Common
            // UBO (entity, block entity, particles, stick figures): a new
            // uMVP / uModel alone does not give them a fresh UBO slot.
            // See SetShaderIgnoresCommonMatrices.
            bool ignoresCommonMatrices = false;
        };
        std::unordered_map<uint32_t, VKShaderInfo> m_shaders;

    public:
        // Stamp a per-shader vertex layout used when building pipelines.
        // Subsystems with non-block vertex formats (viewmodel: 52-byte
        // pos+uv+normal+joints+weights; particle billboards: 24-byte
        // pos+uv+color; etc.) call this after creating their shader so
        // the backend can build matching VkVertexInputAttributeDescription
        // arrays in CreateGraphicsPipeline.
        void RegisterShaderVertexLayout(ShaderHandle shader, const VertexLayout& layout);
        void RegisterShaderInstanceLayout(ShaderHandle shader, const VertexLayout& layout);
        // A portal-layout shader that reads the Common UBO's environment
        // fields (fog, camera, sky brightness) but never its uMVP / uModel.
        // Such shaders are drawn once per entity or block entity with a new
        // MVP each time; without this every one of those draws would copy a
        // fresh 384-byte UBO slot for matrices the shader never reads, and a
        // storage room of chests could run the 8192-slot ring dry.
        void SetShaderIgnoresCommonMatrices(ShaderHandle shader);
    private:

        // ====================================================================
        // PORTAL-FEATURE UNIFORM BUFFERS
        // ====================================================================
        // Common uniforms used across portal/viewmodel/HDR/bloom shaders.
        // Packs every uniform we route from C++ SetUniform* into named
        // fields. Layout matches `layout(std140, set=0, binding=1) uniform
        // Common { ... }` in the _vk shaders. std140 means vec3 takes
        // 16 bytes (rounded up to vec4 alignment) so we use vec4 for
        // vec3-flavored uniforms with the 4th component carrying a
        // related scalar.
        struct CommonUBO {
            glm::mat4 uMVP         = glm::mat4(1.0f);   //   0
            glm::mat4 uModel       = glm::mat4(1.0f);   //  64
            glm::vec4 uPortalColor = {0, 0, 0, 0};      // 128 — rgb=color, w=uPulse
            glm::vec4 uColorDark   = {0, 0, 0, 0};      // 144 — rgb=dark, w=uOpenAmount
            glm::vec4 uColorHot    = {0, 0, 0, 0};      // 160 — rgb=hot,  w=uOpenAmountVS
            glm::vec4 uKeyDir      = {0, 0, 0, 0};      // 176 — xyz=keyDir, w=uKeyIntensity
            glm::vec4 uTint        = {1, 1, 1, 1};      // 192 — rgba (crosshair / glow tint)
            glm::vec4 uUVRange     = {0, 0, 1, 1};      // 208 — (uvMin.xy, uvMax.xy)
            glm::vec4 uScalarsA    = {0, 0, 0, 0};      // 224 — (uTime, uTimeVS, uStaticAmount, uColorScale)
            glm::vec4 uScalarsB    = {0, 0, 0, 0};      // 240 — (uPortalActive, uForceFarDepth, uOutlineMode, uFlashIntensity)
            glm::vec4 uScalarsC    = {1, 0, 1, 0};      // 256 — (uAmbient, uAlphaCutoff, uExposure, uHasBloom)
            glm::vec4 uScalarsD    = {0, 0, 0, 0};      // 272 — (uHasSprite, uUseSkin, uUseTextures, _pad)
            glm::vec2 uScreenSize  = {0, 0};            // 288
            glm::vec2 _pad         = {0, 0};            // 296 — pad to vec4 alignment
            // Environment / fog block (sky, clouds, chunk fog + night dim).
            // APPENDED so older _vk shaders that declare the 304-byte layout
            // stay valid (a shader may declare a smaller UBO block than the
            // bound buffer). New shaders declare the full 352-byte layout.
            glm::vec4 uFogColor    = {1, 1, 1, 1};      // 304 — rgb=fog color, w=1
            glm::vec4 uFogEnv      = {1e9f, 1e9f, 1e9f, 1e9f}; // 320 — (envStart, envEnd, rdStart, rdEnd)
            glm::vec4 uCamPosBright= {0, 0, 0, 1};      // 336 — xyz=uCameraPos, w=uSkyBrightness
            // MC's entity OVERLAY (OverlayTexture) — rgb = overlay colour,
            // w = STRENGTH. Zero is a clean passthrough, which is why the
            // alpha is inverted from vanilla's texel; see shaders/block.frag.
            // Appended for the same reason the fog block was: a _vk shader may
            // declare a smaller layout than the buffer it is bound to.
            glm::vec4 uOverlayColor= {0, 0, 0, 0};      // 352 — rgb=colour, w=strength
            // The view's render origin (RenderOrigin.hpp): the integer block
            // position every float the GPU sees is measured from. The
            // terrain vertex shader subtracts it from the section-origin
            // table in INTEGER arithmetic. Appended, as the fog block was.
            glm::ivec4 uRenderOrigin= {0, 0, 0, 0};     // 368 — xyz = origin, w unused
        };                                              // 384 bytes
        // 96-mat4 bone palette UBO for the viewmodel skinning shader.
        // 6144 bytes — well within the typical UBO size limit (16 KB).
        static constexpr int kMaxBones = 96;
        struct BonesUBO {
            glm::mat4 bones[kMaxBones];   // identity-initialised in CPU staging
        };

        // Per-frame UBO RING BUFFERS (one ring per frame-in-flight so a
        // draw recording the current frame doesn't stomp on a buffer
        // still being read by the GPU for the previous frame).
        // Persistently mapped — we just memcpy into the next slot, then
        // submit; the writes are visible at draw time without an explicit
        // flush because we allocate HOST_VISIBLE | HOST_COHERENT memory.
        //
        // CRITICAL: each portal frame issues MANY draws (stencil mark +
        // depth clear + depth refill + outline = 4 draws per portal pair
        // direction, ×2 directions = 8 per pair, plus crosshair, plus
        // viewmodel parts). vkCmdDrawIndexed only RECORDS the draw —
        // the GPU executes after vkQueueSubmit, at which point every
        // recorded draw reads the FINAL state of any non-versioned UBO.
        // Without per-draw slots, draw N would render with draw M's
        // uniforms (M > N), making the entire portal effect garbage.
        //
        // The descriptors are bound as VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
        // so we pass a per-draw dynamic offset to vkCmdBindDescriptorSets
        // instead of re-creating the descriptor set per draw.
        struct FrameUBOs {
            VkBuffer       commonBuffer    = VK_NULL_HANDLE;
            VkDeviceMemory commonMemory    = VK_NULL_HANDLE;
            uint8_t*       commonMapped    = nullptr;   // byte ptr for slot math
            uint32_t       commonWriteSlot = 0;         // reset to 0 each BeginFrame
            VkBuffer       bonesBuffer     = VK_NULL_HANDLE;
            VkDeviceMemory bonesMemory     = VK_NULL_HANDLE;
            uint8_t*       bonesMapped     = nullptr;
            uint32_t       bonesWriteSlot  = 0;
            // Slot-reuse bookkeeping: a draw whose Common/Bones data has not
            // changed since the previous draw rebinds the previous slot instead
            // of burning a new one — see BindPortalDescriptorForDraw.
            uint32_t lastCommonOffset = 0;
            uint32_t lastBonesOffset  = 0;
            bool     haveCommonSlot   = false;
            bool     haveBonesSlot    = false;
            bool     exhaustWarned    = false;
            // Per-frame descriptor set bound at set=1. Its CommonUBO
            // (binding=0) and BonesUBO (binding=1) entries point at the
            // BASE of the ring buffer with size = kCommonSlotStride /
            // kBonesSlotStride; the per-draw offset is supplied via
            // pDynamicOffsets to vkCmdBindDescriptorSets.
            VkDescriptorSet descriptorSet  = VK_NULL_HANDLE;
        };
        std::array<FrameUBOs, MAX_FRAMES_IN_FLIGHT> m_frameUBOs;
        // Slot count + per-slot stride for the ring buffers. Stride is
        // initialized from minUniformBufferOffsetAlignment at device
        // creation time (MoltenVK on Apple typically reports 16/64/256
        // — we round up to whichever covers both UBO sizes).
        // One slot per DRAW per frame. A scene with many individually-
        // transformed entities burns these fast: a thousand primed TNT is a
        // thousand draws, and 256 was not close.
        //
        // Cost is slotCount * alignUp(sizeof(UBO)) * MAX_FRAMES_IN_FLIGHT.
        // CommonUBO is ~368 B, so 8192 slots is ~3 MB per frame in flight;
        // BonesUBO is larger but only skinned mobs consume it, so it gets a
        // smaller bump. Cheap insurance against a class of bug that manifests
        // as the terrain flashing rather than as anything obviously wrong.
        static constexpr uint32_t kCommonSlotCount = 8192;
        static constexpr uint32_t kBonesSlotCount  = 512;
        uint32_t m_commonSlotStride = 0;   // aligned to device alignment, >= sizeof(CommonUBO)
        uint32_t m_bonesSlotStride  = 0;   // aligned, >= sizeof(BonesUBO)
        uint32_t m_uboAlignment     = 256; // discovered via VkPhysicalDeviceLimits
        // Working copies modified by SetUniform* between draws — copied
        // into the active frame's UBO buffer right before vkCmdDraw*.
        CommonUBO m_commonUBOData;
        BonesUBO  m_bonesUBOData;
        // Tracks whether the working copies have been modified since
        // the last upload, so we only memcpy when needed.
        bool m_commonUBODirty = true;
        bool m_bonesUBODirty  = true;
        // uMVP / uModel changed since the last slot. Kept apart from
        // m_commonUBODirty so a shader that ignores the UBO's matrices
        // (VKShaderInfo::ignoresCommonMatrices) can keep rebinding the
        // previous slot; any slot written copies the current matrices, so
        // a later matrix-reading draw still sees the right ones.
        bool m_commonMatricesDirty = true;

        struct VKMeshInfo {
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
            VertexLayout layout;
            // Instanced meshes (CreateInstancedMesh) additionally carry the
            // per-instance buffer bound at vertex binding 1.
            BufferHandle instanceBuffer = INVALID_BUFFER;
            VertexLayout instanceLayout;
        };
        std::unordered_map<uint32_t, VKMeshInfo> m_meshes;

        // Currently bound state
        ShaderHandle m_boundShader = INVALID_SHADER;
        // Resolved once in BindShader rather than looked up per draw: which
        // pipeline layout the bound shader uses. m_shaders is an
        // unordered_map, so the pointer stays valid until that shader is
        // erased (DestroyShader clears it).
        const VKShaderInfo* m_boundShaderInfo = nullptr;
        VkPipelineLayout    m_boundLayout     = VK_NULL_HANDLE;
        bool                m_boundIsPortal   = false;

        // Last state recorded into the ACTIVE command buffer, so a draw that
        // repeats the previous draw's bindings records nothing for them. All
        // reset in BeginFrame (a fresh command buffer has nothing bound) and
        // after ImGui records its own binds into the same buffer.
        struct RecordedBindings {
            VkBuffer        vertexBuffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            VkDeviceSize    vertexOffsets[2] = {0, 0};
            uint32_t        vertexCount = 0;              // 0 = nothing bound
            VkBuffer        indexBuffer = VK_NULL_HANDLE;
            VkIndexType     indexType   = VK_INDEX_TYPE_UINT32;
            VkDescriptorSet textureSet  = VK_NULL_HANDLE;  // block layout, set 0
            VkPipelineLayout pushLayout = VK_NULL_HANDLE;  // layout the constants were pushed with
            bool            pushValid   = false;
        };
        RecordedBindings m_recorded;
        void ResetRecordedBindings();
        // Shared front half of every draw path: pipeline bind, dynamic stencil,
        // push constants, descriptor sets. False = the draw must be skipped.
        bool PrepareDraw(VkCommandBuffer cmd);
        void BindVertexBuffersCached(VkCommandBuffer cmd, uint32_t count,
                                     const VkBuffer* buffers, const VkDeviceSize* offsets);
        void BindIndexBufferCached(VkCommandBuffer cmd, VkBuffer buffer, VkIndexType type);
        // Slot 0 is the "primary" texture (used by every shader that
        // samples a texture and by the block pipeline layout). Higher
        // slots are for shaders that need multiple textures (portal
        // renderer needs slot 0 = noise + slot 1 = colour ramp; HDR
        // tonemap will use slot 1 too). The portal pipeline layout has
        // a third descriptor set just for the secondary texture so
        // BindTexture(handle, slot=1) can attach a real texture there
        // without rewriting any descriptors mid-frame.
        static constexpr uint32_t kMaxTextureSlots = 4;
        TextureHandle m_boundTexture = INVALID_TEXTURE;       // legacy alias for slot 0
        TextureHandle m_boundTextures[kMaxTextureSlots] = {
            INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE, INVALID_TEXTURE,
        };

        // Mega-buffer bound state
        BufferHandle m_megaBoundVBO = INVALID_BUFFER;
        BufferHandle m_megaBoundIBO = INVALID_BUFFER;
        PipelineState m_currentPipelineState;
        VkPipeline m_currentPipeline = VK_NULL_HANDLE;

        // Portal renderer's stencil override. While enabled, SetPipeline-
        // State splices these values into every state it receives so
        // chunks rendered inside the see-through pass test against
        // the portal silhouette stencil mask.
        struct StencilOverride {
            bool      enabled   = false;
            CompareOp compareOp = CompareOp::Always;
            StencilOp passOp    = StencilOp::Keep;
            uint32_t  reference = 0;
            uint32_t  readMask  = 0xFFu;
            uint32_t  writeMask = 0xFFu;
        };
        StencilOverride m_stencilOverride;

        // Push constant data — must match every shader's
        // layout(push_constant) block exactly. Vulkan guarantees 128 bytes
        // of push constants minimum; we use ALL of it so portal-feature
        // shaders (crosshair, particle, simple HUD) can fit their tiny
        // uniform sets here without needing a UBO. Larger shaders
        // (portal renderer, viewmodel skinning) use UBOs via the
        // m_portalDescriptorLayout path below.
        // CRITICAL: do NOT reorder the first four fields — block_vk /
        // crosshair_vk / highlight_vk / gui_*_vk / player_billboard_vk
        // shaders ALL declare a push_constant block ending at uAlphaTest
        // (offset 76, total 80 bytes). They read pc.uAlphaTest by offset,
        // so moving uAlphaTest off offset 76 breaks alpha discard for
        // every existing shader and transparent texels render as their
        // discarded-pixel default (black). New portal-feature uniforms
        // append AFTER the existing tail.
        struct PushConstantBlock {
            glm::mat4 uMVP        = glm::mat4(1.0f);   // 0-63   (64) — existing
            glm::vec2 uScreenSize = {0, 0};            // 64-71  (8)  — existing
            float     uLineWidth  = 0.0f;              // 72-75  (4)  — existing
            float     uAlphaTest  = 0.0f;              // 76-79  (4)  — existing
            // ---- new fields below; safe to add because GLSL shaders that
            // only declare the first 80 bytes simply ignore the trailing
            // bytes of the push range. ----
            glm::vec4 uColor      = {0, 0, 0, 0};      // 80-95  (16) — tint / portal color
            glm::vec4 uUVRange    = {0, 0, 1, 1};      // 96-111 (16) — (uvMin.xy, uvMax.xy)
            glm::vec4 uScalars    = {0, 0, 0, 0};      // 112-127(16) — per-shader scalar pack
        } m_pushConstants;                              // 128 bytes — Vulkan minimum guarantee
        PushConstantBlock m_lastPushed;                 // what the command buffer last received

        // Clear color
        VkClearColorValue m_clearColor = {{0.5f, 0.7f, 1.0f, 1.0f}};

        // ====================================================================
        // OFFSCREEN RENDER TARGETS
        // ====================================================================
        // The whole frame is ONE render pass on the swapchain image. A render
        // target interrupts it: BindRenderTarget(rt) ends the pass in progress
        // (the frame's or another target's) and begins the target's own;
        // BindRenderTarget(INVALID) ends that and resumes the frame in
        // m_renderPassLoad (colour LOADed — what the frame drew survives).
        //
        // The frame pass stores neither depth nor stencil (DONT_CARE: on a
        // tile GPU that is a full-screen write saved every frame), so an
        // interruption DISCARDS the frame's depth and stencil. The resumed
        // pass clears both, so what follows sees a defined, empty depth
        // buffer. Interrupt only once the world's depth is no longer needed
        // — the entity outline composites after the hand, as MC's does.
        //
        // A target's attachments use the swapchain's colour format and the
        // frame's depth format, and its pass has the frame pass's single
        // subpass and dependency: the passes are COMPATIBLE, so every
        // pipeline built against m_renderPass draws into a target unchanged
        // (compatibility ignores load/store ops and layouts). The colour
        // image rests in SHADER_READ_ONLY_OPTIMAL between uses and is
        // sampled through an ordinary texture handle; the barriers into and
        // out of the pass are explicit, not subpass dependencies, to keep
        // the pass otherwise identical to the frame's.
        struct VKRenderTargetInfo {
            int            width = 0, height = 0;
            VkImage        colorImage  = VK_NULL_HANDLE;
            VkDeviceMemory colorMemory = VK_NULL_HANDLE;
            VkImageView    colorView   = VK_NULL_HANDLE;
            VkImage        depthImage  = VK_NULL_HANDLE;
            VkDeviceMemory depthMemory = VK_NULL_HANDLE;
            VkImageView    depthView   = VK_NULL_HANDLE;
            VkFramebuffer  framebuffer = VK_NULL_HANDLE;
            TextureHandle  colorTexture = INVALID_TEXTURE;   // registered in m_textures
        };
        std::unordered_map<uint32_t, VKRenderTargetInfo> m_renderTargets;
        VkRenderPass       m_targetRenderPass = VK_NULL_HANDLE;
        RenderTargetHandle m_activeTarget = INVALID_RENDER_TARGET;
        // The extent of whatever pass is recording: the swapchain's, or the
        // bound target's. Clear / SetViewport / the scissor clamp use it.
        VkExtent2D ActiveExtent() const;
        bool CreateTargetRenderPass();
        bool CreateTargetImages(VKRenderTargetInfo& rt);
        void DestroyTargetImages(VKRenderTargetInfo& rt);
        // Ends the pass recording now, leaving its colour image ready to be
        // sampled (a target) or re-entered (the frame).
        void SuspendActivePass(VkCommandBuffer cmd);

        // Memory tracking
        GPUMemoryStats m_memStats;

        // ImGui
        VkDescriptorPool m_imguiDescriptorPool = VK_NULL_HANDLE;

        // ====================================================================
        // GPU TIMER QUERIES
        // ====================================================================
        // VK_QUERY_TYPE_TIMESTAMP pairs. Each timer owns two slots (begin/end)
        // and the difference x timestampPeriod is the elapsed GPU nanoseconds.
        //
        // The pool is partitioned per frame-in-flight because query slots must
        // be reset before reuse, and vkCmdResetQueryPool is NOT allowed inside
        // a render pass — our whole frame is one render pass, so the reset
        // happens in BeginFrame before vkCmdBeginRenderPass, and it can only
        // safely clear the slots belonging to the frame slot being started.
        //
        // Unlike GL's GL_TIME_ELAPSED these do not force a flush, so they are
        // far cheaper than the ~2.3ms/query the GL path costs on Apple.
        static constexpr uint32_t kTimersPerFrame = 16;   // 32 query slots each
        VkQueryPool m_timestampPool = VK_NULL_HANDLE;
        float m_timestampPeriodNs = 0.0f;   // 0 = device has no usable timestamps
        struct GPUTimer {
            uint32_t begin = 0;             // query slot index of the begin stamp
            bool     ended = false;
            bool     resolved = false;      // resultMs is final; queries may be gone
            float    resultMs = -1.0f;
            uint32_t frameSlot = 0;         // which partition it was allocated from
        };
        std::unordered_map<uint32_t, GPUTimer> m_gpuTimers;
        uint32_t m_timersUsedThisFrame = 0;

        bool CreateTimestampPool();

        // ====================================================================
        // INITIALIZATION HELPERS
        // ====================================================================
        bool CreateInstance();
        bool SetupDebugMessenger();
        bool CreateSurface(GLFWwindow* window);
        bool PickPhysicalDevice();
        bool CreateLogicalDevice();
        bool CreateSwapchain(GLFWwindow* window);
        bool CreateImageViews();
        bool CreateDepthResources();
        bool CreateRenderPass();
        bool CreateFramebuffers();
        bool CreateCommandPool();
        bool CreateCommandBuffers();
        bool CreateSyncObjects();
        bool CreateDescriptorPool();
        bool CreateDescriptorSetLayout();
        bool CreatePipelineLayout();
        bool CreatePipelineCache();
        // Portal-feature uniform infrastructure
        bool CreatePortalDescriptorLayout();
        bool CreateUniformBlockLayout();
        // Set 4 of the portal pipeline layout: one uniform texel buffer
        // (fragment stage) — the terrain face map (CreateBufferTexture).
        bool CreateTexelBufferLayout();
        bool CreatePortalPipelineLayout();
        bool CreateFrameUBOs();           // allocate per-frame UBO buffers + descriptor sets
        void DestroyFrameUBOs();          // counterpart called from Shutdown
        // Per-draw: ensure the active frame's UBO buffers reflect any
        // SetUniform* changes since last upload, then bind the portal
        // descriptor set (with binding 0 rewritten to the current
        // texture). Called from DrawIndexed/DrawArrays when the bound
        // shader's layoutType == 1.
        bool BindPortalDescriptorForDraw(VkCommandBuffer cmd, TextureHandle tex);

        // ====================================================================
        // SWAPCHAIN RECREATION
        // ====================================================================
        void RecreateSwapchain(GLFWwindow* window);
        void CleanupSwapchain();

        // ====================================================================
        // UTILITY HELPERS
        // ====================================================================
        QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device) const;
        bool CheckDeviceExtensionSupport(VkPhysicalDevice device) const;
        VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const;
        // VK_EXT_swapchain_colorspace was enabled on the instance, so the
        // surface offers VK_COLOR_SPACE_PASS_THROUGH_EXT (see
        // ChooseSwapSurfaceFormat for why that is the one we want).
        bool m_hasSwapchainColorSpaceExt = false;
        VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
        VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) const;
        // Queried once in PickPhysicalDevice; FindMemoryType used to re-query
        // the driver on every buffer and image allocation.
        VkPhysicalDeviceMemoryProperties m_memProperties{};
        VkPhysicalDeviceProperties       m_deviceProperties{};
        uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
        VkFormat FindDepthFormat() const;
        VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates,
                                    VkImageTiling tiling, VkFormatFeatureFlags features) const;

        // Buffer/Image creation helpers
        bool CreateVkBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                           VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory);
        bool CreateVkImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                          VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
                          VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& memory);
        VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels);
        VkShaderModule CreateShaderModule(const std::vector<char>& code) const;

        // Single-use command buffer helpers
        VkCommandBuffer BeginSingleTimeCommands();
        void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
        void TransitionImageLayout(VkImage image, VkFormat format,
                                  VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels);
        // The same two, recorded into a caller's command buffer: one submit
        // (one GPU drain) for a whole texture upload instead of three.
        void RecordImageLayoutTransition(VkCommandBuffer cmd, VkImage image, VkFormat format,
                                         VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels);
        // Both CreateTexture2D forms: the image carries `mipLevels` levels,
        // `data` (optional) fills level 0.
        TextureHandle CreateTexture2DImpl(int width, int height, TextureFormat format,
                                          const void* data, uint32_t mipLevels);
        void RecordCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image,
                                     uint32_t width, uint32_t height);
        void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
        void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

        // Pipeline creation
        VkPipeline GetOrCreatePipeline(const PipelineState& state, ShaderHandle shader);
        VkPipeline CreateGraphicsPipeline(const PipelineState& state, ShaderHandle shader);
        size_t HashPipelineState(const PipelineState& state, ShaderHandle shader) const;

        // Push the per-draw stencil reference + read/write masks. The
        // pipeline declares these as DYNAMIC state (so we don't need a
        // separate VkPipeline per stencil-reference value), which means
        // they MUST be set on the command buffer before any draw that
        // reads stencil. Cheap (3 vkCmd* calls) and skipped entirely when
        // stencil testing is disabled in the current pipeline state.
        void ApplyDynamicStencilState(VkCommandBuffer cmd) const;

        // Vulkan enum conversion
        VkFilter ToVkFilter(TextureFilter filter) const;
        VkSamplerAddressMode ToVkWrap(TextureWrap wrap) const;
        VkCompareOp ToVkCompareOp(CompareOp op) const;
        VkBlendFactor ToVkBlendFactor(BlendFactor factor) const;
        VkCullModeFlagBits ToVkCullMode(CullMode mode) const;
        VkPolygonMode ToVkPolygonMode(PolygonMode mode) const;
        VkFrontFace ToVkFrontFace(FrontFace face) const;

        // File reading
        std::vector<char> ReadBinaryFile(const std::string& path) const;

        // Debug callback
        static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT type,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void* userData);

        // VSync state
        bool m_vsyncEnabled = true;

        // Window reference for swapchain recreation
        GLFWwindow* m_window = nullptr;
    };

} // namespace Render

#endif // HAS_VULKAN
