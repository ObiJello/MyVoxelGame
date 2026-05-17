// File: src/client/renderer/backend/vulkan/VKBackend.hpp
#pragma once

#ifdef HAS_VULKAN

#include "../RenderBackend.hpp"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>
#include <optional>
#include <array>

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
        GLFWwindow* GetWindow() const override { return m_window; }
        void SetVSync(bool enabled) override;

        // Frame
        void BeginFrame() override;
        void EndFrame(GLFWwindow* window) override;
        void SetClearColor(float r, float g, float b, float a) override;
        void Clear(bool color, bool depth, bool stencil = false) override;
        void SetViewport(int x, int y, int width, int height) override;

        // Buffers
        BufferHandle CreateBuffer(BufferUsage usage, size_t size,
                                 const void* data, BufferAccess access) override;
        void UpdateBuffer(BufferHandle handle, size_t offset,
                         size_t size, const void* data) override;
        void DestroyBuffer(BufferHandle handle) override;
        void DeferredDestroyBuffer(BufferHandle handle) override;

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

        // Meshes
        MeshHandle CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                             const VertexLayout& layout) override;
        void DestroyMesh(MeshHandle handle) override;
        void DeferredDestroyMesh(MeshHandle handle) override;

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
        std::vector<VkFramebuffer> m_framebuffers;

        // ====================================================================
        // COMMAND BUFFERS
        // ====================================================================
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_commandBuffers;

        // ====================================================================
        // SYNCHRONIZATION
        // ====================================================================
        std::vector<VkSemaphore> m_imageAvailableSemaphores;
        std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<VkFence> m_inFlightFences;
        uint32_t m_currentFrame = 0;
        uint32_t m_currentImageIndex = 0;
        bool m_framebufferResized = false;
        bool m_frameActive = false;

        // Deferred deletion queue — resources destroyed after GPU is done with them
        struct DeferredDeletion {
            BufferHandle buffer = INVALID_BUFFER;
            MeshHandle mesh = INVALID_MESH;
        };
        std::array<std::vector<DeferredDeletion>, MAX_FRAMES_IN_FLIGHT> m_deletionQueues;

        // ====================================================================
        // BATCHED TEXTURE UPDATES
        // ====================================================================
        // UpdateTexture2D queues updates here; BeginFrame flushes them into the
        // frame command buffer before the render pass starts — zero vkQueueWaitIdle.
        struct PendingTextureUpdate {
            VkImage image;
            int x, y, width, height;
            std::vector<unsigned char> data;
        };
        std::vector<PendingTextureUpdate> m_pendingTextureUpdates;

        // Persistent staging buffer (reused across frames, persistently mapped)
        VkBuffer m_texStagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_texStagingMemory = VK_NULL_HANDLE;
        size_t m_texStagingCapacity = 0;
        void* m_texStagingMapped = nullptr;

        void FlushPendingTextureUpdates(VkCommandBuffer cmd);
        void EnsureTexStagingBuffer(size_t requiredSize);

        // ====================================================================
        // DESCRIPTOR POOL & LAYOUTS
        // ====================================================================
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_textureDescriptorLayout = VK_NULL_HANDLE;
        // Second descriptor set layout used by portal-feature shaders that
        // need richer uniforms (portal renderer, viewmodel skinning, etc.).
        // Slot 0 = sampler2D (matches block shaders for texture reuse),
        // Slot 1 = CommonUBO,
        // Slot 2 = BonesUBO (viewmodel only — bound to a 1-mat4 dummy for
        //                    other portal shaders).
        VkDescriptorSetLayout m_portalDescriptorLayout = VK_NULL_HANDLE;

        // ====================================================================
        // PIPELINE
        // ====================================================================
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        // Second pipeline layout used by portal-feature shaders. Includes
        // the portal descriptor set (texture + UBO + bones UBO) on top of
        // the same 128-byte push-constant range.
        VkPipelineLayout m_portalPipelineLayout = VK_NULL_HANDLE;
        VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
        // We create pipelines per PipelineState
        struct PipelineKey {
            PipelineState state;
            ShaderHandle shader;
            // Simplified hash/equals for now
            bool operator==(const PipelineKey& other) const;
        };
        struct PipelineKeyHash {
            size_t operator()(const PipelineKey& key) const;
        };
        std::unordered_map<size_t, VkPipeline> m_pipelines; // hash -> pipeline

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
            BufferUsage usage;
        };
        std::unordered_map<uint32_t, VKBufferInfo> m_buffers;

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
        };
        std::unordered_map<uint32_t, VKTextureInfo> m_textures;

        struct VKShaderInfo {
            VkShaderModule vertModule = VK_NULL_HANDLE;
            VkShaderModule fragModule = VK_NULL_HANDLE;
            // Which pipeline layout this shader expects. 0 = block (texture
            // sampler only), 1 = portal (texture + UBO + bones UBO). Set
            // by CreateShaderFromFiles vs CreateShaderFromFilesPortal.
            int layoutType = 0;
        };
        std::unordered_map<uint32_t, VKShaderInfo> m_shaders;

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
        };                                              // 304 bytes
        // 96-mat4 bone palette UBO for the viewmodel skinning shader.
        // 6144 bytes — well within the typical UBO size limit (16 KB).
        static constexpr int kMaxBones = 96;
        struct BonesUBO {
            glm::mat4 bones[kMaxBones];   // identity-initialised in CPU staging
        };

        // Per-frame UBO buffers (one set per frame-in-flight so a draw
        // recording the current frame doesn't stomp on a buffer still
        // being read by the GPU for the previous frame). Persistently
        // mapped — we just memcpy into them, then submit; the writes
        // are visible at draw time without an explicit flush because
        // we allocate HOST_VISIBLE | HOST_COHERENT memory.
        struct FrameUBOs {
            VkBuffer       commonBuffer  = VK_NULL_HANDLE;
            VkDeviceMemory commonMemory  = VK_NULL_HANDLE;
            void*          commonMapped  = nullptr;
            VkBuffer       bonesBuffer   = VK_NULL_HANDLE;
            VkDeviceMemory bonesMemory   = VK_NULL_HANDLE;
            void*          bonesMapped   = nullptr;
            // One descriptor set per frame (also per-frame to avoid
            // updating descriptors while previous frame is rendering).
            // Actually we allocate one descriptor set per frame whose
            // binding 0 (the texture sampler) is REWRITTEN per-draw
            // via vkUpdateDescriptorSets. Bindings 1 and 2 (the UBOs)
            // point at this frame's commonBuffer and bonesBuffer for
            // the lifetime of the frame.
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        };
        std::array<FrameUBOs, MAX_FRAMES_IN_FLIGHT> m_frameUBOs;
        // Working copies modified by SetUniform* between draws — copied
        // into the active frame's UBO buffer right before vkCmdDraw*.
        CommonUBO m_commonUBOData;
        BonesUBO  m_bonesUBOData;
        // Tracks whether the working copies have been modified since
        // the last upload, so we only memcpy when needed.
        bool m_commonUBODirty = true;
        bool m_bonesUBODirty  = true;

        struct VKMeshInfo {
            BufferHandle vertexBuffer = INVALID_BUFFER;
            BufferHandle indexBuffer = INVALID_BUFFER;
            VertexLayout layout;
        };
        std::unordered_map<uint32_t, VKMeshInfo> m_meshes;

        // Currently bound state
        ShaderHandle m_boundShader = INVALID_SHADER;
        TextureHandle m_boundTexture = INVALID_TEXTURE;

        // Mega-buffer bound state
        BufferHandle m_megaBoundVBO = INVALID_BUFFER;
        BufferHandle m_megaBoundIBO = INVALID_BUFFER;
        PipelineState m_currentPipelineState;
        VkPipeline m_currentPipeline = VK_NULL_HANDLE;

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

        // Clear color
        VkClearColorValue m_clearColor = {{0.5f, 0.7f, 1.0f, 1.0f}};

        // Memory tracking
        GPUMemoryStats m_memStats;

        // ImGui
        VkDescriptorPool m_imguiDescriptorPool = VK_NULL_HANDLE;

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
        bool CreatePortalPipelineLayout();
        bool CreateFrameUBOs();           // allocate per-frame UBO buffers + descriptor sets
        void DestroyFrameUBOs();          // counterpart called from Shutdown
        // Per-draw: ensure the active frame's UBO buffers reflect any
        // SetUniform* changes since last upload, then bind the portal
        // descriptor set (with binding 0 rewritten to the current
        // texture). Called from DrawIndexed/DrawArrays when the bound
        // shader's layoutType == 1.
        void BindPortalDescriptorForDraw(VkCommandBuffer cmd, TextureHandle tex);

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
        VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) const;
        VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) const;
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
